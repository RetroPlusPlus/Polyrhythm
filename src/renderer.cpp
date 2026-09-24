#include "retropp/renderer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "retropp/asset_policy.h"    // resolveAssetPolicy
#include "retropp/asset_registry.h"  // detail::findEmbeddedAsset
#include "retropp/frame_timing.h"    // frameTiming() — the run loop's sub-tick factor + tick signal
#include "retropp/geometry.h"
#include "retropp/postprocess.h"
#include "retropp/shader_format.h"
#include "retropp/shader_registry.h"
#include "store_upload.h"            // detail::paletteAppend / atlasRect / grownCapacity
#include "shaders/generated/blend_frag.h"
#include "shaders/generated/blit_frag.h"
#include "shaders/generated/blit_vert.h"
#include "shaders/generated/emission_blur_frag.h"
#include "shaders/generated/emission_composite_frag.h"
#include "shaders/generated/emission_extract_frag.h"
#include "shaders/generated/emission_extract_rect_frag.h"
#include "shaders/generated/emission_extract_rect_vert.h"
#include "shaders/generated/colorfill_frag.h"
#include "shaders/generated/colorfill_gather_frag.h"
#include "shaders/generated/displace_frag.h"
#include "shaders/generated/gleam_frag.h"
#include "shaders/generated/raster_frag.h"
#include "shaders/generated/postprocess_vert.h"
#include "shaders/generated/region_batch_vert.h"
#include "shaders/generated/region_select_curve_frag.h"
#include "shaders/generated/region_select_curve_mask_frag.h"
#include "shaders/generated/region_select_frag.h"
#include "shaders/generated/region_stencil_curve_frag.h"
#include "shaders/generated/region_stencil_curve_mask_frag.h"
#include "shaders/generated/region_stencil_frag.h"
#include "shaders/generated/ripple_frag.h"
#include "shaders/generated/swirl_frag.h"
#include "shaders/generated/saturation_frag.h"
#include "shaders/generated/sprite_below_frag.h"
#include "shaders/generated/sprite_emission_frag.h"
#include "shaders/generated/sprite_frag.h"
#include "shaders/generated/sprite_vert.h"
#include "shaders/generated/tile_frag.h"
#include "shaders/generated/tile_vert.h"

namespace retropp {

namespace {

// The backdrop the viewport pass clears to before compositing (opaque black, behind every
// layer) and the letterbox bars around the scaled viewport on the swapchain.
constexpr SDL_FColor kBackdropClear{0.0f, 0.0f, 0.0f, 1.0f};
constexpr SDL_FColor kLetterboxClear{0.0f, 0.0f, 0.0f, 1.0f};

// The offscreen colour intermediates — the viewport composite target, the post-process ping-pong
// scratch, and the per-layer effect scratch — and the captureViewport download all use this format.
// R16G16B16A16_FLOAT lets a colour above 1 survive the effect→blend round-trip (so a Multiply ColorFill
// with fillIntensity > 1 brightens instead of only darkening); the blit to the 8-bit swapchain is the
// single clamp back into displayable range.
constexpr SDL_GPUTextureFormat kViewportColorFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

// Decode an IEEE 754 binary16 (half) bit pattern to float. The float16 viewport texels are halves;
// captureViewport decodes them before quantizing to 8-bit.
inline float halfBitsToFloat(Uint16 h) noexcept {
    const Uint32 sign = static_cast<Uint32>(h & 0x8000u) << 16;
    Uint32       exp  = (h >> 10) & 0x1Fu;
    Uint32       mant = h & 0x03FFu;
    Uint32       bits;
    if (exp == 0u) {
        if (mant == 0u) {
            bits = sign;  // ±0
        } else {          // subnormal half → normalize into a float normal
            exp = 127u - 15u + 1u;
            while ((mant & 0x0400u) == 0u) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x03FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 0x1Fu) {  // inf / NaN
        bits = sign | 0x7F800000u | (mant << 13);
    } else {  // normal
        bits = sign | ((exp - 15u + 127u) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// Encode a float to an IEEE 754 binary16 (half) bit pattern — the inverse of halfBitsToFloat, round-to-
// nearest-even. The baked curve-mask field uploads as R16_FLOAT, so each signed-distance sample is encoded to
// a half; a value past the half range saturates to ±inf (a far distance still reads as unambiguously outside).
inline Uint16 floatToHalfBits(float f) noexcept {
    Uint32 bits;
    std::memcpy(&bits, &f, sizeof(bits));
    const Uint32 sign = (bits >> 16) & 0x8000u;
    const Uint32 exp  = (bits >> 23) & 0xFFu;
    Uint32       mant = bits & 0x7FFFFFu;
    if (exp == 0xFFu) return static_cast<Uint16>(sign | 0x7C00u | (mant ? 0x200u : 0u));  // inf / NaN
    const int e = static_cast<int>(exp) - 127 + 15;  // rebias to the half exponent
    if (e >= 0x1F) return static_cast<Uint16>(sign | 0x7C00u);  // overflow → ±inf
    if (e <= 0) {                                    // subnormal or zero
        if (e < -10) return static_cast<Uint16>(sign);  // too small → ±0
        mant |= 0x800000u;                           // restore the implicit leading 1
        const int    shift   = 14 - e;
        Uint32       half    = mant >> shift;
        const Uint32 rem     = mant & ((1u << shift) - 1u);
        const Uint32 halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (half & 1u))) ++half;  // round to nearest even
        return static_cast<Uint16>(sign | half);
    }
    Uint16       half = static_cast<Uint16>(sign | (static_cast<Uint32>(e) << 10) | (mant >> 13));
    const Uint32 rem  = mant & 0x1FFFu;              // the dropped low 13 bits
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) ++half;  // round to nearest even (carries into exp)
    return half;
}

// Quantize a colour channel to 8-bit the way the swapchain blit's UNORM write does: round(clamp(v,0,1)·255).
// A float16 headroom colour above 1 clamps to 255 — the single clamp into displayable range.
inline std::uint8_t quantizeChannel(float v) noexcept {
    return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// The Game Boy tile edge length. The atlas grid and tilemap addressing are in these units.
constexpr int kTilePx = 8;

// FNV-1a-style 64-bit fold over a tile layer's packed 32-bit cell words — the per-frame content hash a
// hash-path tilemap slot stores and compares to skip re-uploading unchanged cells. Struct padding never
// enters (the fold consumes packed words, not raw TileCell bytes). A false "unchanged" needs a same-dims
// same-layer 64-bit collision — accepted.
constexpr std::uint64_t kFnv64Offset = 14695981039346656037ull;
constexpr std::uint64_t kFnv64Prime  = 1099511628211ull;

// The same fold over a run of raw bytes, seeded so several runs chain into one hash — the content hash a
// sprite slot stores over its built GpuSprite records (and the effect records they point at), and the one
// the frame-wide sprite-effect store keeps. Raw bytes are exactly the right input here: these are the bytes
// the upload copies, both records are padding-free by construction (GpuSprite's two explicit fxPad lanes
// and SpriteFxRecord's all-4-byte fields, both value-initialized), and identical bytes mean an identical
// upload regardless of what floats they decode to.
[[nodiscard]] std::uint64_t foldBytes64(const void* data, std::size_t bytes, std::uint64_t seed) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint64_t h = seed;
    for (std::size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= kFnv64Prime; }
    return h;
}

// ── hashFrameStructure helpers — the frame-level compose-skip fingerprint ─────────────────────────────
// Fold a trivially-copyable value's object bytes into the running hash. Used for the scalar draw-state fields
// (positions, colours, transforms, enums). Never applied to a whole struct that owns a vector/span/string —
// those are followed field-by-field so the hash tracks contents, not container pointers.
template <class T>
[[nodiscard]] std::uint64_t foldValue(std::uint64_t h, const T& v) noexcept {
    static_assert(std::is_trivially_copyable_v<T>, "foldValue hashes object bytes — trivially copyable only");
    return foldBytes64(&v, sizeof(T), h);
}

// Hash a ScreenSpaceEffect. The named built-in fields are folded individually — NOT as one object-byte run —
// because a struct's padding is indeterminate across separately-constructed instances, so a whole-object hash
// would make two identical effects hash unequal (a permanent under-skip for any effect-bearing scene). The
// generated custom-shader union sits after paramTable; it is read only by a Custom effect, so its bytes enter
// the hash only for kind == Custom (catching a custom-param change) and stay out of every built-in's
// fingerprint. paramTable (a span) is followed into its element contents, never hashed as a pointer.
[[nodiscard]] std::uint64_t foldEffect(std::uint64_t h, const ScreenSpaceEffect& e) noexcept {
    h = foldValue(h, e.kind);
    h = foldValue(h, e.customShader);
    h = foldValue(h, e.amplitude);
    h = foldValue(h, e.frequency);
    h = foldValue(h, e.phase);
    h = foldValue(h, e.axis);
    h = foldValue(h, e.edge);
    h = foldValue(h, e.scope);
    h = foldValue(h, e.center);
    h = foldValue(h, e.decay);
    h = foldValue(h, e.stencil);
    h = foldValue(h, e.feather);
    h = foldValue(h, e.fill);
    h = foldValue(h, e.fillIntensity);
    h = foldValue(h, e.sweep);
    h = foldValue(h, e.width);
    h = foldValue(h, e.gain);
    h = foldValue(h, e.slant);
    h = foldValue(h, e.saturation);
    h = foldValue(h, e.radius);
    h = foldValue(h, e.threshold);
    h = foldValue(h, e.intensity);
    if (!e.paramTable.empty())
        h = foldBytes64(e.paramTable.data(), e.paramTable.size() * sizeof(Vec4), h);
    h = foldValue(h, e.paramTable.size());
    if (e.kind == ScreenSpaceEffectKind::Custom) {
        // The generated custom-shader params occupy [end of paramTable, end of struct) — every reflected field
        // a Custom effect sets, folded so a custom-param change is caught. (Trailing struct padding rides
        // along; for a Custom effect that can only ever force an extra recompose, never a wrong skip.)
        const auto* structEnd  = reinterpret_cast<const unsigned char*>(&e) + sizeof(ScreenSpaceEffect);
        const auto* afterTable = reinterpret_cast<const unsigned char*>(&e.paramTable) + sizeof(e.paramTable);
        h = foldBytes64(afterTable, static_cast<std::size_t>(structEnd - afterTable), h);
    }
    return h;
}

[[nodiscard]] std::uint64_t foldShape(std::uint64_t h, const ShapePoints& s) noexcept {
    if (!s.points.empty()) h = foldBytes64(s.points.data(), s.points.size() * sizeof(Point), h);
    h = foldValue(h, s.points.size());
    h = foldValue(h, s.radius);
    h = foldValue(h, s.transform);
    if (!s.curve.empty()) h = foldBytes64(s.curve.data(), s.curve.size() * sizeof(CurveSegment), h);
    h = foldValue(h, s.curve.size());
    h = foldValue(h, s.invert);
    h = foldValue(h, s.strokeWidth);
    return foldValue(h, s.curveMask);
}

[[nodiscard]] std::uint64_t foldRegion(std::uint64_t h, const Region& r) noexcept {
    h = foldBytes64(r.key.value.data(), r.key.value.size(), h);
    h = foldValue(h, r.key.value.size());
    h = foldShape(h, r.shape);
    for (const ScreenSpaceEffect& e : r.effects) h = foldEffect(h, e);
    h = foldValue(h, r.effects.size());
    h = foldValue(h, r.alpha);
    return foldValue(h, r.blend);
}

[[nodiscard]] std::uint64_t foldSprite(std::uint64_t h, const Sprite& s) noexcept {
    h = foldBytes64(s.key.value.data(), s.key.value.size(), h);
    h = foldValue(h, s.key.value.size());
    // Placement + appearance (the fields makeGpuSprite bakes into the record). x/y/alpha/transform/pivot/origin
    // are the interpolated set — hashed at the SUBMISSION value, which equals the composited value when settled
    // (the compose skip only runs settled), so the fingerprint tracks what is drawn.
    h = foldValue(h, s.x);       h = foldValue(h, s.y);       h = foldValue(h, s.z);
    h = foldValue(h, s.size);    h = foldValue(h, s.atlas);   h = foldValue(h, s.tile);
    h = foldValue(h, s.palette); h = foldValue(h, s.alpha);   h = foldValue(h, s.blend);
    h = foldValue(h, s.flipX);   h = foldValue(h, s.flipY);   h = foldValue(h, s.rotation);
    h = foldValue(h, s.transform); h = foldValue(h, s.pivot); h = foldValue(h, s.origin);
    for (const Anchor& a : s.anchors) {
        h = foldBytes64(a.label.data(), a.label.size(), h);
        h = foldValue(h, a.label.size());
        h = foldValue(h, a.x);
        h = foldValue(h, a.y);
    }
    h = foldValue(h, s.anchors.size());
    for (const ScreenSpaceEffect& e : s.effects) h = foldEffect(h, e);
    h = foldValue(h, s.effects.size());
    for (const Region& r : s.regions) h = foldRegion(h, r);
    return foldValue(h, s.regions.size());
}

[[nodiscard]] std::uint64_t foldTiles(std::uint64_t h, const TileContent& t) noexcept {
    h = foldValue(h, t.widthInTiles);
    h = foldValue(h, t.heightInTiles);
    h = foldValue(h, t.wrap);
    if (t.contentChanged.has_value()) {
        // Huge-map opt-out: the caller answers the change question, so never touch the cells here (matching the
        // copy pass, which packs/hashes nothing on this path). Fold only the declaration — a `false` is stable
        // frame-to-frame (skip-eligible), a `true` folds distinctly AND makes the frame declared-dirty at the
        // renderer, which forces a recompose regardless of hash equality (a manual animated map's cells can
        // differ under a stable declaration byte).
        return foldValue(h, static_cast<std::uint8_t>(*t.contentChanged ? 2 : 1));
    }
    h = foldValue(h, static_cast<std::uint8_t>(0));  // auto path
    const std::size_t count = static_cast<std::size_t>(t.widthInTiles) * static_cast<std::size_t>(t.heightInTiles);
    const std::size_t have  = std::min(count, t.cells.size());
    for (std::size_t k = 0; k < count; ++k) {
        const PackedTileCell pc = (k < have) ? packTileCell(t.cells[k]) : PackedTileCell{};  // pad short maps with cell 0
        h ^= pc.w0; h *= kFnv64Prime; h ^= pc.w1; h *= kFnv64Prime;
    }
    return h;
}

[[nodiscard]] std::uint64_t foldLayer(std::uint64_t h, const DrawLayer& l) noexcept {
    h = foldBytes64(l.key.value.data(), l.key.value.size(), h);
    h = foldValue(h, l.key.value.size());
    h = foldValue(h, l.z);
    h = foldValue(h, l.size);
    h = foldValue(h, l.scroll);
    h = foldValue(h, l.alpha);
    h = foldValue(h, l.blend);
    h = foldValue(h, l.transform);
    h = foldValue(h, l.transformEdge);
    h = foldValue(h, l.advancesEvery.value_or(0u));  // 0 stands for "unset" — never a valid declaration
    switch (contentKind(l.content)) {
        case LayerContentKind::Tiles: {
            h = foldValue(h, static_cast<std::uint8_t>(0));
            h = foldTiles(h, std::get<TileContent>(l.content));
            break;
        }
        case LayerContentKind::Sprites: {
            h = foldValue(h, static_cast<std::uint8_t>(1));
            const SpriteContent& sc = std::get<SpriteContent>(l.content);
            for (const Sprite& s : sc.sprites) h = foldSprite(h, s);
            h = foldValue(h, sc.sprites.size());
            break;
        }
        case LayerContentKind::Raster: {
            // Everything about the raster EXCEPT its pixels: a full-color raster differs every time
            // its source draws, so folding it in would cost more than the recompose the fingerprint
            // exists to skip. The generation stands in for them exactly — a new raster is a new
            // number — so the fingerprint SEES a source draw without reading a byte of it, and this
            // content type needs no declared-dirty escape hatch the way a declared tilemap does.
            h = foldValue(h, static_cast<std::uint8_t>(2));
            const RasterContent& gc = std::get<RasterContent>(l.content);
            h = foldValue(h, gc.width);
            h = foldValue(h, gc.height);
            h = foldValue(h, gc.format);
            h = foldValue(h, gc.generation);
            break;
        }
    }
    for (const ScreenSpaceEffect& e : l.effects) h = foldEffect(h, e);
    h = foldValue(h, l.effects.size());
    for (const Region& r : l.regions) h = foldRegion(h, r);
    return foldValue(h, l.regions.size());
}

// Whether any tile layer declares contentChanged == true (the huge-map path saying "these cells changed").
// hashFrameStructure deliberately does not read a declared map's cells, so the renderer forces a recompose on
// a `true` rather than trusting the fingerprint. A raster layer needs nothing here — its generation is
// folded into the fingerprint, so a source that drew is already visible to it.
[[nodiscard]] bool frameDeclaredDirty(const FrameDrawState& frame) noexcept {
    for (const DrawLayer& l : frame.layers) {
        if (contentKind(l.content) != LayerContentKind::Tiles) continue;
        if (std::get<TileContent>(l.content).contentChanged.value_or(false)) return true;
    }
    return false;
}

using detail::kPaletteStoreWidth;  // the palette store's row width, in colours (store_upload.h)

// Per-layer uniform block — must match tile.frag.hlsl's TileUniforms cbuffer exactly
// (std140-style 16-byte-register packing; no member straddles a 16-byte boundary). Each cell carries
// its own atlas + palette handle directly, so there is no per-layer palette-set or atlas-set table:
// the palette handle IS the flat offset, and the atlas handle indexes the global atlas-region store
// texture both frag stages bind.
// Per-layer uniform block for a raster layer — must match raster.frag.hlsl's RasterUniforms cbuffer
// exactly (std140-style 16-byte-register packing; no member straddles a 16-byte boundary). The raster
// is addressed directly, so there is no atlas, palette or cell handle here — its dimensions are the
// whole of what the fragment needs to find a pixel.
struct RasterUniforms {
    float scrollX, scrollY;      // register 0
    float layerW, layerH;
    float rasterW, rasterH;      // register 1: the raster's own dimensions, pixels
    float alpha, composeScale;
    float snap;                  // register 2: 1 = snap the transform's destination pixel to the grid
    float pad0, pad1, pad2;
    float invRow0[4];            // inverse transform homography, rows 0..2 (registers 3..5)
    float invRow1[4];
    float invRow2[4];
    std::uint32_t hasTransform;  // register 6: x = hasTransform (0/1)
    std::uint32_t transformEdge; //              y = footprint edge (0 Blank / 1 Stretch)
    std::uint32_t pad3, pad4;    //              z, w pad
};

struct TileUniforms {
    float scrollX, scrollY;      // register 0
    float layerW, layerH;
    float tilemapW, tilemapH;    // register 1
    float tilePx, alpha;
    float paletteStoreW;         // register 2: palette-store row width (colours); flat offset → (f%W, f/W)
    float composeScale;          //             compose grid ÷ viewport (1 = faithful); output pixel → viewport
    float snap;                  //             1 = snap the transform's destination pixel to the viewport grid
    float pad2;
    float invRow0[4];            // inverse transform homography, rows 0..2 (registers 3..5)
    float invRow1[4];
    float invRow2[4];
    std::uint32_t hasTransform;  // register 6: x = hasTransform (0/1)
    std::uint32_t transformEdge; //              y = footprint edge (0 Blank / 1 Stretch)
    std::uint32_t wrap;          //              z = tilemap wrap mode (0 Repeat / 1 Clamp / 2 Blank)
    std::uint32_t pad3;          //              w pad
};
static_assert(sizeof(TileUniforms) == 112, "TileUniforms must match the HLSL cbuffer layout");

// The sprite vertex stage carries NO uniform buffer: the screen→clip transform is baked CPU-side
// into each GpuSprite (retropp::makeGpuSprite), so the vertex stage is a pure storage-buffer read.
// This sidesteps a Metal [[buffer]]-namespace collision a storage+uniform vertex stage would hit
// under the single-pass shader toolchain.

// Sprite fragment uniform — must match sprite.frag.hlsl's SpriteFragUniforms cbuffer (two 16-byte
// registers). Each sprite names its own atlas, so the sheet's store region (storeY, cols) is looked up
// per-sprite from the global atlas-region store the frag binds — not carried here.
//
// The below and emission sprite fragments declare only the first register of this cbuffer; they read no
// field, so the grid lane means nothing to them and the extra register they never name is harmless.
struct SpriteFragUniforms {
    float tilePx;        // register 0: tile edge length, pixels
    float alpha;         // layer alpha, [0,1]
    float paletteStoreW; // palette-store row width (colours); flat offset → (f%W, f/W)
    float composeScale;  // compose grid ÷ viewport (1 = faithful) — the analytic branch's output→viewport map
    float evalSnap;      // register 1: 1 = Viewport grid (the field read snaps to the cell centre), 0 = Output
    float pad[3];        // the register's remaining lanes — a cbuffer register is 16 bytes whatever it holds
};
static_assert(sizeof(SpriteFragUniforms) == 32, "SpriteFragUniforms must match the HLSL cbuffer");

// Row-displacement stage uniform — must match displace.frag.hlsl's DisplaceUniforms
// cbuffer exactly (two 16-byte registers). Filled from retropp::displaceParams(effect, viewport);
// the layout mirrors DisplaceParams's fields, with the axis carried as a uint.
struct DisplaceFragUniforms {
    float         amplitude, frequency, phase;  // register 0
    std::uint32_t axis;                         //   (0 = Horizontal, 1 = Vertical)
    float         invViewportW, invViewportH;
    std::uint32_t edge;                         //   (0 = Blank, 1 = Stretch)
    std::uint32_t blankTransparent;             //   (0 = opaque backdrop, 1 = transparent) — register 1
    float         snap;                         //   (1 = snap to the viewport grid, crisp) — register 2
    float         pad0, pad1, pad2;
};
static_assert(sizeof(DisplaceFragUniforms) == 48, "DisplaceFragUniforms must match the displace.frag cbuffer");

// Built-in radial-ripple stage uniform — must match ripple.frag.hlsl's RippleUniforms
// cbuffer exactly (two 16-byte registers). Filled from retropp::rippleParams(effect, viewport);
// the layout mirrors RippleParams's fields (centre normalized px→UV, the inverse-viewport amplitude
// scale, the radial decay).
struct RippleFragUniforms {
    float centerU, centerV, amplitude, frequency;  // register 0
    float phase, invViewportW, invViewportH, decay; // register 1
    float snap;                                     // (1 = snap to the viewport grid, crisp) — register 2
    float pad0, pad1, pad2;
};
static_assert(sizeof(RippleFragUniforms) == 48, "RippleFragUniforms must match the ripple.frag cbuffer");

// Built-in angular-twist stage uniform — must match swirl.frag.hlsl's SwirlUniforms cbuffer exactly (two
// 16-byte registers). Filled from retropp::swirlParams(effect): the centre and radius stay in viewport
// pixels (the shader works in square pixel units so the disc is circular on any aspect) and the twist
// arrives already resolved to radians, the degrees-to-radians conversion having happened in swirlParams.
struct SwirlFragUniforms {
    float centerX, centerY, twist, radius;         // register 0
    float invViewportW, invViewportH, snap, pad0;  // register 1
};
static_assert(sizeof(SwirlFragUniforms) == 32, "SwirlFragUniforms must match the swirl.frag cbuffer");

// Built-in colour-fill stage uniform — must match colorfill.frag.hlsl's ColorFillUniforms cbuffer exactly:
// one 16-byte register holding the fill colour (rgb, normalized) + a pad lane. Filled from
// retropp::colorFillParams(effect). The ColorFill stage emits this colour, opaque; opacity belongs to the
// owning Region / Layer / Frame.
struct ColorFillFragUniforms {
    float r, g, b, pad;   // register 0 — fill colour (normalized) + pad
};
static_assert(sizeof(ColorFillFragUniforms) == 16, "ColorFillFragUniforms must match the colorfill.frag cbuffer");

// Built-in gleam stage uniform — must match gleam.frag.hlsl's GleamUniforms cbuffer exactly (one 16-byte
// register: the sweep/width/gain/slant scalars). Filled from retropp::gleamParams(effect). The stage
// multiplies each pixel by a luminance-keyed diagonal band; gain 0 is identity.
struct GleamFragUniforms {
    float sweep, width, gain, slant;   // register 0
};
static_assert(sizeof(GleamFragUniforms) == 16, "GleamFragUniforms must match the gleam.frag cbuffer");

// Built-in colour-saturation stage uniform — must match saturation.frag.hlsl's SaturationUniforms cbuffer (one
// 16-byte register; the shader reads only the first float). Filled from retropp::saturationParams(effect). The
// stage pulls each pixel toward its own luminance; saturation 1 is identity, 0 greyscale.
struct SaturationFragUniforms {
    float saturation, pad0, pad1, pad2;   // register 0
};
static_assert(sizeof(SaturationFragUniforms) == 16, "SaturationFragUniforms must match the saturation.frag cbuffer");

// Emission-extract uniform — must match emission_extract.frag.hlsl's cbuffer exactly (two 16-byte
// registers). Everything the developer authored except the reach folds in at this stage: which kind's
// emission to compute, the threshold that keys it, the intensity that scales it, and Glow's authored tint
// (from retropp::bloomParams / glowParams). Every later stage of the chain is parameter-free.
struct EmissionExtractFragUniforms {
    float glow, threshold, intensity, pad0;  // register 0
    float tintR, tintG, tintB, pad1;         // register 1
};
static_assert(sizeof(EmissionExtractFragUniforms) == 32,
              "EmissionExtractFragUniforms must match the emission_extract.frag cbuffer");

// Rect-extract uniform — must match emission_extract_rect.frag.hlsl's cbuffer exactly (one 16-byte
// register). A field is a viewport-grid image and the accumulator it reads is a compose-grid one, so the
// scale between them is the only thing the pass needs beyond its per-rect record. Threshold and extract kind
// ride the record; intensity and tint are absent because the extract is neutral — one field serves lenses of
// differing strength and colour, each applying its own as it samples.
struct EmissionExtractRectFragUniforms {
    float composeScale, pad0, pad1, pad2;
};
static_assert(sizeof(EmissionExtractRectFragUniforms) == 16,
              "EmissionExtractRectFragUniforms must match the emission_extract_rect.frag cbuffer");

// Emission-blur uniform — must match emission_blur.frag.hlsl's cbuffer exactly (two 16-byte registers).
// One axis of the separable Gaussian: the per-tap UV step (which axis, and how far — one viewport pixel at
// full resolution, four when reduced), the resolved kernel from retropp::emissionChainPlan, and the
// viewport cell grid the crisp path snaps taps to (0/0 evaluates at the exact fragment).
struct EmissionBlurFragUniforms {
    float stepU, stepV, taps, invNorm;   // register 0
    float inv2Sigma2, snapW, snapH, pad0;  // register 1
};
static_assert(sizeof(EmissionBlurFragUniforms) == 32,
              "EmissionBlurFragUniforms must match the emission_blur.frag cbuffer");

// Emission-composite uniform — must match emission_composite.frag.hlsl's cbuffer exactly (one 16-byte
// register). The kind selects the alpha rule; the strength already rides the emission.
struct EmissionCompositeFragUniforms {
    float glow, pad0, pad1, pad2;  // register 0
};
static_assert(sizeof(EmissionCompositeFragUniforms) == 16,
              "EmissionCompositeFragUniforms must match the emission_composite.frag cbuffer");

// Scratch buffer size for a custom effect's cbuffer. A custom shader declares its OWN cbuffer
// (its own named params); the build reflects it and generates a packer (custom_effect_packers.h) that
// writes those params' bytes from the effect's inline fields. The renderer hands the packer a buffer this
// big, then pushes the size the packer reports — it never reads the param fields itself, so its view of
// ScreenSpaceEffect is independent of which params any consumer shader declares. 256 B covers a generous
// cbuffer (16 float4 registers); the packer's size is validated against it.
inline constexpr std::uint32_t kMaxCustomEffectUniformBytes = 256;

// One batched region's GPU instance record — must match region_batch.vert.hlsl's RegionBatchRecord (48 B)
// exactly. uvBox is the covering quad in normalized frame uv (u0,v0,u1,v1 — regionScissorRect converted
// px→uv); spine is the shape's SDF spine (p0.xy, p1.xy) in viewport px; radiusPad.x is the SDF radius
// (yzw padding). Built from a retropp::RegionBatchInstance + the compose dimensions.
struct GpuRegionBatch {
    float uvBox[4];
    float spine[4];
    float radiusPad[4];
};
static_assert(sizeof(GpuRegionBatch) == 48, "GpuRegionBatch must match region_batch.vert's 48-byte record");

// The gather pass's run header — must match the GATHER shaders' RetroppGatherInfo cbuffer (b1, space3)
// exactly (one 16-byte register). `regionCount` is how many per-region records the run's storage buffer
// holds (the gather entry point's loop bound). `strideFloat4s` is one record's size in float4s — read
// only by the built-in ColorFill gather shader, whose vertex allotment (and so its stride) is resolved
// per run; a custom stage's generated gather variant bakes its stride as a compile-time constant and
// receives 0 here. The pad rounds to a full register. Pushed at fragment uniform slot 1.
struct GpuGatherInfo {
    std::uint32_t regionCount;
    std::uint32_t strideFloat4s;
    std::uint32_t pad1, pad2;
};
static_assert(sizeof(GpuGatherInfo) == 16, "GpuGatherInfo must match the GATHER shaders' RetroppGatherInfo cbuffer");

// The engine-controlled custom-effect cbuffer — must match retropp_effect.hlsli's
// RetroppEngineEffect (b0, space3) exactly. Carries the edge mode sampleSource() obeys (from the effect's
// `edge`: 0 = Blank, transparent outside the frame, the default; 1 = Stretch, clamp / smear), this
// effect's per-row data-table location in the row-data store (rowTableY + rowTableRows; rowTableRows == 0
// ⇒ no table), and the evaluation grid (snap + the viewport dims the generated wrapper's snap math needs;
// see the preamble's evaluation-grid note). The engine fills + pushes this for EVERY custom stage
// (slot 0), so the layer governs the edge and the engine forwards the table + grid, not the shader itself.
struct EngineEffectFragUniforms {
    std::uint32_t edgeClamp;      // 0 = blank, 1 = clamp
    std::uint32_t rowTableY;      // this effect's row-table offset (rows) into the row-data store
    std::uint32_t rowTableRows;   // table row count (0 = no table)
    std::uint32_t snap;           // 1 = Viewport grid (crisp), 0 = Output grid (smooth)
    float viewportW;              // logical viewport dimensions for the snap math
    float viewportH;
    float enginePad0;             // → 32 bytes (two cbuffer registers)
    float enginePad1;
};
static_assert(sizeof(EngineEffectFragUniforms) == 32,
              "EngineEffectFragUniforms must match retropp_effect.hlsli's RetroppEngineEffect cbuffer");

// The polygon-vertex cap the region cbuffer carries (packed two-per-register → uPoints[32] in the
// shader). The ShapePoints API stays unbounded (std::vector); a longer polygon is truncated here and
// warned. True-unbounded counts via a fragment storage buffer are a follow-up (needs on-device bring-up).
inline constexpr int kRegionCbufferMaxPoints = 64;
static_assert(static_cast<std::size_t>(kRegionCbufferMaxPoints) == kColorFillGatherMaxPoints,
              "the ColorFill gather record cap must match the region cbuffer cap — the two paths must "
              "truncate a long polygon to the same shape");

// Region-select gate uniform — must match region_select.frag.hlsl's RegionUniforms cbuffer
// exactly (37 × 16-byte registers). The ≤64 polygon vertices pack two-per-register (a cbuffer
// array would 16-byte-pad each float2), so points[128] lays out as the shader's `float4 uPoints[32]`.
// The inverse homography + misc register mirror retropp::regionParams; count is a float (uMisc.z), the
// EFFECTIVE (possibly truncated) vertex count, rounded back to a uint in the shader; the trailing
// register carries the region's blend mode (the gate grades the effect over the scene before the alpha mix).
struct RegionSelectFragUniforms {
    float points[2 * kRegionCbufferMaxPoints];  // registers 0..31 : ≤64 vertices, xy packed 2-per-register
    float invRow0[4];                            // register 32
    float invRow1[4];                            // register 33
    float invRow2[4];                            // register 34
    float invViewportW, invViewportH;            // register 35
    float count;                                 //   (the effective vertex count, rounded to uint in the shader)
    float radius;
    float blend;                                 // register 36 : blend mode (BlendMode as float, rounded to uint)
    float snap;                                  //   (uBlend.y) 1 = snap the gate to the viewport grid (crisp)
    float pad1, pad2;
};
static_assert(sizeof(RegionSelectFragUniforms) == 592, "RegionSelectFragUniforms must match the region_select.frag cbuffer");

// Resolve a region + viewport + blend mode into the region_select cbuffer bytes. Mirrors retropp::regionParams
// + packs the vertices two-per-register, truncating past kRegionCbufferMaxPoints (with a warning) and carrying
// the EFFECTIVE count so the shader never reads an unfilled slot. `blend` is the owning Region's blend mode.
RegionSelectFragUniforms makeRegionUniforms(const ShapePoints& region, ViewportResolution viewport,
                                            float alpha, BlendMode blend, float snap) {
    const RegionParams p = regionParams(region, PixelSize{viewport.width, viewport.height});
    RegionSelectFragUniforms u{};
    const std::size_t cap = static_cast<std::size_t>(kRegionCbufferMaxPoints);
    const std::size_t n   = std::min(region.points.size(), cap);
    if (region.points.size() > cap) {
        SDL_Log("retropp: region polygon has %zu vertices; truncated to %d (cbuffer cap)",
                region.points.size(), kRegionCbufferMaxPoints);
    }
    for (std::size_t i = 0; i < n; ++i) {
        u.points[2 * i]     = region.points[i].x;
        u.points[2 * i + 1] = region.points[i].y;
    }
    u.invRow0[0] = p.invRow0[0]; u.invRow0[1] = p.invRow0[1]; u.invRow0[2] = p.invRow0[2]; u.invRow0[3] = region.invert ? 1.0f : 0.0f;
    u.invRow1[0] = p.invRow1[0]; u.invRow1[1] = p.invRow1[1]; u.invRow1[2] = p.invRow1[2]; u.invRow1[3] = p.strokeWidth;
    u.invRow2[0] = p.invRow2[0]; u.invRow2[1] = p.invRow2[1]; u.invRow2[2] = p.invRow2[2]; u.invRow2[3] = alpha;
    u.invViewportW = p.invViewportW;
    u.invViewportH = p.invViewportH;
    u.count        = static_cast<float>(n);  // the effective (post-truncation) vertex count
    u.radius       = p.radius;
    u.blend        = static_cast<float>(blend);
    u.snap         = snap;                    // uBlend.y — 1 = snap the gate to the viewport grid
    return u;
}

// The curve-boundary segment cap the curve region cbuffer carries (two registers per segment). The
// ShapePoints::curve API stays unbounded; a longer boundary is truncated here and warned, mirroring the
// polygon vertex cap. A genuinely longer boundary would move to a fragment storage buffer (its own
// on-device bring-up).
inline constexpr int kCurveRegionMaxSegments = 32;

// Curve region-select gate uniform — must match region_select_curve.frag.hlsl's CurveRegionUniforms
// cbuffer exactly (69 × 16-byte registers). Each segment packs two registers: register A {start.xy,
// control.xy}, register B {end.xy, degree, pad}. The inverse homography + misc tail mirror
// retropp::curveRegionParams; count is the EFFECTIVE (post-truncation) segment count; the trailing
// register carries the region's blend mode (the gate grades the effect over the scene before the alpha mix).
struct CurveRegionSelectFragUniforms {
    float segs[8 * kCurveRegionMaxSegments];  // registers 0..63 : 2 regs/segment (8 floats)
    float invRow0[4];                          // register 64
    float invRow1[4];                          // register 65
    float invRow2[4];                          // register 66
    float invViewportW, invViewportH;          // register 67
    float count;                               //   (the effective segment count, rounded to uint in the shader)
    float radius;
    float blend;                               // register 68 : blend mode (BlendMode as float, rounded to uint)
    float snap;                                //   (uBlend.y) 1 = snap the gate to the viewport grid (crisp)
    float pad1, pad2;
};
static_assert(sizeof(CurveRegionSelectFragUniforms) == 1104,
              "CurveRegionSelectFragUniforms must match the region_select_curve.frag cbuffer (69 registers)");

// Resolve a curve region + viewport + blend mode into the curve region-select cbuffer bytes. Mirrors
// retropp::curveRegionParams + packs the per-segment control points two registers each, truncating past
// kCurveRegionMaxSegments (with a warning) and carrying the EFFECTIVE count so the shader never reads an
// unfilled slot. The boundary is assumed analytic (linear + quadratic); a cubic boundary is sampled to a
// polygon by sampleCurveRegionToPolygon before this path. `blend` is the owning Region's blend mode.
CurveRegionSelectFragUniforms makeCurveRegionUniforms(const ShapePoints& region,
                                                      ViewportResolution viewport, float alpha,
                                                      BlendMode blend, float snap) {
    const CurveRegionParams p = curveRegionParams(region, PixelSize{viewport.width, viewport.height});
    CurveRegionSelectFragUniforms u{};
    const std::size_t cap = static_cast<std::size_t>(kCurveRegionMaxSegments);
    const std::size_t n   = std::min(region.curve.size(), cap);
    if (region.curve.size() > cap) {
        SDL_Log("retropp: curve region has %zu segments; truncated to %d (cbuffer cap)",
                region.curve.size(), kCurveRegionMaxSegments);
    }
    for (std::size_t i = 0; i < n; ++i) {
        const CurveSegment& s = region.curve[i];
        const Vec2 start = s.p0;
        const Vec2 ctrl  = s.degree == CurveDegree::Quadratic ? s.p1 : s.p0;
        const Vec2 end   = segmentEnd(s);
        float* a = &u.segs[8 * i];
        a[0] = start.x; a[1] = start.y; a[2] = ctrl.x; a[3] = ctrl.y;
        a[4] = end.x;   a[5] = end.y;   a[6] = static_cast<float>(static_cast<int>(s.degree)); a[7] = 0.0f;
    }
    u.invRow0[0] = p.invRow0[0]; u.invRow0[1] = p.invRow0[1]; u.invRow0[2] = p.invRow0[2]; u.invRow0[3] = region.invert ? 1.0f : 0.0f;
    u.invRow1[0] = p.invRow1[0]; u.invRow1[1] = p.invRow1[1]; u.invRow1[2] = p.invRow1[2]; u.invRow1[3] = p.strokeWidth;
    u.invRow2[0] = p.invRow2[0]; u.invRow2[1] = p.invRow2[1]; u.invRow2[2] = p.invRow2[2]; u.invRow2[3] = alpha;
    u.invViewportW = p.invViewportW;
    u.invViewportH = p.invViewportH;
    u.count        = static_cast<float>(n);  // the effective (post-truncation) segment count
    u.radius       = p.radius;
    u.blend        = static_cast<float>(blend);
    u.snap         = snap;                    // uBlend.y — 1 = snap the gate to the viewport grid
    return u;
}

// Sample a curve boundary that carries a cubic segment into a faceted closed polygon (the points path):
// the analytic gate handles linear + quadratic exactly, so a cubic (an explicit cubic or a Catmull-Rom
// throughPoints) renders faceted via region_select.frag until the mask-texture path. radius/transform
// ride along; the curve is emptied so the polygon gate is taken. Logged once.
ShapePoints sampleCurveRegionToPolygon(const ShapePoints& region) {
    static bool warned = false;
    if (!warned) {
        SDL_Log("retropp: curve region contains a cubic segment; sampling to a faceted polygon "
                "(linear + quadratic boundaries are exact)");
        warned = true;
    }
    const Curve c{region.curve, /*closed=*/true};
    ShapePoints out;
    out.radius    = region.radius;
    out.transform = region.transform;
    const int n = kRegionCbufferMaxPoints;  // sample at the polygon cap for the smoothest faceting
    out.points.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const Vec2 v = c.at(static_cast<float>(i) / static_cast<float>(n));
        out.points.push_back(Point{v.x, v.y});
    }
    return out;
}

// Curve-mask region-select gate uniform — must match region_select_curve_mask.frag.hlsl's CurveMaskUniforms
// cbuffer exactly (5 × 16-byte registers). No per-segment data: the boundary's signed distance lives in the
// baked mask texture; the cbuffer carries the inverse homography (invert in row0.w, stroke in row1.w, alpha in
// row2.w), invViewport/radius/blend, and the shape-local bake box (min + 1/extent) the shader maps fragments
// into. The bake box comes from the renderer's CurveMaskEntry, not the per-frame region.
struct CurveMaskSelectFragUniforms {
    float invRow0[4];                                   // register 0
    float invRow1[4];                                   // register 1
    float invRow2[4];                                   // register 2
    float invViewportW, invViewportH, radius, blend;    // register 3
    float bakeMinX, bakeMinY, invBakeExtentX, invBakeExtentY;  // register 4
    float snap, snapPad0, snapPad1, snapPad2;           // register 5 : uSnap.x = 1 → snap gate to viewport grid
};
static_assert(sizeof(CurveMaskSelectFragUniforms) == 96,
              "CurveMaskSelectFragUniforms must match the region_select_curve_mask.frag cbuffer (6 registers)");

CurveMaskSelectFragUniforms makeCurveMaskSelectUniforms(const ShapePoints& region, Vec2 bakeMin, Vec2 bakeExtent,
                                                        ViewportResolution viewport, float alpha, BlendMode blend,
                                                        float snap) {
    const CurveRegionParams p = curveRegionParams(region, PixelSize{viewport.width, viewport.height});
    CurveMaskSelectFragUniforms u{};
    u.invRow0[0] = p.invRow0[0]; u.invRow0[1] = p.invRow0[1]; u.invRow0[2] = p.invRow0[2]; u.invRow0[3] = region.invert ? 1.0f : 0.0f;
    u.invRow1[0] = p.invRow1[0]; u.invRow1[1] = p.invRow1[1]; u.invRow1[2] = p.invRow1[2]; u.invRow1[3] = p.strokeWidth;
    u.invRow2[0] = p.invRow2[0]; u.invRow2[1] = p.invRow2[1]; u.invRow2[2] = p.invRow2[2]; u.invRow2[3] = alpha;
    u.invViewportW = p.invViewportW;
    u.invViewportH = p.invViewportH;
    u.radius       = p.radius;
    u.blend        = static_cast<float>(blend);
    u.bakeMinX = bakeMin.x; u.bakeMinY = bakeMin.y;
    u.invBakeExtentX = bakeExtent.x > 0.0f ? 1.0f / bakeExtent.x : 0.0f;
    u.invBakeExtentY = bakeExtent.y > 0.0f ? 1.0f / bakeExtent.y : 0.0f;
    u.snap = snap;                            // uSnap.x — 1 = snap the gate to the viewport grid
    return u;
}

// Curve-mask stencil gate uniform — must match region_stencil_curve_mask.frag.hlsl's CurveMaskStencilUniforms
// cbuffer exactly (6 × 16-byte registers). The first 5 registers mirror CurveMaskSelectFragUniforms (row2.w is
// unused for stencil; register 3's .w is the stencil mode, not blend); register 5 appends feather.
struct CurveMaskStencilFragUniforms {
    float invRow0[4];                                   // register 0
    float invRow1[4];                                   // register 1
    float invRow2[4];                                   // register 2
    float invViewportW, invViewportH, radius, mode;     // register 3
    float bakeMinX, bakeMinY, invBakeExtentX, invBakeExtentY;  // register 4
    float feather, snap, pad1, pad2;                    // register 5 : uStencil.y (snap) 1 = viewport grid
};
static_assert(sizeof(CurveMaskStencilFragUniforms) == 96,
              "CurveMaskStencilFragUniforms must match the region_stencil_curve_mask.frag cbuffer (6 registers)");

CurveMaskStencilFragUniforms makeCurveMaskStencilUniforms(const ShapePoints& region, Vec2 bakeMin, Vec2 bakeExtent,
                                                          StencilMode mode, float feather, ViewportResolution viewport,
                                                          float snap) {
    const CurveRegionParams p = curveRegionParams(region, PixelSize{viewport.width, viewport.height});
    CurveMaskStencilFragUniforms u{};
    u.invRow0[0] = p.invRow0[0]; u.invRow0[1] = p.invRow0[1]; u.invRow0[2] = p.invRow0[2]; u.invRow0[3] = region.invert ? 1.0f : 0.0f;
    u.invRow1[0] = p.invRow1[0]; u.invRow1[1] = p.invRow1[1]; u.invRow1[2] = p.invRow1[2]; u.invRow1[3] = p.strokeWidth;
    u.invRow2[0] = p.invRow2[0]; u.invRow2[1] = p.invRow2[1]; u.invRow2[2] = p.invRow2[2]; u.invRow2[3] = 0.0f;
    u.invViewportW = p.invViewportW;
    u.invViewportH = p.invViewportH;
    u.radius       = p.radius;
    u.mode         = static_cast<float>(static_cast<std::uint32_t>(mode));
    u.bakeMinX = bakeMin.x; u.bakeMinY = bakeMin.y;
    u.invBakeExtentX = bakeExtent.x > 0.0f ? 1.0f / bakeExtent.x : 0.0f;
    u.invBakeExtentY = bakeExtent.y > 0.0f ? 1.0f / bakeExtent.y : 0.0f;
    u.feather = feather;
    u.snap    = snap;                         // uStencil.y — 1 = snap the gate to the viewport grid
    return u;
}

// Stencil gate uniform — must match region_stencil.frag.hlsl's StencilUniforms cbuffer exactly (37 ×
// 16-byte registers). The first 36 registers are byte-identical to RegionSelectFragUniforms (the same
// polygon SDF + inverse homography + misc tail); register 36 appends the two stencil scalars (mode as a
// float rounded to uint in the shader, feather in shape-local px). New struct — the region_select cbuffer
// is untouched.
struct StencilFragUniforms {
    float points[2 * kRegionCbufferMaxPoints];  // registers 0..31 : ≤64 vertices, xy packed 2-per-register
    float invRow0[4];                            // register 32
    float invRow1[4];                            // register 33
    float invRow2[4];                            // register 34
    float invViewportW, invViewportH;            // register 35
    float count;                                 //   (the effective vertex count, rounded to uint in the shader)
    float radius;
    float mode;                                  // register 36 : 0 TransparentInside, 1 TransparentOutside (rounded to uint)
    float feather;                               //   shape-local px; 0 = hard edge
    float snap;                                  //   (uStencil.z) 1 = snap the stencil to the viewport grid (crisp)
    float pad1;
};
static_assert(sizeof(StencilFragUniforms) == 592, "StencilFragUniforms must match the region_stencil.frag cbuffer");

// Resolve a region + stencil scalars + viewport into the stencil cbuffer bytes. Mirrors retropp::stencilParams
// + packs the vertices two-per-register, truncating past kRegionCbufferMaxPoints (with a warning) and
// carrying the EFFECTIVE count so the shader never reads an unfilled slot.
StencilFragUniforms makeStencilUniforms(const ShapePoints& region, StencilMode mode, float feather,
                                        ViewportResolution viewport, float snap) {
    const StencilParams p = stencilParams(region, mode, feather, PixelSize{viewport.width, viewport.height});
    StencilFragUniforms u{};
    const std::size_t cap = static_cast<std::size_t>(kRegionCbufferMaxPoints);
    const std::size_t n   = std::min(region.points.size(), cap);
    if (region.points.size() > cap) {
        SDL_Log("retropp: stencil region polygon has %zu vertices; truncated to %d (cbuffer cap)",
                region.points.size(), kRegionCbufferMaxPoints);
    }
    for (std::size_t i = 0; i < n; ++i) {
        u.points[2 * i]     = region.points[i].x;
        u.points[2 * i + 1] = region.points[i].y;
    }
    u.invRow0[0] = p.region.invRow0[0]; u.invRow0[1] = p.region.invRow0[1]; u.invRow0[2] = p.region.invRow0[2]; u.invRow0[3] = region.invert ? 1.0f : 0.0f;
    u.invRow1[0] = p.region.invRow1[0]; u.invRow1[1] = p.region.invRow1[1]; u.invRow1[2] = p.region.invRow1[2]; u.invRow1[3] = p.region.strokeWidth;
    u.invRow2[0] = p.region.invRow2[0]; u.invRow2[1] = p.region.invRow2[1]; u.invRow2[2] = p.region.invRow2[2]; u.invRow2[3] = 0.0f;
    u.invViewportW = p.region.invViewportW;
    u.invViewportH = p.region.invViewportH;
    u.count        = static_cast<float>(n);  // the effective (post-truncation) vertex count
    u.radius       = p.region.radius;
    u.mode         = static_cast<float>(p.mode);
    u.feather      = p.feather;
    u.snap         = snap;                    // uStencil.z — 1 = snap the stencil to the viewport grid
    return u;
}

// Curve stencil gate uniform — must match region_stencil_curve.frag.hlsl's CurveStencilUniforms cbuffer
// exactly (69 × 16-byte registers). The first 68 registers are byte-identical to CurveRegionSelectFragUniforms
// (the per-segment control points + inverse homography + misc tail); register 68 appends the two stencil
// scalars. New struct — the curve region_select cbuffer is untouched.
struct CurveStencilFragUniforms {
    float segs[8 * kCurveRegionMaxSegments];  // registers 0..63 : 2 regs/segment (8 floats)
    float invRow0[4];                          // register 64
    float invRow1[4];                          // register 65
    float invRow2[4];                          // register 66
    float invViewportW, invViewportH;          // register 67
    float count;                               //   (the effective segment count, rounded to uint in the shader)
    float radius;
    float mode;                                // register 68 : 0 TransparentInside, 1 TransparentOutside (rounded to uint)
    float feather;                             //   shape-local px; 0 = hard edge
    float snap;                                //   (uStencil.z) 1 = snap the stencil to the viewport grid (crisp)
    float pad1;
};
static_assert(sizeof(CurveStencilFragUniforms) == 1104,
              "CurveStencilFragUniforms must match the region_stencil_curve.frag cbuffer (69 registers)");

// Resolve a curve region + stencil scalars + viewport into the curve stencil cbuffer bytes. Mirrors
// retropp::curveStencilParams + packs the per-segment control points two registers each, truncating past
// kCurveRegionMaxSegments (with a warning). The boundary is assumed analytic (linear + quadratic); a cubic
// boundary is sampled to a polygon by sampleCurveRegionToPolygon before this path.
CurveStencilFragUniforms makeCurveStencilUniforms(const ShapePoints& region, StencilMode mode, float feather,
                                                  ViewportResolution viewport, float snap) {
    const CurveStencilParams p =
        curveStencilParams(region, mode, feather, PixelSize{viewport.width, viewport.height});
    CurveStencilFragUniforms u{};
    const std::size_t cap = static_cast<std::size_t>(kCurveRegionMaxSegments);
    const std::size_t n   = std::min(region.curve.size(), cap);
    if (region.curve.size() > cap) {
        SDL_Log("retropp: stencil curve region has %zu segments; truncated to %d (cbuffer cap)",
                region.curve.size(), kCurveRegionMaxSegments);
    }
    for (std::size_t i = 0; i < n; ++i) {
        const CurveSegment& s = region.curve[i];
        const Vec2 start = s.p0;
        const Vec2 ctrl  = s.degree == CurveDegree::Quadratic ? s.p1 : s.p0;
        const Vec2 end   = segmentEnd(s);
        float* a = &u.segs[8 * i];
        a[0] = start.x; a[1] = start.y; a[2] = ctrl.x; a[3] = ctrl.y;
        a[4] = end.x;   a[5] = end.y;   a[6] = static_cast<float>(static_cast<int>(s.degree)); a[7] = 0.0f;
    }
    u.invRow0[0] = p.region.invRow0[0]; u.invRow0[1] = p.region.invRow0[1]; u.invRow0[2] = p.region.invRow0[2]; u.invRow0[3] = region.invert ? 1.0f : 0.0f;
    u.invRow1[0] = p.region.invRow1[0]; u.invRow1[1] = p.region.invRow1[1]; u.invRow1[2] = p.region.invRow1[2]; u.invRow1[3] = p.region.strokeWidth;
    u.invRow2[0] = p.region.invRow2[0]; u.invRow2[1] = p.region.invRow2[1]; u.invRow2[2] = p.region.invRow2[2]; u.invRow2[3] = 0.0f;
    u.invViewportW = p.region.invViewportW;
    u.invViewportH = p.region.invViewportH;
    u.count        = static_cast<float>(n);  // the effective (post-truncation) segment count
    u.radius       = p.region.radius;
    u.mode         = static_cast<float>(p.mode);
    u.feather      = p.feather;
    u.snap         = snap;                    // uStencil.z — 1 = snap the stencil to the viewport grid
    return u;
}

[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string{what} + ": " + SDL_GetError());
}

// Copy one rectangle of texels into a store texture through `copy`. `texels` is the rectangle's own
// pixels, packed rect.w to a row, so the transfer carries exactly what lands and nothing around it.
// Returns the transfer buffer, which stays alive until the command buffer carrying `copy` is submitted.
SDL_GPUTransferBuffer* uploadStoreRect(SDL_GPUDevice* device, SDL_GPUCopyPass* copy,
                                       SDL_GPUTexture* texture, const detail::StoreRect& rect,
                                       const void* texels, std::size_t texelBytes, const char* what) {
    const Uint32 bytes = static_cast<Uint32>(static_cast<std::size_t>(rect.w) *
                                             static_cast<std::size_t>(rect.h) * texelBytes);
    SDL_GPUTransferBufferCreateInfo tbInfo{};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbInfo.size  = bytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &tbInfo);
    if (!transfer) fail(what);

    void* mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    if (!mapped) fail(what);
    std::memcpy(mapped, texels, bytes);
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = transfer;
    src.offset          = 0;
    src.pixels_per_row  = static_cast<Uint32>(rect.w);
    src.rows_per_layer  = static_cast<Uint32>(rect.h);
    SDL_GPUTextureRegion dst{};
    dst.texture = texture;
    dst.x       = static_cast<Uint32>(rect.x);
    dst.y       = static_cast<Uint32>(rect.y);
    dst.w       = static_cast<Uint32>(rect.w);
    dst.h       = static_cast<Uint32>(rect.h);
    dst.d       = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    return transfer;
}

// Each generated shader header now exposes a ready-made `retropp::shaders::<stem>` ShaderVariants
// constant (the generator does the assembly), so the renderer binds them directly — no per-stem
// Variants() helpers. createShader still selects the live device's format from the variant.

// numStorageBuffers is the last (additive) parameter so existing call sites — which pass
// (numSamplers, numStorageTextures, numUniformBuffers) positionally — are unaffected; the sprite
// vertex stage is the only consumer (its t0 space0 sprite record buffer).
SDL_GPUShader* createShader(SDL_GPUDevice* device, SDL_GPUShaderStage stage,
                            const ShaderVariants& variants, Uint32 numSamplers,
                            Uint32 numStorageTextures = 0, Uint32 numUniformBuffers = 0,
                            Uint32 numStorageBuffers = 0) {
    const auto chosen = selectShader(SDL_GetGPUShaderFormats(device), variants);
    if (!chosen) fail("no compatible shader format for this GPU device");

    SDL_GPUShaderCreateInfo info{};
    info.code_size            = chosen->first.size;
    info.code                 = chosen->first.data;
    info.entrypoint           = chosen->first.entrypoint;
    info.format               = chosen->second;
    info.stage                = stage;
    info.num_samplers         = numSamplers;
    info.num_storage_textures = numStorageTextures;
    info.num_storage_buffers  = numStorageBuffers;
    info.num_uniform_buffers  = numUniformBuffers;

    SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
    if (!shader) fail("SDL_CreateGPUShader failed");
    return shader;
}

}  // namespace

const Renderer* Renderer::instance_ = nullptr;

const Renderer& Renderer::instance() {
    if (!instance_) {
        throw std::logic_error("Renderer::instance() called before a Renderer was constructed");
    }
    return *instance_;
}

PixelSize Renderer::atlasPixelSize(AtlasId atlas) const noexcept {
    const std::size_t i = static_cast<std::size_t>(atlas);
    if (i >= atlases_.size()) return PixelSize{0, 0};
    return PixelSize{atlases_[i].width, atlases_[i].height};
}

bool Renderer::atlasVisibleAt(AtlasId atlas, int x, int y) const noexcept {
    const std::size_t i = static_cast<std::size_t>(atlas);
    if (i >= atlases_.size()) return false;
    const AtlasEntry& e = atlases_[i];
    if (x < 0 || y < 0 || x >= e.width || y >= e.height) return false;
    const std::uint32_t index =
        e.data[static_cast<std::size_t>(y) * static_cast<std::size_t>(e.width) + static_cast<std::size_t>(x)];
    return !e.transparent.contains(static_cast<int>(index));
}

AssetSlot Renderer::atlasSlot(AtlasId atlas, std::size_t slotIndex) const noexcept {
    const std::size_t i = static_cast<std::size_t>(atlas);
    if (i >= atlases_.size()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "retropp: atlasSlot asked about unknown atlas %zu", i);
        return AssetSlot{};
    }
    const AtlasEntry& e = atlases_[i];
    if (e.slotCount <= 0 || slotIndex >= static_cast<std::size_t>(e.slotCount)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                    "retropp: atlasSlot asked for slot %zu of atlas %zu, which carries %d carved slot(s)",
                    slotIndex, i, e.slotCount);
        return AssetSlot{};
    }
    return sliceSlot(PixelSize{e.width, e.height}, e.slotSize, e.slotKind, e.slotOrder,
                     static_cast<int>(slotIndex))
        .value_or(AssetSlot{});
}

Renderer::Renderer(SDL_GPUDevice* device, SDL_Window* window, ViewportResolution viewport)
    : device_(device), window_(window), viewport_(viewport) {
    instance_ = this;  // the one engine renderer; the sprite shape query resolves atlases against its atlases_
    // Detect the Metal backend once: only there does the blocking swapchain acquire busy-wait (see the
    // acquireNonBlocking_ comment in renderer.h). On Metal we acquire non-blocking and let the host
    // loop's frame deadline pace; every other backend keeps the blocking acquire.
    if (const char* driver = SDL_GetGPUDeviceDriver(device_); driver && SDL_strcmp(driver, "metal") == 0) {
        acquireNonBlocking_ = true;
    }

    // Resolve the compose grid — the raster resolution of the offscreen targets and the content pass —
    // and create the four offscreen targets at it. composeScale_ starts at 1 (== the viewport); it is
    // re-resolved per frame once rendering begins (renderFrame → resolveComposeScale → resizeComposeTargets).
    resizeComposeTargets(1);

    // The per-frame row-data store starts as a 1×1 RGBA32F texture so the custom-effect pipeline (which
    // declares one fragment storage texture) always has something to bind, even on a frame with no tables.
    // renderFrame grows + refills it when an effect carries a paramTable.
    {
        SDL_GPUTextureCreateInfo ri{};
        ri.type                 = SDL_GPU_TEXTURETYPE_2D;
        ri.format               = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
        ri.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
        ri.width                = 1;
        ri.height               = 1;
        ri.layer_count_or_depth = 1;
        ri.num_levels           = 1;
        ri.sample_count         = SDL_GPU_SAMPLECOUNT_1;
        rowDataStore_ = SDL_CreateGPUTexture(device_, &ri);
        if (!rowDataStore_) fail("SDL_CreateGPUTexture (row-data store) failed");
        rowDataStoreH_ = 1;
    }

    // The batched-region fast path binds this 1×1 transparent-black texture as its SourceTexture: with a
    // zero source, sampleSource() returns 0 everywhere, so an additive custom shader's body returns exactly
    // its source-independent delta D — which the pass's additive blend accumulates. Cleared to (0,0,0,0)
    // once here via a one-shot command buffer (COLOR_TARGET so it can be a clear pass's target; SAMPLER so
    // the batched fragment can sample it). Persists for the renderer's lifetime.
    {
        SDL_GPUTextureCreateInfo zi{};
        zi.type                 = SDL_GPU_TEXTURETYPE_2D;
        zi.format               = kViewportColorFormat;
        zi.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        zi.width                = 1;
        zi.height               = 1;
        zi.layer_count_or_depth = 1;
        zi.num_levels           = 1;
        zi.sample_count         = SDL_GPU_SAMPLECOUNT_1;
        batchZeroSource_ = SDL_CreateGPUTexture(device_, &zi);
        if (!batchZeroSource_) fail("SDL_CreateGPUTexture (batch zero source) failed");
        if (SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_)) {
            SDL_GPUColorTargetInfo t{};
            t.texture     = batchZeroSource_;
            t.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};  // transparent black
            t.load_op     = SDL_GPU_LOADOP_CLEAR;
            t.store_op    = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
            SDL_EndGPURenderPass(p);
            SDL_SubmitGPUCommandBuffer(cmd);
        }
    }

    // Nearest filtering, clamped — the faithful default (bilinear is a runtime SamplingMode;
    // CRT-style filters are a post-process stage). Shared by the tile compositor (atlas sampling)
    // and the blit (viewport sampling).
    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter     = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mag_filter     = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device_, &samplerInfo);
    if (!sampler_) fail("SDL_CreateGPUSampler failed");

    // Bilinear filtering, same CLAMP_TO_EDGE so the viewport edge never bleeds the letterbox —
    // the blit-only alternate the renderer binds under SamplingMode::Bilinear. The
    // tile/atlas path keeps the nearest sampler above; only the final viewport→swapchain blit
    // swaps to this one. Sampler state is pipeline-independent, so this needs no shader change.
    SDL_GPUSamplerCreateInfo bilinearInfo = samplerInfo;
    bilinearInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    bilinearInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    bilinear_ = SDL_CreateGPUSampler(device_, &bilinearInfo);
    if (!bilinear_) fail("SDL_CreateGPUSampler (bilinear) failed");

    // Tile compositor pipeline: renders into the offscreen viewport target (RGBA8), alpha-
    // blended (SRC_ALPHA / ONE_MINUS_SRC_ALPHA) so per-layer alpha composites back-to-front.
    // The fragment shader binds NO sampler and three read-only storage textures — the indexed
    // atlas (R8_UINT), the tilemap cells (R32_UINT), and the palette store (RGBA8) — plus one
    // uniform buffer (the per-layer block); colour is all integer Load + palette lookup.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::tile_vert, 0, 0, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::tile_frag, 0, 4, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                          = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        tile_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!tile_) fail("SDL_CreateGPUGraphicsPipeline (tile) failed");
    }

    // Raster compositor pipeline: a layer's raster into the same offscreen viewport target with the
    // same alpha blend, so a hosted machine's screen or a program's own raster composites
    // back-to-front with tiles and sprites by z. The fragment shader binds ONE read-only storage
    // texture (the raster, t0 space2) plus one uniform buffer, and no sampler — one integer Load per
    // pixel, so the raster reaches the screen at exactly the texels its source drew.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::tile_vert, 0, 0, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::raster_frag, 0, 1, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                            = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        raster_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!raster_) fail("SDL_CreateGPUGraphicsPipeline (raster) failed");
    }

    // Sprite compositor pipeline: instanced per-sprite quads (TRIANGLELIST, 6 verts × N
    // instances) drawn into the same offscreen viewport target with the same alpha blend as the
    // tile pipeline, so sprites composite back-to-front with tiles by z. The vertex shader reads
    // ONE read-only storage buffer (the per-layer GpuSprite records, t0 space0) and no uniform
    // (the screen→clip transform is baked into each record); the fragment shader binds five
    // read-only storage textures (indexed atlas, palette store, atlas-region table, sprite-effect
    // records, the emission-field array — t0..t4 space2) + one uniform buffer (b0 space3) and no
    // sampler — all integer Load, colour-index-0 discarded for OBJ transparency.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::sprite_vert, 0, 0, 0, 1);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::sprite_frag, 1, 5, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                            = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        sprite_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!sprite_) fail("SDL_CreateGPUGraphicsPipeline (sprite) failed");
    }

    // Below-scope sprite pipeline (scene-facing sprite effects): the SAME instanced sprite vertex stage, but
    // the sprite_below fragment reads the accumulator (the scene, bound as SourceTexture) and the emission
    // field array — 2 samplers — beneath four storage textures (the coverage read + effect records) + one
    // uniform. Same premultiplied-into-a-transparent-scratch blend state as sprite_ (so the below run's
    // scratch is a premultiplied image the renderer composites premultiplied-over the accumulator). Built
    // from the stock below fragment; a below-custom variant builds through the same method
    // (buildSpriteBelowStagePipeline).
    spriteBelow_ = buildSpriteBelowStagePipeline(shaders::sprite_below_frag);

    // Sprite-emission pipeline: the SAME instanced sprite vertex stage and the same four storage textures +
    // uniform as sprite_, but the sprite_emission fragment writes a glowing sprite's EMISSION instead of its
    // art. The blend is purely ADDITIVE on both colour and alpha — emission fields sum, so every sprite in a
    // bucket accumulates into one image the chain then blurs and composites once. The target is the emission
    // scratch, which shares the viewport colour format.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::sprite_vert, 0, 0, 0, 1);
        SDL_GPUShader* fragment =
            createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::sprite_emission_frag, 0, 4, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                            = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        spriteEmission_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!spriteEmission_) fail("SDL_CreateGPUGraphicsPipeline (sprite emission) failed");
    }

    // Row-displacement post-process pipeline: a fullscreen-triangle pass that
    // samples the source viewport at a displaced UV and writes a viewport-sized RGBA8 scratch
    // target. Shares postprocess.vert with future stages; the fragment binds one sampled texture
    // (the source) + one uniform (the displacement params) and no storage. No blend — the stage
    // fully replaces its target. The colour target format is the viewport's (RGBA8), NOT the
    // swapchain's, since it renders into post0_/post1_.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::displace_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        displace_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!displace_) fail("SDL_CreateGPUGraphicsPipeline (displace) failed");
    }

    // Per-layer (Layer scope) composite pipeline: the SAME displace shaders, but this
    // one BLENDS its displaced output into target_ rather than replacing a scratch. The isolated
    // layer is rendered alone over a transparent-cleared scratch first (standard alpha blend → a
    // PREMULTIPLIED image), so this composite uses PREMULTIPLIED-OVER factors (ONE / ONE_MINUS_SRC_ALPHA),
    // not SRC_ALPHA/…, which would multiply by alpha a second time and double-darken translucent edges.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::displace_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                            = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // src rgb is premultiplied
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        displaceBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!displaceBlend_) fail("SDL_CreateGPUGraphicsPipeline (displaceBlend) failed");
    }

    // Built-in radial-ripple post-process pipeline: the second engine effect kind, the
    // SAME shape as displace_ — a fullscreen-triangle pass over postprocess.vert, one sampled source +
    // one uniform (RippleFragUniforms), no blend (replaces its scratch). The runEffect built-in branch
    // dispatches to this by ScreenSpaceEffectKind::Ripple.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::ripple_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        ripple_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!ripple_) fail("SDL_CreateGPUGraphicsPipeline (ripple) failed");
    }

    // Per-layer (Layer scope) ripple composite pipeline: the SAME ripple shaders, premultiplied-over
    // blend onto target_ — mirroring displaceBlend_ (the isolated layer is rendered alone over a
    // transparent-cleared scratch first, so this composites the PREMULTIPLIED result).
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::ripple_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                            = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // src rgb is premultiplied
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        rippleBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!rippleBlend_) fail("SDL_CreateGPUGraphicsPipeline (rippleBlend) failed");
    }

    // Built-in angular-twist post-process pipeline: the SAME shape as ripple_ — a fullscreen-triangle pass
    // over postprocess.vert, one sampled source + one uniform (SwirlFragUniforms), no blend (replaces its
    // scratch). The runEffect built-in branch dispatches to this by ScreenSpaceEffectKind::Swirl.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::swirl_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        swirl_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!swirl_) fail("SDL_CreateGPUGraphicsPipeline (swirl) failed");
    }

    // Per-layer (Layer scope) swirl composite pipeline: the SAME swirl shaders, premultiplied-over blend
    // onto target_ — mirroring rippleBlend_ (the isolated layer is rendered alone over a transparent-cleared
    // scratch first, so this composites the PREMULTIPLIED result).
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::swirl_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                            = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // src rgb is premultiplied
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        swirlBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!swirlBlend_) fail("SDL_CreateGPUGraphicsPipeline (swirlBlend) failed");
    }

    // Built-in colour-fill post-process pipeline: a fullscreen-triangle pass over postprocess.vert with
    // one uniform (ColorFillFragUniforms) and no sampled source — a fill is a pure source colour, so the
    // stage emits it directly, and it means the same thing at every scope. No blend (it replaces its
    // scratch). The runEffect built-in branch dispatches to this by ScreenSpaceEffectKind::ColorFill. A
    // Region confines it to a shape, so a colour fills that shape — a stroked region becomes a drawn
    // line, a filled region a solid shape.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::colorfill_frag, 0, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        colorFill_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!colorFill_) fail("SDL_CreateGPUGraphicsPipeline (colorFill) failed");
    }

    // Per-layer (Layer scope) colour-fill composite pipeline: the SAME colorfill shaders, premultiplied-
    // over blend onto target_ — mirroring rippleBlend_ (the isolated layer is rendered alone over a
    // transparent-cleared scratch first, so this composites the PREMULTIPLIED result).
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::colorfill_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                            = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // src rgb is premultiplied
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        colorFillBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!colorFillBlend_) fail("SDL_CreateGPUGraphicsPipeline (colorFillBlend) failed");
    }

    // Built-in gleam post-process pipeline: the SAME shape as ripple_ — a fullscreen-triangle pass over
    // postprocess.vert, one sampled source + one uniform (GleamFragUniforms), no blend (replaces its
    // scratch). The runEffect built-in branch dispatches to this by ScreenSpaceEffectKind::Gleam. The
    // fragment multiplies each pixel by a luminance-keyed diagonal sheen band (the marquee "shine").
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::gleam_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        gleam_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!gleam_) fail("SDL_CreateGPUGraphicsPipeline (gleam) failed");
    }

    // Per-layer (Layer scope) gleam composite pipeline: the SAME gleam shaders, premultiplied-over blend
    // onto target_ — mirroring rippleBlend_ (the isolated layer is rendered alone over a transparent-cleared
    // scratch first, so this composites the PREMULTIPLIED result).
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::gleam_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                            = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // src rgb is premultiplied
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        gleamBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!gleamBlend_) fail("SDL_CreateGPUGraphicsPipeline (gleamBlend) failed");
    }

    // Built-in colour-saturation post-process pipeline: the SAME shape as gleam_ — a fullscreen-triangle pass
    // over postprocess.vert, one sampled source + one uniform (SaturationFragUniforms), no blend (replaces its
    // scratch). The runEffect built-in branch dispatches to this by ScreenSpaceEffectKind::ColorSaturation. The
    // fragment pulls each pixel toward its own luminance; saturation 1 is identity, 0 greyscale.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::saturation_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        saturation_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!saturation_) fail("SDL_CreateGPUGraphicsPipeline (saturation) failed");
    }

    // Per-layer (Layer scope) colour-saturation composite pipeline: the SAME saturation shaders,
    // premultiplied-over blend onto target_ — mirroring gleamBlend_ (the isolated layer is rendered alone over
    // a transparent-cleared scratch first, so this composites the PREMULTIPLIED result).
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::saturation_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                            = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // src rgb is premultiplied
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        saturationBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!saturationBlend_) fail("SDL_CreateGPUGraphicsPipeline (saturationBlend) failed");
    }

    // The Bloom / Glow emission chain. Five pipelines realize the whole chain for both kinds at every
    // frame-class site (frame postEffects, per-layer Layer + Below, region): the extract writes the
    // per-pixel emission, emissionCopy_ halves it twice when the reach is wide enough to reduce, the blur
    // runs once per axis, and the composite adds the result back over the untouched source. Only the
    // composite needs a premultiplied-over variant (the Layer scope composites its isolated render onto
    // target_); every interior stage writes its own scratch and always replaces.
    //
    // emissionCopy_ doubles as the identity path: an effect whose plan resolves to no chain still owes its
    // caller the source in the destination, and a passthrough delivers it byte-exactly for one cheap pass
    // instead of a gather.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::emission_extract_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        emissionExtract_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!emissionExtract_) fail("SDL_CreateGPUGraphicsPipeline (emissionExtract) failed");
    }

    // The rect-instanced extract: the same emission math over a below field's own rect of the atlas instead
    // of over a whole target, so a layer's below lenses extract in ONE instanced draw. Its vertex stage reads
    // the rects from a storage buffer (no uniform — the sprite.vert / region_batch.vert Metal constraint) and
    // its fragment samples the accumulator, so the resources are one storage buffer + one sampler + one
    // uniform. Replace, not blend: the packer gives every field a rect of its own and nothing else writes
    // there.
    {
        SDL_GPUShader* vertex =
            createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::emission_extract_rect_vert, 0, 0, 0, 1);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT,
                                               shaders::emission_extract_rect_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        emissionExtractRect_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!emissionExtractRect_) fail("SDL_CreateGPUGraphicsPipeline (emissionExtractRect) failed");
    }

    // One axis of the separable blur — run twice per chain, always into its own scratch.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::emission_blur_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        emissionBlur_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!emissionBlur_) fail("SDL_CreateGPUGraphicsPipeline (emissionBlur) failed");
    }

    // The chain's final add (source t0 + blurred emission t1), replace — frame-level, Below, and region.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::emission_composite_frag, 2, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        emissionComposite_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!emissionComposite_) fail("SDL_CreateGPUGraphicsPipeline (emissionComposite) failed");
    }

    // Per-layer (Layer scope) composite: the SAME composite shader, premultiplied-over blend onto target_
    // — mirroring saturationBlend_ (the isolated layer renders alone over a transparent-cleared scratch
    // first, so this composites the PREMULTIPLIED glowed result).
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::emission_composite_frag, 2, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                            = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // src rgb is premultiplied
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        emissionCompositeBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!emissionCompositeBlend_) fail("SDL_CreateGPUGraphicsPipeline (emissionCompositeBlend) failed");
    }

    // The chain's passthrough, over blit.frag (sample source, write it): the ½ reductions run it with the
    // BILINEAR sampler into a half-size target, so one tap averages a 2×2 block; the identity path runs it
    // with the nearest sampler to hand a caller its source back byte-exactly. Replace + premultiplied-over
    // variants, since the identity case inherits whichever compositing its site would have used.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::blit_frag, 1, 0, 0);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        emissionCopy_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!emissionCopy_) fail("SDL_CreateGPUGraphicsPipeline (emissionCopy) failed");
    }
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::blit_frag, 1, 0, 0);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                            = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // src rgb is premultiplied
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        emissionCopyBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!emissionCopyBlend_) fail("SDL_CreateGPUGraphicsPipeline (emissionCopyBlend) failed");
    }

    // Region-select gate pipelines: a fullscreen-triangle pass that reads the effect result
    // (t0) + the original source (t1) and writes `inside(region) ? eff : src`, confining ANY effect to
    // a shape with NO change to the effect shaders. Two variants mirror displace_ / displaceBlend_:
    // regionSelect_ REPLACES its target (frame-level + Below scope); regionSelectBlend_ composites the
    // selected image PREMULTIPLIED-OVER target_ (Layer scope, where eff/src are premultiplied). Both
    // share region_select.frag (2 samplers + 1 uniform), differing only in blend state.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        // 2 samplers (eff t0, src t1) + 1 uniform (b0; carries the ≤64 packed vertices + transform + misc).
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::region_select_frag, 2, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        regionSelect_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionSelect_) fail("SDL_CreateGPUGraphicsPipeline (regionSelect) failed");

        // Same shaders, premultiplied-over blend (ONE / ONE_MINUS_SRC_ALPHA) — the Layer-scope composite-
        // back, mirroring displaceBlend_.
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
        regionSelectBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionSelectBlend_) fail("SDL_CreateGPUGraphicsPipeline (regionSelectBlend) failed");

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
    }

    // ColorFill gather pipelines: ONE fullscreen pass that composites a whole run of ColorFill-confined
    // regions — the built-in peer of a custom stage's gather variant, replacing that run's ~2N per-region
    // passes (each a scissored colorfill.frag pass + a region_select.frag gate). The resource contract is
    // the custom gather variant's exactly (1 sampler + 1 storage texture [the row-data store, layout
    // parity only — unread] + 1 fragment storage buffer of per-region records + 2 uniforms: the engine
    // cbuffer's snap/viewport lanes + the run header with the record stride) — the one register layout
    // whose indices satisfy every backend's binding rules at once. Two variants mirror
    // regionSelect_/regionSelectBlend_: replace (frame-level / Below / mid-chain) and premultiplied-over
    // (a Normal layer's last step compositing onto target_).
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::colorfill_gather_frag,
                                               /*samplers=*/1, /*storageTex=*/1, /*uniforms=*/2, /*storageBuf=*/1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        colorFillGather_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!colorFillGather_) fail("SDL_CreateGPUGraphicsPipeline (colorFillGather) failed");

        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // premultiplied src
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorFillGatherBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!colorFillGatherBlend_) fail("SDL_CreateGPUGraphicsPipeline (colorFillGatherBlend) failed");

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
    }

    // Curve region-select gate pipelines: the curve-boundary peer of regionSelect_/regionSelectBlend_,
    // confining an effect to a CLOSED CURVE (analytic linear + quadratic) instead of a straight-edged
    // polygon, exact between control points. Same I/O (2 samplers + 1 uniform, the curve cbuffer) and
    // the same replace / premultiplied-over blend split; only region_select_curve.frag differs.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::region_select_curve_frag, 2, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        regionSelectCurve_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionSelectCurve_) fail("SDL_CreateGPUGraphicsPipeline (regionSelectCurve) failed");

        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
        regionSelectCurveBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionSelectCurveBlend_) fail("SDL_CreateGPUGraphicsPipeline (regionSelectCurveBlend) failed");

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
    }

    // Stencil pipelines (region see-through): a fullscreen-triangle pass that reads ONE source (the layer's
    // rendered pixels, t0), computes the region SDF, and writes `source × survival` — making the layer
    // see-through in/around the shape to reveal what's behind it. The subtractive sibling of the region_select gate
    // (which selects between two textures); a separate pass, so the gate is untouched. Two variants mirror
    // regionSelect_/regionSelectBlend_: regionStencil_ REPLACES its target (frame-level + Below scope);
    // regionStencilBlend_ composites the stenciled image PREMULTIPLIED-OVER target_ (Layer scope). Both
    // share region_stencil.frag (1 sampler + 1 uniform), differing only in blend state.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::region_stencil_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        regionStencil_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionStencil_) fail("SDL_CreateGPUGraphicsPipeline (regionStencil) failed");

        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // src rgb is premultiplied
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
        regionStencilBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionStencilBlend_) fail("SDL_CreateGPUGraphicsPipeline (regionStencilBlend) failed");

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
    }

    // Curve stencil pipelines: the curve-boundary peer of regionStencil_/regionStencilBlend_, erasing
    // along a CLOSED CURVE (analytic linear + quadratic) instead of a straight-edged polygon, exact
    // between control points. Same I/O (1 sampler + 1 uniform, the curve stencil cbuffer) and the same
    // replace / premultiplied-over split; only region_stencil_curve.frag differs.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::region_stencil_curve_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        regionStencilCurve_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionStencilCurve_) fail("SDL_CreateGPUGraphicsPipeline (regionStencilCurve) failed");

        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
        regionStencilCurveBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionStencilCurveBlend_) fail("SDL_CreateGPUGraphicsPipeline (regionStencilCurveBlend) failed");

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
    }

    // Curve-mask region-select gate pipelines: the curve-boundary peer of regionSelectCurve_ that reads the
    // boundary's signed distance from a baked mask texture (t2, bilinear) instead of solving it per segment —
    // for cubic / Catmull-Rom / arbitrary boundaries. Three samplers (eff, src, mask) + the mask cbuffer; the
    // same replace / premultiplied-over split.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::region_select_curve_mask_frag, 3, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        regionSelectCurveMask_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionSelectCurveMask_) fail("SDL_CreateGPUGraphicsPipeline (regionSelectCurveMask) failed");

        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
        regionSelectCurveMaskBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionSelectCurveMaskBlend_) fail("SDL_CreateGPUGraphicsPipeline (regionSelectCurveMaskBlend) failed");

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
    }

    // Curve-mask stencil pipelines: the curve-boundary peer of regionStencilCurve_ that reads the boundary's
    // signed distance from a baked mask texture (t1, bilinear) — for cubic / arbitrary boundaries. Two samplers
    // (src, mask) + the mask stencil cbuffer; the same replace / premultiplied-over split.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::region_stencil_curve_mask_frag, 2, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        regionStencilCurveMask_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionStencilCurveMask_) fail("SDL_CreateGPUGraphicsPipeline (regionStencilCurveMask) failed");

        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
        regionStencilCurveMaskBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionStencilCurveMaskBlend_) fail("SDL_CreateGPUGraphicsPipeline (regionStencilCurveMaskBlend) failed");

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
    }

    // Blend composite pipeline: a fullscreen-triangle pass that reads the accumulator (t0) + a container's
    // isolated render (t1) and writes applyBlendMode(dst, src, mode) — the programmable blend that a
    // non-Normal Region / DrawLayer / frame selects, where the fixed-function premultiplied-over composite
    // can only alpha-blend. It REPLACES its target (the full blended RGBA, which the caller swaps into the
    // accumulator), so there is one variant — no blend-state split.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::blend_frag, 2, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        blend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!blend_) fail("SDL_CreateGPUGraphicsPipeline (blend) failed");

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
    }

    // Blit pipeline: the fragment shader uses one sampled texture (the viewport); the vertex
    // shader needs none. The pipeline's colour target must match the swapchain — which needs the
    // window. A compose-only renderer (window == nullptr) skips it: it composes + captures the
    // viewport offscreen but never presents, so it has no swapchain and no blit pipeline (blit_ stays
    // null; the destructor and renderFrame's blit section both guard on it).
    if (window_) {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::blit_vert, 0);
        // 1 sampler (the viewport), no uniform buffer — the blit is a plain passthrough sample+write.
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::blit_frag, 1, 0, 0);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        blit_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!blit_) fail("SDL_CreateGPUGraphicsPipeline (blit) failed");
    }
}

void Renderer::resizeComposeTargets(int scale) {
    if (scale == composeScale_ && target_ != nullptr) return;  // already at this grid — no reallocation

    lastComposed_ = nullptr;  // the retained blit source is one of the targets recreated below — drop the stale handle
    composeScale_ = scale;
    const PixelSize compose = composeDimensions(viewport_, composeScale_);
    composeW_ = compose.width;
    composeH_ = compose.height;

    // Release the old targets (if any) before recreating at the new compose grid. SDL_GPU defers the
    // actual free until any in-flight command buffer referencing them completes, so releasing here is
    // safe even mid-loop; a resize only fires on a window-size change, never per steady frame.
    releaseEmissionTargets();  // sized off the compose grid; the next Bloom/Glow reallocates at the new one
    if (layerScratch_) { SDL_ReleaseGPUTexture(device_, layerScratch_); layerScratch_ = nullptr; }
    if (post1_)        { SDL_ReleaseGPUTexture(device_, post1_);        post1_        = nullptr; }
    if (post0_)        { SDL_ReleaseGPUTexture(device_, post0_);        post0_        = nullptr; }
    if (target_)       { SDL_ReleaseGPUTexture(device_, target_);       target_       = nullptr; }

    SDL_GPUTextureCreateInfo texInfo{};
    texInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format               = kViewportColorFormat;
    texInfo.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texInfo.width                = static_cast<Uint32>(composeW_);
    texInfo.height               = static_cast<Uint32>(composeH_);
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels           = 1;
    texInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;

    // Offscreen compose target: the colour target the compositor renders into, and the blit's sampler source.
    target_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!target_) fail("SDL_CreateGPUTexture (viewport) failed");
    // Two scratch targets for the post-process chain: it ping-pongs between them, never writing target_,
    // so two suffice for any stage count. Both COLOR_TARGET (a stage writes one) + SAMPLER (the next stage /
    // the blit reads it). Never touched when frame.postEffects is empty (an empty chain costs nothing).
    post0_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!post0_) fail("SDL_CreateGPUTexture (post0) failed");
    post1_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!post1_) fail("SDL_CreateGPUTexture (post1) failed");
    // Per-layer effect scratch: a Layer-scope effect renders its layer alone here and composites it back
    // displaced; a Below-scope effect displaces the accumulator into here and swaps it with target_. Same
    // format/usage as target_ (the two are interchangeable for the swap).
    layerScratch_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!layerScratch_) fail("SDL_CreateGPUTexture (layerScratch) failed");
}

void Renderer::releaseEmissionTargets() {
    if (emissionLow1_) { SDL_ReleaseGPUTexture(device_, emissionLow1_); emissionLow1_ = nullptr; }
    if (emissionLow0_) { SDL_ReleaseGPUTexture(device_, emissionLow0_); emissionLow0_ = nullptr; }
    if (emissionHalf_) { SDL_ReleaseGPUTexture(device_, emissionHalf_); emissionHalf_ = nullptr; }
    if (emission1_)    { SDL_ReleaseGPUTexture(device_, emission1_);    emission1_    = nullptr; }
    if (emission0_)    { SDL_ReleaseGPUTexture(device_, emission0_);    emission0_    = nullptr; }
}

void Renderer::releaseEmissionAtlas() {
    if (emissionAtlasStore_) { SDL_ReleaseGPUTexture(device_, emissionAtlasStore_); emissionAtlasStore_ = nullptr; }
    if (emissionRects_)     { SDL_ReleaseGPUTexture(device_, emissionRects_);     emissionRects_     = nullptr; }
    if (emissionAtlasLow1_) { SDL_ReleaseGPUTexture(device_, emissionAtlasLow1_); emissionAtlasLow1_ = nullptr; }
    if (emissionAtlasLow0_) { SDL_ReleaseGPUTexture(device_, emissionAtlasLow0_); emissionAtlasLow0_ = nullptr; }
    if (emissionAtlasHalf_) { SDL_ReleaseGPUTexture(device_, emissionAtlasHalf_); emissionAtlasHalf_ = nullptr; }
    if (emissionAtlas1_)    { SDL_ReleaseGPUTexture(device_, emissionAtlas1_);    emissionAtlas1_    = nullptr; }
    if (emissionAtlas0_)    { SDL_ReleaseGPUTexture(device_, emissionAtlas0_);    emissionAtlas0_    = nullptr; }
    emissionAtlasW_      = 0;
    emissionAtlasH_      = 0;
    emissionAtlasSheets_ = 0;
    emissionRectRows_    = 0;
}

bool Renderer::ensureIdleEmissionAtlas() {
    if (emissionAtlasIdle_ && emissionRectsIdle_) return true;
    if (!emissionAtlasIdle_) {
        SDL_GPUTextureCreateInfo ti{};
        ti.type                 = SDL_GPU_TEXTURETYPE_2D_ARRAY;  // matches the store's shape, at one texel
        ti.format               = kViewportColorFormat;
        ti.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER |
                                  SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
        ti.width                = 1;
        ti.height               = 1;
        ti.layer_count_or_depth = 1;
        ti.num_levels           = 1;
        ti.sample_count         = SDL_GPU_SAMPLECOUNT_1;
        emissionAtlasIdle_      = SDL_CreateGPUTexture(device_, &ti);
    }
    if (!emissionRectsIdle_) {
        SDL_GPUTextureCreateInfo ti{};
        ti.type                 = SDL_GPU_TEXTURETYPE_2D;
        ti.format               = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
        ti.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
        ti.width                = static_cast<Uint32>(kEmissionRectTexels);
        ti.height               = 1;
        ti.layer_count_or_depth = 1;
        ti.num_levels           = 1;
        ti.sample_count         = SDL_GPU_SAMPLECOUNT_1;
        emissionRectsIdle_      = SDL_CreateGPUTexture(device_, &ti);
    }
    // A rect read off the idle table is all zeroes, which clamps every sample to one texel of the idle
    // atlas — transparent black. A record naming a field the atlas could not hold therefore adds no light
    // rather than reading somewhere arbitrary.
    if (!emissionAtlasIdle_ || !emissionRectsIdle_)
        SDL_Log("retropp: idle emission atlas allocation failed — region Blooms add no light");
    return emissionAtlasIdle_ != nullptr && emissionRectsIdle_ != nullptr;
}

bool Renderer::ensureEmissionAtlas(int width, int height, int sheets, int rows) {
    if (width <= 0 || height <= 0 || sheets <= 0 || rows <= 0) return false;
    if (emissionAtlas0_ && emissionAtlasW_ == width && emissionAtlasH_ == height &&
        emissionAtlasSheets_ >= sheets && emissionRectRows_ >= rows)
        return true;
    releaseEmissionAtlas();

    // Half and quarter dimensions round UP so the reduced pair is never zero-sized, matching the compose
    // chain's own rounding.
    const int halfW = (width + 1) / 2, halfH = (height + 1) / 2;
    const int lowW  = (halfW + 1) / 2, lowH  = (halfH + 1) / 2;

    SDL_GPUTextureCreateInfo ti{};
    ti.type                 = SDL_GPU_TEXTURETYPE_2D;
    ti.format               = kViewportColorFormat;
    ti.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.layer_count_or_depth = 1;
    ti.num_levels           = 1;
    ti.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    auto make               = [&](int w, int h) -> SDL_GPUTexture* {
        ti.width  = static_cast<Uint32>(w);
        ti.height = static_cast<Uint32>(h);
        return SDL_CreateGPUTexture(device_, &ti);
    };

    emissionAtlas0_    = make(width, height);
    emissionAtlas1_    = make(width, height);
    emissionAtlasHalf_ = make(halfW, halfH);
    emissionAtlasLow0_ = make(lowW, lowH);
    emissionAtlasLow1_ = make(lowW, lowH);

    // The store: one array layer per sheet, which the sprite art fragment LOADS as a storage texture.
    ti.type                 = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    ti.layer_count_or_depth = static_cast<Uint32>(sheets);
    ti.usage |= SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
    emissionAtlasStore_ = make(width, height);
    ti.type                 = SDL_GPU_TEXTURETYPE_2D;
    ti.layer_count_or_depth = 1;

    ti.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    ti.usage  = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
    ti.width  = static_cast<Uint32>(kEmissionRectTexels);
    ti.height = static_cast<Uint32>(rows);
    emissionRects_ = SDL_CreateGPUTexture(device_, &ti);

    // All or nothing: a partial set would leave a pass reading an unwritten target.
    if (!emissionAtlas0_ || !emissionAtlas1_ || !emissionAtlasHalf_ || !emissionAtlasLow0_ ||
        !emissionAtlasLow1_ || !emissionAtlasStore_ || !emissionRects_) {
        SDL_Log("retropp: emission atlas allocation failed (%dx%d x%d sheets, %d fields) — region Blooms add "
                "no light",
                width, height, sheets, rows);
        releaseEmissionAtlas();
        return false;
    }
    emissionAtlasW_      = width;
    emissionAtlasH_      = height;
    emissionAtlasSheets_ = sheets;
    emissionRectRows_    = rows;
    return true;
}

bool Renderer::ensureEmissionTargets() {
    if (emission0_) return true;  // already at this compose grid — resizeComposeTargets releases on a change
    if (composeW_ <= 0 || composeH_ <= 0) return false;

    // Half and quarter dimensions round UP, so a target is never zero-sized on a small viewport. Rounding
    // up on an odd dimension leaves the reduction's bilinear taps a half-texel off the exact 2×2 midpoint;
    // the emission field is a blur input, so the drift is below what the Gaussian that follows resolves.
    const int halfW = (composeW_ + 1) / 2, halfH = (composeH_ + 1) / 2;
    const int lowW  = (halfW + 1) / 2,     lowH  = (halfH + 1) / 2;

    SDL_GPUTextureCreateInfo texInfo{};
    texInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format               = kViewportColorFormat;
    texInfo.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels           = 1;
    texInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;

    auto make = [&](int w, int h) -> SDL_GPUTexture* {
        texInfo.width  = static_cast<Uint32>(w);
        texInfo.height = static_cast<Uint32>(h);
        return SDL_CreateGPUTexture(device_, &texInfo);
    };

    emission0_    = make(composeW_, composeH_);
    emission1_    = make(composeW_, composeH_);
    emissionHalf_ = make(halfW, halfH);
    emissionLow0_ = make(lowW, lowH);
    emissionLow1_ = make(lowW, lowH);

    // All or nothing: a partial set would leave the chain reading an unwritten target. The caller falls
    // back to the identity passthrough, which still delivers the source — a missing halo beats a black frame.
    if (!emission0_ || !emission1_ || !emissionHalf_ || !emissionLow0_ || !emissionLow1_) {
        SDL_Log("retropp: emission scratch allocation failed — Bloom/Glow passes through unchanged");
        releaseEmissionTargets();
        return false;
    }
    return true;
}

int Renderer::resolveComposeScale() const {
    if (!interpolation_ || window_ == nullptr) return 1;  // faithful path: compose at viewport resolution
    int w = 0, h = 0;
    if (!SDL_GetWindowSizeInPixels(window_, &w, &h) || w <= 0 || h <= 0) return 1;
    // The window's integer-scale-to-fit factor (clamped): compose at exactly the drawn-region size so the
    // blit is a 1:1 centring copy — fill parity with the faithful path.
    return composeScaleToFit(PixelSize{w, h},
                             PixelSize{viewport_.width, viewport_.height}, kMaxComposeScale);
}

Renderer::~Renderer() {
    releaseSpriteBuffers();
    releaseRasters();
    releaseTilemaps();
    releaseAtlases();
    for (CurveMaskEntry& m : curveMasks_)
        if (m.texture) SDL_ReleaseGPUTexture(device_, m.texture);
    releaseCustomStages();
    releaseBatchResources();
    if (rowDataStore_) SDL_ReleaseGPUTexture(device_, rowDataStore_);
    if (paletteStore_) SDL_ReleaseGPUTexture(device_, paletteStore_);
    if (blit_)          SDL_ReleaseGPUGraphicsPipeline(device_, blit_);
    if (blend_)         SDL_ReleaseGPUGraphicsPipeline(device_, blend_);
    if (regionStencilCurveMaskBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, regionStencilCurveMaskBlend_);
    if (regionStencilCurveMask_)      SDL_ReleaseGPUGraphicsPipeline(device_, regionStencilCurveMask_);
    if (regionSelectCurveMaskBlend_)  SDL_ReleaseGPUGraphicsPipeline(device_, regionSelectCurveMaskBlend_);
    if (regionSelectCurveMask_)       SDL_ReleaseGPUGraphicsPipeline(device_, regionSelectCurveMask_);
    if (regionStencilCurveBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, regionStencilCurveBlend_);
    if (regionStencilCurve_)      SDL_ReleaseGPUGraphicsPipeline(device_, regionStencilCurve_);
    if (regionStencilBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, regionStencilBlend_);
    if (regionStencil_)  SDL_ReleaseGPUGraphicsPipeline(device_, regionStencil_);
    if (regionSelectCurveBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, regionSelectCurveBlend_);
    if (regionSelectCurve_)      SDL_ReleaseGPUGraphicsPipeline(device_, regionSelectCurve_);
    if (regionSelectBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, regionSelectBlend_);
    if (regionSelect_)  SDL_ReleaseGPUGraphicsPipeline(device_, regionSelect_);
    if (emissionCopyBlend_)      SDL_ReleaseGPUGraphicsPipeline(device_, emissionCopyBlend_);
    if (emissionCopy_)           SDL_ReleaseGPUGraphicsPipeline(device_, emissionCopy_);
    if (emissionCompositeBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, emissionCompositeBlend_);
    if (emissionComposite_)      SDL_ReleaseGPUGraphicsPipeline(device_, emissionComposite_);
    if (emissionBlur_)           SDL_ReleaseGPUGraphicsPipeline(device_, emissionBlur_);
    if (emissionExtractRect_)    SDL_ReleaseGPUGraphicsPipeline(device_, emissionExtractRect_);
    if (emissionExtract_)        SDL_ReleaseGPUGraphicsPipeline(device_, emissionExtract_);
    if (saturationBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, saturationBlend_);
    if (saturation_)      SDL_ReleaseGPUGraphicsPipeline(device_, saturation_);
    if (gleamBlend_)      SDL_ReleaseGPUGraphicsPipeline(device_, gleamBlend_);
    if (gleam_)           SDL_ReleaseGPUGraphicsPipeline(device_, gleam_);
    if (colorFillGatherBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, colorFillGatherBlend_);
    if (colorFillGather_)      SDL_ReleaseGPUGraphicsPipeline(device_, colorFillGather_);
    if (colorFillBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, colorFillBlend_);
    if (colorFill_)     SDL_ReleaseGPUGraphicsPipeline(device_, colorFill_);
    if (swirlBlend_)    SDL_ReleaseGPUGraphicsPipeline(device_, swirlBlend_);
    if (swirl_)         SDL_ReleaseGPUGraphicsPipeline(device_, swirl_);
    if (rippleBlend_)   SDL_ReleaseGPUGraphicsPipeline(device_, rippleBlend_);
    if (ripple_)        SDL_ReleaseGPUGraphicsPipeline(device_, ripple_);
    if (displaceBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, displaceBlend_);
    if (displace_)      SDL_ReleaseGPUGraphicsPipeline(device_, displace_);
    if (sprite_)        SDL_ReleaseGPUGraphicsPipeline(device_, sprite_);
    if (spriteEmission_) SDL_ReleaseGPUGraphicsPipeline(device_, spriteEmission_);
    if (spriteBelow_)   SDL_ReleaseGPUGraphicsPipeline(device_, spriteBelow_);
    if (raster_)        SDL_ReleaseGPUGraphicsPipeline(device_, raster_);
    if (tile_)          SDL_ReleaseGPUGraphicsPipeline(device_, tile_);
    if (bilinear_)      SDL_ReleaseGPUSampler(device_, bilinear_);
    if (sampler_)       SDL_ReleaseGPUSampler(device_, sampler_);
    releaseEmissionTargets();
    if (layerScratch_)  SDL_ReleaseGPUTexture(device_, layerScratch_);
    if (post1_)         SDL_ReleaseGPUTexture(device_, post1_);
    if (post0_)         SDL_ReleaseGPUTexture(device_, post0_);
    if (target_)        SDL_ReleaseGPUTexture(device_, target_);
}

void Renderer::releaseAtlases() {
    if (atlasStore_) SDL_ReleaseGPUTexture(device_, atlasStore_);
    atlasStore_  = nullptr;
    if (atlasRegionStore_) SDL_ReleaseGPUTexture(device_, atlasRegionStore_);
    atlasRegionStore_ = nullptr;
    atlasStoreW_ = 0;
    atlasStoreH_ = 0;
    atlasCapW_   = 0;
    atlasCapH_   = 0;
    atlasRegionCap_ = 0;
    pendingAtlasWrites_.clear();
    pendingRegionWrites_.clear();
    atlases_.clear();
}

void Renderer::releaseTilemaps() {
    for (auto& entry : tilemaps_) {
        if (entry.second.texture) SDL_ReleaseGPUTexture(device_, entry.second.texture);
        if (entry.second.transfer) SDL_ReleaseGPUTransferBuffer(device_, entry.second.transfer);
    }
    tilemaps_.clear();
}

void Renderer::releaseRasters() {
    for (auto& entry : rasters_) {
        if (entry.second.texture) SDL_ReleaseGPUTexture(device_, entry.second.texture);
        if (entry.second.transfer) SDL_ReleaseGPUTransferBuffer(device_, entry.second.transfer);
    }
    rasters_.clear();
}

void Renderer::releaseSpriteBuffers() {
    for (auto& entry : spriteBufs_) {
        if (entry.second.buffer) SDL_ReleaseGPUBuffer(device_, entry.second.buffer);
        if (entry.second.transfer) SDL_ReleaseGPUTransferBuffer(device_, entry.second.transfer);
    }
    spriteBufs_.clear();
    for (SDL_GPUBuffer* b : spriteRunBufs_) {
        if (b) SDL_ReleaseGPUBuffer(device_, b);
    }
    spriteRunBufs_.clear();
    spriteRunCaps_.clear();
    if (spriteFxStore_) { SDL_ReleaseGPUTexture(device_, spriteFxStore_); spriteFxStore_ = nullptr; }
    spriteFxStoreRows_ = 0;
}

void Renderer::releaseCustomStages() {
    for (SDL_GPUGraphicsPipeline* p : customReplace_) {
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    }
    for (SDL_GPUGraphicsPipeline* p : customBlend_) {
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    }
    for (SDL_GPUGraphicsPipeline* p : customGather_) {
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    }
    for (SDL_GPUGraphicsPipeline* p : customGatherBlend_) {
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    }
    for (SDL_GPUGraphicsPipeline* p : customSprite_) {
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    }
    for (SDL_GPUGraphicsPipeline* p : customSpriteBelow_) {
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    }
    for (SDL_GPUGraphicsPipeline* p : customEmission_) {
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    }
    for (SDL_GPUGraphicsPipeline* p : customEmissionRect_) {
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    }
    customReplace_.clear();
    customBlend_.clear();
    customGather_.clear();
    customGatherBlend_.clear();
    customSprite_.clear();
    customSpriteBelow_.clear();
    customEmissionConsumer_.clear();
    customEmission_.clear();
    customEmissionRect_.clear();
}

void Renderer::releaseBatchResources() {
    for (SDL_GPUGraphicsPipeline* p : customBatched_) {
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    }
    customBatched_.clear();
    for (SDL_GPUBuffer* b : batchInstanceBufs_) {
        if (b) SDL_ReleaseGPUBuffer(device_, b);
    }
    batchInstanceBufs_.clear();
    batchInstanceCaps_.clear();
    if (batchZeroSource_) SDL_ReleaseGPUTexture(device_, batchZeroSource_);
    batchZeroSource_ = nullptr;
}

PostProcessStageId Renderer::registerPostProcessStage(const ShaderVariants& fragment) {
    // A direct ShaderVariants registration carries no `// @retropp:emission` declaration (that fact lives in
    // the build-time scan, reachable only through the path form), so it always builds at the stock single-
    // source-sampler layout.
    return registerCustomStagePipelines(fragment, /*numSourceSamplers=*/1);
}

PostProcessStageId Renderer::registerCustomStagePipelines(const ShaderVariants& fragment,
                                                          std::uint32_t numSourceSamplers) {
    // Build the pipeline pair from the game's fragment + the shared fullscreen-triangle vertex stage. The
    // resource contract is fixed (the engine injects it): `numSourceSamplers` sampled textures + samplers (1
    // for a stock stage — SourceTexture at t0/s0; 2 for an emission consumer — SourceTexture t0/s0 +
    // EmissionTexture t1/s1), 1 read-only storage texture (the row-data store, for an effect's per-row
    // paramTable), and TWO uniform cbuffers — slot 0 = the engine cbuffer (RetroppEngineEffect: the edge mode
    // sampleSource() obeys + the effect's row-table location), slot 1 = the shader's OWN reflected params,
    // filled by its generated packer. Two pipelines, differing only in blend state — the no-blend replace
    // (frame-level / Below scope) and the premultiplied-over blend (Layer scope), exactly mirroring
    // displace_ / displaceBlend_.
    auto buildPipeline = [&](bool blend) -> SDL_GPUGraphicsPipeline* {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragShader = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, fragment, numSourceSamplers, 1, 2);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;
        if (blend) {
            colorTarget.blend_state.enable_blend          = true;
            colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // premultiplied src
            colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
            colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
        }

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragShader;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        SDL_GPUGraphicsPipeline* built = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragShader);
        return built;
    };

    SDL_GPUGraphicsPipeline* replace = buildPipeline(/*blend=*/false);
    if (!replace) fail("SDL_CreateGPUGraphicsPipeline (custom replace) failed");
    SDL_GPUGraphicsPipeline* blend = buildPipeline(/*blend=*/true);
    if (!blend) {
        SDL_ReleaseGPUGraphicsPipeline(device_, replace);
        fail("SDL_CreateGPUGraphicsPipeline (custom blend) failed");
    }

    ++storeGeneration_;  // a new custom-shader pipeline the compose can bind — force the next frame to recompose
    const auto id = static_cast<PostProcessStageId>(customReplace_.size());
    customReplace_.push_back(replace);
    customBlend_.push_back(blend);
    customPackers_.push_back(nullptr);  // parallel to the pipeline pair; set by the path overload
    customBatched_.push_back(nullptr);  // set by the path overload iff the shader is additive-declared
    customGather_.push_back(nullptr);       // set by the path overload iff the shader has a gather variant
    customGatherBlend_.push_back(nullptr);  //   (parallel pair — the premultiplied-over peer)
    customSprite_.push_back(nullptr);       // set by the path overload iff the shader has a sprite variant
    customSpriteBelow_.push_back(nullptr);  // set by the path overload iff the shader has a sprite-below variant
    customEmissionConsumer_.push_back(0);   // set by the path overload iff the shader is `// @retropp:emission`
    customEmission_.push_back(nullptr);     // set by the path overload iff the shader has an emission() body
    customEmissionRect_.push_back(nullptr); // set by the path overload iff the shader has an emission() body
    return id;
}

PostProcessStageId Renderer::registerPostProcessStage(LiteralPath shaderPath) {
    // The path is a compile-time string literal (LiteralPath enforces this — a non-literal is a compile
    // error, not a runtime miss). Resolve it against the build-time-compiled shader registry (populated by
    // the generated per-target registry TU's static initializers — see retropp_autocompile_shaders /
    // shader_registry.h).
    const std::string_view path = shaderPath.view();
    const ShaderVariants* fragment = detail::findShaderVariants(path);
    if (!fragment) {
        throw std::runtime_error(
            "registerPostProcessStage: no shader compiled for path \"" + std::string(path) +
            "\" — the literal was not referenced in a SCANNED source (the build-time scan reads the "
            "target's source files for .hlsl path literals; a literal sitting only in a header is not "
            "seen), or its spelling differs from the registered literal");
    }
    // Build the pipeline pair, then attach this shader's generated cbuffer packer (reflected from its own
    // cbuffer; null for a parameterless shader). The packer fills the uniform from the effect's inline fields.
    // An emission-declared stage (`// @retropp:emission`) builds its replace/blend pipelines at (2 samplers,
    // 1 storage, 2 uniforms) so the stage's final pass can read the source at slot 0 AND the blurred emission
    // at slot 1; every other stage builds at the stock single-source-sampler layout.
    const bool emissionConsumer = detail::isEmissionConsumer(path);
    const PostProcessStageId id = registerCustomStagePipelines(*fragment, emissionConsumer ? 2u : 1u);
    customPackers_[static_cast<std::size_t>(id)] = detail::findEffectPacker(path);
    // If the shader carries a `// @retropp:additive` declaration, the build compiled a BATCHED variant too;
    // build its instanced-additive pipeline so the renderer can route eligible same-shader regions through
    // ONE pass. Absent the declaration this is null and the stage stays on the per-region path.
    if (const ShaderVariants* batched = detail::findBatchedShaderVariants(path)) {
        customBatched_[static_cast<std::size_t>(id)] = buildBatchedStagePipeline(*batched);
    }
    // If the shader has a GATHER variant (every custom shader EXCEPT additive- / no-gather-declared ones),
    // build its replace + premultiplied-over gather pipeline pair so the renderer can collapse eligible
    // same-stage replace regions into ONE union-shape pass. Absent the variant both are null and the stage
    // stays on the per-region path (the additive and gather paths are disjoint by stage class — a stage has a batched
    // pipeline XOR a gather pipeline XOR neither).
    if (const ShaderVariants* gather = detail::findGatherShaderVariants(path)) {
        const auto sid = static_cast<std::size_t>(id);
        customGather_[sid]      = buildGatherStagePipeline(*gather, /*blend=*/false);
        customGatherBlend_[sid] = buildGatherStagePipeline(*gather, /*blend=*/true);
    }
    // If the shader has a SPRITE variant (every custom shader EXCEPT no-sprite- / int-uint-param ones), build
    // its sprite-inline pipeline so a sprite can carry this custom effect (Layer-scope, inline in the sprite
    // fragment). Absent the variant this stays null and the sprite skips the effect (a visible warning).
    if (const ShaderVariants* sprite = detail::findSpriteShaderVariants(path)) {
        customSprite_[static_cast<std::size_t>(id)] = buildSpriteStagePipeline(*sprite);
    }
    // If the shader has a SPRITE-BELOW variant (same eligibility as the sprite variant), build its scene-facing
    // below-custom pipeline so a sprite can carry this custom effect at Below scope (distort / grade the scene
    // through the silhouette). Absent the variant this stays null and a Below-scope Custom effect is skipped.
    if (const ShaderVariants* below = detail::findSpriteBelowShaderVariants(path)) {
        customSpriteBelow_[static_cast<std::size_t>(id)] = buildSpriteBelowStagePipeline(*below);
    }
    // Emission consumer: record the dispatch flag (the pipelines above were already built at the 2-sampler
    // layout for it), and build its own EXTRACT pipeline if the shader defines an `emission()` body. Absent
    // the body customEmission_ stays null and the demand extracts through the stock brightpass at `.threshold`.
    if (emissionConsumer) {
        customEmissionConsumer_[static_cast<std::size_t>(id)] = 1;
        if (const ShaderVariants* extract = detail::findEmissionShaderVariants(path)) {
            customEmission_[static_cast<std::size_t>(id)] = buildCustomEmissionExtractPipeline(*extract);
        }
        // The below WRITE pipeline: the same emission() body over the rect-instanced below extract, so a
        // Below-scope Custom lens's field is scene-authored. Non-null iff the frame-class extract is (both key
        // off the same emission() body). Absent it, a below Custom lens fills its field through the stock rect.
        if (const ShaderVariants* rect = detail::findEmissionRectShaderVariants(path)) {
            customEmissionRect_[static_cast<std::size_t>(id)] = buildCustomEmissionRectPipeline(*rect);
        }
    }
    return id;
}

SDL_GPUGraphicsPipeline* Renderer::buildCustomEmissionExtractPipeline(const ShaderVariants& extractFragment) {
    // The shared fullscreen-triangle vertex stage + the shader's `<ns>_emission` extract fragment at the
    // stock custom layout (1 sampler + 1 storage texture + 2 uniforms) — the extract reads the scene through
    // sampleSource() and its OWN params, exactly like a stock custom pass, and CANNOT read emission (that
    // would be circular). No blend: it writes the emission scratch outright, so it always replaces. Target
    // format matches emission0_ (kViewportColorFormat, as emissionExtract_ uses).
    SDL_GPUShader* vertex     = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
    SDL_GPUShader* fragShader = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, extractFragment, 1, 1, 2);

    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = kViewportColorFormat;

    SDL_GPUGraphicsPipelineCreateInfo pipeline{};
    pipeline.vertex_shader                         = vertex;
    pipeline.fragment_shader                       = fragShader;
    pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
    pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
    pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
    pipeline.target_info.color_target_descriptions = &colorTarget;
    pipeline.target_info.num_color_targets         = 1;
    SDL_GPUGraphicsPipeline* built = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

    SDL_ReleaseGPUShader(device_, vertex);
    SDL_ReleaseGPUShader(device_, fragShader);
    if (!built) fail("SDL_CreateGPUGraphicsPipeline (custom emission extract) failed");
    return built;
}

SDL_GPUGraphicsPipeline* Renderer::buildCustomEmissionRectPipeline(const ShaderVariants& rectFragment) {
    // The rect-instanced below extract vertex stage (one storage buffer of GpuEmissionExtract records, no
    // uniform — the sprite-path idiom that dodges Metal's vertex storage+uniform [[buffer]] collision) + the
    // shader's `<ns>_emission_rect` fragment. Resource contract: 1 sampled scene + sampler, 1 storage texture
    // (the sprite-effect records, for the stage's own params), 1 uniform (the compose scale). No blend: each
    // field owns its rect and the extract writes it outright. Target format matches emission0_.
    SDL_GPUShader* vertex =
        createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::emission_extract_rect_vert, 0, 0, 0, 1);
    SDL_GPUShader* fragShader = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, rectFragment, 1, 1, 1);

    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = kViewportColorFormat;

    SDL_GPUGraphicsPipelineCreateInfo pipeline{};
    pipeline.vertex_shader                         = vertex;
    pipeline.fragment_shader                       = fragShader;
    pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
    pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
    pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
    pipeline.target_info.color_target_descriptions = &colorTarget;
    pipeline.target_info.num_color_targets         = 1;
    SDL_GPUGraphicsPipeline* built = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

    SDL_ReleaseGPUShader(device_, vertex);
    SDL_ReleaseGPUShader(device_, fragShader);
    if (!built) fail("SDL_CreateGPUGraphicsPipeline (custom emission rect) failed");
    return built;
}

SDL_GPUGraphicsPipeline* Renderer::buildBatchedStagePipeline(const ShaderVariants& batchedFragment) {
    // The engine's region_batch vertex stage (instanced covering quads; one storage buffer, no uniform —
    // the sprite-path idiom that dodges Metal's vertex storage+uniform [[buffer]] collision) + the shader's
    // BATCHED fragment variant (same 1 sampler + 1 storage texture + 2 uniforms as the normal variant).
    SDL_GPUShader* vertex = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::region_batch_vert,
                                         /*samplers=*/0, /*storageTex=*/0, /*uniforms=*/0, /*storageBuf=*/1);
    SDL_GPUShader* fragShader = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, batchedFragment, 1, 1, 2);

    // ADDITIVE colour blend (ONE / ONE): each region's delta D accumulates onto the destination — the
    // composite IS the blend, so there is no gate pass. Alpha ZERO / ONE preserves the destination alpha
    // (the per-region path's gate output alpha was the source pixel's own; here dst == source at the site).
    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format                            = kViewportColorFormat;
    colorTarget.blend_state.enable_blend          = true;
    colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
    colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineCreateInfo pipeline{};
    pipeline.vertex_shader                         = vertex;
    pipeline.fragment_shader                       = fragShader;
    pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
    pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
    pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
    pipeline.target_info.color_target_descriptions = &colorTarget;
    pipeline.target_info.num_color_targets         = 1;
    SDL_GPUGraphicsPipeline* built = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

    SDL_ReleaseGPUShader(device_, vertex);
    SDL_ReleaseGPUShader(device_, fragShader);
    if (!built) fail("SDL_CreateGPUGraphicsPipeline (batched region) failed");
    return built;
}

SDL_GPUGraphicsPipeline* Renderer::buildGatherStagePipeline(const ShaderVariants& gatherFragment, bool blend) {
    // The shared fullscreen-triangle vertex stage (no per-instance geometry — one triangle covers the frame)
    // + the shader's GATHER fragment variant. Resource contract: 1 sampled source + sampler, 1 storage
    // texture (the row-data store, bound for layout parity — a gather-eligible step carries no paramTable,
    // so it is unread), 1 fragment STORAGE BUFFER (the run's per-region records, t2 space2), and TWO uniforms
    // (b0 = the engine cbuffer, b1 = RetroppGatherInfo — the freed params register carries the run's region
    // count). The replace pipeline (frame-level / Below / mid-chain) writes the gathered image; the blend
    // pipeline (premultiplied-over — ONE / ONE_MINUS_SRC_ALPHA) composites a Normal layer's last gather step
    // onto target_, mirroring runRegionSelect(blend=true) and the customBlend_ state.
    SDL_GPUShader* vertex     = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
    SDL_GPUShader* fragShader = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, gatherFragment,
                                             /*samplers=*/1, /*storageTex=*/1, /*uniforms=*/2, /*storageBuf=*/1);

    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = kViewportColorFormat;
    if (blend) {
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // premultiplied src
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
    }

    SDL_GPUGraphicsPipelineCreateInfo pipeline{};
    pipeline.vertex_shader                         = vertex;
    pipeline.fragment_shader                       = fragShader;
    pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
    pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
    pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
    pipeline.target_info.color_target_descriptions = &colorTarget;
    pipeline.target_info.num_color_targets         = 1;
    SDL_GPUGraphicsPipeline* built = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

    SDL_ReleaseGPUShader(device_, vertex);
    SDL_ReleaseGPUShader(device_, fragShader);
    if (!built) fail("SDL_CreateGPUGraphicsPipeline (gather region) failed");
    return built;
}

SDL_GPUGraphicsPipeline* Renderer::buildSpriteStagePipeline(const ShaderVariants& spriteFragment) {
    // The engine's sprite VERTEX stage (one storage buffer of GpuSprite records, no uniform) + the shader's
    // SPRITE fragment variant — the sprite fragment with this shader's body injected at its Custom step. The
    // resource contract and alpha blend match the stock sprite pipeline exactly (1 sampler + 5 storage
    // textures + 1 uniform), so a run draws through this pipeline identically to the stock one. The emission
    // atlas, its sampler and its rect table are part of that contract whether or not a given custom body sits
    // beside a region Bloom — one layout, every variant.
    SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::sprite_vert, 0, 0, 0, 1);
    SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, spriteFragment, 1, 5, 1);

    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format                            = kViewportColorFormat;
    colorTarget.blend_state.enable_blend          = true;
    colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
    colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineCreateInfo pipeline{};
    pipeline.vertex_shader                         = vertex;
    pipeline.fragment_shader                       = fragment;
    pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
    pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
    pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
    pipeline.target_info.color_target_descriptions = &colorTarget;
    pipeline.target_info.num_color_targets         = 1;
    SDL_GPUGraphicsPipeline* built = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

    SDL_ReleaseGPUShader(device_, vertex);
    SDL_ReleaseGPUShader(device_, fragment);
    if (!built) fail("SDL_CreateGPUGraphicsPipeline (sprite custom) failed");
    return built;
}

SDL_GPUGraphicsPipeline* Renderer::buildSpriteBelowStagePipeline(const ShaderVariants& belowFragment) {
    // The engine's sprite VERTEX stage (one storage buffer of GpuSprite records, no uniform) + the shader's
    // SPRITE_BELOW fragment variant — the below sprite fragment (scene sampler + emission atlas + coverage
    // stores + effect records + the rect table) with this shader's body injected at its Below-custom hook
    // (sampleSource reads the scene). The resource contract (2 samplers + 5 storage textures + 1 uniform) and
    // blend (premultiplied-into-a-transparent-scratch) match the stock spriteBelow_ pipeline exactly, so a
    // below run draws through this pipeline identically to the built-in one. Both emission bindings are part
    // of that contract whether or not a given custom body glows — one layout, every variant.
    SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::sprite_vert, 0, 0, 0, 1);
    SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, belowFragment, 2, 5, 1);

    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format                            = kViewportColorFormat;
    colorTarget.blend_state.enable_blend          = true;
    colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
    colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineCreateInfo pipeline{};
    pipeline.vertex_shader                         = vertex;
    pipeline.fragment_shader                       = fragment;
    pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
    pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
    pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
    pipeline.target_info.color_target_descriptions = &colorTarget;
    pipeline.target_info.num_color_targets         = 1;
    SDL_GPUGraphicsPipeline* built = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

    SDL_ReleaseGPUShader(device_, vertex);
    SDL_ReleaseGPUShader(device_, fragment);
    if (!built) fail("SDL_CreateGPUGraphicsPipeline (sprite below-custom) failed");
    return built;
}

// Core indexed-atlas upload: one palette INDEX per pixel as R32_UINT, so a pixel can address an
// arbitrary palette. Read in-shader by integer Load — no sampler; colour is resolved from
// a palette at render time, not stored here. The public overloads widen 8-/16-bit source indices
// into the 32-bit texel (Texture2D<uint> reads any width identically).
AtlasId Renderer::uploadAtlas32(const std::uint32_t* indices, int width, int height, TransparentIndices transparent) {
    if (width <= 0 || height <= 0) fail("uploadAtlas: non-positive dimensions");

    // Append this atlas to the flat atlas store (mirroring uploadPalette). The store stacks
    // every atlas vertically so a SINGLE map layer can mix tiles from several sheets — TileCell::
    // atlasSelect picks the region. Keep a CPU mirror of each atlas's pixels so the store can be
    // rewritten when a wider atlas re-lays it out.
    AtlasEntry entry;
    entry.data.assign(indices, indices + static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    entry.width       = width;
    entry.height      = height;
    entry.transparent = transparent;
    atlases_.push_back(std::move(entry));
    const AtlasId id = static_cast<AtlasId>(atlases_.size() - 1);

    appendAtlasToStore(id);
    return id;
}

Renderer::AtlasRegionWords Renderer::atlasRegionWords(const AtlasEntry& atlas) {
    return AtlasRegionWords{
        .storeY        = static_cast<std::uint32_t>(atlas.storeY),
        .cols          = static_cast<std::uint32_t>(atlas.width / kTilePx),
        .transparentLo = static_cast<std::uint32_t>(atlas.transparent.mask & 0xFFFFFFFFu),
        .transparentHi = static_cast<std::uint32_t>(atlas.transparent.mask >> 32)};
}

void Renderer::appendAtlasToStore(AtlasId id) {
    AtlasEntry& a       = atlases_[static_cast<std::size_t>(id)];
    const int   top     = atlasStoreH_;       // the rows the atlases before it occupy
    const int   stacked = top + a.height;
    // An atlas wider than the store changes the stride of every row, and one past the allocated height
    // has nowhere to go: both restack the store. Anything else is written over its own rows.
    if (!atlasStore_ || !detail::atlasFitsStore(a.width, stacked, atlasCapW_, atlasCapH_)) {
        rebuildAtlasStore();
        return;
    }
    a.storeY     = top;
    atlasStoreH_ = stacked;

    pendingAtlasWrites_.push_back(id);

    // Stacking a sheet leaves every earlier sheet where it was, so each texel already in the table still
    // describes its atlas and only the new one is written.
    const int index = static_cast<int>(id);
    if (atlasRegionStore_ && index < atlasRegionCap_)
        pendingRegionWrites_.push_back(index);
    else
        rebuildAtlasRegions();
}

void Renderer::flushStoreWrites(SDL_GPUCommandBuffer* cmd, SDL_GPUCopyPass*& copy,
                                FrameScratch& scratch) {
    if (pendingPaletteWrites_.empty() && pendingAtlasWrites_.empty() && pendingRegionWrites_.empty())
        return;
    if (!copy) copy = SDL_BeginGPUCopyPass(cmd);

    for (const PendingColors& run : pendingPaletteWrites_) {
        const detail::PaletteAppend where =
            detail::paletteAppend(run.first, run.count, kPaletteStoreWidth);
        const Rgba16* src = paletteData_.data() + run.first;
        scratch.transfers.push_back(uploadStoreRect(device_, copy, paletteStore_, where.head, src,
                                                    sizeof(Rgba16), "palette append upload failed"));
        if (where.wraps())
            scratch.transfers.push_back(uploadStoreRect(device_, copy, paletteStore_, where.wrapped,
                                                        src + where.head.w, sizeof(Rgba16),
                                                        "palette append upload failed"));
    }
    for (const AtlasId id : pendingAtlasWrites_) {
        const AtlasEntry& a = atlases_[static_cast<std::size_t>(id)];
        scratch.transfers.push_back(uploadStoreRect(
            device_, copy, atlasStore_, detail::atlasRect(a.storeY, a.width, a.height), a.data.data(),
            sizeof(std::uint32_t), "atlas append upload failed"));
    }
    for (const int index : pendingRegionWrites_) {
        const AtlasRegionWords words = atlasRegionWords(atlases_[static_cast<std::size_t>(index)]);
        scratch.transfers.push_back(uploadStoreRect(device_, copy, atlasRegionStore_,
                                                    detail::atlasRegionTexel(index), &words,
                                                    sizeof(AtlasRegionWords),
                                                    "atlas region append upload failed"));
    }

    pendingPaletteWrites_.clear();
    pendingAtlasWrites_.clear();
    pendingRegionWrites_.clear();
}

void Renderer::rebuildAtlasStore() {
    ++storeGeneration_;  // a new store texture the compose binds — force the next frame to recompose
    pendingAtlasWrites_.clear();  // every atlas is written below
    if (atlases_.empty()) return;

    // Stack vertically: store width = the widest atlas, height = Σ heights; assign each atlas its top row.
    int W = 0, H = 0;
    for (AtlasEntry& a : atlases_) {
        W = std::max(W, a.width);
        a.storeY = H;
        H += a.height;
    }
    atlasStoreW_ = W;
    atlasStoreH_ = H;
    // Allocate rows past the ones the atlases occupy, so the sheets uploaded next are written over their
    // own rows instead of bringing every atlas back through here.
    atlasCapW_ = W;
    atlasCapH_ = detail::grownCapacity(H);

    if (atlasStore_) SDL_ReleaseGPUTexture(device_, atlasStore_);
    SDL_GPUTextureCreateInfo texInfo{};
    texInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format               = SDL_GPU_TEXTUREFORMAT_R32_UINT;
    texInfo.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
    texInfo.width                = static_cast<Uint32>(atlasCapW_);
    texInfo.height               = static_cast<Uint32>(atlasCapH_);
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels           = 1;
    texInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    atlasStore_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!atlasStore_) fail("SDL_CreateGPUTexture (atlas store) failed");

    // Build a W×H buffer: each atlas copied left-aligned into its rows, the rest zero (index 0).
    std::vector<std::uint32_t> upload(static_cast<std::size_t>(W) * static_cast<std::size_t>(H), 0u);
    for (const AtlasEntry& a : atlases_) {
        for (int y = 0; y < a.height; ++y) {
            std::copy_n(a.data.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(a.width),
                        static_cast<std::size_t>(a.width),
                        upload.data() + static_cast<std::size_t>(a.storeY + y) * static_cast<std::size_t>(W));
        }
    }

    const Uint32 bytes = static_cast<Uint32>(upload.size()) * static_cast<Uint32>(sizeof(std::uint32_t));
    SDL_GPUTransferBufferCreateInfo tbInfo{};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbInfo.size  = bytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
    if (!transfer) fail("SDL_CreateGPUTransferBuffer (atlas store) failed");

    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (!mapped) fail("SDL_MapGPUTransferBuffer (atlas store) failed");
    std::memcpy(mapped, upload.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) fail("SDL_AcquireGPUCommandBuffer (atlas store) failed");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = transfer;
    src.offset          = 0;
    src.pixels_per_row  = static_cast<Uint32>(W);
    src.rows_per_layer  = static_cast<Uint32>(H);
    SDL_GPUTextureRegion dst{};
    dst.texture = atlasStore_;
    dst.w       = static_cast<Uint32>(W);
    dst.h       = static_cast<Uint32>(H);
    dst.d       = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);

    rebuildAtlasRegions();
}

// Global atlas-region table: one R32G32B32A32_UINT texel per AtlasId = (storeY, cols,
// transparentMaskLo, transparentMaskHi). Both frag stages Load it by a cell's / sprite's atlas handle to
// resolve that sheet's placement (storeY) and stride (cols) in the flat store, and to test structural
// transparency: the sheet's transparent-index set is a 64-bit bitmask split across .z (indices 0–31) and
// .w (indices 32–63), bit i set => index i is a hole. The empty set (None) is mask 0 → both words 0 →
// nothing discarded. The table is allocated with texels past the atlases that have one, so the sheets
// uploaded next write only their own texel.
void Renderer::rebuildAtlasRegions() {
    ++storeGeneration_;  // a new region table the compose binds — force the next frame to recompose
    pendingRegionWrites_.clear();  // every texel is written below
    const int n = static_cast<int>(atlases_.size());
    if (n == 0) return;
    const int capacity = detail::grownCapacity(n);

    static_assert(sizeof(AtlasRegionWords) == 4 * sizeof(std::uint32_t),
                  "the region table's texel is four R32_UINT words, copied as they are laid out");
    std::vector<AtlasRegionWords> words(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        words[static_cast<std::size_t>(i)] = atlasRegionWords(atlases_[static_cast<std::size_t>(i)]);

    if (atlasRegionStore_) SDL_ReleaseGPUTexture(device_, atlasRegionStore_);
    SDL_GPUTextureCreateInfo rInfo{};
    rInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
    rInfo.format               = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT;
    rInfo.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
    rInfo.width                = static_cast<Uint32>(capacity);
    rInfo.height               = 1;
    rInfo.layer_count_or_depth = 1;
    rInfo.num_levels           = 1;
    rInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    atlasRegionStore_ = SDL_CreateGPUTexture(device_, &rInfo);
    if (!atlasRegionStore_) fail("SDL_CreateGPUTexture (atlas region store) failed");
    atlasRegionCap_ = capacity;

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) fail("SDL_AcquireGPUCommandBuffer (atlas region store) failed");
    SDL_GPUCopyPass*       copy     = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBuffer* transfer = uploadStoreRect(
        device_, copy, atlasRegionStore_, detail::StoreRect{.x = 0, .y = 0, .w = n, .h = 1},
        words.data(), sizeof(AtlasRegionWords), "atlas region store upload failed");
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
}

CurveMaskId Renderer::bakeCurveMask(const Curve& boundary, float padding, int maxResolution) {
    // Bake Curve::signedDistance over the boundary's box on the CPU (the device-free producer), then upload it
    // as an R16_FLOAT field the gate samples with the bilinear sampler. Bake is amortized (setup), like an atlas.
    const CurveMaskField field = bakeCurveMaskField(boundary, padding, maxResolution);
    if (field.width <= 0 || field.height <= 0) fail("bakeCurveMask: empty boundary");

    SDL_GPUTextureCreateInfo texInfo{};
    texInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format               = SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
    texInfo.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texInfo.width                = static_cast<Uint32>(field.width);
    texInfo.height               = static_cast<Uint32>(field.height);
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels           = 1;
    texInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* tex = SDL_CreateGPUTexture(device_, &texInfo);
    if (!tex) fail("SDL_CreateGPUTexture (curve mask) failed");

    std::vector<Uint16> halfData(field.distances.size());
    for (std::size_t i = 0; i < field.distances.size(); ++i) halfData[i] = floatToHalfBits(field.distances[i]);

    const Uint32 bytes = static_cast<Uint32>(halfData.size()) * static_cast<Uint32>(sizeof(Uint16));
    SDL_GPUTransferBufferCreateInfo tbInfo{};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbInfo.size  = bytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
    if (!transfer) fail("SDL_CreateGPUTransferBuffer (curve mask) failed");
    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (!mapped) fail("SDL_MapGPUTransferBuffer (curve mask) failed");
    std::memcpy(mapped, halfData.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) fail("SDL_AcquireGPUCommandBuffer (curve mask) failed");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = transfer;
    src.offset          = 0;
    src.pixels_per_row  = static_cast<Uint32>(field.width);
    src.rows_per_layer  = static_cast<Uint32>(field.height);
    SDL_GPUTextureRegion dst{};
    dst.texture = tex;
    dst.w       = static_cast<Uint32>(field.width);
    dst.h       = static_cast<Uint32>(field.height);
    dst.d       = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);

    curveMasks_.push_back(CurveMaskEntry{tex, field.bakeMin, field.bakeExtent, field.width, field.height});
    ++storeGeneration_;  // out-of-frame GPU resource a region compose samples — force the next frame to recompose
    return static_cast<CurveMaskId>(curveMasks_.size());  // 1-based; 0 = none
}

ShapePoints Renderer::bakeCurveRegion(const Curve& boundary, float radius, Transform t, float padding,
                                      int maxResolution) {
    ShapePoints shape = ShapePoints::fromCurve(boundary, radius, t);
    shape.curveMask   = bakeCurveMask(boundary, padding, maxResolution);
    return shape;
}

AtlasManifest Renderer::uploadAtlas(const std::uint8_t* indices, int width, int height, TransparentIndices transparent) {
    // The three-argument form declares the Single carve: the whole image is one asset (slot 0).
    return uploadAtlas(indices, width, height, AssetDimensions{}, ContentKind::Single,
                       ReadOrder::LeftRightThenDown, 0, transparent, 0);
}

AtlasManifest Renderer::uploadAtlas(const std::uint16_t* indices, int width, int height, TransparentIndices transparent) {
    return uploadAtlas(indices, width, height, AssetDimensions{}, ContentKind::Single,
                       ReadOrder::LeftRightThenDown, 0, transparent, 0);
}

AtlasManifest Renderer::uploadAtlas(const std::uint32_t* indices, int width, int height, TransparentIndices transparent) {
    return uploadAtlas(indices, width, height, AssetDimensions{}, ContentKind::Single,
                       ReadOrder::LeftRightThenDown, 0, transparent, 0);
}

AtlasManifest Renderer::uploadAtlas(const std::uint8_t* indices, int width, int height,
                                    AssetDimensions assetSize, ContentKind kind, ReadOrder order,
                                    int count, TransparentIndices transparent, int framesPerAnimation) {
    if (width <= 0 || height <= 0) fail("uploadAtlas: non-positive dimensions");
    const std::vector<std::uint32_t> widened(indices,
                                             indices + static_cast<std::size_t>(width) * height);
    return carveUploaded(uploadAtlas32(widened.data(), width, height, transparent),
                         assetSize, kind, order, count, framesPerAnimation);
}

AtlasManifest Renderer::uploadAtlas(const std::uint16_t* indices, int width, int height,
                                    AssetDimensions assetSize, ContentKind kind, ReadOrder order,
                                    int count, TransparentIndices transparent, int framesPerAnimation) {
    if (width <= 0 || height <= 0) fail("uploadAtlas: non-positive dimensions");
    const std::vector<std::uint32_t> widened(indices,
                                             indices + static_cast<std::size_t>(width) * height);
    return carveUploaded(uploadAtlas32(widened.data(), width, height, transparent),
                         assetSize, kind, order, count, framesPerAnimation);
}

AtlasManifest Renderer::uploadAtlas(const std::uint32_t* indices, int width, int height,
                                    AssetDimensions assetSize, ContentKind kind, ReadOrder order,
                                    int count, TransparentIndices transparent, int framesPerAnimation) {
    if (width <= 0 || height <= 0) fail("uploadAtlas: non-positive dimensions");
    return carveUploaded(uploadAtlas32(indices, width, height, transparent),
                         assetSize, kind, order, count, framesPerAnimation);
}

AtlasManifest Renderer::uploadAtlas(const LoadedImage&) {
    // The image → uploadAtlas route is forbidden: it bypasses the slicing system. Load a PNG with
    // loadAtlas() (it slices the image into an addressable AtlasManifest); uploadAtlas is only for raw
    // index arrays you author yourself.
    throw std::logic_error(
        "uploadAtlas does not take images — load a PNG with loadAtlas() (it slices the image into an "
        "AtlasManifest). uploadAtlas is only for raw index arrays you specify yourself.");
}

// Grouping is a manifest concern, not a carve concern: framesPerAnimation is recorded on the manifest
// only for an AnimationSeries sheet (every grid kind carves identically). For other kinds it is left 0
// (ungrouped) regardless of what the caller passed.
static int seriesFrameGroup(ContentKind kind, int framesPerAnimation) noexcept {
    return kind == ContentKind::AnimationSeries ? framesPerAnimation : 0;
}

AtlasManifest Renderer::loadAtlas(LiteralPath path, AssetDimensions assetSize,
                                 ContentKind kind, ReadOrder order, int count, TransparentIndices transparent,
                                 int framesPerAnimation, std::optional<AssetPolicy> policy) {
    // Resolve embed-vs-load: per-call > loadAtlas's per-type default (LoadFromPath). An Embed atlas
    // decodes from the bytes the build baked in, keyed by its logical path; if none were baked we fall
    // through to the disk read.
    if (resolveAssetPolicy(policy, AssetPolicy::LoadFromPath) == AssetPolicy::Embed) {
        if (const std::span<const std::uint8_t> bytes = detail::findEmbeddedAsset(path.view());
            !bytes.empty()) {
            return loadAtlasFromMemory(bytes, assetSize, kind, order, count, transparent,
                                       framesPerAnimation);
        }
        detail::warnEmbedNotBaked("asset", path.view());
    }
    // LoadFromPath (or an un-baked Embed): resolve the logical path against the runtime asset root.
    const LoadedImage img = loadPng(assetRoot() / path.c_str());  // throws on missing / decode / RGBA
    return uploadAtlas(img.indices.data(), img.width, img.height,  // uploads ONCE + carves + records
                       assetSize, kind, order, count, transparent, framesPerAnimation);
}

AtlasManifest Renderer::loadAtlasFromMemory(std::span<const std::uint8_t> bytes, AssetDimensions assetSize,
                                           ContentKind kind, ReadOrder order, int count,
                                           TransparentIndices transparent, int framesPerAnimation) {
    const LoadedImage img = loadPngFromMemory(bytes);
    return uploadAtlas(img.indices.data(), img.width, img.height,
                       assetSize, kind, order, count, transparent, framesPerAnimation);
}

AtlasManifest Renderer::carveUploaded(AtlasId atlas, AssetDimensions assetSize, ContentKind kind,
                                      ReadOrder order, int count, int framesPerAnimation) {
    // Single ignores assetSize by contract; the carve spans the uploaded pixel size.
    const PixelSize size = atlasPixelSize(atlas);
    const AtlasManifest manifest{
        .atlasId            = atlas,
        .slots              = sliceLayout(size, assetSize, kind, order, count),
        .framesPerAnimation = seriesFrameGroup(kind, framesPerAnimation),
        .kind               = kind};
    // Record the slice geometry on the sheet's AtlasEntry — what atlasSlot (an AnimationFrame's
    // tile()/size()) resolves a slot index through.
    if (const std::size_t i = static_cast<std::size_t>(atlas); i < atlases_.size()) {
        AtlasEntry& e = atlases_[i];
        e.slotSize  = assetSize;
        e.slotKind  = kind;
        e.slotOrder = order;
        e.slotCount = static_cast<int>(manifest.slots.size());
    }
    return manifest;
}

PaletteId Renderer::loadPaletteImage(LiteralPath path, ReadOrder order, int count,
                                     std::optional<AssetPolicy> policy) {
    // Resolve embed-vs-load: per-call > loadPaletteImage's per-type default (Embed — a palette image is
    // bespoke build-time colour data, like a map PNG). An Embed image decodes from the bytes the build
    // baked in, keyed by its logical path; if none were baked we fall through to the disk read.
    if (resolveAssetPolicy(policy, AssetPolicy::Embed) == AssetPolicy::Embed) {
        if (const std::span<const std::uint8_t> bytes = detail::findEmbeddedAsset(path.view());
            !bytes.empty()) {
            return uploadPalette(slicePaletteImage(loadPngFromMemory(bytes), order, count));
        }
        detail::warnEmbedNotBaked("asset", path.view());
    }
    // LoadFromPath (or an un-baked Embed): resolve the logical path against the runtime asset root.
    const LoadedImage img = loadPng(assetRoot() / path.c_str());  // throws on missing / decode
    return uploadPalette(slicePaletteImage(img, order, count));   // slice throws on a non-RGBA source
}

PaletteId Renderer::uploadPalette(std::span<const Rgba8> colors) {
    // The store keeps 16-bit channels; an 8-bit entry widens losslessly (×257) on the way in.
    const std::size_t first = paletteData_.size();
    paletteData_.reserve(first + colors.size());
    for (const Rgba8 c : colors) paletteData_.push_back(widen(c));
    appendPaletteColors(first, colors.size());
    return static_cast<PaletteId>(first);
}

PaletteId Renderer::uploadPalette(std::span<const Rgba16> colors) {
    // A 16-bit colour source appends direct — no widening.
    const std::size_t first = paletteData_.size();
    paletteData_.insert(paletteData_.end(), colors.begin(), colors.end());
    appendPaletteColors(first, colors.size());
    return static_cast<PaletteId>(first);
}

void Renderer::appendPaletteColors(std::size_t first, std::size_t count) {
    const int W = kPaletteStoreWidth;
    // The store is rewritten when the entries need rows the texture does not have, and when a run is long
    // enough to cover a whole row — the rewrite is the one path that writes any shape of content.
    if (!paletteStore_ ||
        detail::paletteStoreRows(paletteData_.size(), W) > paletteStoreRows_ ||
        !detail::paletteAppendFitsTwoRows(first, count, W)) {
        rebuildPaletteStore();
        return;
    }
    if (count == 0) return;  // nothing appended, so nothing to write
    pendingPaletteWrites_.push_back(PendingColors{.first = first, .count = count});
}

void Renderer::rebuildPaletteStore() {
    ++storeGeneration_;  // a new store texture the compose binds — force the next frame to recompose
    pendingPaletteWrites_.clear();  // every colour is written below
    // paletteData_ is the FLAT, contiguous CPU mirror; a PaletteId IS an entry's flat offset into it.
    // The store texture is that flat array wrapped kPaletteStoreWidth colours wide; palettes pack
    // contiguously (no per-palette padding) and may straddle rows. The texture is allocated with rows past
    // what the entries need, and the whole of that flat array is written here — so the palettes uploaded
    // next are appended into the spare rows instead of bringing the array back through here.
    const int W        = kPaletteStoreWidth;
    const int rows     = detail::paletteStoreRows(paletteData_.size(), W);
    const int capacity = detail::grownCapacity(rows);

    if (paletteStore_) SDL_ReleaseGPUTexture(device_, paletteStore_);
    SDL_GPUTextureCreateInfo texInfo{};
    texInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format               = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM;
    texInfo.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
    texInfo.width                = static_cast<Uint32>(W);
    texInfo.height               = static_cast<Uint32>(capacity);
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels           = 1;
    texInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    paletteStore_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!paletteStore_) fail("SDL_CreateGPUTexture (palette store) failed");
    paletteStoreRows_ = capacity;

    // Upload a W×rows buffer: the flat colours followed by opaque-black padding out to the last row.
    std::vector<Rgba16> upload(static_cast<std::size_t>(W) * static_cast<std::size_t>(rows));
    std::copy(paletteData_.begin(), paletteData_.end(), upload.begin());

    const Uint32 bytes = static_cast<Uint32>(upload.size()) * static_cast<Uint32>(sizeof(Rgba16));
    SDL_GPUTransferBufferCreateInfo tbInfo{};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbInfo.size  = bytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
    if (!transfer) fail("SDL_CreateGPUTransferBuffer (palette store) failed");

    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (!mapped) fail("SDL_MapGPUTransferBuffer (palette store) failed");
    std::memcpy(mapped, upload.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) fail("SDL_AcquireGPUCommandBuffer (palette store) failed");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = transfer;
    src.offset          = 0;
    src.pixels_per_row  = static_cast<Uint32>(W);
    src.rows_per_layer  = static_cast<Uint32>(rows);
    SDL_GPUTextureRegion dst{};
    dst.texture = paletteStore_;
    dst.w       = static_cast<Uint32>(W);
    dst.h       = static_cast<Uint32>(rows);
    dst.d       = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
}

SDL_GPUTexture* Renderer::composeViewport(SDL_GPUCommandBuffer* cmd, const FrameDrawState& frame,
                                          FrameScratch& scratch,
                                          const FrameTiming& timing, bool interpolate) {
    ++renderStats_.composePasses;  // this compose (renderFrame + captureViewport both land here)
    // Validate + order the layers (throws or warns per the collision policy).
    const std::vector<std::size_t> order = layerDrawOrder(frame.layers, collisionPolicy_);
    // Validate sprite keys frame-wide under the same policy: the interpolator holds ONE sprite map across
    // every layer, so a sprite key must be present and unique frame-wide (a duplicate would reconcile two
    // sprites to one slot).
    validateSpriteKeys(frame.layers, collisionPolicy_);

    // Crisp evaluation: on the Viewport grid the analytic paths (transformed tiles, effect regions, the
    // sampling effects) snap their spatial math to the viewport grid so the upscaled image is pixel-identical
    // to the viewport-resolution rasterization. A mathematical no-op at compose scale 1. Threaded into the
    // tile uniform, the displace/ripple uniforms, and the region/stencil packers below.
    const bool  snap  = evaluationGrid_ == EvaluationGrid::Viewport;
    const float snapF = snap ? 1.0f : 0.0f;

    // Per-effect row-data table locations in the row-data store (built in the copy pass below, read in
    // runEffect). Keyed by the effect's address — stable across this compose, since the effect steps
    // reference the same frame effects by pointer.
    std::unordered_map<const ScreenSpaceEffect*, RowTableLoc> rowTableLocs;

    // ── Copy pass: (re)create + upload each TILES layer's tilemap, each SPRITES layer's buffer. ──
    // The persistent caches are keyed by ObjectKey, not position, so resolve each layer's slot by its key.
    // `seen*` records the keys that claimed a persistent slot this frame — driving end-of-copy eviction of
    // despawned layers. A degenerate key (empty, or a duplicate of one already claimed this frame — only
    // reachable under WarnAndResolve) gets a per-frame transient slot from `transient*` (pointer-stable
    // deque; its GPU resource is queued into `scratch` at the end for post-submit release) so two colliding
    // keys never share a slot. `tileSlot`/`spriteSlot` bridge this copy pass to the composite: drawLayer
    // reads each layer position's resolved slot from here (the cache is keyed by identity, not by position).
    std::unordered_set<std::string>   seenTileKeys, seenSpriteKeys, seenRasterKeys;
    std::deque<TilemapTex>            transientTiles;
    std::deque<SpriteBuf>             transientSprites;
    std::deque<RasterTex>             transientRasters;
    std::vector<const TilemapTex*>    tileSlot(frame.layers.size(), nullptr);
    std::vector<const SpriteBuf*>     spriteSlot(frame.layers.size(), nullptr);
    std::vector<const RasterTex*>     rasterSlot(frame.layers.size(), nullptr);
    auto tileCacheSlot = [&](std::string_view key) -> TilemapTex& {
        if (key.empty() || !seenTileKeys.emplace(key).second) return transientTiles.emplace_back();
        return tilemaps_[std::string(key)];
    };
    auto rasterCacheSlot = [&](std::string_view key) -> RasterTex& {
        if (key.empty() || !seenRasterKeys.emplace(key).second) {
            return transientRasters.emplace_back();
        }
        return rasters_[std::string(key)];
    };
    auto spriteCacheSlot = [&](std::string_view key) -> SpriteBuf& {
        if (key.empty() || !seenSpriteKeys.emplace(key).second) return transientSprites.emplace_back();
        return spriteBufs_[std::string(key)];
    };
    // A sprite layer splits into contiguous same-(blend, pipeline) runs, each uploaded to its own pool buffer
    // and drawn separately, when its sprites don't all share BlendMode::Normal (non-Normal runs grade onto
    // their container) OR some sprite carries a Custom effect (it draws through a custom sprite-inline
    // pipeline). `spriteLayerRuns[idx]` is populated ONLY for such layers (empty ⇒ the all-Normal, all-stock
    // fast path: one buffer, one draw, unchanged). `spriteRunSlot` hands out pool slots across the frame.
    struct SpriteRunGpu { SDL_GPUBuffer* buffer; int count; BlendMode blend; int pipelineKey; };
    std::vector<std::vector<SpriteRunGpu>> spriteLayerRuns(frame.layers.size());
    int spriteRunSlot = 0;
    // Per-layer Below-scope sprite runs: the layer's Below sprites (their GpuSprites point at their Below
    // effect records in fxStore), split into contiguous same-pipeline runs — the built-in below fragment
    // (spriteBelow_, pipelineKey 0) for ColorFill/Gleam/ColorSaturation/RowDisplacement/Ripple, a scene-read custom variant
    // (customSpriteBelow_[key-1]) for a Below-scope Custom effect. Each run draws in ONE instanced pass into a
    // scratch reading the accumulator (first run CLEAR, the rest LOAD), then the scratch composites
    // premultiplied-over the accumulator BEFORE the layer's own art draws. Pass count tracks the below-pipeline
    // mix, never the sprite count. Empty for a layer with no realized Below-scope effect (the byte-identical
    // no-Below path).
    struct SpriteBelowRunGpu { SDL_GPUBuffer* buffer = nullptr; int count = 0; int pipelineKey = 0; };
    std::vector<std::vector<SpriteBelowRunGpu>> spriteBelowRuns(frame.layers.size());
    // Per-layer sprite-emission buckets: the layer's glowing sprites grouped by (kind, reach) — the only two
    // things downstream of the raster can see. Each bucket rasters its members' emission into the shared
    // buffer in ONE instanced pass, blurs it once, and composites the halo over the layer once, so a layer's
    // glow cost tracks the distinct reaches it authors and not its sprite count. Empty for a layer with no
    // realized Layer-scope Bloom / Glow — every other layer's path is untouched.
    struct SpriteEmissionRunGpu {
        SDL_GPUBuffer* buffer = nullptr;
        int            count  = 0;
        bool           glow   = false;
        float          reach  = 0.0f;   // viewport px
    };
    std::vector<std::vector<SpriteEmissionRunGpu>> spriteEmissionRuns(frame.layers.size());
    // Per-layer emission fields — BOTH paths', in one store. A Layer-scope sprite region's Bloom reads its
    // sprite's own light as a graded source inside the art fragment; a Below-scope lens reads the SCENE's
    // light through its silhouette. They differ in what fills a rect, not in how a rect is found, so they
    // share one plan, one atlas and one rect table, and a page's blur serves whichever kind of field lands
    // on it.
    //
    // One SHEET of the atlas store: the fields the packer fitted in a single pass, their instances, and the
    // reach pages the blur walks. A layer needs more than one sheet only when its fields do not fit on one,
    // and then the next sheet is simply the next array layer of the store — so no field count is ever a
    // ceiling. Each sheet fills in ONE render pass: the below extracts, then the region rasters.
    // A run of below extract instances drawn through one emission stage's own rect pipeline (a Custom lens
    // whose stage authors its field with an emission() body). Grouped per (sheet, stage) so a stage's fields
    // on a sheet fill in one instanced draw; `stage` selects customEmissionRect_[stage].
    struct CustomExtractRun {
        std::uint32_t  stage = 0;
        SDL_GPUBuffer* buf   = nullptr;
        int            count = 0;
    };
    struct EmissionSheetGpu {
        SDL_GPUBuffer*            buffer      = nullptr;  // the region art instances landing on this sheet
        int                       count       = 0;
        SDL_GPUBuffer*            extractBuf  = nullptr;  // the STOCK below extract instances (Bloom / Glow /
        int                       extractCount = 0;       //   body-less Custom) landing on it
        std::vector<CustomExtractRun> customExtracts;     // body-authored Custom lenses, one run per stage
        std::vector<EmissionPage> pages;                  // the fields sharing a reach; one blur pass each
        std::vector<float>        pageReach;              // viewport px, parallel to `pages`
    };
    std::vector<std::vector<EmissionSheetGpu>> layerEmissionSheets(frame.layers.size());
    // What each layer asks the atlas for, and the raw instances that will fill it. Retargeting waits until
    // every layer has been planned: the store is one array sized to the frame's peak, and an instance's map is
    // expressed against that grid.
    //
    // The two demand lists are planned TOGETHER, region fields first and below fields after, so a field's
    // index within that concatenation — offset by `fieldBase` — is the rect-table row its record reads.
    //
    // A layer's demands are planned in successive passes: whatever one pass cannot fit is re-planned onto the
    // next sheet, until nothing is left. `sheetOf` says which sheet each demand landed on (-1 = no sheet at
    // all, which only happens when a single field is larger than a whole sheet).
    struct PendingLayerEmission {
        std::vector<SpriteRegionEmissionDemand> demands;      // region fields, first in the index space
        std::vector<GpuSprite>                  instances;    // parallel to `demands`, before retargeting
        std::vector<SpriteBelowEmissionDemand>  belowDemands; // below fields, after them
        std::vector<EmissionPlacement>          placements;   // parallel to demands ++ belowDemands
        std::vector<int>                        sheetOf;      // parallel to demands ++ belowDemands
        std::vector<EmissionAtlasPlan>          sheets;
        int                                     fieldBase = 0;  // this layer's first row of the rect table

        [[nodiscard]] std::size_t total() const { return demands.size() + belowDemands.size(); }
    };
    std::vector<PendingLayerEmission> layerEmissionPlans(frame.layers.size());
    // The frame's rect side-table, one row per field across every layer, and the atlas that holds them all.
    std::vector<EmissionRectEntry> emissionRectRows;
    int                            emissionAtlasWant  = 0;
    int                            emissionAtlasWantH = 0;
    // The atlas + rect table the sprite ART pass binds for each layer: the real pair once that layer's
    // region-Bloom fields have been prepared into it, the idle stand-ins otherwise. The sprite fragment
    // declares both bindings unconditionally, so every layer binds something valid whether or not it
    // authors a halo.
    std::vector<bool> spriteArtHasField(frame.layers.size(), false);
    auto spriteArtFieldFor = [&](std::size_t idx) -> SDL_GPUTexture* {
        if (spriteArtHasField[idx] && emissionAtlasStore_) return emissionAtlasStore_;
        return ensureIdleEmissionAtlas() ? emissionAtlasIdle_ : nullptr;
    };
    auto spriteArtRectsFor = [&](std::size_t idx) -> SDL_GPUTexture* {
        if (spriteArtHasField[idx] && emissionRects_) return emissionRects_;
        return ensureIdleEmissionAtlas() ? emissionRectsIdle_ : nullptr;
    };
    // Every sprite's flattened effect run, accumulated frame-wide into one storage buffer (spriteFxBuf_).
    // Each GpuSprite carries its absolute fxOffset/fxCount into this, so the single buffer bound on every
    // sprite draw serves all layers and runs regardless of which per-layer buffer the GpuSprite lives in.
    std::vector<SpriteFxRecord> fxStore;
    SDL_GPUCopyPass* copy = nullptr;
    // Whatever the uploads since the last compose added to the stores rides this frame's copy pass, ahead
    // of every draw that reads them.
    flushStoreWrites(cmd, copy, scratch);
    for (const std::size_t idx : order) {
        const DrawLayer& layer = frame.layers[idx];
        if (contentKind(layer.content) != LayerContentKind::Tiles) continue;
        const TileContent& tc = std::get<TileContent>(layer.content);
        if (tc.widthInTiles <= 0 || tc.heightInTiles <= 0) continue;

        TilemapTex& slot = tileCacheSlot(layer.key);
        tileSlot[idx] = &slot;   // resolved BEFORE any skip — the composite reads this slot either way
        if (!slot.texture || slot.widthInTiles != tc.widthInTiles ||
            slot.heightInTiles != tc.heightInTiles) {
            if (slot.texture) SDL_ReleaseGPUTexture(device_, slot.texture);
            SDL_GPUTextureCreateInfo ti{};
            ti.type                 = SDL_GPU_TEXTURETYPE_2D;
            ti.format               = SDL_GPU_TEXTUREFORMAT_R32G32_UINT;  // 2 words/cell: packTileCell
            ti.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
            ti.width                = static_cast<Uint32>(tc.widthInTiles);
            ti.height               = static_cast<Uint32>(tc.heightInTiles);
            ti.layer_count_or_depth = 1;
            ti.num_levels           = 1;
            ti.sample_count         = SDL_GPU_SAMPLECOUNT_1;
            slot.texture = SDL_CreateGPUTexture(device_, &ti);
            if (!slot.texture) fail("SDL_CreateGPUTexture (tilemap) failed");
            slot.widthInTiles  = tc.widthInTiles;
            slot.heightInTiles = tc.heightInTiles;
            // The pooled transfer is sized to the old dims — drop it so it is recreated at the new size,
            // and reset the skip signature so the fresh texture is never skipped against stale state.
            if (slot.transfer) { SDL_ReleaseGPUTransferBuffer(device_, slot.transfer); slot.transfer = nullptr; }
            slot.sig = TilemapTex::TileSig::None;
        }

        const Uint32      count = static_cast<Uint32>(tc.widthInTiles) * static_cast<Uint32>(tc.heightInTiles);
        const std::size_t have  = std::min<std::size_t>(count, tc.cells.size());
        if (tc.contentChanged.has_value()) {
            // Declared path: the caller answers the change question, so the engine never packs or hashes.
            // `false` skips (unchanged) once the slot already holds valid content; `true`, or a first
            // submission with no prior upload, packs + uploads. Declaring `false` over changed cells renders
            // the stale map — the contract.
            if (!*tc.contentChanged && slot.sig != TilemapTex::TileSig::None) {
                ++renderStats_.tilemapSkips;
                continue;  // no pack, no hash, no transfer, no copy-pass entry
            }
            slot.staging.resize(count);
            for (std::size_t k = 0; k < count; ++k)
                slot.staging[k] = (k < have) ? packTileCell(tc.cells[k]) : PackedTileCell{};  // pad short maps with cell 0
            slot.sig = TilemapTex::TileSig::Manual;
        } else {
            // Hash path: pack the cells into the retained staging buffer and fold the packed words into a
            // 64-bit content hash in the same loop; skip the upload when it matches the last one stored.
            slot.staging.resize(count);
            std::uint64_t h = kFnv64Offset;
            for (std::size_t k = 0; k < count; ++k) {
                const PackedTileCell pc = (k < have) ? packTileCell(tc.cells[k]) : PackedTileCell{};  // pad short maps with cell 0
                slot.staging[k] = pc;
                h ^= pc.w0; h *= kFnv64Prime; h ^= pc.w1; h *= kFnv64Prime;  // pad cells hash too — they are uploaded bytes
            }
            if (slot.sig == TilemapTex::TileSig::Hashed && slot.contentHash == h) {
                ++renderStats_.tilemapSkips;
                continue;
            }
            slot.sig         = TilemapTex::TileSig::Hashed;
            slot.contentHash = h;
        }

        if (!slot.transfer) {
            // Pooled per-slot transfer buffer — created once (recreated after a dims change released it
            // above) and reused every subsequent frame.
            SDL_GPUTransferBufferCreateInfo tbInfo{};
            tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            tbInfo.size  = count * static_cast<Uint32>(sizeof(PackedTileCell));  // R32G32_UINT: 2 words/cell
            slot.transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
            if (!slot.transfer) fail("SDL_CreateGPUTransferBuffer (tilemap) failed");
        }

        // cycle=true: the pooled buffer may still be in-flight from a prior frame's copy; SDL cycles the backing.
        auto* dst = static_cast<PackedTileCell*>(SDL_MapGPUTransferBuffer(device_, slot.transfer, true));
        if (!dst) fail("SDL_MapGPUTransferBuffer (tilemap) failed");
        std::memcpy(dst, slot.staging.data(), static_cast<std::size_t>(count) * sizeof(PackedTileCell));
        SDL_UnmapGPUTransferBuffer(device_, slot.transfer);

        if (!copy) copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = slot.transfer;
        src.offset          = 0;
        src.pixels_per_row  = static_cast<Uint32>(tc.widthInTiles);
        src.rows_per_layer  = static_cast<Uint32>(tc.heightInTiles);
        SDL_GPUTextureRegion region{};
        region.texture = slot.texture;
        region.w       = static_cast<Uint32>(tc.widthInTiles);
        region.h       = static_cast<Uint32>(tc.heightInTiles);
        region.d       = 1;
        SDL_UploadToGPUTexture(copy, &src, &region, false);
        ++renderStats_.tilemapUploads;
        // slot.transfer is pooled — NOT pushed to scratch.transfers (that list is per-frame release).
    }

    // Each RASTER layer's pixels, on the same terms: a per-key persistent texture, recreated when the
    // raster's dimensions or pixel layout change, uploaded when its generation moved. There is no hash
    // on this path and never will be — a full-color raster differs every time its source draws, so
    // hashing one to decide costs more than the upload it could save. The submission already says
    // whether a new raster arrived, and that answer is the whole decision.
    for (const std::size_t idx : order) {
        const DrawLayer& layer = frame.layers[idx];
        if (contentKind(layer.content) != LayerContentKind::Raster) continue;
        const RasterContent& gc = std::get<RasterContent>(layer.content);
        if (gc.width <= 0 || gc.height <= 0) continue;
        const std::size_t needed = static_cast<std::size_t>(gc.width) *
                                   static_cast<std::size_t>(gc.height) * bytesPerPixel(gc.format);
        if (gc.pixels.size() < needed) continue;  // a raster short of its own dimensions is not whole

        RasterTex& slot = rasterCacheSlot(layer.key);
        rasterSlot[idx] = &slot;   // resolved BEFORE any skip — the composite reads this slot either way
        if (!slot.texture || slot.width != gc.width || slot.height != gc.height ||
            slot.format != gc.format) {
            if (slot.texture) SDL_ReleaseGPUTexture(device_, slot.texture);
            SDL_GPUTextureCreateInfo ti{};
            ti.type                 = SDL_GPU_TEXTURETYPE_2D;
            ti.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;  // RasterPixelFormat::Rgba8888
            ti.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
            ti.width                = static_cast<Uint32>(gc.width);
            ti.height               = static_cast<Uint32>(gc.height);
            ti.layer_count_or_depth = 1;
            ti.num_levels           = 1;
            ti.sample_count         = SDL_GPU_SAMPLECOUNT_1;
            slot.texture = SDL_CreateGPUTexture(device_, &ti);
            if (!slot.texture) fail("SDL_CreateGPUTexture (raster) failed");
            slot.width  = gc.width;
            slot.height = gc.height;
            slot.format = gc.format;
            // The pooled transfer is sized to the old dimensions — drop it so it is recreated at the new
            // size, and clear the skip state so the fresh texture is never skipped while it holds nothing.
            if (slot.transfer) { SDL_ReleaseGPUTransferBuffer(device_, slot.transfer); slot.transfer = nullptr; }
            slot.uploadedGeneration = 0;  // a fresh texture holds nothing, whatever it held before
        }

        // The only question worth asking: is the raster already on the GPU the one being submitted. A
        // generation the slot has not seen means the source drew; the same one means it did not, and
        // what is resident is still the raster. Generation 0 is a source that has finished nothing,
        // which no slot can be holding — so a first submission always uploads.
        if (gc.generation != 0 && gc.generation == slot.uploadedGeneration) {
            ++renderStats_.rasterSkips;
            continue;
        }

        if (!slot.transfer) {
            SDL_GPUTransferBufferCreateInfo tbInfo{};
            tbInfo.usage  = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            tbInfo.size   = static_cast<Uint32>(needed);
            slot.transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
            if (!slot.transfer) fail("SDL_CreateGPUTransferBuffer (raster) failed");
        }

        // cycle=true: the pooled buffer may still be in-flight from a prior frame's copy.
        void* dst = SDL_MapGPUTransferBuffer(device_, slot.transfer, true);
        if (!dst) fail("SDL_MapGPUTransferBuffer (raster) failed");
        std::memcpy(dst, gc.pixels.data(), needed);
        SDL_UnmapGPUTransferBuffer(device_, slot.transfer);

        if (!copy) copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = slot.transfer;
        src.offset          = 0;
        src.pixels_per_row  = static_cast<Uint32>(gc.width);
        src.rows_per_layer  = static_cast<Uint32>(gc.height);
        SDL_GPUTextureRegion region{};
        region.texture = slot.texture;
        region.w       = static_cast<Uint32>(gc.width);
        region.h       = static_cast<Uint32>(gc.height);
        region.d       = 1;
        SDL_UploadToGPUTexture(copy, &src, &region, false);
        slot.uploadedGeneration = gc.generation;
        ++renderStats_.rasterUploads;
        // slot.transfer is pooled — NOT pushed to scratch.transfers (that list is per-frame release).
    }

    // Upload `byteCount` bytes of instance records to the given run-pool slot, growing the slot on demand,
    // and stage the transfer in `scratch`. Returns the pool buffer — each run draws from its own buffer with
    // first_instance 0 (the region_batch precedent), so the vertex stage needs no base-instance uniform. Same
    // copy pass as the single-buffer sprite uploads. Byte-oriented because the pool serves two record types:
    // GpuSprite runs and the emission extract's own rects.
    auto uploadRunBytes = [&](int slot, const void* data, std::size_t byteCount) -> SDL_GPUBuffer* {
        if (static_cast<int>(spriteRunBufs_.size()) <= slot) {
            spriteRunBufs_.resize(static_cast<std::size_t>(slot) + 1, nullptr);
            spriteRunCaps_.resize(static_cast<std::size_t>(slot) + 1, 0);
        }
        const int      need = static_cast<int>(byteCount);
        SDL_GPUBuffer*& buf = spriteRunBufs_[static_cast<std::size_t>(slot)];
        if (!buf || spriteRunCaps_[static_cast<std::size_t>(slot)] < need) {  // grow-on-demand
            if (buf) SDL_ReleaseGPUBuffer(device_, buf);
            SDL_GPUBufferCreateInfo bi{};
            bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
            bi.size  = static_cast<Uint32>(need);
            buf = SDL_CreateGPUBuffer(device_, &bi);
            if (!buf) fail("SDL_CreateGPUBuffer (sprite run) failed");
            spriteRunCaps_[static_cast<std::size_t>(slot)] = need;
        }
        const Uint32 bytes = static_cast<Uint32>(need);
        SDL_GPUTransferBufferCreateInfo tbInfo{};
        tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbInfo.size  = bytes;
        SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
        if (!transfer) fail("SDL_CreateGPUTransferBuffer (sprite run) failed");
        void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
        if (!mapped) fail("SDL_MapGPUTransferBuffer (sprite run) failed");
        std::memcpy(mapped, data, bytes);
        SDL_UnmapGPUTransferBuffer(device_, transfer);
        if (!copy) copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation srcLoc{};
        srcLoc.transfer_buffer = transfer;
        srcLoc.offset          = 0;
        SDL_GPUBufferRegion dstRegion{};
        dstRegion.buffer = buf;
        dstRegion.offset = 0;
        dstRegion.size   = bytes;
        SDL_UploadToGPUBuffer(copy, &srcLoc, &dstRegion, false);
        ++renderStats_.spriteUploads;
        scratch.transfers.push_back(transfer);
        return buf;
    };

    // `count` GpuSprite records through the same pool.
    auto uploadSpriteRun = [&](int slot, const GpuSprite* data, int count) -> SDL_GPUBuffer* {
        return uploadRunBytes(slot, data, static_cast<std::size_t>(count) * sizeof(GpuSprite));
    };

    // Resolve a sprite's inline Custom effects: pack each chosen-shader Custom chain step's params into its
    // record, neutralize every other Custom step (a different shader, an unroutable one, or an over-budget
    // cbuffer) to a no-op with a visible warning, and return the sprite's pipeline key — handle+1 for the
    // first routable custom shader, 0 for none. `fx` is buildSpriteFxRecords(s); its leading records are the
    // chain steps in `s.effects` order, so the walk pairs each non-None chain effect with its record.
    auto resolveSpriteInlineCustom = [&](const Sprite& s, std::vector<SpriteFxRecord>& fx) -> int {
        std::optional<PostProcessStageId> chosen;   // the first routable custom — the sprite's one pipeline
        for (const ScreenSpaceEffect& e : s.effects) {
            if (e.kind != ScreenSpaceEffectKind::Custom) continue;
            const std::size_t hid = static_cast<std::size_t>(e.customShader);
            if (hid < customSprite_.size() && customSprite_[hid]) { chosen = e.customShader; break; }
        }
        std::size_t chainIdx = 0;   // fx's leading records are the non-None chain effects, in order
        for (const ScreenSpaceEffect& e : s.effects) {
            if (e.kind == ScreenSpaceEffectKind::None) continue;
            if (chainIdx >= fx.size()) break;
            if (e.kind == ScreenSpaceEffectKind::Custom) {
                bool kept = false;
                if (chosen && e.customShader == *chosen) {
                    const std::size_t hid = static_cast<std::size_t>(e.customShader);
                    std::byte buf[kSpriteFxCustomParamBytes];
                    const std::uint32_t n = customPackers_[hid] ? customPackers_[hid](e, buf) : 0u;
                    if (n <= kSpriteFxCustomParamBytes) {
                        writeSpriteFxCustomParams(fx[chainIdx], std::span<const std::byte>(buf, n));
                        kept = true;
                    } else {
                        SDL_Log("retropp: sprite '%s' custom effect cbuffer (%u bytes) exceeds the sprite "
                                "inline budget (%zu bytes) — the effect is skipped",
                                std::string(s.key).c_str(), n, kSpriteFxCustomParamBytes);
                    }
                } else {
                    SDL_Log("retropp: sprite '%s' carries a Custom effect it can't run inline (a second distinct "
                            "custom shader, or one with no sprite variant) — the effect is skipped",
                            std::string(s.key).c_str());
                }
                if (!kept) fx[chainIdx].kind = static_cast<std::uint32_t>(ScreenSpaceEffectKind::None);
            }
            ++chainIdx;
        }
        return chosen ? static_cast<int>(static_cast<std::size_t>(*chosen)) + 1 : 0;
    };

    // Resolve a Below-scope sprite's records + pipeline key. A Below-scope Custom effect routes through a
    // scene-read variant (customSpriteBelow_[handle]) if the handle has one: the sprite's below record is that
    // one custom step (its params packed into the idle lanes), and the returned key is handle+1. The custom
    // body produces the whole graded scene, so any co-resident built-in Below steps are ignored (a visible
    // warning). With no routable Custom, the built-in path (key 0) packs the built-in Below kinds (the
    // whole-silhouette chain plus any region records). static kNoShape: a whole-silhouette custom step carries no shape.
    auto resolveSpriteBelowCustom = [&](const Sprite& s) -> std::pair<std::vector<SpriteFxRecord>, int> {
        static const ShapePoints kNoShape{};
        if (const std::optional<PostProcessStageId> cs = spriteBelowInlineCustomShader(s)) {
            const std::size_t hid = static_cast<std::size_t>(*cs);
            if (hid < customSpriteBelow_.size() && customSpriteBelow_[hid]) {
                const ScreenSpaceEffect* custom = nullptr;
                for (const ScreenSpaceEffect& e : s.effects)
                    if (e.kind == ScreenSpaceEffectKind::Custom && effectIsBelowScope(e) && e.customShader == *cs) {
                        custom = &e;
                        break;
                    }
                if (custom) {
                    std::byte buf[kSpriteFxCustomParamBytes];
                    const std::uint32_t n = customPackers_[hid] ? customPackers_[hid](*custom, buf) : 0u;
                    if (n <= kSpriteFxCustomParamBytes) {
                        SpriteFxRecord rec =
                            packSpriteFxRecord(*custom, /*isRegion=*/false, kNoShape, 1.0f, BlendMode::Normal);
                        writeSpriteFxCustomParams(rec, std::span<const std::byte>(buf, n));
                        if (!buildSpriteBelowRecords(s).empty())
                            SDL_Log("retropp: sprite '%s' Below-scope lens runs a custom shader (it produces the "
                                    "whole scene) — its co-resident built-in Below effects are ignored",
                                    std::string(s.key).c_str());
                        return {std::vector<SpriteFxRecord>{rec}, static_cast<int>(hid) + 1};
                    }
                    SDL_Log("retropp: sprite '%s' Below-scope custom effect cbuffer (%u bytes) exceeds the sprite "
                            "inline budget (%zu bytes) — the effect is skipped",
                            std::string(s.key).c_str(), n, kSpriteFxCustomParamBytes);
                }
            } else {
                SDL_Log("retropp: sprite '%s' carries a Below-scope Custom effect with no scene-read variant "
                        "(the shader is // @retropp:no-sprite or carries an int / uint param) — the effect is skipped",
                        std::string(s.key).c_str());
            }
        }
        return {buildSpriteBelowRecords(s), 0};
    };

    // Build + upload each SPRITES layer's GpuSprite storage buffer (each sprite carries its own atlas +
    // palette handle, baked into the record). Grow-only: the buffer is recreated only when this frame's
    // sprite count exceeds the slot's capacity; otherwise it is reused and overwritten in place.
    for (const std::size_t idx : order) {
        const DrawLayer& layer = frame.layers[idx];
        if (contentKind(layer.content) != LayerContentKind::Sprites) continue;
        const SpriteContent& sc = std::get<SpriteContent>(layer.content);
        const int spriteCount = static_cast<int>(sc.sprites.size());
        if (spriteCount <= 0) continue;
        const std::size_t fxBase = fxStore.size();  // this layer's effect-record slice starts here

        // Placement is the eased float on the interpolation path (sub-pixel on the output grid), falling
        // back to the submission's integer position for an id with no history. The layer scroll eases by
        // the layer id, each sprite by its own id. The sprite clip is baked against the VIEWPORT (clip
        // space is resolution-independent); the output-res target rasterizes the same quad finer, and a
        // fractional position lands it on a different output pixel — smooth motion, crisp texels.
        float lscrollX = static_cast<float>(layer.scroll.x);
        float lscrollY = static_cast<float>(layer.scroll.y);
        if (interpolate) {
            if (const auto ls = interp_.interpolatedLayerScroll(layer.key, timing)) {
                lscrollX = ls->x;
                lscrollY = ls->y;
            }
        }
        // Records draw in instance order, so emit them in spriteDrawOrder — ascending Sprite::z,
        // equal z keeping submission order (stable). Each record names its own atlas + palette, so
        // reordering costs nothing downstream.
        const std::vector<std::size_t> spriteOrder = spriteDrawOrder(sc.sprites);
        std::vector<GpuSprite> records;    // the art-drawing (non-lens) sprites, in draw order
        records.reserve(static_cast<std::size_t>(spriteCount));
        // Per-order-position sprite pipeline key: 0 = the stock sprite pipeline; handle+1 = the custom
        // sprite-inline pipeline the sprite draws through (its first routable Custom effect). Drives the
        // run-split so custom sprites draw through their own pipeline while non-custom sprites stay on the
        // stock single-draw fast path — pass count tracks the authored pipeline mix, never the sprite count.
        std::vector<int> pipelineKeys;
        pipelineKeys.reserve(static_cast<std::size_t>(spriteCount));
        // The sprite indices whose art actually draws — spriteOrder minus the Below-scope lenses (a lens's
        // art is a coverage mask, not drawn). Parallel to `records` / `pipelineKeys`; drives spriteBlendRuns.
        std::vector<std::size_t> artOrder;
        artOrder.reserve(static_cast<std::size_t>(spriteCount));
        // The layer's Below-scope lenses (their placed geometry, pointing at their Below effect records), in
        // draw order with a parallel pipeline key — the built-in below fragment (key 0) or a scene-read custom
        // variant (key = handle + 1). groupSpriteBelowRuns splits them into contiguous same-pipeline runs (each
        // one instanced pass, drawn BEFORE the layer's art over the accumulator); the pass count tracks the
        // below-pipeline mix, never the sprite count.
        std::vector<GpuSprite> belowSprites;
        std::vector<int>       belowKeys;
        // The layer's Layer-scope Bloom / Glow steps: one entry per realized step (a sprite with two glows
        // contributes two), each a COPY of that sprite's art record carrying the emission step's index in
        // its flags word. Parallel arrays — the steps drive the bucketing, the records are what each bucket
        // draws.
        std::vector<GpuSprite>          emitSprites;
        std::vector<SpriteEmissionStep> emitSteps;
        // What this layer's region Blooms ask the atlas for. Collection has to finish before any record can
        // learn where its field landed, because a plan spans the whole layer.
        PendingLayerEmission& pending = layerEmissionPlans[idx];
        for (const std::size_t si : spriteOrder) {
            const Sprite& s = sc.sprites[si];
            float px = static_cast<float>(s.x);
            float py = static_cast<float>(s.y);
            if (interpolate) {
                if (const auto p = interp_.interpolatedSpritePos(s.key, timing)) {
                    px = p->x;
                    py = p->y;
                }
            }
            const GpuSprite gs = makeGpuSprite(s, viewport_.width, viewport_.height,
                                               px, py, lscrollX, lscrollY, layer.transform, evaluationGrid_);

            // A Below-scope sprite is a pure LENS: its art alpha is the silhouette coverage / lens strength
            // (read by the below-sprite shader), and its art is NOT drawn on top — so an opaque mask gives a
            // full-strength refraction with no self-occlusion. It draws only through its below run (the
            // scene-reading pipeline into a scratch composited over the accumulator before this layer's art).
            // resolveSpriteBelowCustom builds the run's records + pipeline key (a Below-scope Custom routes
            // through a scene-read variant; built-in kinds through spriteBelow_). A lens with Layer-scope
            // effects warns (its art doesn't draw, so those are ignored — use a separate sprite for visible art).
            if (spriteHasBelowEffects(s)) {
                // No-silent-skip for the below-region cases buildSpriteBelowRecords drops: a curve boundary
                // (unsupported), and a displacing / Custom kind inside a region (each runs whole-silhouette).
                for (const Region& rg : s.regions) {
                    bool anyBelow = false;
                    for (const ScreenSpaceEffect& re : rg.effects)
                        if (re.kind != ScreenSpaceEffectKind::None && effectIsBelowScope(re)) anyBelow = true;
                    if (!anyBelow) continue;
                    if (!spriteRegionShapeSupported(rg.shape))
                        SDL_Log("retropp: sprite '%s' Below region '%s' has a curve boundary; region curve "
                                "shapes are not supported — the region is skipped",
                                std::string(s.key).c_str(), std::string(rg.key).c_str());
                    for (const ScreenSpaceEffect& re : rg.effects)
                        if (effectIsBelowScope(re) &&
                            (re.kind == ScreenSpaceEffectKind::Custom ||
                             re.kind == ScreenSpaceEffectKind::RowDisplacement ||
                             re.kind == ScreenSpaceEffectKind::Ripple ||
                             re.kind == ScreenSpaceEffectKind::Swirl ||
                             re.kind == ScreenSpaceEffectKind::Glow))
                            SDL_Log("retropp: sprite '%s' Below region '%s' carries a displacing, custom, or "
                                    "glow effect; those run whole-silhouette, not confined to a region — skipped",
                                    std::string(s.key).c_str(), std::string(rg.key).c_str());
                }
                if (spriteHasLayerEffects(s))
                    SDL_Log("retropp: sprite '%s' is a Below-scope lens (its art is the coverage mask, not "
                            "drawn); its Layer-scope effects are ignored — use a separate sprite for art",
                            std::string(s.key).c_str());
                auto [bfx, belowKey] = resolveSpriteBelowCustom(s);
                if (!bfx.empty()) {
                    // A Bloom / Glow lens reads a halo the renderer extracts from the scene, and it reads it
                    // inside its own silhouette — so the field it needs is this lens's drawn footprint.
                    // Asking happens BEFORE the records go to the store, because the demand names the record
                    // it will point at; the answer comes once the whole layer has asked.
                    const PixelBox footprint =
                        spriteFootprint(gs, viewport_.width, viewport_.height);
                    const std::vector<SpriteBelowEmissionDemand> askedBelow =
                        collectSpriteBelowEmissionDemands(bfx, fxStore.size(), footprint);
                    pending.belowDemands.insert(pending.belowDemands.end(), askedBelow.begin(),
                                                askedBelow.end());
                    // An emission-declared Below-scope Custom lens (routed through its scene-read variant,
                    // belowKey = handle + 1) contributes its own demand — always engaged (the declaration IS the
                    // demand). The record's own param lanes ARE its cbuffer, so the demand carries the reach and
                    // threshold from the EFFECT, and the seat writes the field row into the record's gate lanes.
                    if (belowKey > 0) {
                        const std::size_t cid = static_cast<std::size_t>(belowKey - 1);
                        if (cid < customEmissionConsumer_.size() && customEmissionConsumer_[cid]) {
                            const ScreenSpaceEffect* ce = nullptr;
                            for (const ScreenSpaceEffect& e : s.effects)
                                if (e.kind == ScreenSpaceEffectKind::Custom && effectIsBelowScope(e) &&
                                    static_cast<std::size_t>(e.customShader) == cid) {
                                    ce = &e;
                                    break;
                                }
                            if (ce) {
                                const bool hasBody =
                                    cid < customEmissionRect_.size() && customEmissionRect_[cid] != nullptr;
                                if (const std::optional<SpriteBelowEmissionDemand> d =
                                        collectSpriteBelowCustomEmissionDemand(
                                            *ce, static_cast<std::uint32_t>(cid), hasBody, fxStore.size(),
                                            footprint))
                                    pending.belowDemands.push_back(*d);
                            }
                        }
                    }
                    GpuSprite bs = gs;
                    bs.fxOffset  = static_cast<std::uint32_t>(fxStore.size());
                    bs.fxCount   = static_cast<std::uint32_t>(bfx.size());
                    fxStore.insert(fxStore.end(), bfx.begin(), bfx.end());
                    belowSprites.push_back(bs);
                    belowKeys.push_back(belowKey);
                }
                continue;  // a lens draws no art
            }

            records.push_back(gs);
            // Flatten this sprite's LAYER-scope effects chain + regions into the frame-wide store and point
            // the record at its slice. A curve-boundary sprite region, and a region-confined Custom effect,
            // have no inline evaluation on the sprite path — they are skipped with a warning (visible, not
            // silent).
            int pipelineKey = 0;
            if (spriteHasLayerEffects(s)) {
                for (const Region& rg : s.regions) {
                    if (!spriteRegionShapeSupported(rg.shape))
                        SDL_Log("retropp: sprite '%s' region '%s' has a curve boundary; sprite-region curve "
                                "shapes are not supported inline — the region is skipped",
                                std::string(s.key).c_str(), std::string(rg.key).c_str());
                    for (const ScreenSpaceEffect& re : rg.effects) {
                        if (re.kind == ScreenSpaceEffectKind::Custom && !effectIsBelowScope(re))
                            SDL_Log("retropp: sprite '%s' region '%s' carries a Custom effect; a custom shader "
                                    "runs whole-silhouette on a sprite, not confined to a region — skipped",
                                    std::string(s.key).c_str(), std::string(rg.key).c_str());
                        if (re.kind == ScreenSpaceEffectKind::Glow && !effectIsBelowScope(re))
                            SDL_Log("retropp: sprite '%s' region '%s' carries a Glow effect; a glow's tint "
                                    "needs the record lanes a region's shape occupies, so a sprite Glow runs "
                                    "whole-silhouette only — skipped",
                                    std::string(s.key).c_str(), std::string(rg.key).c_str());
                        if ((re.kind == ScreenSpaceEffectKind::RowDisplacement ||
                             re.kind == ScreenSpaceEffectKind::Ripple ||
                             re.kind == ScreenSpaceEffectKind::Swirl) && !effectIsBelowScope(re))
                            SDL_Log("retropp: sprite '%s' region '%s' carries a displacing effect; a "
                                    "displacing kind re-reads the whole silhouette, not a confined region "
                                    "— skipped",
                                    std::string(s.key).c_str(), std::string(rg.key).c_str());
                    }
                }
                std::vector<SpriteFxRecord> fx = buildSpriteFxRecords(s);
                pipelineKey = resolveSpriteInlineCustom(s, fx);

                // A Layer-scope emission-consumer Custom chain step joins the emission grammar: it reads its
                // own blurred silhouette back through sampleEmission(). A Custom step has no emission variant,
                // so the field's content — the art brightpass at the effect's threshold (decision 3: the art
                // path has no scene to hand the stage's emission() body) — is produced by a SYNTHETIC Bloom
                // record appended PAST the sprite's chain slice, outside the art record's fxCount so the art
                // fragment never walks it. The raster instance names that record and reaches it with an
                // extended fxCount; the field row seats into the Custom record's gate lanes; the injected
                // sampleEmission maps the body's quad uv to compose px to read it. Only the chosen inline
                // shader runs, so only its steps ask, and the walk mirrors buildSpriteFxRecords' filter (None
                // and Below-scope skipped) so chainIdx indexes the same chain record resolveSpriteInlineCustom
                // packed.
                const std::size_t realFxCount = fx.size();  // the art chain + regions; synthetics append past it
                std::vector<SpriteRegionEmissionDemand> customAsked;
                if (pipelineKey > 0) {
                    const std::size_t chosen = static_cast<std::size_t>(pipelineKey - 1);
                    if (chosen < customEmissionConsumer_.size() && customEmissionConsumer_[chosen]) {
                        const PixelBox footprint =
                            spriteFootprint(records.back(), viewport_.width, viewport_.height);
                        const float linear = spriteLinearScale(s, layer.transform);
                        std::size_t chainIdx = 0;
                        for (const ScreenSpaceEffect& e : s.effects) {
                            if (e.kind == ScreenSpaceEffectKind::None) continue;
                            if (effectIsBelowScope(e)) continue;
                            if (chainIdx >= realFxCount) break;
                            if (e.kind == ScreenSpaceEffectKind::Custom &&
                                static_cast<std::size_t>(e.customShader) == chosen &&
                                fx[chainIdx].kind == static_cast<std::uint32_t>(ScreenSpaceEffectKind::Custom)) {
                                const std::size_t synthIdx = fx.size();
                                fx.push_back(syntheticLayerEmissionRecord(e));
                                if (const std::optional<SpriteRegionEmissionDemand> d =
                                        collectSpriteLayerCustomEmissionDemand(
                                            e, linear, synthIdx, fxStore.size() + chainIdx, footprint))
                                    customAsked.push_back(*d);
                            }
                            ++chainIdx;
                        }
                    }
                }

                // A Bloom inside a region reads a field of its own, sized to the pixels that read it — the
                // sprite's drawn footprint. Asking happens BEFORE the records go to the store, because the
                // demand names the record it will point at; the answer comes once the whole layer has asked.
                // The synthetic records appended above are non-region, so this collector passes over them.
                const std::vector<SpriteRegionEmissionDemand> asked = collectSpriteRegionEmissionDemands(
                    fx, spriteLinearScale(s, layer.transform), fxStore.size(),
                    spriteFootprint(records.back(), viewport_.width, viewport_.height));
                if (!fx.empty()) {
                    records.back().fxOffset = static_cast<std::uint32_t>(fxStore.size());
                    records.back().fxCount  = static_cast<std::uint32_t>(realFxCount);  // excludes the synthetics
                    fxStore.insert(fxStore.end(), fx.begin(), fx.end());

                    // One emission instance per demand: a copy of the sprite's art record naming that step
                    // in its flags word. Its light is read INSIDE the art fragment, which applies the layer
                    // and per-sprite alpha to the composed pixel afterwards, so the raster runs at neutral
                    // opacity (uniform alpha 1 below, record alpha 1 here) — the chain path's buckets
                    // composite separately and do carry both.
                    for (const SpriteRegionEmissionDemand& d : asked) {
                        GpuSprite es = records.back();
                        es.flags |= (static_cast<std::uint32_t>(d.recordIndex) & kSpriteEmissionIndexMask)
                                    << kSpriteEmissionIndexShift;
                        es.row0[3] = 1.0f;
                        pending.demands.push_back(d);
                        pending.instances.push_back(es);
                    }

                    // A Layer-scope Custom demand rasters the same way — a copy of the art record naming its
                    // SYNTHETIC Bloom record (appended past the chain slice) in the flags word — but the raster
                    // instance's fxCount must reach that record, so it is extended to the full store size while
                    // the art record above kept the shorter realFxCount. The field content is the art
                    // brightpass; the custom body reads it back in the art fragment.
                    for (const SpriteRegionEmissionDemand& d : customAsked) {
                        GpuSprite es = records.back();
                        es.flags |= (static_cast<std::uint32_t>(d.recordIndex) & kSpriteEmissionIndexMask)
                                    << kSpriteEmissionIndexShift;
                        es.fxCount = static_cast<std::uint32_t>(fx.size());  // reach the synthetic record
                        es.row0[3] = 1.0f;
                        pending.demands.push_back(d);
                        pending.instances.push_back(es);
                    }

                    // A Layer-scope Bloom / Glow does not run in the art chain; it rasters its emission
                    // separately. The reach is authored in the sprite's own art pixels and blurs in viewport
                    // pixels, so it maps through the sprite's linear scale before bucketing.
                    const std::size_t emitBefore = emitSteps.size();
                    for (const SpriteEmissionStep& st :
                         spriteEmissionSteps(s, spriteLinearScale(s, layer.transform))) {
                        GpuSprite es = records.back();
                        es.flags |= (static_cast<std::uint32_t>(st.recordIndex) & kSpriteEmissionIndexMask)
                                    << kSpriteEmissionIndexShift;
                        emitSprites.push_back(es);
                        emitSteps.push_back(st);
                    }
                    // A whole-silhouette Bloom / Glow authored alongside a Custom step radiates the colour as
                    // it stood BEFORE the custom shader ran — the emission raster has no variant for a Custom
                    // step. That is a real limitation for an UNDECLARED custom (no emission pass at all); an
                    // emission-DECLARED custom joins the emission grammar by construction, so the message does
                    // not apply to it and the warning is suppressed. Visible, not silent, for the case it fits.
                    if (emitSteps.size() > emitBefore) {
                        const std::optional<PostProcessStageId> ics = spriteInlineCustomShader(s);
                        const bool consumer =
                            ics && static_cast<std::size_t>(*ics) < customEmissionConsumer_.size() &&
                            customEmissionConsumer_[static_cast<std::size_t>(*ics)] != 0;
                        if (spriteCustomShadowsSiblingHalo(ics.has_value(), consumer))
                            SDL_Log("retropp: sprite '%s' carries a Custom step alongside a Bloom / Glow; a "
                                    "custom shader has no emission pass, so the halo radiates the colour from "
                                    "before it",
                                    std::string(s.key).c_str());
                    }
                }
            }
            pipelineKeys.push_back(pipelineKey);
            artOrder.push_back(si);
        }
        // Every sprite has asked, so the layer can be planned — BOTH paths in one plan, region fields first
        // and below fields after. A demand's index within that concatenation, offset by where the layer's
        // fields begin in the frame's table, IS the field index its record carries — one number addressing
        // the placements and the rect table alike. Both kinds measure their reach in viewport pixels, so a
        // page mixes them freely: the blur does not care what filled a rect.
        const std::size_t regionCount = pending.demands.size();
        // The i-th field of the concatenation, and the reach it blurs at.
        auto fieldAt = [&](std::size_t i) -> const EmissionDemand& {
            return i < regionCount ? pending.demands[i].field : pending.belowDemands[i - regionCount].field;
        };
        auto reachAt = [&](std::size_t i) {
            return i < regionCount ? pending.demands[i].blurReach
                                   : pending.belowDemands[i - regionCount].blurReach;
        };
        if (pending.total() != 0) {
            pending.placements.assign(pending.total(), EmissionPlacement{});
            pending.sheetOf.assign(pending.total(), -1);

            // Plan onto sheets until nothing is left over. Each pass packs what it can; the leftovers go to
            // the next sheet, which is the next array layer of the store. That is what keeps the field count
            // off the ceiling — no sheet's area is a limit on the layer, only on that sheet.
            //
            // Within a sheet, plan against the smallest ceiling that holds its content. The packer only
            // widens when a width fails to fit, so handed the full ceiling outright it produces a tall
            // ribbon — 240 fields land in 256×4096, four times the memory of the square holding the same
            // content. Raising the ceiling in steps makes it widen instead.
            std::vector<std::size_t> pendingIdx(pending.total());
            for (std::size_t i = 0; i < pendingIdx.size(); ++i) pendingIdx[i] = i;
            while (!pendingIdx.empty()) {
                std::vector<EmissionDemand> fields;
                fields.reserve(pendingIdx.size());
                for (const std::size_t i : pendingIdx) fields.push_back(fieldAt(i));
                EmissionAtlasPlan plan;
                for (int ceiling = 256;; ceiling <<= 1) {
                    plan = planEmissionAtlas(std::span<const EmissionDemand>{fields}, ceiling);
                    if (plan.dropped == 0 || ceiling >= kEmissionAtlasMaxSize) break;
                }
                const int sheet = static_cast<int>(pending.sheets.size());
                std::vector<std::size_t> leftover;
                for (std::size_t k = 0; k < pendingIdx.size(); ++k) {
                    if (plan.placements[k].page < 0) { leftover.push_back(pendingIdx[k]); continue; }
                    pending.placements[pendingIdx[k]] = plan.placements[k];
                    pending.sheetOf[pendingIdx[k]]    = sheet;
                }
                if (leftover.size() == pendingIdx.size()) {
                    // Nothing fitted, so another sheet would not help either: each of these fields is larger
                    // on its own than a whole sheet. Reported by COUNT, not per frame — a scene in this state
                    // is in it every frame, and a per-frame line would bury everything else in the console.
                    if (static_cast<int>(leftover.size()) != emissionDropReported_) {
                        emissionDropReported_ = static_cast<int>(leftover.size());
                        SDL_Log("retropp: layer '%s' has %zu emission field(s) larger than a %d-square "
                                "sheet on their own; they add no light",
                                std::string(layer.key).c_str(), leftover.size(), kEmissionAtlasMaxSize);
                    }
                    break;
                }
                pending.sheets.push_back(plan);
                emissionAtlasWant  = std::max(emissionAtlasWant, plan.width);
                emissionAtlasWantH = std::max(emissionAtlasWantH, plan.height);
                pendingIdx = std::move(leftover);
            }

            pending.fieldBase = static_cast<int>(emissionRectRows.size());
            // Each path seats its own half of the concatenation, over its own slice of `sheetOf` and from its
            // own base — the below half starts where the region half ends, which is what makes one index
            // space out of two demand lists.
            const std::span<const int> sheetOf{pending.sheetOf};
            const int droppedRegion = seatPlacedRegionEmissionFields(fxStore, pending.demands,
                                                                     sheetOf.first(regionCount),
                                                                     pending.fieldBase);
            const int droppedBelow  = seatPlacedBelowEmissionFields(
                fxStore, pending.belowDemands, sheetOf.subspan(regionCount),
                pending.fieldBase + static_cast<int>(regionCount));
            if (droppedRegion + droppedBelow == 0) emissionDropReported_ = 0;

            // One table row per demand, placed or not, so a field index addresses the same row it was placed
            // in. An unplaced field's row stays zero; its record carries no strength and never reads it.
            for (std::size_t i = 0; i < pending.total(); ++i)
                emissionRectRows.push_back(pending.sheetOf[i] >= 0
                                               ? emissionRectEntry(fieldAt(i), pending.placements[i],
                                                                   pending.sheetOf[i])
                                               : EmissionRectEntry{});

            // The reach each sheet's pages blur at, taken from a member rather than recovered from the page's
            // own spread: the spread is padded to cover the blur's rounding, and the kernel wants the reach.
            std::vector<EmissionSheetGpu>& sheets = layerEmissionSheets[idx];
            sheets.resize(pending.sheets.size());
            for (std::size_t sh = 0; sh < pending.sheets.size(); ++sh) {
                sheets[sh].pages = pending.sheets[sh].pages;
                sheets[sh].pageReach.assign(pending.sheets[sh].pages.size(), 0.0f);
            }
            for (std::size_t i = 0; i < pending.total(); ++i) {
                const int sh = pending.sheetOf[i];
                if (sh < 0) continue;
                sheets[static_cast<std::size_t>(sh)].pageReach[static_cast<std::size_t>(
                    pending.placements[i].page)] = reachAt(i);
            }
        }
        // Group the layer's emission steps by (kind, reach) and upload one instance buffer per bucket.
        for (const SpriteEmissionBucket& b : groupSpriteEmissionBuckets(emitSteps)) {
            std::vector<GpuSprite> members;
            members.reserve(b.members.size());
            for (const std::size_t m : b.members) members.push_back(emitSprites[m]);
            SDL_GPUBuffer* buf = uploadSpriteRun(spriteRunSlot++, members.data(),
                                                 static_cast<int>(members.size()));
            spriteEmissionRuns[idx].push_back(
                SpriteEmissionRunGpu{buf, static_cast<int>(members.size()), b.glow, b.reach});
        }
        for (const SpriteBelowRun& run : groupSpriteBelowRuns(belowKeys)) {
            SDL_GPUBuffer* buf = uploadSpriteRun(spriteRunSlot++, belowSprites.data() + run.first, run.count);
            spriteBelowRuns[idx].push_back(SpriteBelowRunGpu{buf, run.count, run.pipelineKey});
        }
        const int artCount = static_cast<int>(records.size());
        if (artCount == 0) {          // a layer of only lenses — the below run is the whole layer, no art draw
            SpriteBuf& slot = spriteCacheSlot(layer.key);
            spriteSlot[idx] = &slot;
            slot.count = 0;           // clear any stale count so drawLayer issues no instances
            continue;
        }

        // Split the draw order into contiguous same-blend runs. An all-Normal layer (one Normal run
        // spanning everything) takes the single-buffer fast path below — byte-identical. A mixed layer
        // uploads each run to its own pool buffer; the compose loop draws Normal runs straight into the
        // container and grades non-Normal runs onto it. Records are already in draw order, so a run's slice
        // is contiguous and its within-layer z is exact.
        const std::vector<SpriteBlendRun> runs = spriteBlendRuns(sc.sprites, artOrder, pipelineKeys);
        if (runs.size() > 1 || runs[0].blend != BlendMode::Normal || runs[0].pipelineKey != 0) {
            std::vector<SpriteRunGpu>& outRuns = spriteLayerRuns[idx];
            outRuns.reserve(runs.size());
            for (const SpriteBlendRun& run : runs) {
                SDL_GPUBuffer* buf = uploadSpriteRun(spriteRunSlot++, records.data() + run.start,
                                                     static_cast<int>(run.count));
                outRuns.push_back(SpriteRunGpu{buf, static_cast<int>(run.count), run.blend, run.pipelineKey});
            }
            continue;  // a mixed / custom-pipeline layer is drawn from its per-run buffers, not the single slot
        }

        SpriteBuf& slot = spriteCacheSlot(layer.key);
        spriteSlot[idx] = &slot;
        slot.count = artCount;   // the instance count drawLayer issues (lenses excluded)
        if (!slot.buffer || slot.capacity < artCount) {
            if (slot.buffer) SDL_ReleaseGPUBuffer(device_, slot.buffer);
            SDL_GPUBufferCreateInfo bi{};
            bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
            bi.size  = static_cast<Uint32>(artCount) * static_cast<Uint32>(sizeof(GpuSprite));
            slot.buffer = SDL_CreateGPUBuffer(device_, &bi);
            if (!slot.buffer) fail("SDL_CreateGPUBuffer (sprite) failed");
            slot.capacity = artCount;
            // The pooled transfer is sized to the old capacity — drop it so it is recreated at the new one,
            // and clear the signature so the fresh buffer is never skipped against stale state.
            if (slot.transfer) { SDL_ReleaseGPUTransferBuffer(device_, slot.transfer); slot.transfer = nullptr; }
            slot.hashed = false;
        }

        const Uint32 bytes = static_cast<Uint32>(artCount) * static_cast<Uint32>(sizeof(GpuSprite));
        // Skip the upload when the buffer already holds these exact bytes. The records are the buffer's whole
        // content, and the layer's effect-record slice folds in after them so a param mutation that leaves the
        // records identical still re-uploads. The CPU build above always runs — eased positions must be
        // evaluated to know whether they changed — so the skip saves the transfer create/map/memcpy/DMA, not
        // the build. Moving sprites' records genuinely differ every frame and never skip; settled layers (a
        // HUD, an idle overlay) are the win.
        std::uint64_t recordHash = foldBytes64(records.data(), bytes, kFnv64Offset);
        recordHash = foldBytes64(fxStore.data() + fxBase, (fxStore.size() - fxBase) * sizeof(SpriteFxRecord),
                                 recordHash);
        if (slot.hashed && slot.contentCount == artCount && slot.contentHash == recordHash) {
            ++renderStats_.spriteSkips;
            continue;  // no transfer, no map, no DMA, no copy-pass entry
        }
        slot.hashed       = true;
        slot.contentCount = artCount;
        slot.contentHash  = recordHash;

        if (!slot.transfer) {
            // Pooled per-slot transfer buffer, sized to the slot's record capacity — created once (recreated
            // after a grow released it above) and reused every subsequent frame.
            SDL_GPUTransferBufferCreateInfo tbInfo{};
            tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            tbInfo.size  = static_cast<Uint32>(slot.capacity) * static_cast<Uint32>(sizeof(GpuSprite));
            slot.transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
            if (!slot.transfer) fail("SDL_CreateGPUTransferBuffer (sprite) failed");
        }

        // cycle=true: the pooled buffer may still be in-flight from a prior frame's copy; SDL cycles the backing.
        void* mapped = SDL_MapGPUTransferBuffer(device_, slot.transfer, true);
        if (!mapped) fail("SDL_MapGPUTransferBuffer (sprite) failed");
        std::memcpy(mapped, records.data(), bytes);
        SDL_UnmapGPUTransferBuffer(device_, slot.transfer);

        if (!copy) copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation srcLoc{};
        srcLoc.transfer_buffer = slot.transfer;
        srcLoc.offset          = 0;
        SDL_GPUBufferRegion dstRegion{};
        dstRegion.buffer = slot.buffer;
        dstRegion.offset = 0;
        dstRegion.size   = bytes;
        SDL_UploadToGPUBuffer(copy, &srcLoc, &dstRegion, false);
        ++renderStats_.spriteUploads;
        // slot.transfer is pooled — NOT pushed to scratch.transfers (that list is per-frame release).
    }
    // ── Emission atlas: the sheets holding every layer's region-Bloom fields, and the table addressing them ──
    // A sheet is sized to the frame's peak layer, not grown mid-frame, because every layer is planned before
    // any of them renders. Each layer rewrites the store in turn; two layers' fields are never live at once.
    //
    // Retargeting waits for the allocation: an instance's map ends in screen→clip, and which clip that is
    // depends on the texture it draws into. So the raw instances are held through planning and re-expressed
    // here, against the grid they will actually rasterize on.
    if (!emissionRectRows.empty()) {
        // Demand-proportional sizing only holds in one direction unless the atlas can also step back down:
        // growth alone would let one heavy scene size it for the rest of the process. Shrink after a
        // sustained quiet run, never within a frame.
        constexpr int kEmissionAtlasQuietFrames = 120;
        const bool    fitsInHalf = emissionAtlasW_ > 0 && emissionAtlasWant * 2 <= emissionAtlasW_ &&
                                   emissionAtlasWantH * 2 <= emissionAtlasH_;
        emissionAtlasQuiet_ = fitsInHalf ? emissionAtlasQuiet_ + 1 : 0;
        const bool shrink   = emissionAtlasQuiet_ >= kEmissionAtlasQuietFrames;
        if (shrink) emissionAtlasQuiet_ = 0;
        const int wantW = shrink ? emissionAtlasWant  : std::max(emissionAtlasWant,  emissionAtlasW_);
        const int wantH = shrink ? emissionAtlasWantH : std::max(emissionAtlasWantH, emissionAtlasH_);
        const int rows  = static_cast<int>(emissionRectRows.size());
        int       sheets = 1;
        for (const PendingLayerEmission& p : layerEmissionPlans)
            sheets = std::max(sheets, static_cast<int>(p.sheets.size()));

        if (ensureEmissionAtlas(wantW, wantH, sheets, rows)) {
            std::vector<Vec4> texels;
            texels.reserve(emissionRectRows.size() * kEmissionRectTexels);
            for (const EmissionRectEntry& e : emissionRectRows) {
                texels.push_back(Vec4{e.offsetX, e.offsetY, e.innerX, e.innerY});
                texels.push_back(Vec4{e.innerW, e.innerH, e.sheet, 0.0f});
            }
            const Uint32 bytes = static_cast<Uint32>(texels.size()) * static_cast<Uint32>(sizeof(Vec4));
            SDL_GPUTransferBufferCreateInfo tbInfo{};
            tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            tbInfo.size  = bytes;
            SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
            if (!transfer) fail("SDL_CreateGPUTransferBuffer (emission rect table) failed");
            void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
            if (!mapped) fail("SDL_MapGPUTransferBuffer (emission rect table) failed");
            std::memcpy(mapped, texels.data(), bytes);
            SDL_UnmapGPUTransferBuffer(device_, transfer);
            if (!copy) copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureTransferInfo src{};
            src.transfer_buffer = transfer;
            src.offset          = 0;
            src.pixels_per_row  = static_cast<Uint32>(kEmissionRectTexels);
            src.rows_per_layer  = static_cast<Uint32>(emissionRectRows.size());
            SDL_GPUTextureRegion dst{};
            dst.texture = emissionRects_;
            dst.w       = static_cast<Uint32>(kEmissionRectTexels);
            dst.h       = static_cast<Uint32>(emissionRectRows.size());
            dst.d       = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
            scratch.transfers.push_back(transfer);

            // One instance buffer per SHEET and per path: a sheet is one render target, so its fields fill
            // together and the draw count tracks sheets, not fields. The two paths get their own buffers
            // because they draw through different pipelines — the region path re-rasters the sprite's art,
            // the below path extracts the scene over a rect.
            for (std::size_t idx = 0; idx < layerEmissionPlans.size(); ++idx) {
                PendingLayerEmission& p = layerEmissionPlans[idx];
                if (p.total() == 0) continue;
                const std::size_t regionCount = p.demands.size();
                for (std::size_t sh = 0; sh < p.sheets.size(); ++sh) {
                    std::vector<GpuSprite> placed;
                    for (std::size_t i = 0; i < regionCount; ++i) {
                        if (p.sheetOf[i] != static_cast<int>(sh)) continue;
                        const EmissionRectEntry e =
                            emissionRectEntry(p.demands[i].field, p.placements[i], p.sheetOf[i]);
                        placed.push_back(spriteAtlasInstance(p.instances[i], viewport_.width,
                                                             viewport_.height, emissionAtlasW_,
                                                             emissionAtlasH_, static_cast<int>(e.offsetX),
                                                             static_cast<int>(e.offsetY)));
                    }
                    if (!placed.empty()) {
                        layerEmissionSheets[idx][sh].buffer =
                            uploadSpriteRun(spriteRunSlot++, placed.data(), static_cast<int>(placed.size()));
                        layerEmissionSheets[idx][sh].count = static_cast<int>(placed.size());
                    }

                    // Below extracts split by fill pipeline. Bloom / Glow and a body-less Custom lens fill
                    // through the STOCK rect brightpass (one buffer); a body-authored Custom lens fills through
                    // its stage's OWN rect pipeline, grouped per stage so each stage's fields draw in one pass.
                    std::vector<GpuEmissionExtract> extracts;
                    std::vector<std::pair<std::uint32_t, std::vector<GpuEmissionExtract>>> customByStage;
                    for (std::size_t b = 0; b < p.belowDemands.size(); ++b) {
                        const std::size_t i = regionCount + b;
                        if (p.sheetOf[i] != static_cast<int>(sh)) continue;
                        const SpriteBelowEmissionDemand& d = p.belowDemands[b];
                        const EmissionRectEntry e =
                            emissionRectEntry(d.field, p.placements[i], p.sheetOf[i]);
                        GpuEmissionExtract inst =
                            emissionExtractInstance(d, p.placements[i], e, emissionAtlasW_, emissionAtlasH_);
                        if (d.extract == EmissionExtract::Custom && d.hasBody) {
                            auto it = std::find_if(customByStage.begin(), customByStage.end(),
                                                   [&](const auto& g) { return g.first == d.stage; });
                            if (it == customByStage.end()) {
                                customByStage.push_back({d.stage, {inst}});
                            } else {
                                it->second.push_back(inst);
                            }
                        } else {
                            if (d.extract == EmissionExtract::Custom)
                                inst.glow = 0.0f;  // body-less: the stock pipeline fills it as a Bloom brightpass
                            extracts.push_back(inst);
                        }
                    }
                    if (!extracts.empty()) {
                        layerEmissionSheets[idx][sh].extractBuf =
                            uploadRunBytes(spriteRunSlot++, extracts.data(),
                                           extracts.size() * sizeof(GpuEmissionExtract));
                        layerEmissionSheets[idx][sh].extractCount = static_cast<int>(extracts.size());
                    }
                    for (auto& [stage, insts] : customByStage) {
                        SDL_GPUBuffer* buf = uploadRunBytes(spriteRunSlot++, insts.data(),
                                                            insts.size() * sizeof(GpuEmissionExtract));
                        layerEmissionSheets[idx][sh].customExtracts.push_back(
                            CustomExtractRun{stage, buf, static_cast<int>(insts.size())});
                    }
                }
            }
        }
    }
    // ── Sprite-effect store: pack every sprite's flattened effect records into one storage texture ──
    // Ten RGBA32F texels per record, one record per ROW (10 wide) — the sprite fragment Loads a record's
    // texels by (chunk, fxOffset + i). The head chunk's four uint fields are stored as float-valued ints
    // (exact for these small values, and read back with (uint) — no denormal-flush hazard a bit-reinterpret
    // would carry). Always at least one row so the t3 binding is valid; a no-effect frame's dummy row is
    // never read (those sprites carry fxCount 0). Grow-on-demand by row count.
    {
        constexpr int kFxTexelsPerRecord = 10;
        if (fxStore.empty()) fxStore.push_back(SpriteFxRecord{});
        const int rows = static_cast<int>(fxStore.size());
        std::vector<Vec4> texels;
        texels.reserve(static_cast<std::size_t>(rows) * kFxTexelsPerRecord);
        for (const SpriteFxRecord& r : fxStore) {
            texels.push_back(Vec4{static_cast<float>(r.kind), static_cast<float>(r.flags),
                                  static_cast<float>(r.blend), static_cast<float>(r.pointCount)});
            texels.push_back(Vec4{r.alpha, r.radius, r.strokeWidth, r.pad0});
            texels.push_back(Vec4{r.params[0], r.params[1], r.params[2], r.params[3]});
            texels.push_back(Vec4{r.invRow0[0], r.invRow0[1], r.invRow0[2], r.invRow0[3]});
            texels.push_back(Vec4{r.invRow1[0], r.invRow1[1], r.invRow1[2], r.invRow1[3]});
            texels.push_back(Vec4{r.invRow2[0], r.invRow2[1], r.invRow2[2], r.invRow2[3]});
            for (int k = 0; k < 4; ++k)
                texels.push_back(Vec4{r.points[4 * k + 0], r.points[4 * k + 1],
                                      r.points[4 * k + 2], r.points[4 * k + 3]});
        }
        if (!spriteFxStore_ || rows > spriteFxStoreRows_) {  // grow-only recreate
            if (spriteFxStore_) SDL_ReleaseGPUTexture(device_, spriteFxStore_);
            SDL_GPUTextureCreateInfo ti{};
            ti.type                 = SDL_GPU_TEXTURETYPE_2D;
            ti.format               = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
            ti.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
            ti.width                = static_cast<Uint32>(kFxTexelsPerRecord);
            ti.height               = static_cast<Uint32>(rows);
            ti.layer_count_or_depth = 1;
            ti.num_levels           = 1;
            ti.sample_count         = SDL_GPU_SAMPLECOUNT_1;
            spriteFxStore_ = SDL_CreateGPUTexture(device_, &ti);
            if (!spriteFxStore_) fail("SDL_CreateGPUTexture (sprite-fx store) failed");
            spriteFxStoreRows_ = rows;
            spriteFxHashed_    = false;  // a fresh texture holds nothing — never skip against a stale hash
        }
        const Uint32 bytes = static_cast<Uint32>(texels.size()) * static_cast<Uint32>(sizeof(Vec4));
        // Same skip as a sprite layer's: an unchanged store (every sprite's effects settled, or a frame with
        // no sprite effects at all — the dummy row) is already resident, so nothing transfers.
        const std::uint64_t fxHash    = foldBytes64(texels.data(), bytes, kFnv64Offset);
        const bool          storeHeld = spriteFxHashed_ && spriteFxHashRows_ == rows && spriteFxHash_ == fxHash;
        spriteFxHashed_   = true;
        spriteFxHashRows_ = rows;
        spriteFxHash_     = fxHash;
        if (!storeHeld) {
            SDL_GPUTransferBufferCreateInfo tbInfo{};
            tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            tbInfo.size  = bytes;
            SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
            if (!transfer) fail("SDL_CreateGPUTransferBuffer (sprite-fx store) failed");
            void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
            if (!mapped) fail("SDL_MapGPUTransferBuffer (sprite-fx store) failed");
            std::memcpy(mapped, texels.data(), bytes);
            SDL_UnmapGPUTransferBuffer(device_, transfer);
            if (!copy) copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureTransferInfo src{};
            src.transfer_buffer = transfer;
            src.offset          = 0;
            src.pixels_per_row  = static_cast<Uint32>(kFxTexelsPerRecord);
            src.rows_per_layer  = static_cast<Uint32>(rows);
            SDL_GPUTextureRegion dst{};
            dst.texture = spriteFxStore_;
            dst.w       = static_cast<Uint32>(kFxTexelsPerRecord);
            dst.h       = static_cast<Uint32>(rows);
            dst.d       = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
            scratch.transfers.push_back(transfer);
        }
    }

    // ── Row-data store: stack every effect's paramTable into the flat RGBA32F store ──
    // Walk every effect site (frame post-effects, per-layer effects, region effects). Each Custom effect
    // carrying a paramTable gets a vertical region at a storeY; record it by address so runEffect can
    // forward (storeY, rows) to the shader. No tables this frame ⇒ the 1×1 default store stays bound and
    // nothing uploads.
    rowData_.clear();
    auto recordRowTable = [&](const ScreenSpaceEffect& e) {
        if (e.kind != ScreenSpaceEffectKind::Custom || e.paramTable.empty()) return;
        const auto storeY = static_cast<std::uint32_t>(rowData_.size());
        rowData_.insert(rowData_.end(), e.paramTable.begin(), e.paramTable.end());
        rowTableLocs.emplace(&e, RowTableLoc{storeY, static_cast<std::uint32_t>(e.paramTable.size())});
    };
    for (const ScreenSpaceEffect& e : frame.postEffects) recordRowTable(e);
    for (const DrawLayer& layer : frame.layers) {
        for (const ScreenSpaceEffect& e : layer.effects) recordRowTable(e);
        for (const Region& rg : layer.regions)
            for (const ScreenSpaceEffect& e : rg.effects) recordRowTable(e);
    }
    for (const Region& rg : frame.regions)
        for (const ScreenSpaceEffect& e : rg.effects) recordRowTable(e);

    if (!rowData_.empty()) {
        const int rows = static_cast<int>(rowData_.size());
        if (!rowDataStore_ || rows > rowDataStoreH_) {  // grow-only recreate
            if (rowDataStore_) SDL_ReleaseGPUTexture(device_, rowDataStore_);
            SDL_GPUTextureCreateInfo ri{};
            ri.type                 = SDL_GPU_TEXTURETYPE_2D;
            ri.format               = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
            ri.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
            ri.width                = 1;
            ri.height               = static_cast<Uint32>(rows);
            ri.layer_count_or_depth = 1;
            ri.num_levels           = 1;
            ri.sample_count         = SDL_GPU_SAMPLECOUNT_1;
            rowDataStore_ = SDL_CreateGPUTexture(device_, &ri);
            if (!rowDataStore_) fail("SDL_CreateGPUTexture (row-data store) failed");
            rowDataStoreH_ = rows;
        }

        const Uint32 bytes = static_cast<Uint32>(rowData_.size()) * static_cast<Uint32>(sizeof(Vec4));
        SDL_GPUTransferBufferCreateInfo tbInfo{};
        tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbInfo.size  = bytes;
        SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
        if (!transfer) fail("SDL_CreateGPUTransferBuffer (row-data store) failed");
        void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
        if (!mapped) fail("SDL_MapGPUTransferBuffer (row-data store) failed");
        std::memcpy(mapped, rowData_.data(), bytes);
        SDL_UnmapGPUTransferBuffer(device_, transfer);

        if (!copy) copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = transfer;
        src.offset          = 0;
        src.pixels_per_row  = 1;
        src.rows_per_layer  = static_cast<Uint32>(rows);
        SDL_GPUTextureRegion dst{};
        dst.texture = rowDataStore_;
        dst.w       = 1;
        dst.h       = static_cast<Uint32>(rows);
        dst.d       = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
        scratch.transfers.push_back(transfer);
    }

    if (copy) SDL_EndGPUCopyPass(copy);

    // Evict persistent cache slots whose key did not appear this frame — a despawn (the unmountGone pattern
    // from interpolation.cpp), releasing the GPU resource so dead keys don't accumulate. Safe mid-compose:
    // an evicted slot's layer is gone this frame, so its texture/buffer is NOT referenced by this frame's
    // command buffer; SDL defers the actual free until any prior in-flight submit that used it completes.
    // Transient slots live in the local deques, not these maps, so they are never touched here.
    for (auto it = tilemaps_.begin(); it != tilemaps_.end();) {
        if (seenTileKeys.count(it->first) != 0) { ++it; continue; }
        if (it->second.texture) SDL_ReleaseGPUTexture(device_, it->second.texture);
        if (it->second.transfer) SDL_ReleaseGPUTransferBuffer(device_, it->second.transfer);
        it = tilemaps_.erase(it);
    }
    for (auto it = spriteBufs_.begin(); it != spriteBufs_.end();) {
        if (seenSpriteKeys.count(it->first) != 0) { ++it; continue; }
        if (it->second.buffer) SDL_ReleaseGPUBuffer(device_, it->second.buffer);
        if (it->second.transfer) SDL_ReleaseGPUTransferBuffer(device_, it->second.transfer);
        it = spriteBufs_.erase(it);
    }
    for (auto it = rasters_.begin(); it != rasters_.end();) {
        if (seenRasterKeys.count(it->first) != 0) { ++it; continue; }
        if (it->second.texture) SDL_ReleaseGPUTexture(device_, it->second.texture);
        if (it->second.transfer) SDL_ReleaseGPUTransferBuffer(device_, it->second.transfer);
        it = rasters_.erase(it);
    }

    // ── Viewport composite (segmented for per-layer screen-space effects) ───────────────────────
    // Layers composite back-to-front into target_. A layer with NO effect draws straight into the
    // accumulator (consecutive such layers batch into one render pass — CLEAR on the first, LOAD
    // after). A per-layer effect splits the loop: a Below-scope effect draws its content into the
    // accumulator then displaces the WHOLE accumulator (this layer + everything beneath) and swaps it
    // in; a Layer-scope effect renders ITS layer alone into layerScratch_ and composites it back
    // displaced. A frame with NO effect layers never splits → a single CLEAR pass over all layers. ──

    // One tile/sprite layer drawn into the given pass — shared by the batched path, the Below content
    // draw, and the isolated-Layer offscreen render. (The per-layer screen-space effect is realized by
    // the caller, not here; this just composites the layer's content.)
    auto drawLayer = [&](SDL_GPURenderPass* pass, std::size_t idx) {
        const DrawLayer& layer = frame.layers[idx];

        if (contentKind(layer.content) == LayerContentKind::Tiles) {
            const TileContent& tc = std::get<TileContent>(layer.content);
            if (tc.widthInTiles <= 0 || tc.heightInTiles <= 0) return;
            const TilemapTex* slotP = tileSlot[idx];
            if (!slotP || !slotP->texture) return;
            const TilemapTex& slot = *slotP;
            if (!atlasStore_ || !paletteStore_ || !atlasRegionStore_) return;  // nothing uploaded → nothing to draw

            TileUniforms u{};
            // Scroll places at the eased float on the interpolation path (a fractional scroll shifts the
            // sampled tile/pixel boundary by whole output pixels between refreshes → smooth), falling back
            // to the submission's integer scroll for a layer with no history.
            float scrollX = static_cast<float>(layer.scroll.x);
            float scrollY = static_cast<float>(layer.scroll.y);
            if (interpolate) {
                if (const auto ls = interp_.interpolatedLayerScroll(layer.key, timing)) {
                    scrollX = ls->x;
                    scrollY = ls->y;
                }
            }
            u.scrollX   = scrollX;
            u.scrollY   = scrollY;
            u.layerW    = static_cast<float>(composeW_);   // compose grid (output res on the interp path)
            u.layerH    = static_cast<float>(composeH_);
            u.composeScale = static_cast<float>(composeScale_);
            u.snap      = snapF;   // 1 = snap the transform's destination pixel to the viewport grid (crisp)
            u.tilemapW  = static_cast<float>(tc.widthInTiles);
            u.tilemapH  = static_cast<float>(tc.heightInTiles);
            u.tilePx    = static_cast<float>(kTilePx);
            u.alpha     = clampAlpha(layer.alpha);
            u.paletteStoreW = static_cast<float>(kPaletteStoreWidth);

            // Per-layer transform: upload the INVERSE homography (the fragment maps a destination
            // pixel back to content space, perspective divide included) + the footprint edge mode.
            // Identity ⇒ hasTransform 0 ⇒ the fragment takes the faithful untransformed path.
            u.hasTransform  = layer.transform.isIdentity() ? 0u : 1u;
            u.transformEdge = static_cast<std::uint32_t>(layer.transformEdge);
            // Per-layer tilemap wrap mode (Repeat default ⇒ faithful toroidal output).
            u.wrap          = static_cast<std::uint32_t>(tc.wrap);
            const Transform inv = layer.transform.inverse();
            u.invRow0[0] = inv.m00; u.invRow0[1] = inv.m01; u.invRow0[2] = inv.m02; u.invRow0[3] = 0.0f;
            u.invRow1[0] = inv.m10; u.invRow1[1] = inv.m11; u.invRow1[2] = inv.m12; u.invRow1[3] = 0.0f;
            u.invRow2[0] = inv.m20; u.invRow2[1] = inv.m21; u.invRow2[2] = inv.m22; u.invRow2[3] = 0.0f;

            // The tile path is all integer Load — bind four read-only storage textures at t0..t3 (the flat
            // atlas store, this layer's tilemap cells, the palette store, and the global atlas-region table);
            // each cell's atlas + palette handle indexes the stores directly. No sampler.
            SDL_GPUTexture* storageTextures[4] = {atlasStore_, slot.texture, paletteStore_, atlasRegionStore_};
            SDL_BindGPUGraphicsPipeline(pass, tile_);
            SDL_BindGPUFragmentStorageTextures(pass, 0, storageTextures, 4);
            SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);  // one fullscreen triangle
        } else if (contentKind(layer.content) == LayerContentKind::Raster) {
            const RasterContent& gc = std::get<RasterContent>(layer.content);
            if (gc.width <= 0 || gc.height <= 0) return;
            const RasterTex* slotP = rasterSlot[idx];
            if (!slotP || !slotP->texture || slotP->uploadedGeneration == 0) return;  // nothing resident
            const RasterTex& slot = *slotP;

            RasterUniforms u{};
            // Scroll places at the eased float on the interpolation path, exactly as a tile layer's
            // does — a raster is placed in the scene like any other content.
            float scrollX = static_cast<float>(layer.scroll.x);
            float scrollY = static_cast<float>(layer.scroll.y);
            if (interpolate) {
                if (const auto ls = interp_.interpolatedLayerScroll(layer.key, timing)) {
                    scrollX = ls->x;
                    scrollY = ls->y;
                }
            }
            u.scrollX      = scrollX;
            u.scrollY      = scrollY;
            u.layerW       = static_cast<float>(composeW_);   // compose grid (output res on the interp path)
            u.layerH       = static_cast<float>(composeH_);
            u.rasterW      = static_cast<float>(gc.width);
            u.rasterH      = static_cast<float>(gc.height);
            u.alpha        = clampAlpha(layer.alpha);
            u.composeScale = static_cast<float>(composeScale_);
            u.snap         = snapF;

            u.hasTransform  = layer.transform.isIdentity() ? 0u : 1u;
            u.transformEdge = static_cast<std::uint32_t>(layer.transformEdge);
            const Transform inv = layer.transform.inverse();
            u.invRow0[0] = inv.m00; u.invRow0[1] = inv.m01; u.invRow0[2] = inv.m02; u.invRow0[3] = 0.0f;
            u.invRow1[0] = inv.m10; u.invRow1[1] = inv.m11; u.invRow1[2] = inv.m12; u.invRow1[3] = 0.0f;
            u.invRow2[0] = inv.m20; u.invRow2[1] = inv.m21; u.invRow2[2] = inv.m22; u.invRow2[3] = 0.0f;

            // One read-only storage texture — the raster itself — and no sampler: the fragment Loads the
            // texel its source drew.
            SDL_GPUTexture* storageTextures[1] = {slot.texture};
            SDL_BindGPUGraphicsPipeline(pass, raster_);
            SDL_BindGPUFragmentStorageTextures(pass, 0, storageTextures, 1);
            SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);  // one fullscreen triangle
        } else {  // LayerContentKind::Sprites
            const SpriteContent& sc = std::get<SpriteContent>(layer.content);
            const int spriteCount = static_cast<int>(sc.sprites.size());
            if (spriteCount <= 0) return;
            const SpriteBuf* slotP = spriteSlot[idx];
            if (!slotP || !slotP->buffer || slotP->count <= 0) return;  // count excludes Below-scope lenses (no art draw)
            const SpriteBuf& slot = *slotP;
            if (!atlasStore_ || !paletteStore_ || !atlasRegionStore_) return;

            SpriteFragUniforms fu{};
            fu.tilePx        = static_cast<float>(kTilePx);
            fu.alpha         = clampAlpha(layer.alpha);
            fu.paletteStoreW = static_cast<float>(kPaletteStoreWidth);
            fu.composeScale  = static_cast<float>(composeScale_);  // the analytic branch's output→viewport map
            fu.evalSnap      = snapF;                              // the field read's grid

            // Instanced per-sprite quads: the vertex stage reads the sprite records (already in clip
            // space) from a storage buffer (t0 space0) — no vertex uniform; the fragment stage samples the
            // emission atlas (t0/s0 space2) and reads the flat atlas store, palette store, and the global
            // atlas-region table (t1/t2/t3 space2) + its uniform. Each sprite's atlas + palette handle
            // indexes the stores. 6 verts × spriteCount.
            SDL_GPUTexture* field = spriteArtFieldFor(idx);
            SDL_GPUTexture* rects = spriteArtRectsFor(idx);
            if (!field || !rects) return;  // the fragment declares both; nothing valid to bind ⇒ no draw
            SDL_GPUTexture* fragStorage[5] = {atlasStore_, paletteStore_, atlasRegionStore_, spriteFxStore_,
                                              rects};
            const SDL_GPUTextureSamplerBinding fieldBind{field, bilinear_};  // linear, CLAMP
            SDL_BindGPUGraphicsPipeline(pass, sprite_);
            SDL_BindGPUVertexStorageBuffers(pass, 0, &slot.buffer, 1);
            SDL_BindGPUFragmentSamplers(pass, 0, &fieldBind, 1);           // the emission atlas (t0/s0)
            SDL_BindGPUFragmentStorageTextures(pass, 0, fragStorage, 5);   // +fx (t4), +rects (t5)
            SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
            SDL_DrawGPUPrimitives(pass, 6, static_cast<Uint32>(slot.count), 0, 0);
        }
    };

    // Draw ONE contiguous sprite run (a per-run buffer) into `pass`. Mirrors drawLayer's sprite branch — same
    // fragment stores + uniform (layer alpha etc.) — but binds the run's own storage buffer, draws its `count`
    // instances (first_instance 0), and binds the run's pipeline: the stock sprite pipeline for pipelineKey 0,
    // the custom sprite-inline pipeline customSprite_[key-1] otherwise. The layer's frame-wide sprite state
    // (atlas / palette / transform) is baked into each record, so a run needs nothing beyond its buffer.
    auto drawSpriteRun = [&](SDL_GPURenderPass* pass, std::size_t idx, SDL_GPUBuffer* buffer, int count,
                             int pipelineKey) {
        if (count <= 0 || !buffer) return;
        if (!atlasStore_ || !paletteStore_ || !atlasRegionStore_) return;
        const DrawLayer& layer = frame.layers[idx];
        SpriteFragUniforms fu{};
        fu.tilePx        = static_cast<float>(kTilePx);
        fu.alpha         = clampAlpha(layer.alpha);
        fu.paletteStoreW = static_cast<float>(kPaletteStoreWidth);
        fu.composeScale  = static_cast<float>(composeScale_);
        fu.evalSnap      = snapF;  // the field read's grid
        SDL_GPUTexture* field = spriteArtFieldFor(idx);
        SDL_GPUTexture* rects = spriteArtRectsFor(idx);
        if (!field || !rects) return;  // the fragment declares both; nothing valid to bind ⇒ no draw
        SDL_GPUTexture* fragStorage[5] = {atlasStore_, paletteStore_, atlasRegionStore_, spriteFxStore_,
                                          rects};
        const SDL_GPUTextureSamplerBinding fieldBind{field, bilinear_};  // linear, CLAMP
        SDL_GPUGraphicsPipeline* pipe = sprite_;
        if (pipelineKey > 0) {
            const std::size_t cid = static_cast<std::size_t>(pipelineKey - 1);
            if (cid < customSprite_.size() && customSprite_[cid]) pipe = customSprite_[cid];
        }
        SDL_BindGPUGraphicsPipeline(pass, pipe);
        SDL_BindGPUVertexStorageBuffers(pass, 0, &buffer, 1);
        SDL_BindGPUFragmentSamplers(pass, 0, &fieldBind, 1);           // the emission atlas (t0/s0)
        SDL_BindGPUFragmentStorageTextures(pass, 0, fragStorage, 5);   // +fx (t4), +rects (t5)
        SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
        SDL_DrawGPUPrimitives(pass, 6, static_cast<Uint32>(count), 0, 0);
    };

    // Whether a screen-space effect can be rendered this frame. A built-in (RowDisplacement / Ripple /
    // ColorFill) always can; a Custom effect is renderable iff its handle indexes a registered stage (its parameters
    // are the standard inline fields, so there is nothing else to validate). An invalid Custom pass throws
    // under the Throw collision policy (the debug default — surface a bad handle immediately) and is
    // skipped-with-warning under WarnAndResolve (keep a shipped game up). Shared by the per-layer +
    // frame-level realizations below.
    auto effectRenderable = [&](const ScreenSpaceEffect& effect) -> bool {
        if (!effectUsesCustomShader(effect)) return true;
        if (customStagePassValid(effect, customReplace_.size())) return true;
        if (collisionPolicy_ == LayerKeyCollisionPolicy::Throw) {
            throw std::invalid_argument(
                "renderFrame: invalid custom shader stage pass (handle out of range)");
        }
        SDL_Log("retropp: skipping invalid custom shader stage pass (handle out of range)");
        return false;
    };

    // One interior pass of the Bloom / Glow emission chain: bind `pipe`, sample `src` through `smp`, write
    // the whole of `dst`. Every interior stage owns its scratch outright, so there is no load op, no
    // scissor and no compositing to thread through — those belong to the chain's final pass alone.
    auto runEmissionPass = [&](SDL_GPUTexture* dst, SDL_GPUTexture* src, SDL_GPUSampler* smp,
                               SDL_GPUGraphicsPipeline* pipe, const void* uniform, std::uint32_t uniformSize) {
        SDL_GPUColorTargetInfo t{};
        t.texture     = dst;
        t.clear_color = kBackdropClear;
        t.load_op     = SDL_GPU_LOADOP_DONT_CARE;  // the fullscreen triangle covers every texel
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        const SDL_GPUTextureSamplerBinding binding{src, smp};
        SDL_BindGPUGraphicsPipeline(pass, pipe);
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        if (uniform) SDL_PushGPUFragmentUniformData(cmd, 0, uniform, uniformSize);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    };

    // Realize one Bloom or Glow: extract → [reduce ×½ ×2] → blur H → blur V → composite. Presents exactly
    // the contract runEffect does — read `source`, write `dest` under the caller's compositing, load op and
    // scissor — so a chain, a layer or a region cannot tell that several passes ran instead of one. Only
    // the final composite touches `dest`; every earlier stage writes private scratch.
    //
    // The separable pair reproduces the 2-D Gaussian the single gather computed, at 2(2K+1) taps per
    // fragment instead of (2K+1)², and reducing the field first cuts that again by 16 for a wide reach.
    //
    // Reduce (when the reach warrants it) and separably blur the emission field sitting in emission0_.
    // Returns the texture holding the finished halo. Shared by both emitters — an extract of a source image
    // (the frame-class sites) and a raster of glowing sprites (the sprite buckets) leave the same field
    // behind, so everything downstream of the field is one code path.
    auto blurEmission = [&](const EmissionChainPlan& plan) -> SDL_GPUTexture* {
        // Reduce, when the reach is wide enough that the detail lost is detail the blur removes anyway:
        // two ½ bilinear halvings, each tap averaging a 2×2 block.
        SDL_GPUTexture* blurSrc = emission0_;   // the field the horizontal pass reads
        SDL_GPUTexture* blurDst = emission1_;   // its target, which the vertical pass then reads
        SDL_GPUTexture* blurred = emission0_;   // where the vertical pass leaves the finished halo
        if (plan.downsample) {
            runEmissionPass(emissionHalf_, emission0_,    bilinear_, emissionCopy_, nullptr, 0);
            runEmissionPass(emissionLow0_, emissionHalf_, bilinear_, emissionCopy_, nullptr, 0);
            blurSrc = emissionLow0_;
            blurDst = emissionLow1_;
            blurred = emissionLow0_;
        }

        // The two axes. Taps step in VIEWPORT pixels either way — four of them per tap on the reduced path
        // — so the halo keeps the same size in the image whichever resolution produced it. The crisp snap
        // applies only at full resolution: a viewport cell can be smaller than a reduced texel, so snapping
        // there would quantize to a grid the buffer cannot express.
        const float invW      = viewport_.width  > 0 ? 1.0f / static_cast<float>(viewport_.width)  : 0.0f;
        const float invH      = viewport_.height > 0 ? 1.0f / static_cast<float>(viewport_.height) : 0.0f;
        const bool  snapChain = snap && !plan.downsample;
        const float snapW     = snapChain ? static_cast<float>(viewport_.width)  : 0.0f;
        const float snapH     = snapChain ? static_cast<float>(viewport_.height) : 0.0f;
        const float taps      = static_cast<float>(plan.taps);

        const EmissionBlurFragUniforms hu{plan.stepPx * invW, 0.0f, taps, plan.invNorm,
                                          plan.inv2Sigma2, snapW, snapH, 0.0f};
        runEmissionPass(blurDst, blurSrc, sampler_, emissionBlur_, &hu, sizeof(hu));
        const EmissionBlurFragUniforms vu{0.0f, plan.stepPx * invH, taps, plan.invNorm,
                                          plan.inv2Sigma2, snapW, snapH, 0.0f};
        runEmissionPass(blurred, blurDst, sampler_, emissionBlur_, &vu, sizeof(vu));
        return blurred;
    };

    // The chain's last pass — the one that honours the caller: read `source` and the blurred halo, write
    // `dest` under the caller's compositing, load op and scissor. `blurred` null runs the passthrough
    // instead, handing the source straight through (the identity path).
    auto compositeEmission = [&](SDL_GPUTexture* dest, SDL_GPUTexture* source, SDL_GPUTexture* blurred,
                                 bool glow, bool reduced, bool blend, SDL_GPULoadOp loadOp,
                                 const SDL_Rect* scissor) {
        SDL_GPUColorTargetInfo t{};
        t.texture     = dest;
        t.clear_color = kBackdropClear;
        t.load_op     = loadOp;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        if (scissor) SDL_SetGPUScissor(pass, scissor);  // region: shade only the shape's box, as ever
        if (blurred) {
            SDL_BindGPUGraphicsPipeline(pass, blend ? emissionCompositeBlend_ : emissionComposite_);
            // A reduced halo resolves back up bilinearly; a full-resolution one is texel-for-texel with the
            // source, so it samples nearest.
            const SDL_GPUTextureSamplerBinding binds[2] = {{source, sampler_},
                                                           {blurred, reduced ? bilinear_ : sampler_}};
            SDL_BindGPUFragmentSamplers(pass, 0, binds, 2);
            const EmissionCompositeFragUniforms cu{glow ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
            SDL_PushGPUFragmentUniformData(cmd, 0, &cu, sizeof(cu));
        } else {
            SDL_BindGPUGraphicsPipeline(pass, blend ? emissionCopyBlend_ : emissionCopy_);
            const SDL_GPUTextureSamplerBinding binding{source, sampler_};
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        }
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    };

    auto runEmissionChain = [&](SDL_GPUTexture* dest, SDL_GPUTexture* source,
                                const ScreenSpaceEffect& effect, bool blend, SDL_GPULoadOp loadOp,
                                const SDL_Rect* scissor) {
        const bool              glow = effect.kind == ScreenSpaceEffectKind::Glow;
        const EmissionChainPlan plan = emissionChainPlan(effect);

        // An identity effect — and a device that could not give us the scratch — still owes the caller its
        // source in the destination. One passthrough delivers it byte-exactly.
        if (!plan.engaged || !ensureEmissionTargets()) {
            compositeEmission(dest, source, nullptr, glow, false, blend, loadOp, scissor);
            return;
        }

        // Extract, always at full resolution: the threshold is a nonlinear key and belongs on true pixels.
        {
            const BloomParams bp = bloomParams(effect);
            const GlowParams  gp = glowParams(effect);
            const EmissionExtractFragUniforms eu{glow ? 1.0f : 0.0f,
                                                 glow ? gp.threshold : bp.threshold,
                                                 glow ? gp.intensity : bp.intensity,
                                                 0.0f,
                                                 gp.tintR, gp.tintG, gp.tintB, 0.0f};
            runEmissionPass(emission0_, source, sampler_, emissionExtract_, &eu, sizeof(eu));
        }

        compositeEmission(dest, source, blurEmission(plan), glow, plan.downsample, blend, loadOp, scissor);
    };

    // Push a custom stage's two uniform cbuffers for the CURRENTLY-bound pass: slot 0 the engine cbuffer
    // (edge mode + this effect's row-table location + the evaluation grid), slot 1 the shader's OWN reflected
    // params via its generated packer (nothing for a parameterless shader). The exact bytes the stock custom
    // pass pushes — shared by an emission stage's extract pass and its final pass.
    auto pushCustomStageUniforms = [&](const ScreenSpaceEffect& effect, std::size_t id) {
        const auto locIt = rowTableLocs.find(&effect);
        const RowTableLoc loc = locIt != rowTableLocs.end() ? locIt->second : RowTableLoc{};
        const EngineEffectFragUniforms eng{
            effect.edge == DisplacementEdge::Stretch ? 1u : 0u, loc.storeY, loc.rows,
            snap ? 1u : 0u,
            static_cast<float>(viewport_.width), static_cast<float>(viewport_.height),
            0.0f, 0.0f};
        SDL_PushGPUFragmentUniformData(cmd, 0, &eng, sizeof(eng));
        const EffectPacker packer = id < customPackers_.size() ? customPackers_[id] : nullptr;
        if (packer) {
            std::byte ubuf[kMaxCustomEffectUniformBytes];
            const std::uint32_t usize = packer(effect, ubuf);
            if (usize > 0) SDL_PushGPUFragmentUniformData(cmd, 1, ubuf, usize);
        }
    };

    // Realize an emission-declared CUSTOM stage: extract → blur → the stage's own pass, which reads the
    // blurred emission through sampleEmission(). Engagement is UNCONDITIONAL (the declaration IS the demand),
    // so the chain always runs; at radius 0 the blur is skipped and the stage samples the un-blurred extract.
    // The field content is the stage's own emission() body (customEmission_[id]) or, absent a body, the stock
    // brightpass at `.threshold` (neutral: intensity 1, no tint — Bloom-shaped, so the brightpass carries the
    // scene's chroma). The final pass presents the SAME read/write/composite contract runEffect does, so every
    // caller — frame, layer, region — is unaffected.
    auto runCustomEmissionChain = [&](SDL_GPUTexture* dest, SDL_GPUTexture* source,
                                      const ScreenSpaceEffect& effect, std::size_t id, bool blend,
                                      SDL_GPULoadOp loadOp, const SDL_Rect* scissor) {
        const EmissionChainPlan plan = emissionChainPlanCustom(effect.radius);

        // The texture the stage samples at slot 1. Null when the device could not give us the scratch — the
        // stage still runs (it owes dest its output) with the persistent 1×1 transparent stand-in bound, so
        // sampleEmission() returns 0 everywhere, exactly the built-in chain's graceful degradation.
        SDL_GPUTexture* emissionTex = nullptr;
        bool            reduced     = false;
        if (ensureEmissionTargets()) {
            if (id < customEmission_.size() && customEmission_[id]) {
                // The stage authors its own field: run its emission() body over the whole source into
                // emission0_, reading the scene through sampleSource() with the stage's own params.
                SDL_GPUColorTargetInfo t{};
                t.texture  = emission0_;
                t.load_op  = SDL_GPU_LOADOP_DONT_CARE;   // the fullscreen triangle covers every texel
                t.store_op = SDL_GPU_STOREOP_STORE;
                SDL_GPURenderPass* ep = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
                const SDL_GPUTextureSamplerBinding sb{source, sampler_};
                SDL_BindGPUGraphicsPipeline(ep, customEmission_[id]);
                SDL_BindGPUFragmentSamplers(ep, 0, &sb, 1);
                SDL_BindGPUFragmentStorageTextures(ep, 0, &rowDataStore_, 1);
                pushCustomStageUniforms(effect, id);
                SDL_DrawGPUPrimitives(ep, 3, 1, 0, 0);
                SDL_EndGPURenderPass(ep);
            } else {
                // No body: the stock brightpass at the effect's threshold, neutral (glow 0, intensity 1, no
                // tint) — a source-coloured emission the stage then shapes.
                const EmissionExtractFragUniforms eu{0.0f, static_cast<float>(effect.threshold) / 255.0f,
                                                     1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
                runEmissionPass(emission0_, source, sampler_, emissionExtract_, &eu, sizeof(eu));
            }
            emissionTex = plan.taps > 0 ? blurEmission(plan) : emission0_;  // radius 0 ⇒ the un-blurred extract
            reduced     = plan.downsample;
        }

        // The stage's own pass: read source (slot 0) + the blurred emission (slot 1), write dest under the
        // caller's compositing / load op / scissor. A reduced halo resolves up bilinearly; a full-resolution
        // one samples nearest — the compositeEmission convention.
        SDL_GPUColorTargetInfo t{};
        t.texture     = dest;
        t.clear_color = kBackdropClear;
        t.load_op     = loadOp;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        if (scissor) SDL_SetGPUScissor(pass, scissor);
        SDL_BindGPUGraphicsPipeline(pass, blend ? customBlend_[id] : customReplace_[id]);
        const SDL_GPUTextureSamplerBinding binds[2] = {
            {source, sampler_},
            {emissionTex ? emissionTex : batchZeroSource_, (emissionTex && reduced) ? bilinear_ : sampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, binds, 2);
        SDL_BindGPUFragmentStorageTextures(pass, 0, &rowDataStore_, 1);
        pushCustomStageUniforms(effect, id);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    };

    // Realize one layer's sprite-emission buckets over the texture holding that layer's content, and return
    // the texture the content now lives in. Per bucket: clear the emission buffer, raster every member
    // sprite's emission into it additively in ONE instanced pass, blur it, and composite the halo. The blur
    // and composite are fixed, so N glowing sprites at one reach cost what one costs; a second reach costs a
    // second bucket, which is the authored structure and not the sprite count.
    //
    // The composite ping-pongs through `pool` (three textures, of which `content` is one or none), so each
    // bucket writes a texture it does not read. The halo lands over the layer's whole art, so a sprite
    // drawn later in the same layer does not occlude an earlier sprite's halo.
    auto applySpriteEmission = [&](std::size_t idx, SDL_GPUTexture* content,
                                   SDL_GPUTexture* p0, SDL_GPUTexture* p1,
                                   SDL_GPUTexture* p2) -> SDL_GPUTexture* {
        const std::vector<SpriteEmissionRunGpu>& runs = spriteEmissionRuns[idx];
        if (runs.empty() || !atlasStore_ || !paletteStore_ || !atlasRegionStore_) return content;
        if (!ensureEmissionTargets()) return content;  // no scratch: the art still draws, the halo does not

        const DrawLayer& layer = frame.layers[idx];
        SpriteFragUniforms fu{};
        fu.tilePx        = static_cast<float>(kTilePx);
        fu.alpha         = clampAlpha(layer.alpha);
        fu.paletteStoreW = static_cast<float>(kPaletteStoreWidth);
        fu.composeScale  = static_cast<float>(composeScale_);
        SDL_GPUTexture* fragStorage[4] = {atlasStore_, paletteStore_, atlasRegionStore_, spriteFxStore_};

        SDL_GPUTexture* result = content;
        for (const SpriteEmissionRunGpu& run : runs) {
            if (run.count <= 0 || !run.buffer) continue;
            ScreenSpaceEffect probe{};   // the reach and kind are all the plan reads beyond intensity
            probe.kind      = run.glow ? ScreenSpaceEffectKind::Glow : ScreenSpaceEffectKind::Bloom;
            probe.intensity = 255;       // a step that did not engage never became a bucket
            const EmissionChainPlan plan = emissionChainPlan(probe, run.reach);
            if (!plan.engaged) continue;

            {   // Raster: a fresh field per bucket, summed across its member sprites.
                SDL_GPUColorTargetInfo t{};
                t.texture     = emission0_;
                t.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
                t.load_op     = SDL_GPU_LOADOP_CLEAR;
                t.store_op    = SDL_GPU_STOREOP_STORE;
                SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
                SDL_BindGPUGraphicsPipeline(pass, spriteEmission_);
                SDL_BindGPUVertexStorageBuffers(pass, 0, &run.buffer, 1);
                SDL_BindGPUFragmentStorageTextures(pass, 0, fragStorage, 4);
                SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
                SDL_DrawGPUPrimitives(pass, 6, static_cast<Uint32>(run.count), 0, 0);
                SDL_EndGPURenderPass(pass);
            }

            SDL_GPUTexture* blurred = blurEmission(plan);
            SDL_GPUTexture* out     = nullptr;
            for (SDL_GPUTexture* t : {p0, p1, p2})
                if (t != result && !out) out = t;
            if (!out) continue;   // no free scratch to write into — leave the layer as drawn
            compositeEmission(out, result, blurred, run.glow, plan.downsample, /*blend=*/false,
                              SDL_GPU_LOADOP_DONT_CARE, nullptr);
            result = out;
        }
        return result;
    };

    // Run one effect pass: read `source`, write `dest`. `blend` picks the replace pipeline (the opaque
    // accumulator displace for a Below effect / the frame-level chain) or the premultiplied-over
    // composite pipeline (an isolated Layer's effected image back onto target_). The pass is shader-
    // agnostic: a built-in effect dispatches BY KIND to its pipeline pair + resolved uniform —
    // RowDisplacement → displace_/displaceBlend_ + DisplaceParams (`blankTransparent` controlling the
    // Blank-edge colour), Ripple → ripple_/rippleBlend_ + RippleParams, ColorFill → colorFill_/
    // colorFillBlend_ + ColorFillParams; a Custom effect binds the registered pipeline pair + pushes the
    // game's own uniform bytes. Same scope/compositing/ping-pong plumbing for every kind.
    auto runEffect = [&](SDL_GPUTexture* dest, SDL_GPUTexture* source, const ScreenSpaceEffect& effect,
                         bool blankTransparent, bool blend, SDL_GPULoadOp loadOp,
                         const SDL_Rect* scissor = nullptr) {
        // Bloom and Glow are neighbourhood reads, so they realize as the multi-pass emission chain rather
        // than as one pass here. It presents this same contract, so every caller is unaffected.
        if (effect.kind == ScreenSpaceEffectKind::Bloom || effect.kind == ScreenSpaceEffectKind::Glow) {
            runEmissionChain(dest, source, effect, blend, loadOp, scissor);
            return;
        }
        // An emission-declared CUSTOM stage is likewise a chain (extract → blur → the stage's own pass reading
        // the blurred field), not a single pass. The customEmissionConsumer_ flag is the dispatch — every other
        // Custom stage falls through to the single-pass branch below.
        if (effectUsesCustomShader(effect)) {
            const auto cid = static_cast<std::size_t>(effect.customShader);
            if (cid < customEmissionConsumer_.size() && customEmissionConsumer_[cid]) {
                runCustomEmissionChain(dest, source, effect, cid, blend, loadOp, scissor);
                return;
            }
        }
        SDL_GPUColorTargetInfo t{};
        t.texture     = dest;
        t.clear_color = kBackdropClear;
        t.load_op     = loadOp;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        // Region-confined effect: shade only the shape's bounding box. The region gate that follows discards
        // everything outside the shape, so the effect result matters only inside — byte-identical output, far
        // fewer shaded pixels. nullptr (every whole-frame caller) leaves the pass at full frame.
        if (scissor) SDL_SetGPUScissor(pass, scissor);

        const SDL_GPUTextureSamplerBinding binding{source, sampler_};  // nearest, CLAMP_TO_EDGE
        if (effectUsesCustomShader(effect)) {
            const auto id = static_cast<std::size_t>(effect.customShader);
            SDL_BindGPUGraphicsPipeline(pass, blend ? customBlend_[id] : customReplace_[id]);
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            // The row-data store — always bound (the pipeline declares one fragment storage texture; the
            // 1×1 default covers an effect with no table). This effect's table location rides the engine
            // cbuffer so paramRow / paramRowAtUv read the right rows.
            SDL_BindGPUFragmentStorageTextures(pass, 0, &rowDataStore_, 1);
            const auto locIt = rowTableLocs.find(&effect);
            const RowTableLoc loc = locIt != rowTableLocs.end() ? locIt->second : RowTableLoc{};
            // Slot 0 — the engine cbuffer: the edge mode sampleSource() obeys, from the effect's `edge`
            // (Blank ⇒ transparent outside the frame, the default; Stretch ⇒ clamp; the layer decides it),
            // this effect's row-table location (storeY, rows; rows == 0 ⇒ no table), and the evaluation
            // grid (snap + viewport dims) the generated wrapper snaps by.
            const EngineEffectFragUniforms eng{
                effect.edge == DisplacementEdge::Stretch ? 1u : 0u, loc.storeY, loc.rows,
                snap ? 1u : 0u,
                static_cast<float>(viewport_.width), static_cast<float>(viewport_.height),
                0.0f, 0.0f};
            SDL_PushGPUFragmentUniformData(cmd, 0, &eng, sizeof(eng));
            // Slot 1 — the shader's OWN cbuffer, filled by its generated packer from the effect's inline
            // param fields (custom_effect_packers.h). A parameterless shader (null packer) pushes nothing.
            const EffectPacker packer = id < customPackers_.size() ? customPackers_[id] : nullptr;
            if (packer) {
                std::byte ubuf[kMaxCustomEffectUniformBytes];
                const std::uint32_t usize = packer(effect, ubuf);
                if (usize > 0) SDL_PushGPUFragmentUniformData(cmd, 1, ubuf, usize);
            }
        } else if (effect.kind == ScreenSpaceEffectKind::Ripple) {
            const RippleParams p = rippleParams(effect, PixelSize{viewport_.width, viewport_.height});
            const RippleFragUniforms ru{p.centerU, p.centerV, p.amplitude, p.frequency,
                                        p.phase, p.invViewportW, p.invViewportH, p.decay,
                                        snapF, 0.0f, 0.0f, 0.0f};
            SDL_BindGPUGraphicsPipeline(pass, blend ? rippleBlend_ : ripple_);
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_PushGPUFragmentUniformData(cmd, 0, &ru, sizeof(ru));
        } else if (effect.kind == ScreenSpaceEffectKind::Swirl) {
            // The centre and radius stay in viewport pixels; the twist arrives already in radians from
            // swirlParams (the public surface is degrees). The inverse viewport carries the px<->UV scale.
            const SwirlParams p = swirlParams(effect);
            const float invW = viewport_.width  > 0 ? 1.0f / static_cast<float>(viewport_.width)  : 0.0f;
            const float invH = viewport_.height > 0 ? 1.0f / static_cast<float>(viewport_.height) : 0.0f;
            const SwirlFragUniforms wu{p.centerX, p.centerY, p.twist, p.radius,
                                       invW, invH, snapF, 0.0f};
            SDL_BindGPUGraphicsPipeline(pass, blend ? swirlBlend_ : swirl_);
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_PushGPUFragmentUniformData(cmd, 0, &wu, sizeof(wu));
        } else if (effect.kind == ScreenSpaceEffectKind::ColorFill) {
            const ColorFillParams p = colorFillParams(effect);
            const ColorFillFragUniforms cu{p.r, p.g, p.b, 0.0f};
            SDL_BindGPUGraphicsPipeline(pass, blend ? colorFillBlend_ : colorFill_);
            SDL_PushGPUFragmentUniformData(cmd, 0, &cu, sizeof(cu));  // no source: a fill reads nothing
        } else if (effect.kind == ScreenSpaceEffectKind::Gleam) {
            const GleamParams p = gleamParams(effect);
            const GleamFragUniforms gu{p.sweep, p.width, p.gain, p.slant};
            SDL_BindGPUGraphicsPipeline(pass, blend ? gleamBlend_ : gleam_);
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_PushGPUFragmentUniformData(cmd, 0, &gu, sizeof(gu));
        } else if (effect.kind == ScreenSpaceEffectKind::ColorSaturation) {
            const SaturationParams p = saturationParams(effect);
            const SaturationFragUniforms su{p.saturation, 0.0f, 0.0f, 0.0f};
            SDL_BindGPUGraphicsPipeline(pass, blend ? saturationBlend_ : saturation_);
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_PushGPUFragmentUniformData(cmd, 0, &su, sizeof(su));
        } else {
            const DisplaceParams p =
                displaceParams(effect, PixelSize{viewport_.width, viewport_.height}, blankTransparent);
            const DisplaceFragUniforms du{p.amplitude, p.frequency, p.phase, p.axis,
                                          p.invViewportW, p.invViewportH, p.edge, p.blankTransparent,
                                          snapF, 0.0f, 0.0f, 0.0f};
            SDL_BindGPUGraphicsPipeline(pass, blend ? displaceBlend_ : displace_);
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_PushGPUFragmentUniformData(cmd, 0, &du, sizeof(du));
        }
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);  // one fullscreen triangle
        SDL_EndGPURenderPass(pass);
    };

    // The region gate: read `eff` (the full-frame effect result, t0) + `source` (the
    // original, t1) and write `inside(region) ? eff : src`. `blend` picks regionSelectBlend_ (the
    // premultiplied-over Layer-scope composite onto target_) vs regionSelect_ (replace, for frame-level
    // + Below). Only invoked when region.hasRegion(); the eff buffer is produced by a prior runEffect.
    // No effect shader is touched — the gate is uniform across built-in and Custom effect kinds.
    auto runRegionSelect = [&](SDL_GPUTexture* dest, SDL_GPUTexture* eff, SDL_GPUTexture* source,
                               const ShapePoints& region, float alpha, BlendMode mode, bool blend,
                               SDL_GPULoadOp loadOp) {
        SDL_GPUColorTargetInfo t{};
        t.texture     = dest;
        t.clear_color = kBackdropClear;
        t.load_op     = loadOp;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);

        const SDL_GPUTextureSamplerBinding binds[2] = {{eff, sampler_}, {source, sampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, binds, 2);

        // The boundary's evaluation path (regionCurvePath): an analytic linear+quadratic curve takes the curve
        // pipelines + cbuffer (exact, no facets); a cubic / arbitrary curve WITH a baked mask samples that mask
        // texture (t2, bilinear); a cubic curve with no mask, or a curve-free region, takes the polygon
        // pipelines (the latter byte-identical to the shipped path). `mode` is the owning Region's blend mode.
        switch (regionCurvePath(region)) {
            case CurveRegionPath::Analytic: {
                const CurveRegionSelectFragUniforms cu = makeCurveRegionUniforms(region, viewport_, alpha, mode, snapF);
                SDL_BindGPUGraphicsPipeline(pass, blend ? regionSelectCurveBlend_ : regionSelectCurve_);
                SDL_PushGPUFragmentUniformData(cmd, 0, &cu, sizeof(cu));
                break;
            }
            case CurveRegionPath::Mask: {
                const CurveMaskEntry& m = curveMasks_[static_cast<std::uint32_t>(region.curveMask) - 1];
                const SDL_GPUTextureSamplerBinding maskBind{m.texture, bilinear_};  // linear, CLAMP_TO_EDGE
                SDL_BindGPUFragmentSamplers(pass, 2, &maskBind, 1);
                const CurveMaskSelectFragUniforms cu =
                    makeCurveMaskSelectUniforms(region, m.bakeMin, m.bakeExtent, viewport_, alpha, mode, snapF);
                SDL_BindGPUGraphicsPipeline(pass, blend ? regionSelectCurveMaskBlend_ : regionSelectCurveMask_);
                SDL_PushGPUFragmentUniformData(cmd, 0, &cu, sizeof(cu));
                break;
            }
            default: {  // Polygon or SampledPolygon (a cubic curve with no mask is sampled to a faceted polygon)
                const RegionSelectFragUniforms ru = region.curve.empty()
                    ? makeRegionUniforms(region, viewport_, alpha, mode, snapF)
                    : makeRegionUniforms(sampleCurveRegionToPolygon(region), viewport_, alpha, mode, snapF);
                SDL_BindGPUGraphicsPipeline(pass, blend ? regionSelectBlend_ : regionSelect_);
                SDL_PushGPUFragmentUniformData(cmd, 0, &ru, sizeof(ru));
                break;
            }
        }
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    };

    // The blend composite: read the accumulator `dst` (t0) + a container's isolated render `src` (t1) and
    // write applyBlendMode(dst, src, mode) into `dest` (a REPLACE pass — the full blended RGBA, which the
    // caller swaps into the accumulator). The programmable peer of the fixed-function premultiplied-over
    // composite, used where a container's blend mode is not Normal. Mirrors retropp::applyBlendMode.
    auto runBlendComposite = [&](SDL_GPUTexture* dest, SDL_GPUTexture* dst, SDL_GPUTexture* src,
                                 BlendMode mode, SDL_GPULoadOp loadOp) {
        SDL_GPUColorTargetInfo t{};
        t.texture     = dest;
        t.clear_color = kBackdropClear;
        t.load_op     = loadOp;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);

        const SDL_GPUTextureSamplerBinding binds[2] = {{dst, sampler_}, {src, sampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, binds, 2);
        const float bu[4] = {static_cast<float>(mode), 0.0f, 0.0f, 0.0f};  // BlendUniforms: x = mode
        SDL_BindGPUGraphicsPipeline(pass, blend_);
        SDL_PushGPUFragmentUniformData(cmd, 0, bu, sizeof(bu));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    };

    // The stencil pass: read ONE `source` (t0) and write `source × survival` — erasing the source's own
    // pixels in/around `region` per `mode`/`feather` (TransparentInside punches a hole, TransparentOutside keeps the
    // inside). `blend` picks regionStencilBlend_ (premultiplied-over composite onto target_, Layer scope)
    // vs regionStencil_ (replace, frame-level + Below). An analytic curve boundary takes the curve
    // pipelines + cbuffer; a curve-free region OR a cubic boundary (sampled to a polygon here) takes the
    // polygon pipelines. A single-texture pass distinct from runRegionSelect (which selects between two
    // textures) — Stencil produces transparency, so there is no effect result to gate.
    auto runStencil = [&](SDL_GPUTexture* dest, SDL_GPUTexture* source, const ShapePoints& region,
                          StencilMode mode, float feather, bool blend, SDL_GPULoadOp loadOp) {
        SDL_GPUColorTargetInfo t{};
        t.texture     = dest;
        t.clear_color = kBackdropClear;
        t.load_op     = loadOp;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);

        const SDL_GPUTextureSamplerBinding binding{source, sampler_};  // nearest, CLAMP_TO_EDGE
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

        switch (regionCurvePath(region)) {
            case CurveRegionPath::Analytic: {
                const CurveStencilFragUniforms cu = makeCurveStencilUniforms(region, mode, feather, viewport_, snapF);
                SDL_BindGPUGraphicsPipeline(pass, blend ? regionStencilCurveBlend_ : regionStencilCurve_);
                SDL_PushGPUFragmentUniformData(cmd, 0, &cu, sizeof(cu));
                break;
            }
            case CurveRegionPath::Mask: {
                const CurveMaskEntry& m = curveMasks_[static_cast<std::uint32_t>(region.curveMask) - 1];
                const SDL_GPUTextureSamplerBinding maskBind{m.texture, bilinear_};  // linear, CLAMP_TO_EDGE
                SDL_BindGPUFragmentSamplers(pass, 1, &maskBind, 1);
                const CurveMaskStencilFragUniforms cu =
                    makeCurveMaskStencilUniforms(region, m.bakeMin, m.bakeExtent, mode, feather, viewport_, snapF);
                SDL_BindGPUGraphicsPipeline(pass, blend ? regionStencilCurveMaskBlend_ : regionStencilCurveMask_);
                SDL_PushGPUFragmentUniformData(cmd, 0, &cu, sizeof(cu));
                break;
            }
            default: {  // Polygon or SampledPolygon (a cubic curve with no mask is sampled to a faceted polygon)
                const StencilFragUniforms su = region.curve.empty()
                    ? makeStencilUniforms(region, mode, feather, viewport_, snapF)
                    : makeStencilUniforms(sampleCurveRegionToPolygon(region), mode, feather, viewport_, snapF);
                SDL_BindGPUGraphicsPipeline(pass, blend ? regionStencilBlend_ : regionStencil_);
                SDL_PushGPUFragmentUniformData(cmd, 0, &su, sizeof(su));
                break;
            }
        }
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    };

    // One confined-effect application: an effect plus the shape it is confined to. `confined == false`
    // is the whole-reach case (a DrawLayer::effects-chain effect / FrameDrawState::postEffects — no shape).
    // A Region's effects become confined steps (shape = the region's shape). The shape is held by value so
    // an inverted() temporary outlives the step.
    struct ConfinedStep {
        const ScreenSpaceEffect* eff;
        bool                     confined;
        ShapePoints              shape;
        float                    alpha;     // the owning Region's alpha (opacity of its effects); 1 = full
        BlendMode                blend;     // the owning Region's blend mode (Normal for a whole-reach effect)
    };

    // One batched additive run, resolved for issue: the pipeline stage, the pooled instance-buffer slot
    // holding its per-region records, the record count, the packed shader-uniform bytes (all steps in a
    // run share them), and the CPU records (uploaded in the second copy pass). Built in the pre-pass.
    struct BatchRun {
        std::uint32_t               stage = 0;
        int                         slot  = 0;
        int                         count = 0;
        std::vector<std::byte>      params;
        std::vector<GpuRegionBatch> records;
    };
    // A site's batching plan: the run grouping (stepRun disposition parallel to the site's step list) and
    // the resolved runs (parallel to grouping.runs). At step i, stepRun[i] < 0 ⇒ the existing per-step
    // path; else the step belongs to a run, issued once at the run's first step.
    struct SiteBatch {
        RegionBatchGrouping   grouping;
        std::vector<BatchRun> runs;
    };

    // One resolved gather run: the pipeline stage (a custom handle, or kColorFillGatherStage for the
    // built-in ColorFill run), its edge mode (for the engine cbuffer — a run's regions are same-stage
    // confined effects, expected to share an edge; the first step's is used; ColorFill never resamples,
    // so its runs leave it 0), the pooled buffer slot holding the concatenated per-region records, the
    // region count (the gather entry point's loop bound / RetroppGatherInfo), the record stride in
    // float4s (ColorFill runs only — a custom variant bakes its stride as a compile-time constant and
    // carries 0), and the record bytes (uploaded in the copy pass). Built in the pre-pass. Named
    // distinctly from postprocess::GatherRun (the grouping's run) — this is the GPU-side peer.
    struct GatherRunGpu {
        std::uint32_t          stage  = 0;
        std::uint32_t          edge   = 0;   // ScreenSpaceEffect::edge as a uint (0 = Blank, 1 = Stretch)
        std::uint32_t          stride = 0;   // record stride in float4s (ColorFill runs; 0 for custom)
        int                    slot   = 0;
        int                    count  = 0;   // per-region record count == uRegionCount
        IntRect                unionBox{};   // union of the records' covering quads, compose px — the
                                             // replace pass shades only this (source is pre-copied to
                                             // the destination); the full compose rect disables it
        std::vector<std::byte> bytes;        // count records concatenated, one stride each
    };
    // A site's gather plan: the run grouping (stepRun parallel to the site's step list) + the resolved runs.
    // Disjoint from SiteBatch by stage class — a step belongs to at most one of the two dispositions.
    struct SiteGather {
        GatherGrouping            grouping;
        std::vector<GatherRunGpu> runs;
    };

    // Append the confined step for ONE effect. Every effect is region-agnostic: it confines to `defaultShape`
    // (the owning Region's shape) when it has one, else it is whole-reach. `regionAlpha` is the owning
    // Region's opacity (1 for a whole-reach effect). A Transparency is no exception — its shape comes from
    // its Region just like a colour effect's. None / invalid-Custom effects are dropped.
    auto appendEffectSteps = [&](std::vector<ConfinedStep>& steps, const ScreenSpaceEffect& e,
                                 bool hasDefaultShape, const ShapePoints& defaultShape, float regionAlpha,
                                 BlendMode regionBlend) {
        if (e.kind == ScreenSpaceEffectKind::None || !effectRenderable(e)) return;
        if (hasDefaultShape) {
            // A region with no shape is a WHOLE-VIEWPORT region: synthesize a viewport-covering rectangle so
            // it flows through the same gate as a shaped region and honours its blend + alpha across the whole
            // frame (a shaped region keeps its own shape). Inflated a few px so every viewport pixel is
            // strictly inside the gate. This is what makes a whole-frame colour grade / flash / fade — a
            // ColorFill region with no shape — composite correctly.
            ShapePoints shape = defaultShape.hasRegion()
                ? defaultShape
                : ShapePoints::rectangle(Point{-4.0f, -4.0f},
                                         static_cast<float>(viewport_.width) + 8.0f,
                                         static_cast<float>(viewport_.height) + 8.0f);
            steps.push_back({&e, true, std::move(shape), regionAlpha, regionBlend});
        } else {
            steps.push_back({&e, false, {}, 1.0f, BlendMode::Normal});  // whole-reach (no shape; uses frame/layer blend)
        }
    };

    // A layer's confined-step list: each whole-reach effect in the layer's effects chain (in order), then
    // each region's effects (confined to that region's shape, at that region's alpha). The per-layer loop
    // partitions the result by scope (Layer vs Below) — so a Below-scope step (e.g. a transparency side
    // effect) runs on the accumulator after the layer composites, reaching the layers showing through.
    auto buildSteps = [&](const DrawLayer& layer) {
        std::vector<ConfinedStep> steps;
        for (const ScreenSpaceEffect& e : layer.effects)
            appendEffectSteps(steps, e, /*hasDefaultShape=*/false, {}, 1.0f, BlendMode::Normal);
        for (const Region& region : layer.regions)
            for (const ScreenSpaceEffect& e : region.effects)
                appendEffectSteps(steps, e, /*hasDefaultShape=*/true, region.shape, region.alpha, region.blend);
        return steps;
    };

    // ── Region batching (the instanced-additive fast path) ─────────────────────────────────────
    //
    // A running count of per-run instance-buffer slots claimed this frame (buildBatchPlan assigns one per
    // run; the second copy pass uploads each run's records into batchInstanceBufs_[slot]).
    int batchSlotCount = 0;

    // Pack a shape's covering quad + spine + radius into the GPU record (px→uv for the box, viewport-px
    // spine/radius). The single px→uv authority is regionBatchInstance's box == regionScissorRect.
    auto toGpuRegionBatch = [&](const RegionBatchInstance& in) {
        const float cw = static_cast<float>(composeW_ > 0 ? composeW_ : 1);
        const float ch = static_cast<float>(composeH_ > 0 ? composeH_ : 1);
        GpuRegionBatch g{};
        g.uvBox[0] = static_cast<float>(in.box.x) / cw;
        g.uvBox[1] = static_cast<float>(in.box.y) / ch;
        g.uvBox[2] = static_cast<float>(in.box.x + in.box.width)  / cw;
        g.uvBox[3] = static_cast<float>(in.box.y + in.box.height) / ch;
        g.spine[0] = in.p0.x; g.spine[1] = in.p0.y; g.spine[2] = in.p1.x; g.spine[3] = in.p1.y;
        g.radiusPad[0] = in.radius;
        return g;
    };

    // Resolve a site's confined steps into a SiteBatch: per-step batch keys (the postprocess eligibility
    // predicate AND a batched pipeline for the stage), grouped by (stage, packed params) within contiguous
    // eligible stretches (postprocess::groupRegionBatches), then each ≥2 group turned into a BatchRun with
    // its instance records + a claimed buffer slot. Pure CPU; records upload in the second copy pass.
    auto buildBatchPlan = [&](const std::vector<ConfinedStep>& steps) -> SiteBatch {
        SiteBatch sb;
        if (steps.empty()) return sb;
        std::vector<std::array<std::byte, kMaxCustomEffectUniformBytes>> paramBufs(steps.size());
        std::vector<std::uint32_t> paramLens(steps.size(), 0);
        std::vector<BatchStep>     keys(steps.size());
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const ConfinedStep& s = steps[i];
            if (!s.confined || !s.shape.hasRegion()) continue;
            if (!regionBatchEligible(*s.eff, s.shape, s.alpha, s.blend)) continue;
            const auto id = static_cast<std::size_t>(s.eff->customShader);
            if (id >= customBatched_.size() || customBatched_[id] == nullptr) continue;  // stage not additive
            keys[i].eligible = true;
            keys[i].stage    = static_cast<std::uint32_t>(id);
            const EffectPacker packer = id < customPackers_.size() ? customPackers_[id] : nullptr;
            if (packer) paramLens[i] = packer(*s.eff, paramBufs[i].data());
            keys[i].params = std::span<const std::byte>(paramBufs[i].data(), paramLens[i]);
        }
        sb.grouping = groupRegionBatches(keys);
        sb.runs.reserve(sb.grouping.runs.size());
        for (const RegionBatchRun& run : sb.grouping.runs) {
            BatchRun br;
            br.stage = run.stage;
            br.slot  = batchSlotCount++;
            br.count = static_cast<int>(run.steps.size());
            const std::uint32_t first = run.steps.front();  // all steps share (stage, params) by construction
            br.params.assign(paramBufs[first].begin(), paramBufs[first].begin() + paramLens[first]);
            br.records.reserve(run.steps.size());
            for (const std::uint32_t si : run.steps)
                br.records.push_back(toGpuRegionBatch(
                    regionBatchInstance(steps[si].shape, composeScale_, composeW_, composeH_)));
            sb.runs.push_back(std::move(br));
        }
        return sb;
    };

    // Resolve a site's confined steps into a SiteGather: per-step gather keys (the eligibility predicate
    // AND a gather pipeline for the stage — a custom stage's registered pair, or the built-in ColorFill
    // pair under its reserved stage id), grouped into maximal contiguous same-stage stretches
    // (postprocess::groupGatherRuns), then each ≥2 run turned into a GatherRunGpu with its concatenated
    // per-region records + a claimed buffer slot. A custom record is the 48-byte header + the step's OWN
    // packed params (per-region params are the point, so they ride the records); a ColorFill record is
    // the region's full gate state — fill, alpha, blend mode, inverse homography, vertices — at the run's
    // uniform stride. Pure CPU; records upload in the copy pass.
    // Grow `u` to cover `b` — the run's union box accumulates over its records' covering quads.
    auto growUnion = [](IntRect& u, const IntRect& b) {
        if (u.width <= 0 || u.height <= 0) { u = b; return; }
        const int x0 = std::min(u.x, b.x), y0 = std::min(u.y, b.y);
        const int x1 = std::max(u.x + u.width, b.x + b.width);
        const int y1 = std::max(u.y + u.height, b.y + b.height);
        u = IntRect{x0, y0, x1 - x0, y1 - y0};
    };

    auto buildGatherPlan = [&](const std::vector<ConfinedStep>& steps) -> SiteGather {
        SiteGather sg;
        if (steps.empty()) return sg;
        std::vector<std::array<std::byte, kMaxCustomEffectUniformBytes>> paramBufs(steps.size());
        std::vector<std::uint32_t> paramLens(steps.size(), 0);
        std::vector<GatherStep>    keys(steps.size());
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const ConfinedStep& s = steps[i];
            if (!s.confined || !s.shape.hasRegion()) continue;
            if (s.eff->kind == ScreenSpaceEffectKind::ColorFill) {
                // Built-in ColorFill gathers under its reserved stage id, with wider eligibility than a
                // custom stage: alpha, blend mode, invert, stroke, and transform all ride the record.
                if (!colorFillGatherEligible(*s.eff, s.shape) || colorFillGather_ == nullptr) continue;
                keys[i].eligible = true;
                keys[i].stage    = kColorFillGatherStage;
                continue;
            }
            if (!gatherEligible(*s.eff, s.shape, s.alpha, s.blend)) continue;
            const auto id = static_cast<std::size_t>(s.eff->customShader);
            if (id >= customGather_.size() || customGather_[id] == nullptr) continue;  // stage does not gather
            keys[i].eligible = true;
            keys[i].stage    = static_cast<std::uint32_t>(id);
            const EffectPacker packer = id < customPackers_.size() ? customPackers_[id] : nullptr;
            if (packer) paramLens[i] = packer(*s.eff, paramBufs[i].data());
        }
        sg.grouping = groupGatherRuns(keys);
        sg.runs.reserve(sg.grouping.runs.size());
        for (const GatherRun& run : sg.grouping.runs) {
            GatherRunGpu gr;
            gr.stage = run.stage;
            gr.slot  = batchSlotCount++;
            gr.count = static_cast<int>(run.steps.size());
            const std::uint32_t first = run.steps.front();
            if (run.stage == kColorFillGatherStage) {
                // The run's record stride comes from its largest (post-truncation) vertex count, so
                // every record in the storage buffer indexes uniformly.
                std::size_t maxPts = 0;
                for (const std::uint32_t si : run.steps)
                    maxPts = std::max(maxPts,
                                      std::min(steps[si].shape.points.size(), kColorFillGatherMaxPoints));
                gr.stride = colorFillGatherStrideFloat4s(maxPts);
                for (const std::uint32_t si : run.steps) {
                    const ConfinedStep&       s  = steps[si];
                    const RegionBatchInstance in =
                        regionBatchInstance(s.shape, composeScale_, composeW_, composeH_);
                    growUnion(gr.unionBox, in.box);
                    const std::vector<std::byte> rec = colorFillGatherRecordBytes(
                        s.shape, *s.eff, s.alpha, s.blend,
                        PixelSize{viewport_.width, viewport_.height}, in, composeW_, composeH_,
                        gr.stride);
                    gr.bytes.insert(gr.bytes.end(), rec.begin(), rec.end());
                }
            } else {
                gr.edge = static_cast<std::uint32_t>(steps[first].eff->edge);
                for (const std::uint32_t si : run.steps) {
                    const RegionBatchInstance in =
                        regionBatchInstance(steps[si].shape, composeScale_, composeW_, composeH_);
                    growUnion(gr.unionBox, in.box);
                    const std::vector<std::byte> rec = gatherRecordBytes(
                        in, composeW_, composeH_,
                        std::span<const std::byte>(paramBufs[si].data(), paramLens[si]));
                    gr.bytes.insert(gr.bytes.end(), rec.begin(), rec.end());
                }
            }
            sg.runs.push_back(std::move(gr));
        }
        return sg;
    };

    // Issue ONE batched additive pass: bind the stage's batched pipeline, the run's instance buffer (vertex
    // storage t0 space0), the zero source + row-data store (fragment t0/t1 space2) + the engine cbuffer
    // (rows 0) and the shader's own params, then draw 6 × count instances onto `dest`. `loadOp` LOADs the
    // destination so the ADDITIVE blend accumulates each region's delta in place. No gate pass, no scissor.
    auto runBatch = [&](SDL_GPUTexture* dest, SDL_GPULoadOp loadOp, const BatchRun& run) {
        if (run.count <= 0 || run.stage >= customBatched_.size() || customBatched_[run.stage] == nullptr) return;
        SDL_GPUBuffer* buf = run.slot < static_cast<int>(batchInstanceBufs_.size())
                                 ? batchInstanceBufs_[run.slot] : nullptr;
        if (!buf) return;
        SDL_GPUColorTargetInfo t{};
        t.texture     = dest;
        t.clear_color = kBackdropClear;
        t.load_op     = loadOp;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, customBatched_[run.stage]);
        SDL_BindGPUVertexStorageBuffers(pass, 0, &buf, 1);
        const SDL_GPUTextureSamplerBinding binding{batchZeroSource_, sampler_};  // zero source ⇒ delta only
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_BindGPUFragmentStorageTextures(pass, 0, &rowDataStore_, 1);  // bound but unread (rows = 0)
        // The engine cbuffer: no row table (rows 0; eligibility forbids a paramTable), the evaluation grid
        // + viewport dims the batched entry point's gate + snap use. Edge is irrelevant — the zero source
        // samples 0 in and out of bounds.
        const EngineEffectFragUniforms eng{
            0u, 0u, 0u, snap ? 1u : 0u,
            static_cast<float>(viewport_.width), static_cast<float>(viewport_.height), 0.0f, 0.0f};
        SDL_PushGPUFragmentUniformData(cmd, 0, &eng, sizeof(eng));
        if (!run.params.empty())
            SDL_PushGPUFragmentUniformData(cmd, 1, run.params.data(),
                                           static_cast<Uint32>(run.params.size()));
        SDL_DrawGPUPrimitives(pass, 6, static_cast<Uint32>(run.count), 0, 0);
        SDL_EndGPURenderPass(pass);
    };

    // Issue ONE gather pass: a fullscreen triangle that reads `source` (the previous image, bound as
    // the REAL SourceTexture — unlike the additive zero-source pass) + the run's per-region records (fragment storage
    // buffer t0) and writes the union-shape result to `dest`. Per fragment the generated entry tests every
    // record's gate (uv-bbox reject → n≤2 SDF, LAST region wins) and either passes the source through
    // (outside every shape, byte-exact) or evaluates the shader with the winning region's params loaded.
    // `blend`: false = replace (frame-level / Below / mid-chain); true = premultiplied-over composite onto
    // target_ (a Normal layer's last step), with `loadOp` LOADing the accumulator beneath.
    auto runGather = [&](SDL_GPUTexture* dest, SDL_GPUTexture* source, const GatherRunGpu& run, bool blend,
                         SDL_GPULoadOp loadOp) {
        if (run.count <= 0) return;
        const bool builtinColorFill = (run.stage == kColorFillGatherStage);
        SDL_GPUGraphicsPipeline* pipe = nullptr;
        if (builtinColorFill) {
            pipe = blend ? colorFillGatherBlend_ : colorFillGather_;
        } else {
            if (run.stage >= customGather_.size() || customGather_[run.stage] == nullptr) return;
            pipe = blend ? customGatherBlend_[run.stage] : customGather_[run.stage];
        }
        if (!pipe) return;
        SDL_GPUBuffer* buf = run.slot < static_cast<int>(batchInstanceBufs_.size())
                                 ? batchInstanceBufs_[run.slot] : nullptr;
        if (!buf) return;
        // A REPLACE pass whose records' union covers less than the compose target shades only the
        // union: the source is first copied to the destination (a blit-engine copy, no fragment work),
        // the pass LOADs it, and a scissor confines the per-pixel record walk. Pixels outside the union
        // hold the source bytes exactly — the same value the pass-through arm writes, so output is
        // unchanged and the walk cost tracks the shapes' coverage, not the frame. The premultiplied-over
        // blend pass composites the whole isolated image onto its target, so it stays fullscreen.
        const bool scissored = !blend &&
            !(run.unionBox.x == 0 && run.unionBox.y == 0 &&
              run.unionBox.width >= composeW_ && run.unionBox.height >= composeH_);
        if (scissored) {
            SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureLocation csrc{};
            csrc.texture = source;
            SDL_GPUTextureLocation cdst{};
            cdst.texture = dest;
            SDL_CopyGPUTextureToTexture(cp, &csrc, &cdst, static_cast<Uint32>(composeW_),
                                        static_cast<Uint32>(composeH_), 1, false);
            SDL_EndGPUCopyPass(cp);
        }
        SDL_GPUColorTargetInfo t{};
        t.texture     = dest;
        t.clear_color = kBackdropClear;
        t.load_op     = scissored ? SDL_GPU_LOADOP_LOAD : loadOp;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        if (scissored) {
            const SDL_Rect sc{run.unionBox.x, run.unionBox.y, run.unionBox.width, run.unionBox.height};
            SDL_SetGPUScissor(pass, &sc);
        }
        SDL_BindGPUGraphicsPipeline(pass, pipe);
        const SDL_GPUTextureSamplerBinding binding{source, sampler_};  // the real previous image (nearest CLAMP)
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        // Both gather pipelines declare the row-data store for layout parity (unread — a custom run's
        // eligibility forbids a paramTable, and ColorFill has none).
        SDL_BindGPUFragmentStorageTextures(pass, 0, &rowDataStore_, 1);
        SDL_BindGPUFragmentStorageBuffers(pass, 0, &buf, 1);            // the run's per-region records
        // The engine cbuffer (b0): the run's edge for sampleSource (0 for ColorFill, which never
        // resamples), no row table, the evaluation grid + viewport dims the entry point's gate + snap use.
        const EngineEffectFragUniforms eng{
            run.edge, 0u, 0u, snap ? 1u : 0u,
            static_cast<float>(viewport_.width), static_cast<float>(viewport_.height), 0.0f, 0.0f};
        SDL_PushGPUFragmentUniformData(cmd, 0, &eng, sizeof(eng));
        // b1 = the run header: region count + the ColorFill record stride (0 on a custom run, whose
        // generated variant bakes its stride as a compile-time constant).
        const GpuGatherInfo info{static_cast<std::uint32_t>(run.count), run.stride, 0u, 0u};
        SDL_PushGPUFragmentUniformData(cmd, 1, &info, sizeof(info));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);  // one fullscreen triangle
        SDL_EndGPURenderPass(pass);
    };

    // Apply one Below-scope confined step to the WHOLE accumulator (target_): transform it (or make it
    // see-through) confined to the step's shape into layerScratch_, then swap it in — this layer's content
    // (already composited
    // into target_) AND everything beneath it. The single-step case is the plain whole-layer Below composite.
    auto applyBelowStep = [&](const ConfinedStep& s) {
        if (s.eff->kind == ScreenSpaceEffectKind::Transparency) {
            runStencil(layerScratch_, target_, s.shape, s.eff->stencil, s.eff->feather,
                       /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
        } else if (s.confined && s.shape.hasRegion()) {
            const IntRect  r = regionScissorRect(s.shape, composeScale_, composeW_, composeH_);
            const SDL_Rect sc{r.x, r.y, r.width, r.height};
            runEffect(post0_, target_, *s.eff, /*blankTransparent=*/false, /*blend=*/false,
                      SDL_GPU_LOADOP_DONT_CARE, &sc);
            runRegionSelect(layerScratch_, post0_, target_, s.shape, s.alpha, s.blend, /*blend=*/false,
                            SDL_GPU_LOADOP_DONT_CARE);
        } else {
            runEffect(layerScratch_, target_, *s.eff, /*blankTransparent=*/false, /*blend=*/false,
                      SDL_GPU_LOADOP_DONT_CARE);
        }
        std::swap(target_, layerScratch_);
    };

    // Apply an isolated layer's confined-step chain on its own premultiplied scratch (starting at
    // `startTex`, where renderLayerIsolated left the layer content — layerScratch_ for the usual layer, a
    // ping-pong spare when a mixed-blend sprite layer's run composite finished there): each step replaces
    // into a ping-pong scratch (so step n+1 sees step n's output). When the layer's blend is Normal, the
    // LAST step composites premultiplied-over target_ (the byte-identical alpha-over path). When the layer's
    // blend is NOT Normal, every step replaces into a scratch and the finished isolated image is composited
    // onto the accumulator with `layerBlend` (the programmable blend composite) — the accumulator must
    // already hold the layers beneath (the caller initializes target_ first). A single step is the plain
    // per-layer composite. The three-texture ping-pong pool is fixed {layerScratch_, post0_, post1_};
    // `startTex` is always one of them, so `other()` still finds two free peers.
    auto applyLayerChain = [&](const std::vector<ConfinedStep>& steps, const SiteBatch& batch,
                               const SiteGather& gather, BlendMode layerBlend, SDL_GPULoadOp compositeLoad,
                               SDL_GPUTexture* startTex) {
        const bool blendComposite = (layerBlend != BlendMode::Normal);
        SDL_GPUTexture* pool[3] = {layerScratch_, post0_, post1_};
        auto other = [&](SDL_GPUTexture* a, SDL_GPUTexture* b) -> SDL_GPUTexture* {
            for (SDL_GPUTexture* t : pool)
                if (t != a && t != b) return t;
            return pool[0];
        };
        const std::size_t n = steps.size();
        // A Normal layer's LAST step composites premultiplied-over target_ — but ONLY when that last step is
        // a solo (per-region) step OR a GATHER run (both write a fresh image, so the blend pipeline can
        // composite it straight onto target_). When the last step belongs to a additive batched run (which
        // blends in place onto `cur`, never to target_), no per-step composite runs, so a final
        // premultiplied-over composite of `cur` follows the loop (see below). `lastIsAdditiveRun` — the
        // additive grouping's stepRun[n-1] ≥ 0 — is that one case; every other last step composites in-loop.
        const bool lastIsAdditiveRun = (n > 0) && (batch.grouping.stepRun[n - 1] >= 0);
        SDL_GPUTexture* cur = startTex;
        for (std::size_t i = 0; i < n; ++i) {
            const int rn = batch.grouping.stepRun[i];
            if (rn >= 0) {  // additive batched run: additive in place onto cur (no ping-pong advance), once per run
                if (i == batch.grouping.runs[rn].steps.front())
                    runBatch(cur, SDL_GPU_LOADOP_LOAD, batch.runs[rn]);
                continue;
            }
            const int rnG = gather.grouping.stepRun[i];
            if (rnG >= 0) {  // gather run: a replace step (reads cur, writes a fresh image), once per run
                const std::vector<std::uint32_t>& runSteps = gather.grouping.runs[rnG].steps;
                if (i == runSteps.front()) {
                    // The run occupies its member positions; it is the layer's last content iff its LAST
                    // member is the last step. If so (and Normal), it composites premultiplied-over target_.
                    const bool          runIsLast = (runSteps.back() == n - 1);
                    const bool          toTarget  = runIsLast && !blendComposite;
                    const SDL_GPULoadOp lop       = toTarget ? compositeLoad : SDL_GPU_LOADOP_DONT_CARE;
                    SDL_GPUTexture*     dest       = toTarget ? target_ : other(cur, cur);
                    runGather(dest, cur, gather.runs[rnG], /*blend=*/toTarget, lop);
                    if (!toTarget) cur = dest;
                }
                continue;
            }
            const ConfinedStep& s    = steps[i];
            const bool          last = (i + 1 == n);
            // Composite-over target_ only on the last step of a Normal layer (a solo step; a last gather run
            // is handled in its own branch above, a last additive run in the post-loop composite below).
            const bool          toTarget = last && !lastIsAdditiveRun && !blendComposite;
            const SDL_GPULoadOp lop      = toTarget ? compositeLoad : SDL_GPU_LOADOP_DONT_CARE;
            if (s.eff->kind == ScreenSpaceEffectKind::Transparency) {
                SDL_GPUTexture* dest = toTarget ? target_ : other(cur, cur);
                runStencil(dest, cur, s.shape, s.eff->stencil, s.eff->feather, /*blend=*/toTarget, lop);
                if (!toTarget) cur = dest;
            } else if (s.confined && s.shape.hasRegion()) {
                SDL_GPUTexture* tmp  = other(cur, cur);
                SDL_GPUTexture* dest = toTarget ? target_ : other(cur, tmp);
                const IntRect  r = regionScissorRect(s.shape, composeScale_, composeW_, composeH_);
                const SDL_Rect sc{r.x, r.y, r.width, r.height};
                runEffect(tmp, cur, *s.eff, /*blankTransparent=*/true, /*blend=*/false,
                          SDL_GPU_LOADOP_DONT_CARE, &sc);
                runRegionSelect(dest, tmp, cur, s.shape, s.alpha, s.blend, /*blend=*/toTarget, lop);
                if (!toTarget) cur = dest;
            } else {
                SDL_GPUTexture* dest = toTarget ? target_ : other(cur, cur);
                runEffect(dest, cur, *s.eff, /*blankTransparent=*/true, /*blend=*/toTarget, lop);
                if (!toTarget) cur = dest;
            }
        }
        if (blendComposite) {
            // `cur` holds the layer's finished isolated image; composite it onto the accumulator with the mode.
            SDL_GPUTexture* dest = other(cur, target_);  // a free scratch (target_ is not in the pool)
            runBlendComposite(dest, target_, cur, layerBlend, SDL_GPU_LOADOP_DONT_CARE);
            std::swap(target_, dest);
        } else if (lastIsAdditiveRun) {
            // Normal layer whose LAST step is a additive batched run: nothing composited `cur` to target_, so do
            // it now. The isolated content is PREMULTIPLIED, so this must be the premultiplied-over composite
            // (ONE / ONE_MINUS_SRC_ALPHA), NOT runBlendComposite's straight alpha-over (which would double-
            // darken translucent edges). An empty-region gate (count 0 ⇒ pass-through) on the blend pipeline
            // IS that premultiplied-over composite. A gather last step does NOT reach here — it
            // composited onto target_ within the loop (its blend branch). One O(1) pass, run-path-only.
            runRegionSelect(target_, cur, cur, ShapePoints{}, 1.0f, BlendMode::Normal, /*blend=*/true,
                            compositeLoad);
        }
    };

    bool               targetInitialized = false;  // has target_ been cleared this frame?
    SDL_GPURenderPass* batch             = nullptr; // open target_ pass batching consecutive direct draws
    auto openBatch = [&]() {
        SDL_GPUColorTargetInfo t{};
        t.texture     = target_;
        t.clear_color = kBackdropClear;
        t.load_op     = targetInitialized ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        batch = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        targetInitialized = true;
    };
    auto closeBatch = [&]() {
        if (batch) { SDL_EndGPURenderPass(batch); batch = nullptr; }
    };
    // Clear target_ to the backdrop if no layer has yet initialized it. A blended layer composite reads the
    // accumulator as a texture, so it must hold the backdrop (the layers beneath) before the blend runs.
    auto ensureTargetInitialized = [&]() {
        if (targetInitialized) return;
        closeBatch();
        SDL_GPUColorTargetInfo t{};
        t.texture     = target_;
        t.clear_color = kBackdropClear;
        t.load_op     = SDL_GPU_LOADOP_CLEAR;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        SDL_EndGPURenderPass(p);
        targetInitialized = true;
    };
    // Adopt the texture a sprite-emission composite left the accumulator in. A composite cannot write the
    // texture it reads, so each bucket's halo lands in a free scratch and the accumulator's identity follows
    // it there. A no-op when the layer ran no bucket (the returned texture is still target_).
    auto adoptEmissionResult = [&](SDL_GPUTexture* r) {
        if (r == post0_)             std::swap(target_, post0_);
        else if (r == post1_)        std::swap(target_, post1_);
        else if (r == layerScratch_) std::swap(target_, layerScratch_);
    };
    // Below-scope sprites: render this layer's Below runs into layerScratch_ (transparent-cleared) reading the
    // accumulator (the scene beneath, bound as SourceTexture) and writing the effect-graded scene masked by
    // each sprite's art coverage. A built-in run (pipelineKey 0) draws through spriteBelow_ (ColorFill / Gleam
    // / ColorSaturation / RowDisplacement / Ripple over the scene); a custom run (key = handle+1) draws through
    // customSpriteBelow_[key-1] (the game shader distorts / grades the scene through the silhouette). Each run
    // is ONE instanced pass — N-flat; pass count tracks the below-pipeline mix. All runs read the SAME
    // pre-layer accumulator (target_) and accumulate into the scratch (first CLEAR, the rest LOAD). The scratch
    // is a PREMULTIPLIED image (the stock sprite blend state over a transparent clear), so it composites
    // premultiplied-over the accumulator (an empty-region pass-through on regionSelectBlend_ IS that
    // composite): inside a silhouette the distorted scene replaces, the transparent surround leaves the
    // accumulator byte-identical. Runs BEFORE the layer's own art draws, so the art rides on the distortion,
    // and after the layer's fields are prepared, so a lens has its halo to read.

    // One pass of the atlas chain, confined to a page's band. Same shape as runEmissionPass, with the two
    // things a shared atlas needs: a scissor, so a pass touches only the page it is blurring, and a load op,
    // so writing one band leaves the others intact — DONT_CARE over a shared target would discard them.
    auto runAtlasPass = [&](SDL_GPUTexture* dst, SDL_GPUTexture* src, SDL_GPUSampler* smp,
                            SDL_GPUGraphicsPipeline* pipe, const void* uniform, std::uint32_t uniformSize,
                            SDL_GPULoadOp loadOp, int bandY, int bandH, int dstW, int dstH) {
        if (bandH <= 0) return;
        SDL_GPUColorTargetInfo t{};
        t.texture     = dst;
        t.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
        t.load_op     = loadOp;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        const SDL_Rect scissor{0, std::max(0, bandY), dstW, std::min(bandH, dstH - std::max(0, bandY))};
        if (scissor.h > 0) SDL_SetGPUScissor(pass, &scissor);
        const SDL_GPUTextureSamplerBinding binding{src, smp};
        SDL_BindGPUGraphicsPipeline(pass, pipe);
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        if (uniform) SDL_PushGPUFragmentUniformData(cmd, 0, uniform, uniformSize);
        if (scissor.h > 0) SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    };

    // Copy a finished sheet out of the 2-D scratch into its array layer of the store. The scratch is 2-D
    // because the blur SAMPLES it and a sampled bind is a whole texture; the store is an array because the art
    // fragment reads every sheet through one binding. This pass is the seam between the two.
    auto storeEmissionSheet = [&](int sheet) {
        SDL_GPUColorTargetInfo t{};
        t.texture              = emissionAtlasStore_;
        t.layer_or_depth_plane = static_cast<Uint32>(sheet);
        t.clear_color          = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
        t.load_op              = SDL_GPU_LOADOP_DONT_CARE;  // the fullscreen triangle covers every texel
        t.store_op             = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        const SDL_GPUTextureSamplerBinding binding{emissionAtlas0_, sampler_};
        SDL_BindGPUGraphicsPipeline(pass, emissionCopy_);
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    };

    // Make this layer's emission fields, before any of them is read. Each SHEET fills in ONE render pass —
    // the below extracts, then the region rasters, every instance into its own rect — and then blurs once per
    // PAGE, the fields sharing a reach. So the pass count tracks the reaches a layer authors and the sheets it
    // needs, never its field count, and never the mix of the two kinds.
    //
    // Neither path composites here: a region's halo is the source colour its blend grades with, and a lens's
    // is a scene-sourced halo its own record adds as it samples. Both are consumed by the fragment that reads
    // them.
    //
    // It runs before this layer's below lenses draw, and marks the layer so the art pass binds the store and
    // its rect table in place of the idle stand-ins.
    auto prepareEmissionSheet = [&](std::size_t idx, std::size_t sheetIndex) {
        const EmissionSheetGpu& layerFields = layerEmissionSheets[idx][sheetIndex];
        const bool hasRegion    = layerFields.buffer && layerFields.count > 0;
        const bool hasStockBelow = layerFields.extractBuf && layerFields.extractCount > 0;
        const bool hasBelow     = hasStockBelow || !layerFields.customExtracts.empty();
        if (!hasRegion && !hasBelow) return;
        if (hasRegion && (!atlasStore_ || !paletteStore_ || !atlasRegionStore_)) return;
        // Without the atlas there is no field to make. Every seated record still reads the idle pair and adds
        // nothing, so the sprite draws un-bloomed rather than not at all.
        if (!emissionAtlas0_ || !emissionAtlas1_ || !emissionAtlasLow0_ || !emissionAtlasStore_) return;
        closeBatch();
        // The below extract reads the accumulator, which must hold the scene beneath this layer before it is
        // sampled — the same guarantee the lenses themselves need.
        if (hasBelow) ensureTargetInitialized();

        const int atlasW = emissionAtlasW_, atlasH = emissionAtlasH_;
        const int lowW   = (((atlasW + 1) / 2) + 1) / 2, lowH = (((atlasH + 1) / 2) + 1) / 2;

        {   // Fill: every field's own light, each in its own rect, in one pass.
            //
            // The whole atlas clears first. A rect's margin ring is read by its own blur — that is what makes
            // the ring the isolation — so it has to start genuinely zero, and the atlas is rewritten once per
            // layer, which puts the previous layer's light exactly where this blur would read it.
            SDL_GPUColorTargetInfo t{};
            t.texture     = emissionAtlas0_;
            t.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
            t.load_op     = SDL_GPU_LOADOP_CLEAR;
            t.store_op    = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);

            const EmissionExtractRectFragUniforms eu{static_cast<float>(composeScale_), 0.0f, 0.0f, 0.0f};
            const SDL_GPUTextureSamplerBinding    scene{target_, sampler_};
            if (hasStockBelow) {
                // The scene's emission over each Bloom / Glow / body-less Custom field's rect, all in one draw.
                SDL_BindGPUGraphicsPipeline(pass, emissionExtractRect_);
                SDL_BindGPUVertexStorageBuffers(pass, 0, &layerFields.extractBuf, 1);
                SDL_BindGPUFragmentSamplers(pass, 0, &scene, 1);
                SDL_PushGPUFragmentUniformData(cmd, 0, &eu, sizeof(eu));
                SDL_DrawGPUPrimitives(pass, 6, static_cast<Uint32>(layerFields.extractCount), 0, 0);
            }
            // Body-authored Custom lenses: each stage authors its own fields through its own rect pipeline,
            // reading the scene through sampleSource and the stage's params from the fx record lanes.
            for (const CustomExtractRun& run : layerFields.customExtracts) {
                if (!run.buf || run.count <= 0) continue;
                const std::size_t sid = static_cast<std::size_t>(run.stage);
                if (sid >= customEmissionRect_.size() || !customEmissionRect_[sid] || !spriteFxStore_) continue;
                SDL_BindGPUGraphicsPipeline(pass, customEmissionRect_[sid]);
                SDL_BindGPUVertexStorageBuffers(pass, 0, &run.buf, 1);
                SDL_BindGPUFragmentSamplers(pass, 0, &scene, 1);
                SDL_BindGPUFragmentStorageTextures(pass, 0, &spriteFxStore_, 1);
                SDL_PushGPUFragmentUniformData(cmd, 0, &eu, sizeof(eu));
                SDL_DrawGPUPrimitives(pass, 6, static_cast<Uint32>(run.count), 0, 0);
            }

            if (hasRegion) {
                SpriteFragUniforms fu{};
                fu.tilePx        = static_cast<float>(kTilePx);
                fu.alpha         = 1.0f;  // neutral: the art fragment applies the layer alpha to the composed pixel
                fu.paletteStoreW = static_cast<float>(kPaletteStoreWidth);
                // One atlas texel IS one viewport pixel, so the analytic branch's crisp snap needs no scaling
                // here — that is the whole reason a field is a viewport-space image: its size, and so its
                // memory, does not follow the window.
                fu.composeScale  = 1.0f;
                SDL_GPUTexture* fragStorage[4] = {atlasStore_, paletteStore_, atlasRegionStore_, spriteFxStore_};
                SDL_BindGPUGraphicsPipeline(pass, spriteEmission_);
                SDL_BindGPUVertexStorageBuffers(pass, 0, &layerFields.buffer, 1);
                SDL_BindGPUFragmentStorageTextures(pass, 0, fragStorage, 4);
                SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
                SDL_DrawGPUPrimitives(pass, 6, static_cast<Uint32>(layerFields.count), 0, 0);
            }
            SDL_EndGPURenderPass(pass);
        }

        // Resolve each page's chain once — the reach is all that differs between them.
        std::vector<EmissionChainPlan> plans;
        plans.reserve(layerFields.pages.size());
        bool anyReduced = false;
        for (std::size_t p = 0; p < layerFields.pages.size(); ++p) {
            ScreenSpaceEffect probe{};   // the reach and kind are all the plan reads beyond intensity
            probe.kind      = ScreenSpaceEffectKind::Bloom;
            probe.intensity = 255;       // a page exists only for steps whose own plan engaged
            plans.push_back(emissionChainPlan(probe, layerFields.pageReach[p]));
            anyReduced = anyReduced || (plans.back().engaged && plans.back().downsample);
        }
        // Reduce the whole atlas once, not per page: the two halvings read the raster, which no page's blur
        // has touched yet, so one reduction serves every reduced page and the full-resolution ones ignore it.
        if (anyReduced) {
            runEmissionPass(emissionAtlasHalf_, emissionAtlas0_,    bilinear_, emissionCopy_, nullptr, 0);
            runEmissionPass(emissionAtlasLow0_, emissionAtlasHalf_, bilinear_, emissionCopy_, nullptr, 0);
        }

        for (std::size_t p = 0; p < layerFields.pages.size(); ++p) {
            const EmissionChainPlan& plan = plans[p];
            if (!plan.engaged) continue;
            const EmissionPage& page = layerFields.pages[p];

            // Taps step in VIEWPORT pixels, which on this grid is one atlas texel each.
            //
            // The step is always measured against the FULL atlas, never the reduced copy: a reduced texture
            // covers the same region at a quarter the texels, so its UV span is the same and one quarter-res
            // texel already IS four atlas texels. Dividing by the reduced width instead would step four
            // times too far and smear the halo away entirely.
            //
            // The crisp snap walks from viewport-cell centres, which here are the texels themselves; at
            // quarter resolution a cell is smaller than a texel, so no snap is applied there.
            const float stepU = plan.stepPx / static_cast<float>(atlasW);
            const float stepV = plan.stepPx / static_cast<float>(atlasH);
            const bool  snapPage = snap && !plan.downsample;
            const float snapW = snapPage ? static_cast<float>(atlasW) : 0.0f;
            const float snapH = snapPage ? static_cast<float>(atlasH) : 0.0f;
            const float taps  = static_cast<float>(plan.taps);
            const EmissionBlurFragUniforms hu{stepU, 0.0f, taps, plan.invNorm, plan.inv2Sigma2,
                                              snapW, snapH, 0.0f};
            const EmissionBlurFragUniforms vu{0.0f, stepV, taps, plan.invNorm, plan.inv2Sigma2,
                                              snapW, snapH, 0.0f};

            if (!plan.downsample) {
                runAtlasPass(emissionAtlas1_, emissionAtlas0_, sampler_, emissionBlur_, &hu, sizeof(hu),
                             SDL_GPU_LOADOP_CLEAR, page.y, page.h, atlasW, atlasH);
                runAtlasPass(emissionAtlas0_, emissionAtlas1_, sampler_, emissionBlur_, &vu, sizeof(vu),
                             SDL_GPU_LOADOP_LOAD, page.y, page.h, atlasW, atlasH);
                continue;
            }
            // The reduced path blurs a quarter-size copy and resolves it back over the page's own band. The
            // band rounds outward at quarter resolution, so two pages may share a texel row there; it lies
            // in a rect's margin, which nothing reads.
            const int lowY = page.y / kEmissionDownsampleFactor;
            const int lowB = (page.y + page.h + kEmissionDownsampleFactor - 1) / kEmissionDownsampleFactor;
            runAtlasPass(emissionAtlasLow1_, emissionAtlasLow0_, sampler_, emissionBlur_, &hu, sizeof(hu),
                         SDL_GPU_LOADOP_CLEAR, lowY, lowB - lowY, lowW, lowH);
            runAtlasPass(emissionAtlasLow0_, emissionAtlasLow1_, sampler_, emissionBlur_, &vu, sizeof(vu),
                         SDL_GPU_LOADOP_LOAD, lowY, lowB - lowY, lowW, lowH);
            runAtlasPass(emissionAtlas0_, emissionAtlasLow0_, bilinear_, emissionCopy_, nullptr, 0,
                         SDL_GPU_LOADOP_LOAD, page.y, page.h, atlasW, atlasH);
        }
        storeEmissionSheet(static_cast<int>(sheetIndex));
        spriteArtHasField[idx] = true;
    };

    // Every sheet this layer needs, in order. One sheet is the common case; a layer authoring more fields than
    // one holds simply gets another, which is what keeps the field count off any ceiling.
    auto prepareEmissionFields = [&](std::size_t idx) {
        for (std::size_t sh = 0; sh < layerEmissionSheets[idx].size(); ++sh) prepareEmissionSheet(idx, sh);
    };

    auto runBelowSprites = [&](std::size_t idx) {
        const std::vector<SpriteBelowRunGpu>& runs = spriteBelowRuns[idx];
        if (runs.empty()) return;
        if (!atlasStore_ || !paletteStore_ || !atlasRegionStore_) return;
        closeBatch();
        ensureTargetInitialized();  // target_ holds the scene beneath — the below sprites distort it
        // This layer's fields were prepared before this call, so the store and its rect table are what a lens
        // reads; a layer that authored none binds the idle pair, which its records never read.
        SDL_GPUTexture* fieldBind = spriteArtFieldFor(idx);
        SDL_GPUTexture* rectBind  = spriteArtRectsFor(idx);
        if (!fieldBind || !rectBind) return;  // nothing valid to bind: leave the scene rather than draw wrong
        const DrawLayer& layer = frame.layers[idx];
        SpriteFragUniforms fu{};
        fu.tilePx        = static_cast<float>(kTilePx);
        fu.alpha         = clampAlpha(layer.alpha);
        fu.paletteStoreW = static_cast<float>(kPaletteStoreWidth);
        fu.composeScale  = static_cast<float>(composeScale_);
        fu.evalSnap      = snap ? 1.0f : 0.0f;
        // The scene (nearest, CLAMP) and the emission atlas (linear, CLAMP — a field is stored per viewport
        // pixel and filtered back onto the compose grid by the read).
        const SDL_GPUTextureSamplerBinding sceneBinds[2] = {{target_, sampler_}, {fieldBind, bilinear_}};
        SDL_GPUTexture* fragStorage[5] = {atlasStore_, paletteStore_, atlasRegionStore_, spriteFxStore_,
                                          rectBind};
        bool first = true;
        for (const SpriteBelowRunGpu& run : runs) {
            if (!run.buffer || run.count <= 0) continue;
            SDL_GPUGraphicsPipeline* pipe = spriteBelow_;
            if (run.pipelineKey > 0) {
                const std::size_t cid = static_cast<std::size_t>(run.pipelineKey - 1);
                if (cid < customSpriteBelow_.size() && customSpriteBelow_[cid]) pipe = customSpriteBelow_[cid];
            }
            SDL_GPUColorTargetInfo t{};
            t.texture     = layerScratch_;
            t.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};  // transparent → the premultiplied below image
            t.load_op     = first ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
            t.store_op    = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(pass, pipe);
            SDL_BindGPUVertexStorageBuffers(pass, 0, &run.buffer, 1);
            SDL_BindGPUFragmentSamplers(pass, 0, sceneBinds, 2);
            SDL_BindGPUFragmentStorageTextures(pass, 0, fragStorage, 5);
            SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
            SDL_DrawGPUPrimitives(pass, 6, static_cast<Uint32>(run.count), 0, 0);
            SDL_EndGPURenderPass(pass);
            first = false;
        }
        // Composite the premultiplied scratch premultiplied-over the accumulator (empty region ⇒ pass-through).
        runRegionSelect(target_, layerScratch_, layerScratch_, ShapePoints{}, 1.0f, BlendMode::Normal,
                        /*blend=*/true, SDL_GPU_LOADOP_LOAD);
    };

    // Composite a mixed-blend sprite layer's runs into a container, returning the texture that now holds
    // the result. `t0` is the container — the accumulator on the direct path (firstLoad = LOAD keeps the
    // scene beneath) or a transparent scratch on the isolated path (firstLoad = CLEAR). `t1`/`t2` are two
    // free ping-pong textures. A Normal run draws straight onto the current result; a non-Normal run mirrors
    // the non-Normal-LAYER composite at run granularity — render the run alone into a free texture
    // (transparent-cleared → premultiplied), then runBlendComposite it onto the result under the run's mode,
    // writing the other free texture, which becomes the new result. The result texture is one of {t0,t1,t2}
    // (which one depends on the non-Normal run count); the caller reconciles it into the canonical slot.
    // Pass count scales with the non-Normal run count (authored structure), never with the sprite count.
    auto compositeSpriteRuns = [&](std::size_t idx, SDL_GPUTexture* t0, SDL_GPUTexture* t1,
                                   SDL_GPUTexture* t2, SDL_GPULoadOp firstLoad) -> SDL_GPUTexture* {
        if (firstLoad == SDL_GPU_LOADOP_CLEAR) {  // an isolated scratch starts empty (transparent)
            SDL_GPUColorTargetInfo t{};
            t.texture     = t0;
            t.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
            t.load_op     = SDL_GPU_LOADOP_CLEAR;
            t.store_op    = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
            SDL_EndGPURenderPass(p);
        }
        SDL_GPUTexture* result = t0;
        for (const SpriteRunGpu& run : spriteLayerRuns[idx]) {
            // The two textures that are not the current result: a scratch to render the run into and a
            // destination for the composite (pool holds three distinct textures, so both are found).
            SDL_GPUTexture* pool[3] = {t0, t1, t2};
            SDL_GPUTexture* scratch = nullptr;
            SDL_GPUTexture* out     = nullptr;
            for (SDL_GPUTexture* t : pool) {
                if (t == result) continue;
                (scratch ? out : scratch) = t;
            }
            if (run.blend == BlendMode::Normal) {
                SDL_GPUColorTargetInfo t{};
                t.texture     = result;
                t.clear_color = kBackdropClear;
                t.load_op     = SDL_GPU_LOADOP_LOAD;   // keep the container's content beneath the run
                t.store_op    = SDL_GPU_STOREOP_STORE;
                SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
                drawSpriteRun(p, idx, run.buffer, run.count, run.pipelineKey);
                SDL_EndGPURenderPass(p);
            } else {
                SDL_GPUColorTargetInfo t{};
                t.texture     = scratch;
                t.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};  // transparent → premultiplied run image
                t.load_op     = SDL_GPU_LOADOP_CLEAR;
                t.store_op    = SDL_GPU_STOREOP_STORE;
                SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
                drawSpriteRun(p, idx, run.buffer, run.count, run.pipelineKey);
                SDL_EndGPURenderPass(p);
                runBlendComposite(out, result, scratch, run.blend, SDL_GPU_LOADOP_DONT_CARE);
                result = out;
            }
        }
        return result;
    };

    // Render one layer alone into layerScratch_ (transparent-cleared) — the isolated content for a blended
    // or effected layer. Returns the texture holding that content: layerScratch_ for a tile layer or an
    // all-Normal sprite layer; for a MIXED-blend sprite layer the run composite may finish in a ping-pong
    // spare, so the returned texture (one of {layerScratch_, post0_, post1_}) is what the caller must
    // composite from — never assume layerScratch_.
    auto renderLayerIsolated = [&](std::size_t idx) -> SDL_GPUTexture* {
        if (!spriteLayerRuns[idx].empty()) {  // mixed-blend sprite layer: composite runs into the scratch
            return applySpriteEmission(idx,
                                       compositeSpriteRuns(idx, layerScratch_, post0_, post1_,
                                                           SDL_GPU_LOADOP_CLEAR),
                                       layerScratch_, post0_, post1_);
        }
        SDL_GPUColorTargetInfo lt{};
        lt.texture     = layerScratch_;
        lt.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};  // transparent
        lt.load_op     = SDL_GPU_LOADOP_CLEAR;
        lt.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* lp = SDL_BeginGPURenderPass(cmd, &lt, 1, nullptr);
        drawLayer(lp, idx);
        SDL_EndGPURenderPass(lp);
        // The layer's own glow composites onto its isolated content, so the layer's blend, alpha and
        // Layer-scope chain treat the halo as part of the layer — which is what an inline glow always was.
        return applySpriteEmission(idx, layerScratch_, layerScratch_, post0_, post1_);
    };

    // ── Pre-pass: build every site's confined-step list + batch plan, then upload all runs' instance
    //    records in ONE copy pass — BEFORE any composite render pass begins. The composite loop consumes
    //    the prebuilt lists + plans (instead of calling buildSteps mid-loop), so each run's records are on
    //    the GPU before its batched pass draws them. Each layer partitions into its Layer-scope chain and
    //    its Below-scope steps (two of the three confined sites); the frame chain is the third. ──────────
    struct LayerSitePlan {
        std::vector<ConfinedStep> layerSteps, belowSteps;
        SiteBatch                 layerBatch, belowBatch;    // additive-batching dispositions
        SiteGather                layerGather, belowGather;  // gather dispositions
    };
    std::vector<LayerSitePlan> layerPlans(frame.layers.size());
    for (const std::size_t idx : order) {
        std::vector<ConfinedStep> steps = buildSteps(frame.layers[idx]);
        LayerSitePlan lp;
        for (const ConfinedStep& s : steps)
            (effectIsBelowScope(*s.eff) ? lp.belowSteps : lp.layerSteps).push_back(s);
        lp.layerBatch  = buildBatchPlan(lp.layerSteps);
        lp.belowBatch  = buildBatchPlan(lp.belowSteps);
        lp.layerGather = buildGatherPlan(lp.layerSteps);
        lp.belowGather = buildGatherPlan(lp.belowSteps);
        layerPlans[idx] = std::move(lp);
    }
    // The frame-level steps (whole-frame postEffects, then each frame region's effects) + their batch + gather
    // plans.
    std::vector<ConfinedStep> frameSteps;
    for (const ScreenSpaceEffect& e : frame.postEffects)
        appendEffectSteps(frameSteps, e, /*hasDefaultShape=*/false, {}, 1.0f, BlendMode::Normal);
    for (const Region& region : frame.regions)
        for (const ScreenSpaceEffect& e : region.effects)
            appendEffectSteps(frameSteps, e, /*hasDefaultShape=*/true, region.shape, region.alpha, region.blend);
    SiteBatch  frameBatch  = buildBatchPlan(frameSteps);
    SiteGather frameGather = buildGatherPlan(frameSteps);

    if (batchSlotCount > 0) {
        if (static_cast<int>(batchInstanceBufs_.size()) < batchSlotCount) {
            batchInstanceBufs_.resize(static_cast<std::size_t>(batchSlotCount), nullptr);
            batchInstanceCaps_.resize(static_cast<std::size_t>(batchSlotCount), 0);
        }
        SDL_GPUCopyPass* bcopy = nullptr;
        // Generalized to byte blobs so additive instance records (GpuRegionBatch[]) and gather records
        // (48-byte header + padded params, variable stride) travel one path and share the pool
        // (batchInstanceBufs_ / batchInstanceCaps_ track BYTE capacity). Grows the slot's storage buffer
        // on demand, uploads the blob, and stages the transfer buffer in `scratch` for the frame.
        auto uploadBlob = [&](int slot, const std::byte* data, std::size_t bytesLen) {
            if (bytesLen == 0) return;
            const int need = static_cast<int>(bytesLen);
            SDL_GPUBuffer*& buf = batchInstanceBufs_[static_cast<std::size_t>(slot)];
            if (!buf || batchInstanceCaps_[static_cast<std::size_t>(slot)] < need) {  // grow-on-demand
                if (buf) SDL_ReleaseGPUBuffer(device_, buf);
                SDL_GPUBufferCreateInfo bi{};
                bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
                bi.size  = static_cast<Uint32>(need);
                buf = SDL_CreateGPUBuffer(device_, &bi);
                if (!buf) fail("SDL_CreateGPUBuffer (region records) failed");
                batchInstanceCaps_[static_cast<std::size_t>(slot)] = need;
            }
            const Uint32 bytes = static_cast<Uint32>(need);
            SDL_GPUTransferBufferCreateInfo tb{};
            tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            tb.size  = bytes;
            SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tb);
            if (!transfer) fail("SDL_CreateGPUTransferBuffer (region records) failed");
            void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
            if (!mapped) fail("SDL_MapGPUTransferBuffer (region records) failed");
            std::memcpy(mapped, data, bytes);
            SDL_UnmapGPUTransferBuffer(device_, transfer);
            if (!bcopy) bcopy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTransferBufferLocation src{};
            src.transfer_buffer = transfer;
            src.offset          = 0;
            SDL_GPUBufferRegion dst{};
            dst.buffer = buf;
            dst.offset = 0;
            dst.size   = bytes;
            SDL_UploadToGPUBuffer(bcopy, &src, &dst, false);
            scratch.transfers.push_back(transfer);
        };
        auto uploadBatchRun = [&](const BatchRun& run) {
            uploadBlob(run.slot, reinterpret_cast<const std::byte*>(run.records.data()),
                       run.records.size() * sizeof(GpuRegionBatch));
        };
        auto uploadGatherRun = [&](const GatherRunGpu& run) {
            uploadBlob(run.slot, run.bytes.data(), run.bytes.size());
        };
        for (const std::size_t idx : order) {
            for (const BatchRun&     r : layerPlans[idx].layerBatch.runs)   uploadBatchRun(r);
            for (const BatchRun&     r : layerPlans[idx].belowBatch.runs)   uploadBatchRun(r);
            for (const GatherRunGpu& r : layerPlans[idx].layerGather.runs)  uploadGatherRun(r);
            for (const GatherRunGpu& r : layerPlans[idx].belowGather.runs)  uploadGatherRun(r);
        }
        for (const BatchRun&     r : frameBatch.runs)  uploadBatchRun(r);
        for (const GatherRunGpu& r : frameGather.runs) uploadGatherRun(r);
        if (bcopy) SDL_EndGPUCopyPass(bcopy);
    }

    for (const std::size_t idx : order) {
        const DrawLayer&     layer = frame.layers[idx];
        const LayerSitePlan& plan  = layerPlans[idx];

        // This layer's emission fields, both paths', made before anything reads one: the lenses below sample
        // theirs as they draw, and the art fragment reads the region ones as it shades. A below field is an
        // extract of the accumulator, so this must come after the scene beneath is composited and before the
        // lenses distort it — which is exactly here.
        prepareEmissionFields(idx);

        // Below-scope sprites distort the scene beneath this layer (confined to their silhouettes) BEFORE the
        // layer's own content composites — so the art rides on top of the distortion. A no-op unless the
        // layer carries Below sprites (spriteBelowRuns[idx] populated). It closes any open batch and
        // ensures the accumulator holds the scene beneath, so it slots in ahead of every layer-handling path.
        runBelowSprites(idx);

        // Nothing to do beyond the layer's own content. A Normal layer takes the batched faithful path; a
        // non-Normal layer renders isolated and composites onto the accumulator with its blend mode.
        if (plan.layerSteps.empty() && plan.belowSteps.empty()) {
            if (layer.blend == BlendMode::Normal) {
                if (!spriteLayerRuns[idx].empty()) {
                    // Mixed-blend sprite layer: split into runs and grade non-Normal runs onto the
                    // accumulator directly (the container is the scene beneath + earlier-z runs of this layer).
                    closeBatch();
                    ensureTargetInitialized();
                    SDL_GPUTexture* r = compositeSpriteRuns(idx, target_, post0_, post1_, SDL_GPU_LOADOP_LOAD);
                    if (r == post0_) std::swap(target_, post0_);
                    else if (r == post1_) std::swap(target_, post1_);
                    adoptEmissionResult(applySpriteEmission(idx, target_, post0_, post1_, layerScratch_));
                } else if (!spriteEmissionRuns[idx].empty()) {
                    // A glowing layer draws its art into the batch as ever, then closes it so the halo can
                    // composite over the accumulator (a composite reads target_, which an open pass owns).
                    if (!batch) openBatch();
                    drawLayer(batch, idx);
                    closeBatch();
                    adoptEmissionResult(applySpriteEmission(idx, target_, post0_, post1_, layerScratch_));
                } else {
                    if (!batch) openBatch();
                    drawLayer(batch, idx);
                }
            } else {
                closeBatch();
                ensureTargetInitialized();
                SDL_GPUTexture* iso = renderLayerIsolated(idx);  // layerScratch_ or (mixed sprite) a spare
                SDL_GPUTexture* out = (iso == post0_) ? post1_ : post0_;  // a free spare ≠ iso and ≠ target_
                runBlendComposite(out, target_, iso, layer.blend, SDL_GPU_LOADOP_DONT_CARE);
                if (out == post0_) std::swap(target_, post0_); else std::swap(target_, post1_);
            }
            continue;
        }

        // Two-phase scope partition (done in the pre-pass). Layer-scope steps work on the layer's OWN
        // isolated content (composited over the accumulator); Below-scope steps adjust the WHOLE accumulator
        // after the layer composites. Submission order is preserved within each phase; Layer always runs
        // before Below — the only coherent order, since a Below step reads the post-composite accumulator.
        if (!plan.layerSteps.empty()) {
            // Layer (isolated) scope: render this layer alone into its scratch (transparent-cleared; a
            // mixed-blend sprite layer bakes its per-sprite blends into that scratch here), then apply the
            // Layer-scope chain on the layer's own content — starting from whichever texture holds it.
            closeBatch();
            SDL_GPUTexture* iso = renderLayerIsolated(idx);
            if (layer.blend != BlendMode::Normal) ensureTargetInitialized();
            const SDL_GPULoadOp compositeLoad = targetInitialized ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
            targetInitialized = true;
            applyLayerChain(plan.layerSteps, plan.layerBatch, plan.layerGather, layer.blend, compositeLoad, iso);
        } else if (!spriteLayerRuns[idx].empty()) {
            // No Layer-scope step, but a mixed-blend sprite layer: grade its non-Normal runs straight onto
            // the accumulator (the container the Below-scope steps then adjust).
            closeBatch();
            ensureTargetInitialized();
            SDL_GPUTexture* r = compositeSpriteRuns(idx, target_, post0_, post1_, SDL_GPU_LOADOP_LOAD);
            if (r == post0_) std::swap(target_, post0_);
            else if (r == post1_) std::swap(target_, post1_);
            adoptEmissionResult(applySpriteEmission(idx, target_, post0_, post1_, layerScratch_));
        } else {
            // No Layer-scope step: composite the layer's own content straight into the accumulator (the
            // batched faithful draw); the Below-scope steps below then adjust the whole accumulator.
            if (!batch) openBatch();
            drawLayer(batch, idx);
            closeBatch();
            adoptEmissionResult(applySpriteEmission(idx, target_, post0_, post1_, layerScratch_));
        }

        // Below-scope phase: each step (or run) adjusts the whole accumulator at this z (this layer's content
        // AND everything beneath it), confined to its region's shape, in submission order. A additive batched
        // run blends its regions' deltas additively ONTO target_ in place (LOAD) at the run's first step; a
        // gather run reads target_ → writes the union-shape result into layerScratch_ (a replace pass —
        // its passthrough covers everything outside the shapes) and swaps it in; the per-step path handles
        // ineligible steps and eligible singletons. Disjoint by stage class, so at most one branch fires per i.
        for (std::size_t i = 0; i < plan.belowSteps.size(); ++i) {
            const int rn  = plan.belowBatch.grouping.stepRun[i];
            const int rnG = plan.belowGather.grouping.stepRun[i];
            if (rn >= 0) {
                if (i == plan.belowBatch.grouping.runs[rn].steps.front())
                    runBatch(target_, SDL_GPU_LOADOP_LOAD, plan.belowBatch.runs[rn]);
            } else if (rnG >= 0) {
                if (i == plan.belowGather.grouping.runs[rnG].steps.front()) {
                    runGather(layerScratch_, target_, plan.belowGather.runs[rnG], /*blend=*/false,
                              SDL_GPU_LOADOP_DONT_CARE);
                    std::swap(target_, layerScratch_);
                }
            } else {
                applyBelowStep(plan.belowSteps[i]);
            }
        }
    }
    closeBatch();

    // Empty frame (no layer ever cleared target_): clear it to the backdrop so the blit shows the
    // backdrop rather than stale contents.
    if (!targetInitialized) {
        SDL_GPUColorTargetInfo t{};
        t.texture     = target_;
        t.clear_color = kBackdropClear;
        t.load_op     = SDL_GPU_LOADOP_CLEAR;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        SDL_EndGPURenderPass(pass);
    }

    // ── Post-process chain: frame-level screen-space effects, run on the finished viewport image
    //    before the blit. Each active effect is one fullscreen-triangle pass that reads the previous
    //    image and writes a scratch target; the two scratch targets strictly alternate by APPLIED-pass
    //    count, so a pass never samples the texture it writes (target_ is never written). Effects
    //    dispatch on kind — a built-in RowDisplacement binds displace_ + resolved params; a Custom
    //    effect binds its registered replace pipeline + pushes the game's uniform, composing in
    //    submission order with the built-ins. An invalid Custom pass is skipped (under WarnAndResolve)
    //    without advancing the ping-pong. An empty chain leaves blitSource == target_ → the blit
    //    samples the composited viewport directly. ──────────────────────────────────────────────────
    // The frame-level steps (frameSteps) + their batch plan (frameBatch) were built in the pre-pass above.
    // Scope plays no role here — frame-level steps are inherently whole-frame, run in submission order on
    // the composited image.
    SDL_GPUTexture* blitSource = target_;
    {
        SDL_GPUTexture* readTex    = target_;
        SDL_GPUTexture* scratch[2] = {post0_, post1_};
        std::size_t     applied    = 0;  // counts only rendered passes → preserves read≠write alternation
        for (std::size_t i = 0; i < frameSteps.size(); ++i) {
            SDL_GPUTexture* writeTex = scratch[applied % 2];

            // A batched run: blend its regions' deltas additively onto the running image. Mid-chain (readTex
            // is a scratch) it blends in place, no ping-pong advance. First in the chain (readTex == target_,
            // which stays unwritten by the compose invariant) it first copies target_ → writeTex via an
            // empty-region pass-through, then blends onto writeTex and advances. Issued once, at the run's
            // first step; the run's other steps are skipped.
            const int rn = frameBatch.grouping.stepRun[i];
            if (rn >= 0) {
                if (i != frameBatch.grouping.runs[rn].steps.front()) continue;
                const BatchRun& run = frameBatch.runs[rn];
                if (readTex == target_) {
                    runRegionSelect(writeTex, target_, target_, ShapePoints{}, 1.0f, BlendMode::Normal,
                                    /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);  // pass-through copy
                    runBatch(writeTex, SDL_GPU_LOADOP_LOAD, run);
                    blitSource = writeTex;
                    readTex    = writeTex;
                    ++applied;
                } else {
                    runBatch(readTex, SDL_GPU_LOADOP_LOAD, run);  // additive in place, no advance
                    blitSource = readTex;
                }
                continue;
            }

            // A gather run: read the running image (readTex) → write the union-shape result to a fresh
            // writeTex (replace — its passthrough covers everything outside the shapes), advancing the
            // ping-pong like any ordinary applied pass. readTex == target_ first-in-chain is fine — the pass
            // only SAMPLES target_, never writes it (the compose invariant holds). Issued once, at the run's
            // first step.
            const int rnG = frameGather.grouping.stepRun[i];
            if (rnG >= 0) {
                if (i != frameGather.grouping.runs[rnG].steps.front()) continue;
                runGather(writeTex, readTex, frameGather.runs[rnG], /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
                blitSource = writeTex;
                readTex    = writeTex;
                ++applied;
                continue;
            }

            const ConfinedStep& s = frameSteps[i];
            // runEffect dispatches the built-in (displace_) vs Custom (customReplace_) pass; blend=false
            // + blankTransparent=false = the frame-level replace. A confined step lands the effect
            // full-frame in layerScratch_ (free during the frame-level chain) and the gate selects
            // inside(shape)?eff:readTex into writeTex. A whole-frame step → the single replace pass. A
            // Transparency makes the composited frame see-through in/around the shape (replace into writeTex)
            // → reveals the backdrop — one applied pass, advancing the ping-pong like any other.
            if (s.eff->kind == ScreenSpaceEffectKind::Transparency) {
                runStencil(writeTex, readTex, s.shape, s.eff->stencil, s.eff->feather,
                           /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
            } else if (s.confined && s.shape.hasRegion()) {
                const IntRect  r = regionScissorRect(s.shape, composeScale_, composeW_, composeH_);
                const SDL_Rect sc{r.x, r.y, r.width, r.height};
                runEffect(layerScratch_, readTex, *s.eff,
                          /*blankTransparent=*/false, /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE, &sc);
                runRegionSelect(writeTex, layerScratch_, readTex, s.shape, s.alpha, s.blend,
                                /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
            } else if (frame.blend == BlendMode::Normal) {
                runEffect(writeTex, readTex, *s.eff,
                          /*blankTransparent=*/false, /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
            } else {
                // The frame's whole-frame postEffect output combines over the composited image with the
                // frame's blend mode: render the effect into a scratch (layerScratch_ is free here), then
                // blend-composite it onto the running image.
                runEffect(layerScratch_, readTex, *s.eff,
                          /*blankTransparent=*/false, /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
                runBlendComposite(writeTex, readTex, layerScratch_, frame.blend, SDL_GPU_LOADOP_DONT_CARE);
            }
            blitSource = writeTex;
            readTex    = writeTex;
            ++applied;
        }
    }

    // Transient slots (degenerate-keyed layers) were used by THIS frame's command buffer, so — like the
    // upload transfer buffers — they outlive the submit and the caller releases them after it.
    for (const TilemapTex& t : transientTiles) {
        if (t.texture) scratch.textures.push_back(t.texture);
        if (t.transfer) scratch.transfers.push_back(t.transfer);
    }
    for (const SpriteBuf& s : transientSprites) {
        if (s.buffer) scratch.buffers.push_back(s.buffer);
        if (s.transfer) scratch.transfers.push_back(s.transfer);
    }

    return blitSource;
}

void Renderer::renderFrame(const FrameDrawState& frame) {
    // The frame's phase split, published on renderStats() when the frame ends. Every exit writes it,
    // including the early one, so a reader never sees the previous frame's numbers wearing this frame's.
    using Clock = std::chrono::steady_clock;
    const auto elapsedMs = [](Clock::time_point from, Clock::time_point to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    };
    RenderStats::PhaseTimings phase{};

    const auto acquireStart = Clock::now();
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    phase.acquireMs = elapsedMs(acquireStart, Clock::now());
    if (!cmd) {
        ++renderStats_.presentSkips;
        renderStats_.lastFrame = phase;
        return;
    }

    // Automatic interpolation: read the run loop's (alpha, tickAdvanced) from the frame-timing channel. On
    // a committed tick, rotate the per-id mirror to this submission; every frame, composite each object
    // eased between its previous and current tick state by the sub-tick factor. Off (or no loop publishing
    // → the default (0, false)) composites the submission verbatim. Reconcile BEFORE the skip decision — a
    // tick that introduces motion must flip allSettled() to false so this frame is not wrongly skipped.
    const auto interpStart = Clock::now();
    FrameTiming timing;         // interpolation off ⇒ the defaults, which ease nothing
    bool        settled = true; // interpolation off ⇒ nothing eases ⇒ always settled
    if (interpolation_) {
        timing = frameTiming();
        if (timing.tickAdvanced) interp_.reconcile(frame, timing);
        settled = interp_.allSettled();
    }
    phase.interpMs = elapsedMs(interpStart, Clock::now());

    // Resolve the compose grid from the window and resize the offscreen targets when it changes. On the
    // interpolation path this composites at the output resolution so sub-pixel motion has a finer grid to
    // land on; off / headless it stays 1 (viewport res, blit upscales) — the faithful, byte-identical path.
    // A recreate here nulls lastComposed_, so a window resize this frame forces a recompose.
    resizeComposeTargets(resolveComposeScale());

    // ── Frame-level compose skip ────────────────────────────────────────────────────────────────
    // When the submission is provably bit-identical to the frame that produced the retained output, skip
    // composeViewport entirely (copy pass → layer composite → post-process) and re-blit lastComposed_. All
    // conditions must hold: the frame is settled (alpha is output-irrelevant); a retained output exists that
    // was itself composed settled (never re-blit a mid-ease frame as settled); no out-of-frame store upload
    // since (generation match); the structural fingerprint matches; and no tile layer declares itself dirty
    // (the huge-map path whose cells the fingerprint does not read). Only settled frames pay the fingerprint
    // cost — a moving frame short-circuits on `settled` before hashing.
    bool          skip = false;
    std::uint64_t fp   = 0;
    if (settled) {
        fp   = hashFrameStructure(frame);
        skip = lastComposed_ != nullptr && lastComposeSettled_ &&
               storeGeneration_ == lastComposeGeneration_ &&
               fp == lastFingerprint_ && !frameDeclaredDirty(frame);
    }

    // Compose the finished viewport image (copy pass → layer composite → post-process chain), then blit it
    // to the swapchain — unless the skip re-blits the retained output. The compose is shared verbatim with
    // captureViewport (which never skips — the golden/inspection path). scratch stays empty on a skip.
    FrameScratch    scratch;
    SDL_GPUTexture* blitSource = nullptr;
    if (skip) {
        blitSource = lastComposed_;
        ++renderStats_.composeSkips;
        phase.composeSkipped = true;
    } else {
        const auto composeStart = Clock::now();
        const FrameDrawState* toCompose =
            interpolation_ ? &interp_.interpolate(frame, timing) : &frame;
        // The ease is interpolation work, not compose work — a scene whose motion is expensive to resolve
        // and one whose image is expensive to draw are different problems and read differently here.
        phase.interpMs += elapsedMs(composeStart, Clock::now());
        const auto viewportStart = Clock::now();
        blitSource             = composeViewport(cmd, *toCompose, scratch, timing, interpolation_);
        lastComposed_          = blitSource;
        lastComposeGeneration_ = storeGeneration_;
        lastComposeSettled_    = settled;
        if (settled) lastFingerprint_ = fp;  // fresh on every settled compose; left stale (guarded) during motion
        phase.composeMs = elapsedMs(viewportStart, Clock::now());
    }

    // ── Blit pass: viewport → swapchain, integer-scaled + letterboxed. ──────────────────────────
    const auto      presentStart = Clock::now();
    SDL_GPUTexture* swapchain = nullptr;
    Uint32 width  = 0;
    Uint32 height = 0;
    // On Metal, the BLOCKING acquire busy-waits a core (SDL bug); use the non-blocking acquire there and
    // let the host frame deadline pace. Non-blocking returns true with a null swapchain when the frame
    // isn't ready yet → the existing `swapchain != nullptr` guard skips this frame's blit/present cleanly
    // (paced, so skips are rare). Vulkan/D3D12 keep the blocking acquire (they OS-block, no spin).
    const bool acquired = window_ != nullptr && (acquireNonBlocking_
        ? SDL_AcquireGPUSwapchainTexture(cmd, window_, &swapchain, &width, &height)
        : SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &swapchain, &width, &height));
    if (acquired && swapchain != nullptr) {
        SDL_GPUColorTargetInfo scTarget{};
        scTarget.texture     = swapchain;
        scTarget.clear_color = kLetterboxClear;
        scTarget.load_op     = SDL_GPU_LOADOP_CLEAR;
        scTarget.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &scTarget, 1, nullptr);

        // The compose image always fills the window at the largest integer scale that fits, centred +
        // letterboxed. Output SIZE is the window's size (Platform owns it, via setWindowSize sized to
        // viewport × the chosen scale); the blit just fills whatever window it's given, crisply. The
        // scale is relative to the compose grid (== the viewport at composeScale_ 1).
        const IntRect dest = integerScaleToFitRect(
            PixelSize{static_cast<int>(width), static_cast<int>(height)},
            PixelSize{composeW_, composeH_});

        const SDL_GPUViewport vp{static_cast<float>(dest.x), static_cast<float>(dest.y),
                                 static_cast<float>(dest.width), static_cast<float>(dest.height),
                                 0.0f, 1.0f};
        SDL_SetGPUViewport(pass, &vp);

        const int sx = std::max(0, dest.x);
        const int sy = std::max(0, dest.y);
        const int sr = std::min(static_cast<int>(width), dest.x + dest.width);
        const int sb = std::min(static_cast<int>(height), dest.y + dest.height);
        const SDL_Rect scissor{sx, sy, std::max(0, sr - sx), std::max(0, sb - sy)};
        SDL_SetGPUScissor(pass, &scissor);

        SDL_BindGPUGraphicsPipeline(pass, blit_);
        // Select the blit sampler by the runtime mode — nearest (crisp) or bilinear (smoothed). Same blit
        // pipeline; only the bound sampler differs (sampler state is pipeline-independent, no shader change).
        SDL_GPUSampler* blitSampler = (sampling_ == SamplingMode::Bilinear) ? bilinear_ : sampler_;
        // The blit source is the post-process chain's final output, or target_ when the chain is empty.
        const SDL_GPUTextureSamplerBinding binding{blitSource, blitSampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);  // one fullscreen triangle

        SDL_EndGPURenderPass(pass);
    }

    // Submit even with no swapchain texture (e.g. minimised) so the command buffer is never
    // leaked; then release this frame's staged scratch — upload transfer buffers plus any transient
    // textures/buffers a degenerate-keyed layer needed (both were used by the just-submitted buffer).
    SDL_SubmitGPUCommandBuffer(cmd);
    phase.presentMs = elapsedMs(presentStart, Clock::now());
    phase.presented = acquired && swapchain != nullptr;
    if (phase.presented) ++renderStats_.presentPasses;
    else                 ++renderStats_.presentSkips;
    renderStats_.lastFrame = phase;
    for (SDL_GPUTransferBuffer* transfer : scratch.transfers) SDL_ReleaseGPUTransferBuffer(device_, transfer);
    for (SDL_GPUTexture* texture : scratch.textures)          SDL_ReleaseGPUTexture(device_, texture);
    for (SDL_GPUBuffer* buffer : scratch.buffers)             SDL_ReleaseGPUBuffer(device_, buffer);
}

std::vector<Rgba8> Renderer::captureViewport(const FrameDrawState& frame) {
    return captureViewport(frame, 1);  // scale 1 — the byte-identical golden-capture path
}

std::vector<Rgba8> Renderer::captureViewport(const FrameDrawState& frame, int composeScale) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) fail("SDL_AcquireGPUCommandBuffer (captureViewport) failed");

    // Compose at the requested scale and download it instead of presenting (the same path renderFrame blits).
    // At scale 1 the compose grid is viewport res and the download region (viewport_.width × height) is one
    // texel per viewport pixel (the golden-capture subject, captured with no interpolation).
    // At a scale > 1 the compose grid is composeScale·viewport, so the download captures the output-resolution
    // image the current evaluation grid produces — the parity seam for the crisp harness. The snap flag comes
    // from the renderer's evaluation grid (a no-op at scale 1, so the golden path is grid-independent).
    resizeComposeTargets(std::max(1, composeScale));
    FrameScratch scratch;
    SDL_GPUTexture* composed = composeViewport(cmd, frame, scratch, FrameTiming{}, false);

    const int w = composeW_;
    const int h = composeH_;
    // The download buffer holds the SOURCE texels (8 B/px for R16G16B16A16_FLOAT, 4 B/px for R8G8B8A8_UNORM);
    // the pack below converts them to Rgba8.
    constexpr Uint32 srcTexelBytes =
        (kViewportColorFormat == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT) ? 8u : 4u;
    const Uint32 bytes = static_cast<Uint32>(w) * static_cast<Uint32>(h) * srcTexelBytes;

    SDL_GPUTransferBufferCreateInfo dlInfo{};
    dlInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    dlInfo.size  = bytes;
    SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(device_, &dlInfo);
    if (!download) fail("SDL_CreateGPUTransferBuffer (captureViewport) failed");

    // Download the composed viewport tightly packed (w pixels/row, no padding) into the transfer buffer.
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region{};
    region.texture = composed;
    region.w       = static_cast<Uint32>(w);
    region.h       = static_cast<Uint32>(h);
    region.d       = 1;
    SDL_GPUTextureTransferInfo dst{};
    dst.transfer_buffer = download;
    dst.offset          = 0;
    dst.pixels_per_row  = static_cast<Uint32>(w);
    dst.rows_per_layer  = static_cast<Uint32>(h);
    SDL_DownloadFromGPUTexture(copy, &region, &dst);
    SDL_EndGPUCopyPass(copy);

    // One-shot capture, not the runtime loop — submit and block on the fence until the download lands.
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) {
        SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
    }
    for (SDL_GPUTransferBuffer* transfer : scratch.transfers) SDL_ReleaseGPUTransferBuffer(device_, transfer);
    for (SDL_GPUTexture* texture : scratch.textures)          SDL_ReleaseGPUTexture(device_, texture);
    for (SDL_GPUBuffer* buffer : scratch.buffers)             SDL_ReleaseGPUBuffer(device_, buffer);

    // Pack the downloaded texels into Rgba8, keyed on the offscreen colour format. R8G8B8A8_UNORM is already
    // packed Rgba8 (a straight copy). R16G16B16A16_FLOAT carries the post-process headroom (a channel may
    // exceed 1); decode each half and quantize with round(clamp(v,0,1)·255) — the same conversion the 8-bit
    // swapchain blit applies on write, so the capture matches what a present would show.
    std::vector<Rgba8> pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    const void* mapped = SDL_MapGPUTransferBuffer(device_, download, false);
    if (!mapped) fail("SDL_MapGPUTransferBuffer (captureViewport) failed");
    if constexpr (kViewportColorFormat == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM) {
        std::memcpy(pixels.data(), mapped, pixels.size() * sizeof(Rgba8));
    } else {
        static_assert(kViewportColorFormat == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                      "captureViewport packs R8G8B8A8_UNORM or R16G16B16A16_FLOAT — add a branch for any "
                      "other viewport colour format");
        const auto* texels = static_cast<const Uint16*>(mapped);
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = Rgba8{quantizeChannel(halfBitsToFloat(texels[i * 4 + 0])),
                              quantizeChannel(halfBitsToFloat(texels[i * 4 + 1])),
                              quantizeChannel(halfBitsToFloat(texels[i * 4 + 2])),
                              quantizeChannel(halfBitsToFloat(texels[i * 4 + 3]))};
        }
    }
    SDL_UnmapGPUTransferBuffer(device_, download);
    SDL_ReleaseGPUTransferBuffer(device_, download);

    // captureViewport composes into the shared offscreen targets, so any renderFrame-retained blit source is
    // now stale — drop it so the next renderFrame recomposes rather than re-blitting a disturbed target.
    lastComposed_ = nullptr;
    return pixels;
}

std::uint64_t hashFrameStructure(const FrameDrawState& frame) noexcept {
    std::uint64_t h = kFnv64Offset;
    for (const DrawLayer& l : frame.layers) h = foldLayer(h, l);
    h = foldValue(h, frame.layers.size());
    h = foldValue(h, frame.blend);
    h = foldValue(h, frame.advancesEvery);
    for (const ScreenSpaceEffect& e : frame.postEffects) h = foldEffect(h, e);
    h = foldValue(h, frame.postEffects.size());
    for (const Region& r : frame.regions) h = foldRegion(h, r);
    return foldValue(h, frame.regions.size());
}

}  // namespace retropp

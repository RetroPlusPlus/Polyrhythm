#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include "retropp/atlas_manifest.h"  // AtlasManifest (loadAtlas's return type; re-exported here)
#include "retropp/asset_policy.h"  // AssetPolicy (loadAtlas's optional embed/load override)
#include "retropp/draw_state.h"
#include "retropp/image.h"        // AssetSlot, ContentKind, ReadOrder, sliceLayout
#include "retropp/interpolation.h"  // Interpolator (the per-id retained mirror)
#include "retropp/literal_path.h"  // LiteralPath (registerPostProcessStage path must be a string literal)
#include "retropp/output.h"
#include "retropp/palette.h"
#include "retropp/shader_format.h"
#include "retropp/shader_registry.h"  // EffectPacker (custom-shader cbuffer packers)
#include "retropp/viewport.h"

namespace retropp {

// The GPU renderer: owns the offscreen internal viewport (an SDL_GPU colour target the game
// draws into), the tile-compositing pipeline + indexed-atlas/tilemap/palette textures that
// fill it from submitted draw state, the blit pipeline + sampler that scale it onto the
// swapchain, and the per-frame submission. It is constructed from a live device + window
// (handed out by SdlPlatform) — drawing is the renderer's job, the platform owns
// window/device/input.
//
// Colour model: the atlas is INDEXED (one palette index per pixel); palettes are a separate
// amortized resource (uploadPalette → a row of a renderer-owned palette store); each tile cell
// selects a palette within its layer's set; the tile shader applies the colour per-pixel. This
// is the faithful GB/C model — colour = index + selected palette at render time — not a
// baked-RGBA atlas and not a hardware palette-RAM poke.
//
// renderFrame() composites the submitted FrameDrawState into the offscreen viewport (z-sorted,
// alpha-blended; indexed TILES layers and SPRITES layers — instanced per-sprite quads with
// colour-index-0 transparency — interleave by z; frame-level colour modifiers fold into the
// final blit) and then blits it integer-scaled + letterboxed onto the swapchain. Atlas index
// data and palette colour data are each uploaded once (amortized); the draw state references
// them by handle and is rebuilt fresh each frame.
//
// Like SdlPlatform, the renderer needs a live GPU device/swapchain — and so a display the
// headless CI runners lack — so it is exercised by the demos and runtime-verified on a dev
// machine, while its device-free math (draw order, tilemap coordinates, shader-format
// selection) is unit-tested.
class Renderer {
public:
    // The settable default viewport — seeded by EngineConfig::setActive() so a bare
    // `Renderer{device, window}` inherits the host's configured resolution instead of having it
    // threaded to every ctor. ViewportResolution lives in viewport.h (already included). Initializes
    // to GameBoyColor (160×144) — the faithful default until setActive() changes it.
    static inline ViewportResolution defaultViewport = ViewportResolution::GameBoyColor;

    // The settable default blit sampling mode — seeded by EngineConfig::setActive() so a bare
    // `Renderer{device, window}` inherits the host's configured sampling instead of the call site having
    // to apply it. Initializes to Nearest (the faithful crisp-pixel default until setActive() changes it).
    // samplingMode() remains the per-renderer runtime override.
    static inline SamplingMode defaultSamplingMode = SamplingMode::Nearest;

    // The settable default for automatic interpolation — seeded by EngineConfig::setActive() from
    // EngineConfig::interpolation so a bare `Renderer{device, window}` inherits the host's choice.
    // Initializes to true (the smooth baseline). automaticInterpolation() is the per-renderer runtime override.
    static inline bool defaultInterpolation = true;

    // The settable default evaluation grid — seeded by EngineConfig::setActive() from
    // EngineConfig::evaluationGrid so a bare `Renderer{device, window}` inherits the host's choice.
    // Initializes to Viewport (crisp — the analytic paths evaluate on the viewport grid, so the upscaled
    // image is pixel-identical to the viewport-resolution rasterization). evaluationGrid() is the
    // per-renderer runtime override. (EvaluationGrid lives in output.h beside SamplingMode.)
    static inline EvaluationGrid defaultEvaluationGrid = EvaluationGrid::Viewport;

    // Creates the offscreen viewport target, the tile + blit pipelines (selecting the
    // bytecode format the device accepts), and a nearest sampler. Throws std::runtime_error
    // on any GPU resource-creation failure. The window must already be claimed for the
    // device (SdlPlatform does this at construction). `viewport` defaults to `defaultViewport`
    // (GameBoyColor until setActive() changes it).
    //
    // `window == nullptr` builds a COMPOSE-ONLY renderer: it creates the device-side compose
    // resources but no blit pipeline and never acquires a swapchain, so it composes + captures the
    // viewport offscreen (captureViewport) but cannot present (renderFrame's blit is skipped). The
    // only thing the windowed path needs the window for is the swapchain-format query the blit
    // pipeline is built against; compose needs only the device. This is the offscreen-capture seam.
    Renderer(SDL_GPUDevice* device, SDL_Window* window, ViewportResolution viewport = defaultViewport);
    ~Renderer();

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Upload an INDEXED tile/sprite atlas once (amortized — at load time / tileset swap), returning
    // its AtlasManifest — the sheet: the uploaded-atlas handle plus the carved slots (where only the
    // handle is wanted, write the projection explicitly: `sheet.atlasId`). ONE palette index per
    // pixel, stored as R32_UINT so a pixel can address an arbitrary palette — NOT RGBA; colour comes
    // from a palette at render time. 8-bit and 16-bit source indices are widened into the 32-bit
    // texel; a 32-bit source uploads directly. The renderer owns the GPU texture; the handle stays
    // valid until the renderer is destroyed (no eviction here). Throws std::runtime_error on a GPU
    // failure.
    //
    // Every upload declares the sheet's CARVE — the meaning slot resolution (atlasSlot, an
    // AnimationFrame's tile()/size()) reads the sheet through. The three-argument form carves Single:
    // the whole image is one asset (slot 0). The assetSize/kind form declares a grid, in the same
    // sentence loadAtlas speaks minus the decode — one carve per sheet, stated at upload.
    //
    // `transparent` is this sheet's structural transparent-index set: the palette indices that render
    // as HOLES (discarded, revealing whatever is behind them). BOTH the tile and sprite paths honour
    // it. The default TransparentIndices::None ({}) declares no structural hole → every index draws
    // (subject only to its palette entry's own alpha — material transparency). Game-Boy-style art opts
    // its OBJ hole in with TransparentIndices::GameBoy ({0}); an arbitrary set is
    // TransparentIndices::of({...}).
    AtlasManifest uploadAtlas(const std::uint8_t*  indices, int width, int height, TransparentIndices transparent = TransparentIndices::None);
    AtlasManifest uploadAtlas(const std::uint16_t* indices, int width, int height, TransparentIndices transparent = TransparentIndices::None);
    AtlasManifest uploadAtlas(const std::uint32_t* indices, int width, int height, TransparentIndices transparent = TransparentIndices::None);
    AtlasManifest uploadAtlas(const std::uint8_t* indices, int width, int height,
                              AssetDimensions assetSize, ContentKind kind,
                              ReadOrder order = ReadOrder::LeftRightThenDown, int count = 0,
                              TransparentIndices transparent = TransparentIndices::None,
                              int framesPerAnimation = 0);
    AtlasManifest uploadAtlas(const std::uint16_t* indices, int width, int height,
                              AssetDimensions assetSize, ContentKind kind,
                              ReadOrder order = ReadOrder::LeftRightThenDown, int count = 0,
                              TransparentIndices transparent = TransparentIndices::None,
                              int framesPerAnimation = 0);
    AtlasManifest uploadAtlas(const std::uint32_t* indices, int width, int height,
                              AssetDimensions assetSize, ContentKind kind,
                              ReadOrder order = ReadOrder::LeftRightThenDown, int count = 0,
                              TransparentIndices transparent = TransparentIndices::None,
                              int framesPerAnimation = 0);

    // A loaded PNG must NOT be pushed straight to uploadAtlas — that bypasses the slicing system. Load
    // an image via loadAtlas() (below), which slices it into an addressable AtlasManifest; uploadAtlas
    // is only for raw index arrays you author yourself. This overload exists solely to make the
    // image → uploadAtlas route an error: it always throws std::logic_error. (Taking a LoadedImage's
    // .indices.data() and calling a raw-pointer overload can't be caught at the type level — that path
    // is, by construction, the deliberate "I'm specifying the bytes myself" escape hatch — but handing
    // a whole LoadedImage to uploadAtlas is the obvious mistake, and it now fails loudly.)
    AtlasManifest uploadAtlas(const LoadedImage&);

    // Load an indexed PNG, upload it as ONE atlas, and slice it into addressable sub-asset slots —
    // the ergonomic chain over loadPng → uploadAtlas → sliceLayout. Returns an
    // AtlasManifest { atlas, slots } whose `slots[i]` carries the i-th carved asset's top-left atlas
    // cell + dimensions (in `order`, per `kind`). `order` defaults to the western LeftRightThenDown.
    // `count` caps how many assets are carved (0 = the whole grid; a positive count emits only the
    // first `count` in read order — for a sheet with room for more cells than the art uses, so the
    // manifest holds exactly the real assets, not trailing empties; see sliceLayout). `transparent`
    // passes straight through to uploadAtlas (the structural transparent-index set — None = no hole).
    // A degenerate slice yields an empty `slots` (sliceLayout never throws); a load / decode /
    // GPU failure throws std::runtime_error (loadPng's + uploadAtlas's contract).
    //
    // `framesPerAnimation` is recorded on the returned manifest (AtlasManifest::framesPerAnimation) for
    // an AnimationSeries sheet — the grid holds multiple animations, this many frames each, so
    // manifest.animation(g) yields each animation's run. It is consulted ONLY for ContentKind::AnimationSeries
    // (grouping is a manifest concern, not a carve concern — the slot carve is identical for every grid
    // kind); 0 (the default) leaves the manifest ungrouped. `count`, if set, caps the flat carve first;
    // grouping then applies to the capped result.
    //
    // The path is a LITERAL, project-root-relative logical path (a string literal — the build-time scan
    // reads it to bake or copy the asset, so a runtime/computed path is a compile error; load a runtime
    // file with loadAtlasFromMemory(readFile(...)) instead). `policy` selects whether the
    // atlas image is read from disk (LoadFromPath) or decoded from bytes baked into the binary at build
    // time (Embed). nullopt (the default) falls through to loadAtlas's per-type default (LoadFromPath —
    // atlases are the copyright surface). A LoadFromPath asset resolves against the runtime asset root
    // (assetRoot()); an Embed asset the build did not bake falls through to that disk read.
    AtlasManifest loadAtlas(LiteralPath path, AssetDimensions assetSize,
                            ContentKind kind, ReadOrder order = ReadOrder::LeftRightThenDown,
                            int count = 0, TransparentIndices transparent = TransparentIndices::None,
                            int framesPerAnimation = 0, std::optional<AssetPolicy> policy = {});
    // Same, from an in-memory PNG byte span (headless tests, embeddable assets).
    AtlasManifest loadAtlasFromMemory(std::span<const std::uint8_t> bytes, AssetDimensions assetSize,
                                      ContentKind kind, ReadOrder order = ReadOrder::LeftRightThenDown,
                                      int count = 0, TransparentIndices transparent = TransparentIndices::None,
                                      int framesPerAnimation = 0);

    // Upload one palette's colours once (amortized — on change), returning the handle the draw
    // state's palette set references. ARBITRARY entry count — no cap: the colours are
    // appended to a flat, contiguous renderer-owned palette store (the returned PaletteId IS this
    // palette's flat offset into it), and the store texture grows to fit. Valid until the renderer
    // is destroyed (no eviction). Throws std::runtime_error on a GPU failure.
    //
    // The store keeps 16-bit-per-channel colour. The Rgba8 overload widens each entry losslessly
    // (×257, so 255 → 65535); the Rgba16 overload (for a 16-bit colour source) appends direct. Both
    // produce identical output for an 8-bit palette — the shader samples the UNORM store as float4
    // either way.
    PaletteId uploadPalette(std::span<const Rgba8> colors);
    PaletteId uploadPalette(std::span<const Rgba16> colors);

    // Load a colour PNG as a palette: decode it, slice it one-pixel-per-entry in `order`, and upload the
    // entries to the palette store — the colour-store sibling of loadAtlas (which uploads to the atlas
    // store). Returns the PaletteId of the first entry; the entries are contiguous, so a cell/sprite that
    // names this palette selects entry k by index k (no manifest — palette entries don't scatter the way
    // sliced atlas sub-assets do). `order` defaults to the western LeftRightThenDown; `count` caps how
    // many entries are taken (0 = every pixel; past capacity clamps + logs, per readOrderCells). A
    // truecolour (RGBA) PNG is required — an indexed source throws std::runtime_error (slicePaletteImage's
    // contract); a load / decode / GPU failure throws std::runtime_error.
    //
    // The path is a LITERAL, project-root-relative logical path (a string literal — the build-time scan
    // reads it to bake or copy the asset, so a runtime/computed path is a compile error). `policy` selects
    // whether the image is read from disk (LoadFromPath) or decoded from bytes baked into the binary at
    // build time (Embed). nullopt (the default) falls through to loadPaletteImage's per-type default
    // (Embed — a palette image is bespoke build-time colour data, like a map PNG). A LoadFromPath asset
    // resolves against the runtime asset root (assetRoot()); an Embed asset the build did not bake falls
    // through to that disk read.
    //
    // There is no FromMemory sibling: a palette is already buildable directly from colour data via
    // uploadPalette (which takes Rgba8 / Rgba16 spans). For a runtime-supplied palette PNG, compose the
    // public primitives — uploadPalette(slicePaletteImage(loadPngFromMemory(bytes), order, count)).
    PaletteId loadPaletteImage(LiteralPath path,
                               ReadOrder order = ReadOrder::LeftRightThenDown,
                               int count = 0,
                               std::optional<AssetPolicy> policy = {});

    // Bake a closed curve boundary into a signed-distance mask once, returning a handle a region references
    // via ShapePoints::curveMask. A cubic / Catmull-Rom / arbitrary boundary has no closed-form GPU distance;
    // Curve::signedDistance is sampled over the boundary's bounding box (inflated by `padding` px so radius /
    // stroke inflation up to that margin reads a valid distance) into an R16_FLOAT field, uploaded as a
    // bilinear-sampled texture. `maxResolution` caps the field's longer axis (the shorter scales by aspect).
    // Bake once at setup; a region samples it every frame, and the region transform moves / scales / skews it
    // with no re-bake. The renderer owns the texture; the handle stays valid until the renderer is destroyed
    // (no eviction). Linear + quadratic boundaries do not need a mask — they are exact analytically. Throws
    // std::runtime_error on a GPU failure.
    CurveMaskId bakeCurveMask(const Curve& boundary, float padding = 8.0f, int maxResolution = 256);

    // The one-call ergonomic over bakeCurveMask: bake `boundary`'s mask and return a ready region shape whose
    // boundary IS the curve and whose curveMask is the baked handle (`radius` inflates it, `t` warps it). The
    // default authoring path for a cubic / arbitrary curved region — equivalent to
    // ShapePoints::fromCurve(boundary, radius, t) with .curveMask set from bakeCurveMask(boundary, ...).
    [[nodiscard]] ShapePoints bakeCurveRegion(const Curve& boundary, float radius = 0.0f, Transform t = {},
                                              float padding = 8.0f, int maxResolution = 256);

    // Register a game-authored custom shader stage BY PATH, returning a handle the draw state
    // references via ScreenSpaceEffect{ .kind = Custom, .customShader = <handle> } at EITHER scope — per-
    // layer (DrawLayer::effect) or frame-level (postEffects). A custom shader is a first-class effect kind,
    // composing with the built-ins in the same machinery and driven by the SAME inline parameter fields:
    //
    //   auto stage = renderer.registerPostProcessStage("game/shaders/my_effect.frag.hlsl");
    //
    // That path is the whole registration — no ShaderVariants, no generated-header include, no CMake rule,
    // no uniform struct or size. A build-time source scan (CMake retropp_autocompile_shaders) sees the
    // `.hlsl` path referenced in the code, INJECTS the standard preamble (the source texture + sampler +
    // sampleSource() + the platform edge-mode cbuffer at b0; shaders/include/retropp_effect.hlsli), compiles
    // it to this platform's GPU bytecode, embeds it, REFLECTS the shader's OWN parameter cbuffer (its own
    // named fields at b1/space3) into a generated packer, and registers it under that exact path string.
    // The game's shader is therefore its OWN cbuffer + a `main()` body that samples through sampleSource();
    // the game sets the shader's OWN reflected params as inline fields on the effect (.kind = Custom,
    // .customShader = <handle>, .<param> = …), exactly like a built-in. Two pipelines are built once
    // (no-blend replace for frame-level / Below, premultiplied-over for Layer); handles stay valid until
    // the renderer is destroyed.
    //
    // The path is a LiteralPath: it MUST be a string literal, because the build-time scan reads it out of
    // the source verbatim. Passing a runtime variable / std::string / computed path is a COMPILE error
    // (see literal_path.h) — not a runtime surprise. The runtime std::runtime_error below is the residual
    // backstop for a literal referenced from a source the scan does not read (e.g. a header): no shader
    // was compiled for it. The ShaderVariants overload is the lower-level seam the path form resolves to.
    PostProcessStageId registerPostProcessStage(LiteralPath shaderPath);
    PostProcessStageId registerPostProcessStage(const ShaderVariants& fragment);

    // One frame: composite the submitted INDEXED TILES + SPRITES layers into the offscreen
    // viewport (z-sorted, alpha-blended; per-tile palette-select + flip applied in-shader from
    // the layer's palette set), run the frame-level post-process chain (frame.postEffects; an
    // empty chain is a no-op), then blit the result integer-scaled + letterboxed to the swapchain
    // with the frame-level colour transform. Throws std::invalid_argument on a layer-key collision
    // when the collision policy is Throw (the default in debug builds; see setLayerCollisionPolicy).
    //
    // When interpolation is on (the default), the renderer reads the
    // run loop's sub-tick factor + tick signal from the frame-timing channel (frame_timing.h), reconciles
    // this submission into its per-id retained mirror once per tick, and composites each layer/sprite eased
    // between its previous and current tick state — so the game submits its latest state and the platform
    // blends. With interpolation off (automaticInterpolation(false)) the submission composites verbatim.
    void renderFrame(const FrameDrawState& frame);

    // Automatic interpolation, runtime-dynamic. A renderer starts at defaultInterpolation (seeded from
    // EngineConfig::interpolation by setActive()); this toggles it on the fly. On: ease each object between
    // its previous and current sim-tick state by the loop's sub-tick factor (the per-id mirror tracks the
    // last two ticks). Off: composite each submission exactly as given (no mirror, no blend).
    void               automaticInterpolation(bool enabled) noexcept { interpolation_ = enabled; }
    [[nodiscard]] bool automaticInterpolation() const noexcept { return interpolation_; }

    // Evaluation grid, runtime-dynamic. A renderer starts at defaultEvaluationGrid (seeded from
    // EngineConfig::evaluationGrid by setActive()); this is the runtime override — call it to switch the
    // analytic paths' evaluation granularity on the fly (e.g. a settings toggle). Viewport = the analytic
    // math (transformed tiles, effect regions, displace/ripple) evaluates on the viewport grid, so the
    // upscaled image is pixel-identical to the viewport-resolution rasterization (crisp); Output = evaluate
    // per output pixel (smooth edges/displacement under upscale). A mathematical no-op when the compositor
    // runs at viewport resolution — the choice only bites once placement composites onto a finer grid.
    void                 evaluationGrid(EvaluationGrid grid) noexcept { evaluationGrid_ = grid; }
    [[nodiscard]] EvaluationGrid evaluationGrid() const noexcept { return evaluationGrid_; }

    // Compose `frame` and download the finished viewport image as packed Rgba8 (viewport width × height,
    // row-major, top-to-bottom). Runs the same compose path renderFrame blits — copy pass, layer
    // composite, post-process chain — then downloads the composed offscreen image instead of presenting
    // it (the swapchain blit is skipped; the composed viewport is the captured subject). Blocks on a fence
    // until the download lands. The offscreen intermediates are R16G16B16A16_FLOAT (a colour channel may
    // exceed 1); the download quantizes each channel with round(clamp(v,0,1)·255) — the same 8-bit clamp
    // the swapchain blit applies — so the result matches what a present would show. This is the
    // offscreen-capture seam for the golden-readback harness, not part of the runtime render loop. Works
    // on any renderer (windowed or compose-only).
    [[nodiscard]] std::vector<Rgba8> captureViewport(const FrameDrawState& frame);

    // Compose `frame` at an explicit compose scale and download the finished image (composeScale·viewport
    // wide × tall, packed Rgba8, row-major top-to-bottom). captureViewport(frame) is exactly this at scale 1
    // — the byte-identical golden-capture path. A scale > 1 composes on the finer grid the interpolation path
    // uses, so it captures the output-resolution image the current evaluation grid produces (Viewport →
    // pixel-identical to the scale-1 capture nearest-upscaled; Output → the smooth output-res evaluation).
    // The parity seam for the crisp harness — not part of the runtime render loop; works on any renderer.
    [[nodiscard]] std::vector<Rgba8> captureViewport(const FrameDrawState& frame, int composeScale);

    // The platform renderer. A program constructs exactly one, and Sprite::mask / freezeMask / maskShape
    // resolve a sprite's AtlasId against its already-uploaded atlas pixels through this handle. Throws
    // std::logic_error if called before the renderer is constructed.
    [[nodiscard]] static const Renderer& instance();

    // The uploaded pixel size of an atlas, or {0, 0} for an unknown id. Reads the CPU atlas mirror the
    // upload already keeps; the sprite shape query's cell math uses it.
    [[nodiscard]] PixelSize atlasPixelSize(AtlasId atlas) const noexcept;

    // Is the atlas pixel at (x, y) VISIBLE — its palette index is not a structural hole for that sheet? An
    // out-of-bounds coordinate or an unknown atlas is not visible. This is the device-free coverage read the
    // sprite shape query answers contains() with, off the same CPU atlas mirror the upload keeps — no GPU.
    [[nodiscard]] bool atlasVisibleAt(AtlasId atlas, int x, int y) const noexcept;

    // Slot `slotIndex` of a sheet — its top-left atlas cell + pixel size, exactly the values the
    // sheet's AtlasManifest carries at [slotIndex], resolved arithmetically from the slice geometry
    // recorded at upload (no slot list is consulted). This is how a sheet named only by its AtlasId
    // answers slot → cell/size — an AnimationFrame's tile()/size() read. Returns AssetSlot{} (and
    // logs a warning) for an unknown atlas or an index at/past the carved slot count.
    [[nodiscard]] AssetSlot atlasSlot(AtlasId atlas, std::size_t slotIndex) const noexcept;

    // The runtime reaction when a frame submits colliding layer keys (duplicate z or label).
    // Defaults to kDefaultCollisionPolicy (Throw in debug, WarnAndResolve in release); a host
    // can override it (e.g. force Throw in a soak test, or WarnAndResolve in a kiosk build).
    void setLayerCollisionPolicy(LayerKeyCollisionPolicy policy) noexcept { collisionPolicy_ = policy; }
    [[nodiscard]] LayerKeyCollisionPolicy layerCollisionPolicy() const noexcept { return collisionPolicy_; }

    // Blit sampling, runtime-dynamic. A renderer starts at defaultSamplingMode (seeded from
    // config.enhancements.sampling by EngineConfig::setActive(), so the call site doesn't apply it); this
    // is the runtime override — call it to switch sampling on the fly (e.g. a settings toggle).
    // Nearest = crisp integer pixels (the faithful default); Bilinear = smoothed upscale. Both samplers are
    // created at construction; the blit binds the one this selects. The viewport always fills the window at
    // the largest integer scale that fits (integerScaleToFitRect) — output SIZE is the window's size, owned
    // by the platform (window().size()), not a renderer mode.
    void samplingMode(SamplingMode mode) noexcept { sampling_ = mode; }
    [[nodiscard]] SamplingMode samplingMode() const noexcept { return sampling_; }

    // The renderer's diagnostic surface: cumulative work counters since construction, plus what the most
    // recent frame spent phase by phase.
    //
    // The counters say how much per-layer upload and full-frame compose work the renderer issued versus
    // skipped as redundant. A game reads these to confirm the redundant-work elimination is engaging — on a
    // still screen the skip counters climb while the issued counters hold steady; under motion they invert.
    // Each `*Skips` is meaningful against its issued counterpart (the pair is the ratio).
    //
    // Snapshot by value — the caller reads a copy, not a live handle.
    struct RenderStats {
        std::uint64_t tilemapUploads = 0;  // per-layer tilemap-texture uploads issued
        std::uint64_t tilemapSkips   = 0;  // ...skipped (cell content unchanged since the last upload)
        std::uint64_t spriteUploads  = 0;  // per-layer sprite-record buffer uploads issued
        std::uint64_t spriteSkips    = 0;  // ...skipped (built records unchanged)
        std::uint64_t rasterUploads  = 0;  // per-layer raster uploads issued
        std::uint64_t rasterSkips    = 0;  // ...skipped (the raster's generation had not moved)
        std::uint64_t composePasses  = 0;  // full-frame composes run (both renderFrame and captureViewport)
        std::uint64_t composeSkips   = 0;  // ...skipped (frame bit-identical → retained output re-blitted)
        std::uint64_t presentPasses  = 0;  // renderFrame calls that blitted a swapchain texture
        std::uint64_t presentSkips   = 0;  // ...that had none to blit (no window, or not ready this frame)

        // What the last renderFrame spent, in milliseconds of CPU time, split by phase. Read alongside the
        // counters: a phase that grows while its counter holds steady is doing more work per unit, not more
        // units of work.
        //
        // `presented` is the honest source of frame rate. The Metal path acquires the swapchain
        // non-blocking, so the render callback keeps firing at full rate while presents are silently
        // skipped — counting callbacks reports 60 for a scene the eye sees as frozen. Count presented
        // frames instead, and read the callback rate beside it: callbacks far above presents is a GPU that
        // has fallen behind, both falling together is a CPU or callback stall.
        struct PhaseTimings {
            double acquireMs      = 0.0;    // acquiring the command buffer
            double interpMs       = 0.0;    // interpolation reconcile + interpolate
            double composeMs      = 0.0;    // composing the viewport image; 0 when the frame skipped it
            double presentMs      = 0.0;    // swapchain acquire, blit and submit
            bool   presented      = false;  // this frame reached the screen
            bool   composeSkipped = false;  // the retained output was re-blitted instead of recomposed
        };
        PhaseTimings lastFrame{};
    };

    // A snapshot of the diagnostic surface, by value.
    [[nodiscard]] RenderStats renderStats() const noexcept { return renderStats_; }

private:
    // An uploaded indexed atlas: not its own texture — every atlas lives as a region of the single
    // flat atlas store (atlasStore_), exactly as every palette lives in the flat palette store.
    // `data` is the CPU mirror of this atlas's R32_UINT pixels (kept so the store can be rewritten when
    // a wider atlas re-lays it out, mirroring paletteData_); `width`/`height` are its
    // pixel dimensions (tile-grid addressing); `transparent` its structural transparent-index set
    // ({} = none); `storeY` its top row in the vertically-stacked store. AtlasId = index.
    // One atlas's row of the region table: where its rows start in the store, how many tile columns it
    // has, and its transparent-index set as a 64-bit mask split across two words (bit i set => index i is
    // a hole). Four R32_UINT words, which is the texel both frag stages Load by atlas handle.
    struct AtlasRegionWords {
        std::uint32_t storeY        = 0;
        std::uint32_t cols          = 0;
        std::uint32_t transparentLo = 0;
        std::uint32_t transparentHi = 0;
    };

    struct AtlasEntry {
        std::vector<std::uint32_t> data;
        int                width = 0, height = 0, storeY = 0;
        TransparentIndices transparent{};
        // The slice geometry loadAtlas carved this sheet with — upload-time asset metadata, like
        // width/height/transparent above (slotCount 0 = uploaded without slicing, e.g. bare uploadAtlas).
        // atlasSlot() resolves a slot index arithmetically from these, so a sheet named only by its
        // AtlasId (an AnimationFrame's `.sheet`) answers slot → cell/size with no manifest retained.
        AssetDimensions    slotSize{};
        ContentKind        slotKind  = ContentKind::Single;
        ReadOrder          slotOrder{};
        int                slotCount = 0;
    };
    // A baked curve signed-distance mask: its own R16_FLOAT texture (sampled, bilinear) plus the shape-local
    // bake box the shader maps fragments into. CurveMaskId is 1-based (0 = none); curveMasks_[id − 1] is this.
    struct CurveMaskEntry {
        SDL_GPUTexture* texture = nullptr;
        Vec2            bakeMin{};      // bake box min corner (shape-local px)
        Vec2            bakeExtent{};   // bake box size (shape-local px)
        int             width = 0, height = 0;
    };
    // A per-layer tilemap cell texture (R32_UINT, packTileCell'd), recreated when its tile
    // dimensions change, and the state that lets the slot skip re-uploading unchanged content.
    struct TilemapTex {
        SDL_GPUTexture*             texture = nullptr;
        int                         widthInTiles  = 0;
        int                         heightInTiles = 0;
        // Upload-skip state. `staging` is the retained CPU pack target (reused every frame — no
        // per-frame allocation); `transfer` is the slot's pooled transfer buffer (sized to content,
        // released + recreated with the texture on a dims change, mapped with cycle=true). `sig`
        // records what the last upload stored: None never skips (fresh slot / just recreated), Hashed
        // skips when the packed-cell hash matches (the auto path), Manual holds a caller-declared upload
        // (the contentChanged path) whose content is valid but whose hash was never computed. Storing the
        // KIND keeps a Manual slot from ever producing a false auto-path skip against its stale hash.
        enum class TileSig : std::uint8_t { None, Hashed, Manual };
        TileSig                     sig = TileSig::None;
        std::uint64_t               contentHash = 0;
        std::vector<PackedTileCell> staging;
        SDL_GPUTransferBuffer*      transfer = nullptr;
    };
    // A per-layer raster texture (RGBA8, the raster's own dimensions), recreated when its dimensions
    // or pixel layout change. There is no hash beside it: a full-color raster differs every time its
    // source draws, so hashing one costs more than the upload it could save. `uploadedGeneration` is
    // which generation this texture holds — a submission carrying that same generation is already
    // resident and skips, and 0 means nothing has landed here.
    struct RasterTex {
        SDL_GPUTexture*        texture = nullptr;
        int                    width   = 0;
        int                    height  = 0;
        RasterPixelFormat      format  = RasterPixelFormat::Rgba8888;
        std::uint64_t          uploadedGeneration = 0;
        SDL_GPUTransferBuffer* transfer = nullptr;
    };
    // A per-layer sprite storage buffer (GpuSprite records). Grow-only: recreated only when a
    // frame's sprite count exceeds its capacity (in sprites), reused across frames otherwise.
    // `count` is the number of GpuSprite records actually uploaded (the art-drawing sprites — a Below-scope
    // lens draws no art, so it is excluded), which is the instance count drawLayer issues; `capacity` is the
    // buffer's allocated record capacity (grow-only).
    struct SpriteBuf {
        SDL_GPUBuffer* buffer = nullptr;
        int            capacity = 0;
        int            count = 0;
        // Upload-skip state, mirroring TilemapTex's. `transfer` is the slot's pooled transfer buffer, sized
        // to `capacity` records and released + recreated with the buffer when it grows, mapped with
        // cycle=true (it may still be in-flight from a prior frame's copy). `hashed` is false on a fresh or
        // just-recreated slot so a new buffer is never skipped against stale state; once true, a frame whose
        // built records hash to `contentHash` at `contentCount` records is already in the buffer and skips.
        bool                   hashed = false;
        std::uint64_t          contentHash  = 0;
        int                    contentCount = 0;
        SDL_GPUTransferBuffer* transfer = nullptr;
    };

    // The GPU resources one compose stages and hands back for the caller to release AFTER the command
    // buffer is submitted: the per-upload transfer buffers, plus any transient textures/buffers a
    // degenerate-keyed layer needed this frame (see the tilemaps_/spriteBufs_ note). Both are used by the
    // frame's command buffer, so — unlike a persistent slot evicted for a despawn (safe to release the
    // instant it stops being submitted) — they must outlive the submit. renderFrame and captureViewport
    // each own one, pass it to composeViewport, and drain all three lists post-submit.
    struct FrameScratch {
        std::vector<SDL_GPUTransferBuffer*> transfers;
        std::vector<SDL_GPUTexture*>        textures;
        std::vector<SDL_GPUBuffer*>         buffers;
    };

    // Core indexed-atlas upload (R32_UINT); the public uploadAtlas overloads widen into it.
    AtlasId uploadAtlas32(const std::uint32_t* indices, int width, int height, TransparentIndices transparent);

    // Carve an uploaded atlas into its manifest and record the slice geometry on the sheet's
    // AtlasEntry (atlasSlot resolves from it) — the shared carve step every upload/load door runs.
    AtlasManifest carveUploaded(AtlasId atlas, AssetDimensions assetSize, ContentKind kind,
                                ReadOrder order, int count, int framesPerAnimation);

    // Write the atlas `id` just appended to atlases_ into the store, and give it its row in the region
    // table. An atlas that fits the room the store texture already has is written over its own rows, and
    // its row of the table travels in the same submission; a wider one, or one past the allocated height,
    // goes through rebuildAtlasStore.
    void appendAtlasToStore(AtlasId id);

    // The region-table row describing one atlas.
    static AtlasRegionWords atlasRegionWords(const AtlasEntry& atlas);

    // Recreate atlasStore_ from the atlases_ CPU mirrors: stack them vertically (width = widest,
    // height = Σ heights), assign each entry's storeY, upload every atlas, and rebuild the region table
    // beside it. The store texture is allocated with room past that height, so the atlases uploaded next
    // are appended rather than restacked.
    void rebuildAtlasStore();

    // Send every store write an upload has recorded through `copy`, opening it on `cmd` if it is not open
    // yet, and record each transfer buffer in `scratch` for release after the frame is submitted.
    void flushStoreWrites(SDL_GPUCommandBuffer* cmd, SDL_GPUCopyPass*& copy, FrameScratch& scratch);

    // Recreate atlasRegionStore_ with a texel per atlas, allocated with room past the current count.
    void rebuildAtlasRegions();

    // Write the `count` colours appended at flat offset `first` into the palette store: one upload of
    // those colours, or two when the run crosses a row edge. A run past the allocated rows, or one long
    // enough to cover a whole row, goes through rebuildPaletteStore.
    void appendPaletteColors(std::size_t first, std::size_t count);

    // Recreate paletteStore_ from the flat paletteData_ mirror: a kPaletteStoreWidth-wide
    // R16G16B16A16_UNORM texture holding every entry, the whole store uploaded. The texture is allocated
    // with rows past what the entries need, so the palettes uploaded next are appended rather than
    // rewritten.
    void rebuildPaletteStore();

    // Compose the finished viewport image for `frame` into an offscreen target and return which texture
    // holds it (target_ when the post-process chain is empty, else the chain's final ping-pong scratch).
    // This is everything renderFrame does up to the blit: the copy pass (tilemap/sprite/row-data uploads,
    // recording transfer buffers in `scratch` for the caller to release after submit), the layer
    // composite, and the post-process chain. renderFrame = composeViewport + blit; captureViewport =
    // composeViewport + download. The composed bytes are identical in both — one compose path, no drift.
    // When `interpolate` is set, each layer's / sprite's placement position is read as the eased float
    // (interpolatedLayerScroll / interpolatedSpritePos) at `timing` — the sub-pixel position on the
    // output-resolution grid; the frame's integer fields are the fallback for an id with no history.
    // Off (captureViewport, interpolation disabled) places at the frame's integer positions.
    SDL_GPUTexture* composeViewport(SDL_GPUCommandBuffer* cmd, const FrameDrawState& frame,
                                    FrameScratch& scratch,
                                    const FrameTiming& timing, bool interpolate);

    // The compose scale for this frame: the window drawable's integer-scale-to-fit factor (clamped to
    // [1, kMaxComposeScale]) when interpolation is on and a window exists; 1 otherwise. 1 is the
    // faithful path (compose at viewport res, blit upscales) — the byte-identical guarantee attaches to
    // it. captureViewport does not call this (it pins 1 directly).
    [[nodiscard]] int resolveComposeScale() const;

    // Point composeScale_/composeW_/composeH_ at `scale` and (re)create the four offscreen targets
    // (target_/post0_/post1_/layerScratch_) at the new compose grid. A no-op when `scale` already
    // matches and the targets exist — guarded so a steady window never reallocates. Called from the
    // ctor (scale 1), from renderFrame after resolveComposeScale, and from captureViewport (pinning 1).
    void resizeComposeTargets(int scale);

    // (Re)create the Bloom / Glow emission scratch at the current compose grid, on first use and after a
    // resize. Returns false if any allocation fails, in which case the caller falls back to the identity
    // passthrough rather than dropping the effect's source.
    [[nodiscard]] bool ensureEmissionTargets();
    void releaseEmissionTargets();

    // (Re)create the emission atlas and its reduction chain at `width` × `height`, and the rect side-table
    // at `rows` fields. Grows to hold a frame's peak demand and steps back down a power of two once demand
    // has stayed under half for a sustained run — growth alone would ratchet, so one heavy scene would size
    // the atlas for the rest of the process. Returns false if any allocation fails, in which case the layer
    // binds the idle pair and its Bloom / Glow fields add no light.
    [[nodiscard]] bool ensureEmissionAtlas(int width, int height, int sheets, int rows);
    void releaseEmissionAtlas();

    // (Re)create the 1×1 idle atlas and its 1-texel rect table — what a layer binds when it authors no
    // field. Both bindings are declared unconditionally in every sprite fragment variant, so something valid
    // is always bound; a layer that does not glow pays one texel each.
    [[nodiscard]] bool ensureIdleEmissionAtlas();

    void releaseAtlases();
    void releaseTilemaps();
    void releaseRasters();
    void releaseSpriteBuffers();
    void releaseCustomStages();
    void releaseBatchResources();

    // Build the instanced-additive region pipeline for a custom stage whose shader carries the
    // `// @retropp:additive` declaration: the platform's region_batch vertex stage + the shader's BATCHED
    // fragment variant, with ADDITIVE colour blend (ONE / ONE) and destination-alpha-preserving alpha
    // blend (ZERO / ONE) — so many same-shader additive regions accumulate in ONE pass. Called by the
    // path-registering overload when findBatchedShaderVariants(path) is non-null; returns nullptr on
    // failure (the stage then stays on the per-region path).
    [[nodiscard]] SDL_GPUGraphicsPipeline* buildBatchedStagePipeline(const ShaderVariants& batchedFragment);

    // Build ONE gathered-region pipeline for a custom stage whose shader has a gather variant: the
    // shared fullscreen-triangle vertex stage + the shader's GATHER fragment variant (1 sampler + 1 storage
    // texture + 2 uniforms + 1 fragment storage buffer of per-region records). `blend` picks the replace
    // pipeline (no blend, frame-level / Below / mid-chain) or the premultiplied-over pipeline (ONE /
    // ONE_MINUS_SRC_ALPHA — the Normal-layer last-step composite onto target_). Returns nullptr on failure
    // (the stage then stays on the per-region path). Registered as the pair customGather_/customGatherBlend_.
    [[nodiscard]] SDL_GPUGraphicsPipeline* buildGatherStagePipeline(const ShaderVariants& gatherFragment,
                                                                    bool blend);

    // Build the sprite-inline pipeline for a custom stage whose shader has a sprite variant: the platform's
    // sprite vertex stage + the shader's SPRITE fragment variant (the sprite fragment with the custom body
    // injected), with the SAME resource contract and alpha blend as the stock sprite pipeline. Called by the
    // path-registering overload when findSpriteShaderVariants(path) is non-null; returns nullptr on failure
    // (the stage then can't run inline on a sprite). Registered as customSprite_.
    [[nodiscard]] SDL_GPUGraphicsPipeline* buildSpriteStagePipeline(const ShaderVariants& spriteFragment);

    // Build the below-custom pipeline for a custom stage whose shader has a sprite-below variant: the platform's
    // sprite vertex stage + the shader's SPRITE_BELOW fragment variant (the below sprite fragment with the
    // custom body injected, sampleSource reading the scene), with the SAME resource contract and blend as the
    // built-in spriteBelow_ pipeline. Called by the path-registering overload when
    // findSpriteBelowShaderVariants(path) is non-null; the built-in spriteBelow_ is built from this too (the
    // stock fragment). Registered as customSpriteBelow_.
    [[nodiscard]] SDL_GPUGraphicsPipeline* buildSpriteBelowStagePipeline(const ShaderVariants& belowFragment);

    // Build a custom stage's replace/blend pipeline pair from `fragment` and push it (plus default entries in
    // every parallel vector) at the next stage id, returning that id. `numSourceSamplers` is 1 for a stock
    // custom stage (SourceTexture at t0/s0) and 2 for an emission consumer (SourceTexture at t0/s0 +
    // EmissionTexture at t1/s1) — the only difference between the two builds, so both the public ShaderVariants
    // overload (which passes 1 — a direct registration carries no emission declaration) and the path overload
    // (which passes 2 when isEmissionConsumer(path)) share this one seam.
    PostProcessStageId registerCustomStagePipelines(const ShaderVariants& fragment,
                                                    std::uint32_t numSourceSamplers);

    // Build a custom stage's emission EXTRACT pipeline from its `<ns>_emission` variant: a fullscreen replace
    // pipeline at the stock (1 sampler, 1 storage, 2 uniforms) layout targeting the emission scratch. Called
    // by the path overload when findEmissionShaderVariants(path) is non-null; nullptr on failure (the demand
    // then extracts through the stock brightpass). Registered as customEmission_.
    [[nodiscard]] SDL_GPUGraphicsPipeline* buildCustomEmissionExtractPipeline(const ShaderVariants& extractFragment);
    // The below WRITE pipeline for a Below-scope Custom lens: the stage's emission() body injected into the
    // rect-instanced below extract fragment (1 sampler [scene] + 1 storage [uFxStore] + 1 uniform). Built by the
    // path overload when findEmissionRectShaderVariants(path) is non-null; nullptr on failure (the lens then
    // fills its field through the stock rect brightpass). Registered as customEmissionRect_.
    [[nodiscard]] SDL_GPUGraphicsPipeline* buildCustomEmissionRectPipeline(const ShaderVariants& rectFragment);

    SDL_GPUDevice*           device_;
    SDL_Window*              window_;
    ViewportResolution       viewport_;
    // The compose grid content is rasterized onto: viewport × composeScale_. Held distinct from the
    // viewport's normalization role — effects and regions measure against the viewport (invViewportW/H,
    // region radii), while offscreen-target sizing and content placement use the compose grid. At
    // composeScale_ == 1 the two coincide. Resolved per frame from the window drawable size when
    // interpolation is on (so sub-pixel motion has a finer grid to land on), pinned to 1 otherwise
    // (interpolation off, no window / headless, or captureViewport — the faithful, byte-identical path).
    // resolveComposeScale() computes it; resizeComposeTargets() reallocates the offscreen targets when
    // it changes. composeW_/composeH_ are viewport × composeScale_.
    int                      composeScale_ = 1;
    int                      composeW_     = 0;   // viewport_.width  * composeScale_
    int                      composeH_     = 0;   // viewport_.height * composeScale_
    // The compose grid's ceiling. Sub-pixel placement is already visually continuous well below this;
    // the cap bounds the four float16 offscreen targets' memory (viewport×N)² · 8 B · 4 — at 16× and a
    // 160×144 viewport that is 2560×2304 ≈ 189 MB, reached only by a >4K drawable. Below the cap the
    // compose scale equals the window's integer-scale-to-fit factor, so the blit is a 1:1 centring copy
    // (fill parity with the faithful path); above it the blit integer-upscales the remainder as usual.
    static constexpr int     kMaxComposeScale = 16;
    // macOS/Metal ONLY: SDL's Metal GPU backend implements the BLOCKING swapchain wait as a CPU
    // busy-wait spin (SDL_gpu_metal.m METAL_WaitForFences: `while(!complete) // Spin!`), so the
    // blocking acquire burns a core while VSYNC holds the fence to vblank. On Metal we use the
    // NON-blocking acquire (a single fence check, skip-if-not-ready) and let the host-loop frame
    // deadline pace cadence instead. Vulkan/D3D12 OS-block correctly, so they keep the blocking
    // acquire. Set once at ctor.
    bool                     acquireNonBlocking_ = false;
    SDL_GPUTexture*          target_       = nullptr;  // offscreen viewport colour target
    SDL_GPUTexture*          post0_        = nullptr;  // post-process scratch A (viewport-sized)
    SDL_GPUTexture*          post1_        = nullptr;  // post-process scratch B (ping-ponged with A)
    SDL_GPUTexture*          layerScratch_ = nullptr;  // per-layer effect scratch; swapped with target_ for Below
    // The Bloom / Glow emission scratch, allocated on first use and released with the compose targets: two
    // at the compose grid (the extract writes one, the full-resolution blur ping-pongs the pair), one at
    // half and two at quarter for the reduced path. A game that never glows never pays for them.
    SDL_GPUTexture*          emission0_    = nullptr;  // extract output; the full-resolution blur's V target
    SDL_GPUTexture*          emission1_    = nullptr;  // the full-resolution blur's H target
    SDL_GPUTexture*          emissionHalf_ = nullptr;  // first ½ reduction
    SDL_GPUTexture*          emissionLow0_ = nullptr;  // second ½ reduction; the reduced blur's V target
    SDL_GPUTexture*          emissionLow1_ = nullptr;  // the reduced blur's H target
    // Every finished halo, both paths': footprint-sized fields packed into one atlas, so a layer's capacity is
    // atlas area rather than a count. The chain mirrors the scratch above — atlas0 takes the raster and the
    // full-resolution blur's second axis, atlas1 its first, and the reduced trio serves a wide reach — but
    // every pass is scissored to one page, the fields sharing a reach, so passes track the reaches a layer
    // authors and never its field count. `rects` is the side-table a field's record indexes to find where its
    // own rect sits; the two idle stand-ins keep both bindings valid on a layer that authors none.
    SDL_GPUTexture*          emissionAtlas0_    = nullptr;
    SDL_GPUTexture*          emissionAtlas1_    = nullptr;
    SDL_GPUTexture*          emissionAtlasHalf_ = nullptr;
    SDL_GPUTexture*          emissionAtlasLow0_ = nullptr;
    SDL_GPUTexture*          emissionAtlasLow1_ = nullptr;
    // The finished sheets the art and below fragments read: one array layer per sheet, so a layer needing more
    // fields than one sheet holds simply takes another. The scratch above is 2-D because the blur SAMPLES it,
    // and a sampled bind is a whole texture — so a sheet is blurred in 2-D and copied into its layer here.
    SDL_GPUTexture*          emissionAtlasStore_ = nullptr;
    SDL_GPUTexture*          emissionAtlasIdle_ = nullptr;
    SDL_GPUTexture*          emissionRects_     = nullptr;
    SDL_GPUTexture*          emissionRectsIdle_ = nullptr;
    int                      emissionAtlasW_    = 0;   // the allocated sheet, texels
    int                      emissionAtlasH_    = 0;
    int                      emissionAtlasSheets_ = 0; // array layers in the store
    int                      emissionRectRows_  = 0;   // fields the side-table can address
    int                      emissionAtlasQuiet_ = 0;  // consecutive frames whose demand fits half the atlas
    // The last over-capacity count reported. A layer over capacity is over it on every frame, and a per-frame
    // log would bury everything else in the console, so the report tracks the number rather than the frame.
    int                      emissionDropReported_ = 0;
    SDL_GPUGraphicsPipeline* tile_         = nullptr;  // indexed tilemap → atlas → palette compositor
    SDL_GPUGraphicsPipeline* raster_       = nullptr;  // a raster layer, one Load per pixel
    SDL_GPUGraphicsPipeline* sprite_       = nullptr;  // instanced per-sprite-quad → atlas → palette
    SDL_GPUGraphicsPipeline* spriteBelow_  = nullptr;  // Below-scope sprites: scene-reading, coverage-masked
    SDL_GPUGraphicsPipeline* displace_     = nullptr;  // row-displacement post-process stage
    SDL_GPUGraphicsPipeline* displaceBlend_ = nullptr; // displace + premultiplied-over composite (Layer scope)
    SDL_GPUGraphicsPipeline* ripple_       = nullptr;  // built-in radial ripple post-process stage
    SDL_GPUGraphicsPipeline* rippleBlend_  = nullptr;  // ripple + premultiplied-over composite (Layer scope)
    SDL_GPUGraphicsPipeline* swirl_        = nullptr;  // built-in angular-twist post-process stage
    SDL_GPUGraphicsPipeline* swirlBlend_   = nullptr;  // swirl + premultiplied-over composite (Layer scope)
    SDL_GPUGraphicsPipeline* colorFill_      = nullptr; // built-in colour fill / tint post-process stage
    SDL_GPUGraphicsPipeline* colorFillBlend_ = nullptr; // colorFill + premultiplied-over composite (Layer scope)
    SDL_GPUGraphicsPipeline* colorFillGather_      = nullptr; // N ColorFill regions in ONE pass, replace
    SDL_GPUGraphicsPipeline* colorFillGatherBlend_ = nullptr; // ColorFill gather + premultiplied-over composite
    SDL_GPUGraphicsPipeline* gleam_          = nullptr; // built-in gleam (diagonal sheen sweep) post-process stage
    SDL_GPUGraphicsPipeline* gleamBlend_     = nullptr; // gleam + premultiplied-over composite (Layer scope)
    SDL_GPUGraphicsPipeline* saturation_      = nullptr; // built-in colour-saturation (desaturate toward luma) post-process stage
    SDL_GPUGraphicsPipeline* saturationBlend_ = nullptr; // saturation + premultiplied-over composite (Layer scope)
    // The Bloom / Glow emission chain: extract → [reduce ×½ ×2] → blur H → blur V → composite. One
    // pipeline per stage, plus the premultiplied-over composite variant for the Layer scope. emissionCopy_
    // is the plain passthrough the reductions sample through and the identity path writes.
    SDL_GPUGraphicsPipeline* emissionExtract_        = nullptr; // per-pixel emission (threshold/intensity/tint folded)
    SDL_GPUGraphicsPipeline* emissionExtractRect_    = nullptr; // the scene's emission into each below field's rect
    SDL_GPUGraphicsPipeline* emissionBlur_           = nullptr; // one axis of the separable Gaussian
    SDL_GPUGraphicsPipeline* emissionComposite_      = nullptr; // source + blurred emission, replace
    SDL_GPUGraphicsPipeline* emissionCompositeBlend_ = nullptr; // composite + premultiplied-over (Layer scope)
    SDL_GPUGraphicsPipeline* emissionCopy_           = nullptr; // passthrough: the ½ reductions + identity, replace
    SDL_GPUGraphicsPipeline* emissionCopyBlend_      = nullptr; // passthrough + premultiplied-over (Layer scope identity)
    SDL_GPUGraphicsPipeline* spriteEmission_         = nullptr; // glowing sprites → the shared emission buffer, additive
    SDL_GPUGraphicsPipeline* regionSelect_      = nullptr; // region gate: inside?eff:src, replace
    SDL_GPUGraphicsPipeline* regionSelectBlend_ = nullptr; // region gate + premultiplied-over composite (Layer scope)
    SDL_GPUGraphicsPipeline* regionSelectCurve_      = nullptr; // curve-boundary region gate (analytic), replace
    SDL_GPUGraphicsPipeline* regionSelectCurveBlend_ = nullptr; // curve-boundary region gate + premultiplied-over composite
    SDL_GPUGraphicsPipeline* regionStencil_      = nullptr; // region see-through (source × survival), replace
    SDL_GPUGraphicsPipeline* regionStencilBlend_ = nullptr; // region see-through + premultiplied-over composite (Layer scope)
    SDL_GPUGraphicsPipeline* regionStencilCurve_      = nullptr; // curve-boundary region see-through (analytic), replace
    SDL_GPUGraphicsPipeline* regionStencilCurveBlend_ = nullptr; // curve-boundary region see-through + premultiplied-over composite
    SDL_GPUGraphicsPipeline* regionSelectCurveMask_       = nullptr; // curve-boundary region gate (baked SDF mask), replace
    SDL_GPUGraphicsPipeline* regionSelectCurveMaskBlend_  = nullptr; // curve-mask region gate + premultiplied-over composite
    SDL_GPUGraphicsPipeline* regionStencilCurveMask_      = nullptr; // curve-boundary region see-through (baked SDF mask), replace
    SDL_GPUGraphicsPipeline* regionStencilCurveMaskBlend_ = nullptr; // curve-mask region see-through + premultiplied-over composite
    SDL_GPUGraphicsPipeline* blend_        = nullptr;  // programmable blend composite: applyBlendMode(dst, src, mode), replace
    SDL_GPUGraphicsPipeline* blit_         = nullptr;  // viewport → swapchain blit pipeline
    SDL_GPUSampler*          sampler_      = nullptr;  // nearest, clamped (tile atlas + faithful blit)
    SDL_GPUSampler*          bilinear_     = nullptr;  // linear, clamped (blit only; SamplingMode::Bilinear)
    SDL_GPUTexture*          paletteStore_ = nullptr;  // R16G16B16A16_UNORM store; PaletteId = flat colour offset
    int                      paletteStoreRows_ = 0;    // rows the store texture holds; the entries fill as many
                                                       // as they need and an upload appends into the rest
    SDL_GPUTexture*          atlasStore_   = nullptr;  // R32_UINT flat atlas store: all atlases
                                                       // stacked vertically; width = max atlas width,
                                                       // height = Σ heights; AtlasId → AtlasEntry::storeY
    int                      atlasStoreW_  = 0;         // widest atlas (px); 0 = no atlas uploaded
    int                      atlasStoreH_  = 0;         // rows the atlases occupy (px)
    int                      atlasCapW_    = 0;         // store texture width (px) — an atlas wider than this
                                                        // restacks the store
    int                      atlasCapH_    = 0;         // store texture height (px); the atlases occupy
                                                        // atlasStoreH_ of them and an upload appends into the rest
    SDL_GPUTexture*          atlasRegionStore_ = nullptr; // R32G32B32A32_UINT, one texel per AtlasId =
                                                          // (storeY, cols, transpMaskLo, transpMaskHi); the
                                                          // transparent-index set is a 64-bit bitmask split
                                                          // across .z (0–31) and .w (32–63). The global region
                                                          // table both frag stages index by a cell's / sprite's
                                                          // atlas handle
    int                      atlasRegionCap_ = 0;         // texels the region table holds; one per atlas is
                                                          // used and an upload appends into the rest
    // What an upload has added to the CPU mirrors and not yet written to the GPU. The writes travel in the
    // copy pass the next compose opens, so a game uploading a set of palettes pays one submission for the
    // set rather than one apiece — and nothing reads a store outside compose, so they land before any
    // draw that could see them. Recreating a store writes it whole and drops the notes it makes redundant.
    struct PendingColors {
        std::size_t first = 0;
        std::size_t count = 0;
    };
    std::vector<PendingColors> pendingPaletteWrites_;
    std::vector<AtlasId>       pendingAtlasWrites_;
    std::vector<int>           pendingRegionWrites_;
    std::vector<AtlasEntry>  atlases_;                 // indexed by AtlasId (region within atlasStore_)
    static const Renderer*   instance_;                // the one renderer (instance()), set at
                                                       // construction; the sprite shape query reads its atlases_
    std::vector<CurveMaskEntry> curveMasks_;           // indexed by CurveMaskId − 1 (1-based; 0 = none)
    // Per-layer GPU caches keyed by the layer's ObjectKey (DrawLayer::key), not its position in
    // frame.layers — so a slot follows its layer across reorders/spawns/despawns and is reused by
    // identity. Layer counts are small (compositing planes, tens at most), so a string-keyed map is the
    // right shape. A slot is created on first sight of its key, reused across frames, and evicted the
    // frame its key stops appearing (the unmountGone pattern; its GPU resource released then). A
    // degenerate-keyed layer — empty key, or a duplicate within one frame (only reachable under
    // WarnAndResolve; Throw rejects both before compose) — never touches these maps: it gets a per-frame
    // transient slot (released with the frame scratch) so two colliding keys can't share one texture/buffer.
    std::unordered_map<std::string, TilemapTex>    tilemaps_;
    std::unordered_map<std::string, SpriteBuf>     spriteBufs_;
    std::unordered_map<std::string, RasterTex>     rasters_;
    // Per-run sprite-record buffers for MIXED-blend sprite layers (a layer whose sprites don't all share
    // BlendMode::Normal). An all-Normal layer keeps the single spriteBufs_ buffer + one instanced draw
    // (byte-identical); a mixed layer splits its draw order into contiguous same-blend runs (spriteBlendRuns)
    // and uploads each run's records to its own pool buffer, drawn with first_instance 0 — the region_batch
    // precedent: the sprite vertex stage carries no base-instance uniform, so uSprites[SV_InstanceID] is
    // 0-based and correct on every backend. A grow-on-demand pool assigned by a per-frame slot counter;
    // capacities in BYTES. Released with the sprite buffers.
    std::vector<SDL_GPUBuffer*> spriteRunBufs_;
    std::vector<int>            spriteRunCaps_;
    // One per-frame storage TEXTURE holding every sprite's flattened effect run (its effects chain + its
    // regions, packed as SpriteFxRecords). Each record is ten RGBA32F texels on one row (10 wide); a sprite's
    // GpuSprite.fxOffset/fxCount slice it by row, and the sprite fragment loops the slice inline. A storage
    // texture, not a storage buffer: a fragment storage buffer sits in Metal's [[buffer]] namespace after the
    // uniforms, so its index only matches the toolchain's when the texture and uniform counts coincide — a
    // storage texture (its own [[texture]] namespace) avoids that. Bound (t3 space2) on every sprite draw —
    // always at least one row (a dummy when the frame carries no sprite effects; never read, since those
    // sprites have fxCount 0), so the binding is always valid. Grow-on-demand by row count. Released with the
    // sprite buffers.
    SDL_GPUTexture*            spriteFxStore_ = nullptr;
    int                        spriteFxStoreRows_ = 0;
    // The store's upload-skip state, per SpriteBuf's: `spriteFxHashed_` is false until an upload has landed
    // (and again after a grow recreates the texture), so a fresh texture is never skipped against a stale
    // hash; a frame whose packed texels hash to `spriteFxHash_` over `spriteFxHashRows_` rows is already
    // resident and skips.
    bool                       spriteFxHashed_ = false;
    std::uint64_t              spriteFxHash_ = 0;
    int                        spriteFxHashRows_ = 0;
    std::vector<Rgba16>      paletteData_;             // CPU mirror of the store; flat, contiguous 16-bit palette colours (PaletteId = flat offset)
    SDL_GPUTexture*          rowDataStore_ = nullptr;  // per-frame RGBA32F data store: every effect's
                                                       // paramTable stacked vertically (width 1, one Vec4
                                                       // per row); a Custom effect Loads its rows from it.
                                                       // A 1×1 default always exists so the pipeline binds.
    std::vector<Vec4>        rowData_;                 // CPU mirror rebuilt each frame (the rows uploaded)
    int                      rowDataStoreH_ = 0;       // store texture height in rows; grows to fit
    // Registered custom shader stages, indexed by PostProcessStageId. Each stage builds
    // a pipeline PAIR from the game's fragment — replace (frame-level / Below scope) + premultiplied-over
    // blend (Layer scope) — mirroring displace_/displaceBlend_. A custom shader declares its OWN cbuffer;
    // customPackers_[id] (generated, custom_effect_packers.h) fills it from the effect's inline param
    // fields. Null packer = a parameterless shader (no uniform pushed). All three vectors stay parallel.
    std::vector<SDL_GPUGraphicsPipeline*> customReplace_;       // no-blend; one per registered stage
    std::vector<SDL_GPUGraphicsPipeline*> customBlend_;         // premultiplied-over; one per registered stage
    std::vector<EffectPacker>             customPackers_;       // cbuffer packer; one per registered stage
    // Instanced-additive region batching. customBatched_[id] is the stage's batched pipeline when its
    // shader carries `// @retropp:additive`, else nullptr (nullptr IS the "not additive" flag — the
    // renderer routes eligible same-shader regions through the batched pass only when it exists). Parallel
    // to the three vectors above. batchZeroSource_ is a 1×1 transparent-black texture bound as the batched
    // pass's SourceTexture — with a zero source, an additive shader returns exactly its source-independent
    // delta D, which the additive blend accumulates. batchInstanceBufs_ is a grow-on-demand pool of
    // per-run instance-record storage buffers (one bound per batched pass; first_instance 0, so
    // uRecords[SV_InstanceID] is correct on every backend — no vertex uniform, avoiding the Metal
    // storage+uniform [[buffer]] collision the sprite path documents).
    std::vector<SDL_GPUGraphicsPipeline*> customBatched_;       // instanced-additive; nullptr = not additive
    // Gathered-region rendering: customGather_[id] is the stage's union-shape REPLACE pipeline when its
    // shader has a gather variant (every custom shader EXCEPT additive- / no-gather-declared), else nullptr
    // (nullptr IS the "stage does not gather" flag). customGatherBlend_[id] is its premultiplied-over peer
    // (the Normal-layer last-step composite onto target_). Parallel to customReplace_/customBatched_. A
    // gather pass reads the previous image (real SourceTexture) + a fragment storage buffer of per-region
    // records (claimed from the SAME batchInstanceBufs_ pool as the additive runs) and writes the next image.
    std::vector<SDL_GPUGraphicsPipeline*> customGather_;        // replace; nullptr = stage does not gather
    std::vector<SDL_GPUGraphicsPipeline*> customGatherBlend_;   // premultiplied-over; parallel to customGather_
    // Sprite-inline rendering: customSprite_[id] is the stage's sprite-inline pipeline (the sprite fragment
    // with the custom body injected) when its shader has a sprite variant (every custom shader EXCEPT
    // no-sprite- / int-uint-param ones), else nullptr (the "stage can't run on a sprite" flag). Parallel to
    // customReplace_. A sprite carrying a Custom effect through this stage draws through this pipeline.
    std::vector<SDL_GPUGraphicsPipeline*> customSprite_;        // sprite-inline; nullptr = not on the sprite path
    // Below-custom rendering: customSpriteBelow_[id] is the stage's scene-facing sprite-inline pipeline (the
    // below sprite fragment with the custom body injected, sampleSource reading the scene) when its shader has
    // a sprite-below variant, else nullptr (the "no below-custom variant" flag). Parallel to customSprite_. A
    // sprite carrying a Below-scope Custom effect through this stage draws its lens through this pipeline.
    std::vector<SDL_GPUGraphicsPipeline*> customSpriteBelow_;    // below-custom; nullptr = no scene-read variant
    // Emission-declared custom stages (`// @retropp:emission`). customEmissionConsumer_[id] is 1 when the
    // stage carries the declaration — THE dispatch flag: runEffect runs the extract → blur → stage chain for
    // it instead of a single pass, and its customReplace_/customBlend_ pipelines were built at (2 samplers,
    // 1 storage, 2 uniforms) so the stage's final pass reads the source at slot 0 and the blurred emission at
    // slot 1. customEmission_[id] is the stage's own extract pipeline (its emission() body at the stock
    // (1,1,2) layout) when the shader defines one, else nullptr (the demand extracts through the stock
    // brightpass at `.threshold`). Parallel to customReplace_.
    std::vector<unsigned char>            customEmissionConsumer_;  // 1 = emission consumer (the dispatch flag)
    std::vector<SDL_GPUGraphicsPipeline*> customEmission_;          // custom extract; nullptr = stock brightpass
    // A Below-scope Custom lens's field is authored by the stage's emission() body over the field's RECT:
    // customEmissionRect_[id] is that rect-extract pipeline (the emission() body injected into
    // emission_extract_rect.frag, at 1 sampler [scene] + 1 storage [uFxStore] + 1 uniform) when the shader
    // defines an emission() body, else nullptr (the lens fills its field through the stock rect brightpass at
    // `.threshold`). Parallel to customReplace_. Non-null iff customEmission_ is non-null (same body detection).
    std::vector<SDL_GPUGraphicsPipeline*> customEmissionRect_;      // below custom extract; nullptr = stock rect
    SDL_GPUTexture*                       batchZeroSource_ = nullptr;  // 1×1 transparent-black source
    std::vector<SDL_GPUBuffer*>           batchInstanceBufs_;    // per-run instance/gather records (pooled, grown)
    std::vector<int>                      batchInstanceCaps_;    // each pool buffer's capacity in BYTES (additive
                                                                 // records + gather blobs share the pool)
    LayerKeyCollisionPolicy  collisionPolicy_ = kDefaultCollisionPolicy;
    SamplingMode             sampling_     = defaultSamplingMode;  // blit sampler; seeded by setActive()
    bool                     interpolation_ = defaultInterpolation;  // automatic interpolation; seeded by setActive()
    EvaluationGrid           evaluationGrid_ = defaultEvaluationGrid; // analytic-path evaluation grid; seeded by setActive()
    Interpolator             interp_;        // the per-id retained mirror (prev/cur tick state, by id)
    RenderStats              renderStats_{};  // work counters + the last frame's phase split

    // ── Frame-level compose-skip state ───────────────────────────────────────────────────────────
    // renderFrame skips composeViewport entirely and re-blits `lastComposed_` when the submission is provably
    // bit-identical to the frame that produced it. `storeGeneration_` ticks on every out-of-frame upload the
    // compose reads (atlas/palette store rebuild, curve-mask bake, custom-shader registration); a bump since
    // the retained compose forces a recompose. `lastComposed_` is the retained blit source (target_ or a
    // post-chain scratch — persistent textures, nulled on a target recreate so a stale pointer never blits).
    // `lastFingerprint_` is hashFrameStructure of the last SETTLED compose; `lastComposeSettled_` records
    // whether that retained frame was composed settled — a mid-ease frame is never re-blitted as if settled.
    std::uint64_t   storeGeneration_       = 0;
    SDL_GPUTexture* lastComposed_          = nullptr;
    std::uint64_t   lastFingerprint_       = 0;
    std::uint64_t   lastComposeGeneration_ = 0;
    bool            lastComposeSettled_    = false;
};

// Order-sensitive structural fingerprint of a frame's draw state: every compose-consumed field — per-layer
// key/z/size/scroll/alpha/blend/transform/wrap and tile-cell or sprite content, each layer's and the frame's
// effect chains and regions (kind, params, paramTable contents, shape points/curve/stroke/mask) — folded into
// one 64-bit hash. Excludes what the compose does NOT read from the submission: sub-tick timing/alpha (the
// compose skip gates settledness separately), the blit-stage sampling mode and window size, and out-of-frame
// GPU store state (tracked by the renderer's store generation). Two frames with an equal fingerprint compose to
// the same image given equal store generation. Device-free and pure — the headless fingerprint-sensitivity
// tests build FrameDrawStates and call it directly. A tile layer that declares `contentChanged` (the huge-map
// opt-out) contributes only its declaration, not a cell hash — the renderer forces a recompose on a `true`.
[[nodiscard]] std::uint64_t hashFrameStructure(const FrameDrawState& frame) noexcept;

}  // namespace retropp

// Custom kind on a sprite (Layer-scope inline). A sprite carrying a ScreenSpaceEffectKind::Custom effect
// runs the registered shader's body inline in the sprite fragment (the generated sprite-inline pipeline
// variant), with sampleSource() reading the sprite's own art (the transparent field). Coverage:
//
//   1. Selection + record mapping (device-free). spriteInlineCustomShader picks the sprite's pipeline (the
//      first Custom chain effect); spriteBlendRuns splits runs by (blend, pipeline key); a custom shader's
//      packed cbuffer bytes round-trip through the record's idle chain lanes (writeSpriteFxCustomParams /
//      spriteFxCustomParamFloat4, the CPU mirror of the generated loader's texel read).
//
//   2. Evaluation (device-backed, via captureViewport). A solid sprite carries a Custom effect whose math is
//      exactly computable (saturate(art * tint + lift)); each covered pixel is checked against that CPU
//      expected, off-sprite pixels stay identical to the no-sprite scene, and a twin scene running the SAME
//      shader as a layer region over the sprite's footprint matches the inline result.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/generated/custom_effect_packers.h"  // pack_sprite_custom_probe_frag (reflected from the probe)
#include "retropp/palette.h"
#include "retropp/postprocess.h"
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

// ── Part 1: selection + record mapping (device-free) ───────────────────────────────────────────

TEST(SpriteCustomSelection, FirstChainCustomIsThePipeline) {
    Sprite s{.key = "s"};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{1, 2, 3, 255}},
                 ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Custom,
                                   .customShader = static_cast<PostProcessStageId>(7)},
                 ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Custom,
                                   .customShader = static_cast<PostProcessStageId>(9)}};
    const auto h = spriteInlineCustomShader(s);
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(static_cast<std::size_t>(*h), 7u);   // the FIRST Custom in the chain
}

TEST(SpriteCustomSelection, NoCustomIsNullopt) {
    Sprite s{.key = "s"};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Gleam, .gain = 1.0f}};
    EXPECT_FALSE(spriteInlineCustomShader(s).has_value());
}

TEST(SpriteCustomRuns, PipelineKeySplitsRunsWithinOneBlend) {
    std::array<Sprite, 4> sp{{{.key = "a"}, {.key = "b"}, {.key = "c"}, {.key = "d"}}};  // all Normal blend
    const std::array<std::size_t, 4> order{0, 1, 2, 3};
    const std::array<int, 4> keys{0, 3, 3, 0};   // stock, custom3, custom3, stock
    const std::vector<SpriteBlendRun> runs =
        spriteBlendRuns(std::span<const Sprite>(sp), std::span<const std::size_t>(order),
                        std::span<const int>(keys));
    ASSERT_EQ(runs.size(), 3u);
    EXPECT_EQ(runs[0].pipelineKey, 0); EXPECT_EQ(runs[0].count, 1u);
    EXPECT_EQ(runs[1].pipelineKey, 3); EXPECT_EQ(runs[1].count, 2u);   // coalesced same-key
    EXPECT_EQ(runs[2].pipelineKey, 0); EXPECT_EQ(runs[2].count, 1u);
}

TEST(SpriteCustomRuns, EmptyKeysReduceToBlendOnly) {
    std::array<Sprite, 3> sp{{{.key = "a"}, {.key = "b"}, {.key = "c"}}};
    sp[1].blend = BlendMode::Multiply;
    const std::array<std::size_t, 3> order{0, 1, 2};
    const std::vector<SpriteBlendRun> runs =
        spriteBlendRuns(std::span<const Sprite>(sp), std::span<const std::size_t>(order));  // no keys
    ASSERT_EQ(runs.size(), 3u);
    for (const SpriteBlendRun& run : runs) EXPECT_EQ(run.pipelineKey, 0);
    EXPECT_EQ(runs[1].blend, BlendMode::Multiply);
}

TEST(SpriteCustomParams, PackedBytesRoundTripThroughTheRecordLanes) {
    // The probe's cbuffer is { float3 tint; float lift; } — one 16-byte register. The packer writes those
    // bytes; writeSpriteFxCustomParams lands them in the record's params texel (register 0); the loader mirror
    // reads them back as (tint.x, tint.y, tint.z, lift).
    ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Custom};
    e.tint = Vec3{0.25f, 0.5f, 0.75f};
    e.lift = 0.125f;
    std::byte buf[kSpriteFxCustomParamBytes];
    const std::uint32_t n = shaders::pack_sprite_custom_probe_frag(e, buf);
    EXPECT_EQ(n, 16u);
    SpriteFxRecord r{};
    writeSpriteFxCustomParams(r, std::span<const std::byte>(buf, n));
    const Vec4 reg0 = spriteFxCustomParamFloat4(r, 0);
    EXPECT_FLOAT_EQ(reg0.x, 0.25f);
    EXPECT_FLOAT_EQ(reg0.y, 0.5f);
    EXPECT_FLOAT_EQ(reg0.z, 0.75f);
    EXPECT_FLOAT_EQ(reg0.w, 0.125f);
}

// ── Part 2: evaluation (device-backed) ─────────────────────────────────────────────────────────

constexpr int kW = 64, kH = 64, kSX = 24, kSY = 20;   // an 8×8 sprite at (24, 20)

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

std::uint8_t quant(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}
int chDelta(Rgba8 a, Rgba8 b) {
    return std::max({std::abs(int(a.r) - int(b.r)), std::abs(int(a.g) - int(b.g)),
                     std::abs(int(a.b) - int(b.b)), std::abs(int(a.a) - int(b.a))});
}
bool exactEq(Rgba8 a, Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

class SpriteCustom : public ::testing::Test {
protected:
    static inline SDL_GPUDevice* device_ = nullptr;
    static inline std::string    initError_;
    static void SetUpTestSuite() {
        if (!SDL_Init(SDL_INIT_VIDEO)) { initError_ = std::string("SDL_Init failed: ") + SDL_GetError(); return; }
        device_ = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB, false, nullptr);
        if (!device_) initError_ = std::string("SDL_CreateGPUDevice failed: ") + SDL_GetError();
    }
    static void TearDownTestSuite() {
        if (device_) { SDL_DestroyGPUDevice(device_); device_ = nullptr; }
        SDL_Quit();
    }
    void SetUp() override {
        if (!device_) {
            if (kDeviceOptional)
                GTEST_SKIP() << "Windows on ARM has no production-representative GPU backend in CI; the record "
                                "mapping is the device-free part, and the D3D12 path is covered by Windows x64. ("
                             << initError_ << ")";
            FAIL() << "no GPU device reachable — " << initError_;
        }
    }
};

struct Art { AtlasId atlas{}; PaletteId palette{}; };

Art uploadBg(Renderer& r) {
    std::array<std::uint8_t, 16 * 16> idx{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            idx[static_cast<std::size_t>(y) * 16 + x] = static_cast<std::uint8_t>(((x / 4) + (y / 4)) % 4);
    const AtlasId a = r.uploadAtlas(idx.data(), 16, 16).atlasId;
    const std::array<Rgba8, 4> pal{{{20, 20, 30}, {200, 60, 60}, {60, 200, 90}, {230, 230, 240}}};
    return {a, r.uploadPalette(std::span<const Rgba8>(pal))};
}
DrawLayer bgLayer(const Art& art, std::vector<TileCell>& keep) {
    keep.resize(8 * 8);
    for (int ty = 0; ty < 8; ++ty)
        for (int tx = 0; tx < 8; ++tx)
            keep[static_cast<std::size_t>(ty) * 8 + tx] =
                TileCell{.atlas = art.atlas, .tile = static_cast<std::uint16_t>((tx + ty) % 4), .palette = art.palette};
    DrawLayer l{.key = "bg"};
    l.z = 0; l.size = PixelSize{kW, kH};
    l.content = TileContent{.widthInTiles = 8, .heightInTiles = 8, .cells = std::span<const TileCell>(keep)};
    return l;
}
Art uploadSolid(Renderer& r, Rgba8 colour) {
    std::array<std::uint8_t, 8 * 8> idx{};
    const AtlasId a = r.uploadAtlas(idx.data(), 8, 8).atlasId;
    const std::array<Rgba8, 4> pal{{colour, colour, colour, colour}};
    return {a, r.uploadPalette(std::span<const Rgba8>(pal))};
}

TEST_F(SpriteCustom, TintLiftRendersTheComputedColourOnTheSilhouette) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const PostProcessStageId probe = r.registerPostProcessStage("tests/shaders/sprite_custom_probe.frag.hlsl");
    const Art   bg          = uploadBg(r);
    const Rgba8 spriteColour{110, 140, 190, 255};
    const Art   solid = uploadSolid(r, spriteColour);
    const Vec3  tint{0.5f, 0.75f, 1.0f};
    const float lift = 0.05f;

    std::vector<TileCell> cB, cN, cG;
    FrameDrawState base;
    base.layers.push_back(bgLayer(bg, cB));
    const std::vector<Rgba8> B = r.captureViewport(base);

    auto scene = [&](bool withEffect, std::vector<TileCell>& cells, std::vector<Sprite>& keepS) {
        FrameDrawState f;
        f.layers.push_back(bgLayer(bg, cells));
        Sprite s{.key = "fx", .x = kSX, .y = kSY, .atlas = solid.atlas, .tile = 0, .palette = solid.palette};
        if (withEffect) {
            ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Custom, .customShader = probe};
            e.tint = tint;
            e.lift = lift;
            s.effects = {e};
        }
        keepS = {s};
        DrawLayer sp{.key = "sprites"};
        sp.z = 10; sp.size = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keepS)};
        f.layers.push_back(sp);
        return f;
    };
    std::vector<Sprite> sN, sG;
    FrameDrawState fN = scene(false, cN, sN);
    FrameDrawState fG = scene(true, cG, sG);
    const std::vector<Rgba8> N = r.captureViewport(fN);   // the sprite, no effect
    const std::vector<Rgba8> G = r.captureViewport(fG);   // the sprite, custom effect
    ASSERT_EQ(N.size(), B.size());
    ASSERT_EQ(G.size(), B.size());

    // The expected covered-pixel color: saturate(art * tint + lift), opaque over the background.
    const Vec4  col = Vec4{spriteColour.r / 255.0f, spriteColour.g / 255.0f, spriteColour.b / 255.0f, 1.0f};
    const Rgba8 want{quant(col.x * tint.x + lift), quant(col.y * tint.y + lift),
                     quant(col.z * tint.z + lift), 255};

    int covered = 0, moved = 0;
    for (std::size_t i = 0; i < B.size(); ++i) {
        if (!exactEq(N[i], B[i])) {   // the sprite covers here
            ++covered;
            EXPECT_LE(chDelta(G[i], want), 2) << "covered pixel " << i << " custom output";
            if (chDelta(G[i], N[i]) > 2) ++moved;
        } else {
            EXPECT_TRUE(exactEq(G[i], B[i])) << "off-sprite pixel " << i << " leaked";
        }
    }
    EXPECT_GT(covered, 0);
    EXPECT_GT(moved, 0) << "the custom effect changed no covered pixel";
}

TEST_F(SpriteCustom, InlineMatchesTheSameShaderAsALayerRegionOverTheFootprint) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const PostProcessStageId probe = r.registerPostProcessStage("tests/shaders/sprite_custom_probe.frag.hlsl");
    const Art  bg    = uploadBg(r);
    const Art  solid = uploadSolid(r, Rgba8{120, 90, 200, 255});
    const Vec3 tint{0.6f, 0.9f, 0.4f};
    const float lift = 0.0f;   // 0 so an off-sprite transparent read stays transparent in both scenes

    std::vector<TileCell> cI, cR;
    std::vector<Sprite>   sI, sR;
    std::vector<Region>   regR;

    FrameDrawState fI;
    fI.layers.push_back(bgLayer(bg, cI));
    {
        Sprite s{.key = "fx", .x = kSX, .y = kSY, .atlas = solid.atlas, .tile = 0, .palette = solid.palette};
        ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Custom, .customShader = probe};
        e.tint = tint;
        e.lift = lift;
        s.effects = {e};
        sI = {s};
        DrawLayer sp{.key = "sprites"};
        sp.z = 10; sp.size = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(sI)};
        fI.layers.push_back(sp);
    }

    FrameDrawState fR;
    fR.layers.push_back(bgLayer(bg, cR));
    {
        Sprite s{.key = "fx", .x = kSX, .y = kSY, .atlas = solid.atlas, .tile = 0, .palette = solid.palette};
        sR = {s};
        DrawLayer sp{.key = "sprites"};
        sp.z = 10; sp.size = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(sR)};
        ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Custom, .customShader = probe};
        e.tint = tint;
        e.lift = lift;
        // Layer scope: the region custom transforms the layer's own isolated content (the sprite) inside the
        // shape — a rectangle over the sprite's footprint, so the effected pixels are exactly the silhouette.
        regR = {Region{.key = "reg", .shape = ShapePoints::rectangle(Point{kSX, kSY}, 8, 8), .effects = {e}}};
        sp.regions = regR;
        fR.layers.push_back(sp);
    }

    const std::vector<Rgba8> I = r.captureViewport(fI);
    const std::vector<Rgba8> R = r.captureViewport(fR);
    ASSERT_EQ(I.size(), R.size());
    int matched = 0;
    for (std::size_t i = 0; i < I.size(); ++i) {
        ASSERT_LE(chDelta(I[i], R[i]), 3) << "inline vs layer-region custom differ at pixel " << i;
        ++matched;
    }
    EXPECT_GT(matched, 0);
}

}  // namespace

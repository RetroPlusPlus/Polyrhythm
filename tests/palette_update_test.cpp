// A palette's colors change in place. updatePalette gives the palette at a PaletteId new colors and keeps
// the handle: the next frame draws them, a settled frame the compose would otherwise skip included; the
// store does not grow; and a call the renderer refuses writes nothing. Each image is compared against a
// renderer that uploads the new colors directly, so the claim is that an updated palette draws exactly as a
// palette uploaded with those colors does.
//
// Device-backed, compose-only + windowless (a GPU device, no display): the same harness the store-upload
// and compose-skip tests use, so it runs on a software rasterizer in CI — lavapipe (Vulkan) on Linux, WARP
// (D3D12) on Windows, Metal on the Mac. A device is REQUIRED on every production-representative platform;
// the one skip is Windows on ARM (a courtesy coverage runner with no production-representative GPU backend
// in CI).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <SDL3/SDL.h>

#include "retropp/draw_state.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/viewport.h"
#include "src/store_upload.h"

namespace {
using namespace retropp;
using retropp::detail::kPaletteStoreWidth;

constexpr int kViewW = 64;
constexpr int kViewH = 64;

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

// A 16×16 sheet of palette indices, distinct per `seed`, reading all four entries of a palette.
std::vector<std::uint8_t> sheetPixels(int width, int height, int seed) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            px[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>((x + y * 3 + seed * 2) % 4);
    return px;
}

// Four colors far enough apart that a wrong entry is visible, seeded so each palette differs.
std::vector<Rgba8> paletteColors(int seed) {
    return {Rgba8{static_cast<std::uint8_t>(20 + seed * 30), 20, 30, 255},
            Rgba8{200, static_cast<std::uint8_t>(60 + seed * 20), 60, 255},
            Rgba8{60, 200, static_cast<std::uint8_t>(90 + seed * 15), 255},
            Rgba8{230, 230, 240, 255}};
}

// Filler colors so a later palette lands at a chosen flat offset.
std::vector<Rgba8> fillerColors(std::size_t count) {
    std::vector<Rgba8> px(count);
    for (std::size_t i = 0; i < count; ++i)
        px[i] = Rgba8{static_cast<std::uint8_t>(i & 0xFFu), static_cast<std::uint8_t>((i >> 8) & 0xFFu), 7,
                      255};
    return px;
}

// One tile layer drawing `atlas`'s second cell through `palette`, its cells held beside the frame that
// points into them. Built once, so the compose skip compares one submission of it against the next.
struct Scene {
    Scene(AtlasId atlas, PaletteId palette)
        : cells(8 * 8, TileCell{.atlas = atlas, .tile = 1, .palette = palette}) {
        DrawLayer bg{.key = "bg"};
        bg.z       = 0;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles  = 8,
                                 .heightInTiles = 8,
                                 .cells         = std::span<const TileCell>(cells)};
        frame.layers = {bg};
    }
    Scene(const Scene&)            = delete;
    Scene& operator=(const Scene&) = delete;

    std::vector<TileCell> cells;
    FrameDrawState        frame;
};

std::vector<Rgba8> renderSheet(Renderer& r, AtlasId atlas, PaletteId palette) {
    const Scene scene{atlas, palette};
    return r.captureViewport(scene.frame);
}

// Uploads enough filler to pass the rows the palette store holds, so the store is recreated and every
// color in it is written again from the CPU copy.
void writeStoreWhole(Renderer& r) {
    (void)r.uploadPalette(
        std::span<const Rgba8>(fillerColors(2u * static_cast<std::size_t>(kPaletteStoreWidth))));
}

::testing::AssertionResult sameImage(const std::vector<Rgba8>& a, const std::vector<Rgba8>& b) {
    if (a.size() != b.size())
        return ::testing::AssertionFailure() << "sizes differ: " << a.size() << " vs " << b.size();
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b || a[i].a != b[i].a)
            return ::testing::AssertionFailure()
                   << "pixel " << i << " differs: " << +a[i].r << "," << +a[i].g << "," << +a[i].b << ","
                   << +a[i].a << " vs " << +b[i].r << "," << +b[i].g << "," << +b[i].b << "," << +b[i].a;
    return ::testing::AssertionSuccess();
}

// An image with one color in it would compare equal to any other such image, so every comparison below
// first establishes that the scene actually drew the sheet.
::testing::AssertionResult carriesArt(const std::vector<Rgba8>& px) {
    if (px.empty()) return ::testing::AssertionFailure() << "captured nothing";
    for (const Rgba8 c : px)
        if (c.r != px[0].r || c.g != px[0].g || c.b != px[0].b) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << "the capture is one flat color — the sheet did not draw";
}

// The scene still draws `before`, both from the store as it stands and after the store is written whole
// from the CPU copy — so a refused call reached neither.
void expectNothingWritten(Renderer& r, const Scene& scene, const std::vector<Rgba8>& before) {
    EXPECT_TRUE(carriesArt(before));
    EXPECT_TRUE(sameImage(r.captureViewport(scene.frame), before)) << "the store";
    writeStoreWhole(r);
    EXPECT_TRUE(sameImage(r.captureViewport(scene.frame), before)) << "the CPU copy of the store";
}

class PaletteUpdateTest : public ::testing::Test {
protected:
    static inline SDL_GPUDevice* device_ = nullptr;
    static inline std::string    initError_;

    static void SetUpTestSuite() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            initError_ = std::string("SDL_Init(SDL_INIT_VIDEO) failed: ") + SDL_GetError();
            return;
        }
        device_ = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
            /*debug_mode=*/false, /*name=*/nullptr);
        if (!device_) initError_ = std::string("SDL_CreateGPUDevice failed: ") + SDL_GetError();
    }

    static void TearDownTestSuite() {
        if (device_) {
            SDL_DestroyGPUDevice(device_);
            device_ = nullptr;
        }
        SDL_Quit();
    }

    void SetUp() override {
        if (!device_) {
            if (kDeviceOptional) {
                GTEST_SKIP() << "Windows on ARM is a courtesy runner with no production-representative GPU "
                                "backend in CI; its production path (D3D12 + DXIL) is covered by the Windows "
                                "x64 job. ("
                             << initError_ << ")";
            }
            FAIL() << "no GPU device reachable — " << initError_
                   << ". This device-backed test requires a GPU device on every production-representative "
                      "platform (macOS/Metal, Windows-x64/D3D12, Linux/Vulkan); install a GPU driver (a "
                      "software rasterizer such as lavapipe suffices) and, on a headless runner, set "
                      "SDL_VIDEODRIVER=offscreen so SDL video init succeeds.";
        }
    }
};

// An updated palette draws exactly as a palette uploaded with those colors does, and differently from how
// it drew before the update.
TEST_F(PaletteUpdateTest, AnUpdatedPaletteRendersAsOneUploadedWithThoseColorsDoes) {
    const std::vector<std::uint8_t> sheet = sheetPixels(16, 16, /*seed=*/1);

    Renderer        updated{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   atlasA   = updated.uploadAtlas(sheet.data(), 16, 16).atlasId;
    const PaletteId subjectA = updated.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    const std::vector<Rgba8> before = renderSheet(updated, atlasA, subjectA);
    updated.updatePalette(subjectA, std::span<const Rgba8>(paletteColors(2)));
    const std::vector<Rgba8> after = renderSheet(updated, atlasA, subjectA);

    Renderer        uploaded{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   atlasB   = uploaded.uploadAtlas(sheet.data(), 16, 16).atlasId;
    const PaletteId subjectB = uploaded.uploadPalette(std::span<const Rgba8>(paletteColors(2)));
    const std::vector<Rgba8> direct = renderSheet(uploaded, atlasB, subjectB);

    EXPECT_EQ(subjectA, subjectB);
    EXPECT_TRUE(carriesArt(before));
    EXPECT_FALSE(sameImage(before, after)) << "the update changed nothing on screen";
    EXPECT_TRUE(carriesArt(after));
    EXPECT_TRUE(sameImage(after, direct));
}

// A settled scene drawn twice skips its second compose. After an update the same submission recomposes —
// its structure is unchanged, the colors it reads are not — and draws the new colors; the frame after that
// skips again.
TEST_F(PaletteUpdateTest, AnUpdateReachesTheNextFrameOfASettledScene) {
    const std::vector<std::uint8_t> sheet = sheetPixels(16, 16, /*seed=*/2);

    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    r.automaticInterpolation(false);
    const AtlasId   atlas   = r.uploadAtlas(sheet.data(), 16, 16).atlasId;
    const PaletteId subject = r.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    const Scene     scene{atlas, subject};

    r.renderFrame(scene.frame);  // composes
    r.renderFrame(scene.frame);  // identical and settled — skips
    ASSERT_EQ(r.renderStats().composePasses, 1u);
    ASSERT_EQ(r.renderStats().composeSkips, 1u);

    r.updatePalette(subject, std::span<const Rgba8>(paletteColors(3)));
    r.renderFrame(scene.frame);
    EXPECT_EQ(r.renderStats().composePasses, 2u) << "the frame after an update recomposes";
    EXPECT_EQ(r.renderStats().composeSkips, 1u);

    r.renderFrame(scene.frame);
    EXPECT_EQ(r.renderStats().composePasses, 2u);
    EXPECT_EQ(r.renderStats().composeSkips, 2u) << "the skip resumes once the new colors are composed";

    const std::vector<Rgba8> shown = r.captureViewport(scene.frame);

    Renderer        uploaded{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   atlasB   = uploaded.uploadAtlas(sheet.data(), 16, 16).atlasId;
    const PaletteId subjectB = uploaded.uploadPalette(std::span<const Rgba8>(paletteColors(3)));
    const std::vector<Rgba8> direct = renderSheet(uploaded, atlasB, subjectB);

    EXPECT_TRUE(carriesArt(shown));
    EXPECT_TRUE(sameImage(shown, direct));
}

// An update writes over the palette's own entries, so the store holds as many entries after fifty updates
// as before them: the palette uploaded next lands where it would have landed with none.
TEST_F(PaletteUpdateTest, TheStoreDoesNotGrowAcrossUpdates) {
    Renderer        r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const PaletteId p = r.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    const PaletteId q = r.uploadPalette(std::span<const Rgba8>(paletteColors(1)));
    for (int i = 0; i < 50; ++i) r.updatePalette(p, std::span<const Rgba8>(paletteColors(i)));
    const PaletteId next = r.uploadPalette(std::span<const Rgba8>(paletteColors(2)));

    EXPECT_EQ(static_cast<std::size_t>(next), static_cast<std::size_t>(q) + 4u);
}

// Colors that do not number the palette's entries are refused, and nothing they carry is written: not to
// the screen, not to the store a settled frame is skipping against, and not to the CPU copy the store is
// written whole from.
TEST_F(PaletteUpdateTest, ALengthMismatchIsRefusedAndNothingIsWritten) {
    const std::vector<std::uint8_t> sheet = sheetPixels(16, 16, /*seed=*/3);

    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    r.automaticInterpolation(false);
    const AtlasId   atlas   = r.uploadAtlas(sheet.data(), 16, 16).atlasId;
    const PaletteId subject = r.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    const Scene     scene{atlas, subject};
    const std::vector<Rgba8> before = r.captureViewport(scene.frame);

    r.renderFrame(scene.frame);  // composes
    r.renderFrame(scene.frame);  // skips
    const Renderer::RenderStats settled = r.renderStats();
    ASSERT_EQ(settled.composeSkips, 1u);

    std::vector<Rgba8> fewer = paletteColors(2);
    fewer.pop_back();
    std::vector<Rgba8> more = paletteColors(2);
    more.push_back(Rgba8{255, 0, 255, 255});
    EXPECT_THROW(r.updatePalette(subject, std::span<const Rgba8>(fewer)), std::invalid_argument);
    EXPECT_THROW(r.updatePalette(subject, std::span<const Rgba8>(more)), std::invalid_argument);

    r.renderFrame(scene.frame);
    EXPECT_EQ(r.renderStats().composePasses, settled.composePasses) << "a refused call forces no recompose";
    EXPECT_EQ(r.renderStats().composeSkips, settled.composeSkips + 1u);

    expectNothingWritten(r, scene, before);
}

// A PaletteId that is not the first entry of an uploaded palette is refused — an offset inside the
// palette, and the offset past every palette — and nothing is written.
TEST_F(PaletteUpdateTest, AnOffsetInsideAPaletteIsRefusedAndNothingIsWritten) {
    const std::vector<std::uint8_t> sheet = sheetPixels(16, 16, /*seed=*/4);

    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    r.automaticInterpolation(false);
    const AtlasId   atlas   = r.uploadAtlas(sheet.data(), 16, 16).atlasId;
    const PaletteId subject = r.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    const Scene     scene{atlas, subject};
    const std::vector<Rgba8> before = r.captureViewport(scene.frame);

    r.renderFrame(scene.frame);  // composes
    r.renderFrame(scene.frame);  // skips
    const Renderer::RenderStats settled = r.renderStats();
    ASSERT_EQ(settled.composeSkips, 1u);

    const std::vector<Rgba8> colors = paletteColors(2);
    const std::size_t        first  = static_cast<std::size_t>(subject);
    const std::size_t        end    = first + colors.size();  // the store holds the subject's entries alone
    for (const std::size_t id : {first + 1u, first + 3u, end}) {
        EXPECT_THROW(r.updatePalette(static_cast<PaletteId>(id), std::span<const Rgba8>(colors)),
                     std::invalid_argument)
            << "PaletteId " << id;
    }

    r.renderFrame(scene.frame);
    EXPECT_EQ(r.renderStats().composePasses, settled.composePasses) << "a refused call forces no recompose";
    EXPECT_EQ(r.renderStats().composeSkips, settled.composeSkips + 1u);

    expectNothingWritten(r, scene, before);
}

// The two overloads write the same store: an Rgba8 color widens as uploadPalette widens it, so an update of
// one set of colors through either form draws the same image.
TEST_F(PaletteUpdateTest, Rgba8AndRgba16UpdatesOfOneColorMatch) {
    const std::vector<std::uint8_t> sheet  = sheetPixels(16, 16, /*seed=*/1);
    const std::vector<Rgba8>        narrow = paletteColors(3);
    std::vector<Rgba16>             wide;
    for (const Rgba8 c : narrow) wide.push_back(widen(c));

    Renderer        a{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   atlasA   = a.uploadAtlas(sheet.data(), 16, 16).atlasId;
    const PaletteId subjectA = a.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    const std::vector<Rgba8> before = renderSheet(a, atlasA, subjectA);
    a.updatePalette(subjectA, std::span<const Rgba8>(narrow));
    const std::vector<Rgba8> fromNarrow = renderSheet(a, atlasA, subjectA);

    Renderer        b{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   atlasB   = b.uploadAtlas(sheet.data(), 16, 16).atlasId;
    const PaletteId subjectB = b.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    b.updatePalette(subjectB, std::span<const Rgba16>(wide));
    const std::vector<Rgba8> fromWide = renderSheet(b, atlasB, subjectB);

    EXPECT_TRUE(carriesArt(fromNarrow));
    EXPECT_FALSE(sameImage(before, fromNarrow)) << "the update changed nothing on screen";
    EXPECT_TRUE(sameImage(fromNarrow, fromWide));
}

// A palette whose entries cross the store's row edge updates whole. The store is 16384 colors wide, so the
// subject sits two colors short of the edge and half of it is on the next row. The renderer it is compared
// against has its store written whole, so it reads what the colors are wherever a run lands.
TEST_F(PaletteUpdateTest, APaletteCrossingTheRowEdgeUpdatesWhole) {
    const std::vector<std::uint8_t> sheet     = sheetPixels(16, 16, /*seed=*/2);
    const std::size_t               toTheEdge = static_cast<std::size_t>(kPaletteStoreWidth) - 2u;

    Renderer        updated{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   atlasA = updated.uploadAtlas(sheet.data(), 16, 16).atlasId;
    (void)updated.uploadPalette(std::span<const Rgba8>(fillerColors(toTheEdge)));
    const PaletteId subjectA = updated.uploadPalette(std::span<const Rgba8>(paletteColors(3)));
    ASSERT_EQ(static_cast<std::size_t>(subjectA), toTheEdge);
    const std::vector<Rgba8> before = renderSheet(updated, atlasA, subjectA);
    updated.updatePalette(subjectA, std::span<const Rgba8>(paletteColors(1)));
    const std::vector<Rgba8> after = renderSheet(updated, atlasA, subjectA);

    Renderer        rewritten{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   atlasB = rewritten.uploadAtlas(sheet.data(), 16, 16).atlasId;
    (void)rewritten.uploadPalette(std::span<const Rgba8>(fillerColors(toTheEdge)));
    const PaletteId subjectB = rewritten.uploadPalette(std::span<const Rgba8>(paletteColors(1)));
    writeStoreWhole(rewritten);
    const std::vector<Rgba8> direct = renderSheet(rewritten, atlasB, subjectB);

    EXPECT_EQ(subjectA, subjectB);
    EXPECT_TRUE(carriesArt(before));
    EXPECT_FALSE(sameImage(before, after)) << "the update changed nothing on screen";
    EXPECT_TRUE(carriesArt(after));
    EXPECT_TRUE(sameImage(after, direct));
}

// A palette longer than two rows of the store updates whole. Its sheet is 16-bit so its indices reach the
// palette's first entry, its second, and the first entry of each of the next two rows — the last past any
// two rows.
TEST_F(PaletteUpdateTest, APaletteLongerThanTwoRowsUpdatesWhole) {
    const std::size_t W     = static_cast<std::size_t>(kPaletteStoreWidth);
    const std::size_t count = 2u * W + 1u;

    const std::array<std::uint16_t, 4> reach{0, 1, static_cast<std::uint16_t>(W),
                                             static_cast<std::uint16_t>(2u * W)};
    std::vector<std::uint16_t>         sheet(16 * 16);
    for (std::size_t y = 0; y < 16; ++y)
        for (std::size_t x = 0; x < 16; ++x) sheet[y * 16 + x] = reach[(x + y * 3) % 4];

    const auto longColors = [count](unsigned seed) {
        std::vector<Rgba8> c(count);
        for (std::size_t i = 0; i < count; ++i)
            c[i] = Rgba8{static_cast<std::uint8_t>((i * 37 + seed * 101) & 0xFFu),
                         static_cast<std::uint8_t>((i * 11 + seed * 53) & 0xFFu),
                         static_cast<std::uint8_t>((i * 5 + seed * 29) & 0xFFu), 255};
        return c;
    };

    Renderer        updated{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   atlasA   = updated.uploadAtlas(sheet.data(), 16, 16).atlasId;
    const PaletteId subjectA = updated.uploadPalette(std::span<const Rgba8>(longColors(0)));
    ASSERT_EQ(static_cast<std::size_t>(subjectA), 0u);
    const std::vector<Rgba8> before = renderSheet(updated, atlasA, subjectA);
    updated.updatePalette(subjectA, std::span<const Rgba8>(longColors(1)));
    const std::vector<Rgba8> after = renderSheet(updated, atlasA, subjectA);

    Renderer        uploaded{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   atlasB   = uploaded.uploadAtlas(sheet.data(), 16, 16).atlasId;
    const PaletteId subjectB = uploaded.uploadPalette(std::span<const Rgba8>(longColors(1)));
    const std::vector<Rgba8> direct = renderSheet(uploaded, atlasB, subjectB);

    EXPECT_TRUE(carriesArt(before));
    EXPECT_FALSE(sameImage(before, after)) << "the update changed nothing on screen";
    EXPECT_TRUE(carriesArt(after));
    EXPECT_TRUE(sameImage(after, direct));
}

// An upload of no colors occupies no entries, so its PaletteId is the offset of whatever is uploaded next,
// and an update at that id changes that palette. An update of no colors at it is refused, as it is when
// nothing follows the empty upload at all.
TEST_F(PaletteUpdateTest, AnEmptyUploadHasNoColorsToChange) {
    const std::vector<std::uint8_t> sheet = sheetPixels(16, 16, /*seed=*/3);

    Renderer        updated{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   atlasA = updated.uploadAtlas(sheet.data(), 16, 16).atlasId;
    const PaletteId empty  = updated.uploadPalette(std::span<const Rgba8>{});
    const PaletteId held   = updated.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    EXPECT_EQ(empty, held);
    updated.updatePalette(empty, std::span<const Rgba8>(paletteColors(2)));
    const std::vector<Rgba8> after = renderSheet(updated, atlasA, held);
    EXPECT_THROW(updated.updatePalette(empty, std::span<const Rgba8>{}), std::invalid_argument);

    Renderer        uploaded{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   atlasB   = uploaded.uploadAtlas(sheet.data(), 16, 16).atlasId;
    const PaletteId subjectB = uploaded.uploadPalette(std::span<const Rgba8>(paletteColors(2)));
    const std::vector<Rgba8> direct = renderSheet(uploaded, atlasB, subjectB);

    EXPECT_TRUE(carriesArt(after));
    EXPECT_TRUE(sameImage(after, direct));

    Renderer        alone{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const PaletteId nothing = alone.uploadPalette(std::span<const Rgba8>{});
    EXPECT_THROW(alone.updatePalette(nothing, std::span<const Rgba8>{}), std::invalid_argument);
}

}  // namespace

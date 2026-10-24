// Store uploads write only what they append. The palette store and the atlas store each hold everything a
// game has uploaded, and an upload adds to the end: it writes its own colors or its own pixels into the
// room the store texture already has, and recreates the texture only when the content outgrows it.
//
// Two halves. The device-free half pins the destination rectangles and the growth decision — a rectangle
// with the wrong offset writes correct bytes to the wrong place and nothing fails, so the arithmetic is
// exercised directly. The device half renders the same scene from a store built by appending and from one
// the same content was written into whole, and requires the two images to be identical: that is the claim
// the whole change rests on.
//
// Device-backed, compose-only + windowless (a GPU device, no display): the same harness the golden-readback
// and compose-skip tests use, so it runs on a software rasterizer in CI — lavapipe (Vulkan) on Linux, WARP
// (D3D12) on Windows, Metal on the Mac. A device is REQUIRED on every production-representative platform;
// the one skip is Windows on ARM (a courtesy coverage runner with no production-representative GPU backend
// in CI).

#include <cstddef>
#include <cstdint>
#include <span>
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
using retropp::detail::atlasFitsStore;
using retropp::detail::atlasRect;
using retropp::detail::atlasRegionTexel;
using retropp::detail::grownCapacity;
using retropp::detail::kPaletteStoreWidth;
using retropp::detail::paletteAppend;
using retropp::detail::paletteAppendFitsTwoRows;
using retropp::detail::paletteStoreRows;
using retropp::detail::StoreRect;

// ── Where an append lands ──────────────────────────────────────────────────────────────────────────

constexpr int kW = 16;  // a narrow store, so the row edge is reachable in a readable test

TEST(PaletteAppendGeometry, ARunAtTheStartOfAStoreOccupiesTheFirstColumns) {
    const auto where = paletteAppend(/*first=*/0, /*count=*/4, kW);
    EXPECT_EQ(where.head, (StoreRect{.x = 0, .y = 0, .w = 4, .h = 1}));
    EXPECT_FALSE(where.wraps());
}

TEST(PaletteAppendGeometry, ARunStartsAtItsOwnFlatOffset) {
    const auto where = paletteAppend(/*first=*/37, /*count=*/4, kW);
    EXPECT_EQ(where.head, (StoreRect{.x = 37 % kW, .y = 37 / kW, .w = 4, .h = 1}));
    EXPECT_FALSE(where.wraps());
}

TEST(PaletteAppendGeometry, ARunReachingTheRowEdgeContinuesOnTheNextRow) {
    // 14 of the 16 columns are taken, so a run of 4 puts 2 colors on this row and 2 on the next.
    const auto where = paletteAppend(/*first=*/14, /*count=*/4, kW);
    EXPECT_EQ(where.head, (StoreRect{.x = 14, .y = 0, .w = 2, .h = 1}));
    ASSERT_TRUE(where.wraps());
    EXPECT_EQ(where.wrapped, (StoreRect{.x = 0, .y = 1, .w = 2, .h = 1}));
    EXPECT_EQ(where.head.w + where.wrapped.w, 4) << "the two rectangles cover the run exactly once";
}

TEST(PaletteAppendGeometry, ARunEndingOnTheRowEdgeStaysOnOneRow) {
    const auto where = paletteAppend(/*first=*/12, /*count=*/4, kW);
    EXPECT_EQ(where.head, (StoreRect{.x = 12, .y = 0, .w = 4, .h = 1}));
    EXPECT_FALSE(where.wraps());
}

TEST(PaletteAppendGeometry, AnEmptyRunLandsNowhere) {
    const auto where = paletteAppend(/*first=*/5, /*count=*/0, kW);
    EXPECT_EQ(where.head.w, 0);
    EXPECT_FALSE(where.wraps());
}

TEST(PaletteAppendGeometry, ARunWithinTwoRowsIsWrittenInPlaceAndALongerOneIsNot) {
    EXPECT_TRUE(paletteAppendFitsTwoRows(/*first=*/14, /*count=*/18, kW));   // 2 + 16 — the last it holds
    EXPECT_FALSE(paletteAppendFitsTwoRows(/*first=*/14, /*count=*/19, kW));  // one past it
    EXPECT_TRUE(paletteAppendFitsTwoRows(/*first=*/0, /*count=*/kW, kW));
}

TEST(PaletteStoreGeometry, TheStoreHoldsTheRowsItsColoursNeedAndNeverFewerThanOne) {
    EXPECT_EQ(paletteStoreRows(0, kW), 1) << "the texture a shader binds has a row before anything is in it";
    EXPECT_EQ(paletteStoreRows(1, kW), 1);
    EXPECT_EQ(paletteStoreRows(kW, kW), 1);
    EXPECT_EQ(paletteStoreRows(kW + 1, kW), 2);
    EXPECT_EQ(paletteStoreRows(3 * kW, kW), 3);
}

TEST(StoreGrowth, RoomIsDoubledSoAppendsOutnumberRecreations) {
    EXPECT_EQ(grownCapacity(1), 2);
    EXPECT_EQ(grownCapacity(48), 96);
    EXPECT_EQ(grownCapacity(0), 1) << "a store always has room for something";
}

TEST(AtlasStoreGeometry, AnAtlasNoWiderThanTheStoreAndWithinItsRowsIsWrittenInPlace) {
    EXPECT_TRUE(atlasFitsStore(/*width=*/16, /*totalHeight=*/32, /*capacityWidth=*/16,
                               /*capacityHeight=*/32));
    EXPECT_TRUE(atlasFitsStore(/*width=*/8, /*totalHeight=*/17, /*capacityWidth=*/16,
                               /*capacityHeight=*/32));
}

TEST(AtlasStoreGeometry, AWiderAtlasIsNotWrittenInPlace) {
    EXPECT_FALSE(atlasFitsStore(/*width=*/17, /*totalHeight=*/32, /*capacityWidth=*/16,
                                /*capacityHeight=*/32))
        << "a wider atlas changes the stride of every row";
}

TEST(AtlasStoreGeometry, AnAtlasPastTheStoresRowsIsNotWrittenInPlace) {
    EXPECT_FALSE(atlasFitsStore(/*width=*/16, /*totalHeight=*/33, /*capacityWidth=*/16,
                                /*capacityHeight=*/32));
}

TEST(AtlasStoreGeometry, AnAtlasOccupiesItsOwnRowsFromTheLeftEdge) {
    EXPECT_EQ(atlasRect(/*top=*/48, /*width=*/32, /*height=*/16),
              (StoreRect{.x = 0, .y = 48, .w = 32, .h = 16}));
}

TEST(AtlasStoreGeometry, AnAtlasRegionIsTheSingleTexelInItsOwnColumn) {
    EXPECT_EQ(atlasRegionTexel(3), (StoreRect{.x = 3, .y = 0, .w = 1, .h = 1}));
}

// ── What the store then renders ────────────────────────────────────────────────────────────────────

constexpr int kViewW = 64;
constexpr int kViewH = 64;

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

// A 16×16 sheet of palette indices, distinct per `seed`, so a sheet written to the wrong rows renders as
// another sheet's art rather than as something plausible.
std::vector<std::uint8_t> sheetPixels(int width, int height, int seed) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            px[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>((x + y * 3 + seed * 2) % 4);
    return px;
}

// Four colors far enough apart that a wrong palette row is visible, seeded so each palette differs.
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

// One tile layer drawing `atlas`'s second cell through `palette`, captured as raw pixels.
std::vector<Rgba8> renderSheet(Renderer& r, AtlasId atlas, PaletteId palette) {
    std::vector<TileCell> cells(8 * 8, TileCell{.atlas = atlas, .tile = 1, .palette = palette});
    DrawLayer             bg{.key = "bg"};
    bg.z       = 0;
    bg.size    = PixelSize{kViewW, kViewH};
    bg.content = TileContent{.widthInTiles  = 8,
                             .heightInTiles = 8,
                             .cells         = std::span<const TileCell>(cells)};
    FrameDrawState frame;
    frame.layers = {bg};
    return r.captureViewport(frame);
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
    return ::testing::AssertionFailure() << "the capture is one flat colour — the sheet did not draw";
}

class StoreUploadTest : public ::testing::Test {
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

// A palette appended into the store's spare columns renders exactly as the same palette does after the
// whole store has been written afresh. The third palette is the subject: it is appended in the first
// renderer, and rewritten by the growth in the second.
TEST_F(StoreUploadTest, AnAppendedPaletteRendersAsARewrittenOneDoes) {
    const std::vector<std::uint8_t> sheet = sheetPixels(16, 16, /*seed=*/1);

    Renderer        appended{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   atlasA = appended.uploadAtlas(sheet.data(), 16, 16).atlasId;
    (void)appended.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    (void)appended.uploadPalette(std::span<const Rgba8>(paletteColors(1)));
    const PaletteId subjectA = appended.uploadPalette(std::span<const Rgba8>(paletteColors(2)));
    const std::vector<Rgba8> fromAppend = renderSheet(appended, atlasA, subjectA);

    Renderer        rewritten{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   atlasB = rewritten.uploadAtlas(sheet.data(), 16, 16).atlasId;
    (void)rewritten.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    (void)rewritten.uploadPalette(std::span<const Rgba8>(paletteColors(1)));
    const PaletteId subjectB = rewritten.uploadPalette(std::span<const Rgba8>(paletteColors(2)));
    // Past the rows the store holds, so every color in it — the subject included — is written again.
    (void)rewritten.uploadPalette(
        std::span<const Rgba8>(fillerColors(2u * static_cast<std::size_t>(kPaletteStoreWidth))));
    const std::vector<Rgba8> fromRewrite = renderSheet(rewritten, atlasB, subjectB);

    EXPECT_EQ(subjectA, subjectB);
    EXPECT_TRUE(carriesArt(fromAppend));
    EXPECT_TRUE(sameImage(fromAppend, fromRewrite));
}

// A palette whose colors cross the store's row edge is written as two rectangles, and renders exactly as
// it does after the whole store has been written afresh. The store is 16384 colors wide, so the subject
// is placed two colors short of the edge and half of it lands on the next row.
TEST_F(StoreUploadTest, APaletteCrossingTheRowEdgeRendersAsARewrittenOneDoes) {
    const std::vector<std::uint8_t> sheet    = sheetPixels(16, 16, /*seed=*/2);
    const std::size_t               toTheEdge = static_cast<std::size_t>(kPaletteStoreWidth) - 2u;

    Renderer        appended{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   atlasA = appended.uploadAtlas(sheet.data(), 16, 16).atlasId;
    (void)appended.uploadPalette(std::span<const Rgba8>(fillerColors(toTheEdge)));
    const PaletteId subjectA = appended.uploadPalette(std::span<const Rgba8>(paletteColors(3)));
    ASSERT_EQ(static_cast<std::size_t>(subjectA), toTheEdge);
    const std::vector<Rgba8> fromAppend = renderSheet(appended, atlasA, subjectA);

    Renderer        rewritten{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   atlasB = rewritten.uploadAtlas(sheet.data(), 16, 16).atlasId;
    (void)rewritten.uploadPalette(std::span<const Rgba8>(fillerColors(toTheEdge)));
    const PaletteId subjectB = rewritten.uploadPalette(std::span<const Rgba8>(paletteColors(3)));
    (void)rewritten.uploadPalette(
        std::span<const Rgba8>(fillerColors(2u * static_cast<std::size_t>(kPaletteStoreWidth))));
    const std::vector<Rgba8> fromRewrite = renderSheet(rewritten, atlasB, subjectB);

    EXPECT_TRUE(carriesArt(fromAppend));
    EXPECT_TRUE(sameImage(fromAppend, fromRewrite));
}

// An atlas appended over its own rows renders exactly as it does after the store has been restacked and
// every atlas written again. The second sheet is the subject: appended in the first renderer, rewritten by
// the third upload's growth in the second.
TEST_F(StoreUploadTest, AnAppendedAtlasRendersAsARewrittenOneDoes) {
    const std::vector<std::uint8_t> first  = sheetPixels(16, 16, /*seed=*/1);
    const std::vector<std::uint8_t> second = sheetPixels(16, 16, /*seed=*/2);
    const std::vector<std::uint8_t> third  = sheetPixels(16, 16, /*seed=*/3);

    Renderer      appended{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    (void)appended.uploadAtlas(first.data(), 16, 16);
    const AtlasId   subjectA  = appended.uploadAtlas(second.data(), 16, 16).atlasId;
    const PaletteId paletteA  = appended.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    const std::vector<Rgba8> fromAppend = renderSheet(appended, subjectA, paletteA);

    Renderer      rewritten{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    (void)rewritten.uploadAtlas(first.data(), 16, 16);
    const AtlasId subjectB = rewritten.uploadAtlas(second.data(), 16, 16).atlasId;
    (void)rewritten.uploadAtlas(third.data(), 16, 16);  // past the store's rows → every atlas written again
    const PaletteId paletteB = rewritten.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    const std::vector<Rgba8> fromRewrite = renderSheet(rewritten, subjectB, paletteB);

    EXPECT_EQ(subjectA, subjectB);
    EXPECT_TRUE(carriesArt(fromAppend));
    EXPECT_TRUE(sameImage(fromAppend, fromRewrite));
}

// The same claim where the store already holds several sheets: the sixth is appended onto the end of five
// and must render as it does once a seventh forces every one of them to be written again.
TEST_F(StoreUploadTest, AnAtlasAppendedOntoSeveralRendersAsARewrittenOneDoes) {
    std::vector<std::vector<std::uint8_t>> sheets;
    for (int i = 0; i < 7; ++i) sheets.push_back(sheetPixels(16, 16, i + 1));

    Renderer appended{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    AtlasId  subjectA{};
    for (int i = 0; i < 6; ++i) subjectA = appended.uploadAtlas(sheets[static_cast<std::size_t>(i)].data(), 16, 16).atlasId;
    const PaletteId          paletteA   = appended.uploadPalette(std::span<const Rgba8>(paletteColors(1)));
    const std::vector<Rgba8> fromAppend = renderSheet(appended, subjectA, paletteA);

    Renderer rewritten{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    AtlasId  subjectB{};
    for (int i = 0; i < 6; ++i) subjectB = rewritten.uploadAtlas(sheets[static_cast<std::size_t>(i)].data(), 16, 16).atlasId;
    (void)rewritten.uploadAtlas(sheets[6].data(), 16, 16);  // past the store's rows → all written again
    const PaletteId          paletteB    = rewritten.uploadPalette(std::span<const Rgba8>(paletteColors(1)));
    const std::vector<Rgba8> fromRewrite = renderSheet(rewritten, subjectB, paletteB);

    EXPECT_EQ(subjectA, subjectB);
    EXPECT_TRUE(carriesArt(fromAppend));
    EXPECT_TRUE(sameImage(fromAppend, fromRewrite));
}

// Sheets stack in upload order, so a sheet uploaded later begins where the ones before it end and cannot
// move them. The first sheet renders the same before and after another is appended beside it.
TEST_F(StoreUploadTest, AppendingASheetLeavesTheOnesBeforeItWhereTheyAre) {
    const std::vector<std::uint8_t> first  = sheetPixels(16, 16, /*seed=*/1);
    const std::vector<std::uint8_t> second = sheetPixels(16, 16, /*seed=*/2);

    Renderer        r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   subject = r.uploadAtlas(first.data(), 16, 16).atlasId;
    const PaletteId palette = r.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    const std::vector<Rgba8> before = renderSheet(r, subject, palette);

    (void)r.uploadAtlas(second.data(), 16, 16);
    const std::vector<Rgba8> after = renderSheet(r, subject, palette);

    EXPECT_TRUE(carriesArt(before));
    EXPECT_TRUE(sameImage(before, after));
}

// A wider sheet restacks the store — every row's stride changes and the whole store is written again —
// and the sheets already in it come back reading exactly as they did.
TEST_F(StoreUploadTest, AWiderSheetRestacksTheStoreWithoutDisturbingWhatIsInIt) {
    const std::vector<std::uint8_t> first  = sheetPixels(16, 16, /*seed=*/1);
    const std::vector<std::uint8_t> second = sheetPixels(16, 16, /*seed=*/2);
    const std::vector<std::uint8_t> wider  = sheetPixels(32, 16, /*seed=*/4);

    Renderer        r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    (void)r.uploadAtlas(first.data(), 16, 16);
    const AtlasId   subject = r.uploadAtlas(second.data(), 16, 16).atlasId;
    const PaletteId palette = r.uploadPalette(std::span<const Rgba8>(paletteColors(2)));
    const std::vector<Rgba8> before = renderSheet(r, subject, palette);

    (void)r.uploadAtlas(wider.data(), 32, 16);
    const std::vector<Rgba8> after = renderSheet(r, subject, palette);

    EXPECT_TRUE(carriesArt(before));
    EXPECT_TRUE(sameImage(before, after));
}

}  // namespace

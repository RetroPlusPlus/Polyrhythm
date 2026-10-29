// Releasing what a game uploaded. A released sheet draws nothing; its handle is minted again by a later
// upload and draws the new sheet; uploading and releasing a sheet over and over leaves the store its size;
// the room a released sheet held is taken by the next sheet that fits it, and a sheet that fits no released
// room is placed by a repack before the store grows; a sheet past the store's largest size is refused with
// nothing minted; sheets of different shapes share the store and each draws as it does alone; a released
// palette's entries are taken by the next palette that fits them; a store that grows has room for the next
// sheet; and the store holds at most 16384 sheets at once.
//
// Device-backed, compose-only + windowless (a GPU device, no display): the same harness the golden-readback
// and compose-skip tests use, so it runs on a software rasterizer in CI — lavapipe (Vulkan) on Linux, WARP
// (D3D12) on Windows, Metal on the Mac. A device is REQUIRED on every production-representative platform;
// the one skip is Windows on ARM (a courtesy coverage runner with no production-representative GPU backend
// in CI).

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

namespace {
using namespace retropp;

constexpr int kViewW = 64;
constexpr int kViewH = 64;

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

// A sheet of palette indices, distinct per `seed` and distinct from one 8×8 cell to the next, so a sheet
// read from the wrong texels — another sheet's, or its own a cell over — renders as different art rather
// than as something plausible.
std::vector<std::uint8_t> sheetPixels(int width, int height, int seed) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            px[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>((x + y * 3 + (x / 8) * 5 + (y / 8) * 7 + seed) % 4);
    return px;
}

// Four colors far enough apart that a wrong palette row is visible, seeded so each palette differs.
std::vector<Rgba8> paletteColors(int seed) {
    return {Rgba8{static_cast<std::uint8_t>(20 + seed * 30), 20, 30, 255},
            Rgba8{200, static_cast<std::uint8_t>(60 + seed * 20), 60, 255},
            Rgba8{60, 200, static_cast<std::uint8_t>(90 + seed * 15), 255},
            Rgba8{230, 230, 240, 255}};
}

// One tile layer drawing `atlas`'s cell `tile` (its second, unless named) through `palette`, captured as
// raw pixels.
std::vector<Rgba8> renderSheet(Renderer& r, AtlasId atlas, PaletteId palette, std::uint16_t tile = 1) {
    std::vector<TileCell> cells(8 * 8, TileCell{.atlas = atlas, .tile = tile, .palette = palette});
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
    return ::testing::AssertionFailure() << "the capture is one flat color — the sheet did not draw";
}

class AtlasReleaseTest : public ::testing::Test {
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

// A sheet draws until it is released, and draws nothing after.
TEST_F(AtlasReleaseTest, AReleasedSheetDrawsNothing) {
    const std::vector<std::uint8_t> sheet = sheetPixels(16, 16, /*seed=*/1);

    Renderer        r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   a = r.uploadAtlas(sheet.data(), 16, 16).atlasId;
    const PaletteId p = r.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    EXPECT_TRUE(carriesArt(renderSheet(r, a, p)));

    r.releaseAtlas(a);
    EXPECT_FALSE(carriesArt(renderSheet(r, a, p)));
}

// The next upload takes the retired handle, and the handle draws the new sheet exactly as that sheet draws
// on a renderer that never held the first.
TEST_F(AtlasReleaseTest, ARetiredHandleIsMintedAgainAndDrawsTheNewSheet) {
    const std::vector<std::uint8_t> first  = sheetPixels(16, 16, /*seed=*/1);
    const std::vector<std::uint8_t> second = sheetPixels(16, 16, /*seed=*/2);

    Renderer        r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const PaletteId p = r.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    const AtlasId   a = r.uploadAtlas(first.data(), 16, 16).atlasId;
    r.releaseAtlas(a);
    const AtlasId b = r.uploadAtlas(second.data(), 16, 16).atlasId;
    EXPECT_EQ(a, b);
    const std::vector<Rgba8> reminted = renderSheet(r, b, p);

    Renderer        fresh{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const PaletteId freshPalette = fresh.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    const AtlasId   freshSheet   = fresh.uploadAtlas(second.data(), 16, 16).atlasId;
    const std::vector<Rgba8> alone = renderSheet(fresh, freshSheet, freshPalette);

    EXPECT_TRUE(carriesArt(reminted));
    EXPECT_TRUE(sameImage(reminted, alone));
}

// Uploading a sheet and releasing it, ten thousand times over, leaves the store the size it was and mints
// the same handle each time: each upload takes the room and the handle the last release gave back.
TEST_F(AtlasReleaseTest, TenThousandUploadAndReleaseCyclesDoNotGrowTheStore) {
    const std::vector<std::uint8_t> sheet = sheetPixels(16, 16, /*seed=*/1);

    Renderer      r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId first = r.uploadAtlas(sheet.data(), 16, 16).atlasId;
    r.releaseAtlas(first);
    const int width  = r.renderStats().atlasStoreWidth;
    const int height = r.renderStats().atlasStoreHeight;

    for (int i = 0; i < 10000; ++i) {
        const AtlasId id = r.uploadAtlas(sheet.data(), 16, 16).atlasId;
        r.releaseAtlas(id);
    }
    EXPECT_EQ(r.renderStats().atlasStoreWidth, width);
    EXPECT_EQ(r.renderStats().atlasStoreHeight, height);
    EXPECT_EQ(r.uploadAtlas(sheet.data(), 16, 16).atlasId, first);
}

// Eight 64×64 sheets fill the store to its last texel, so the room two released sheets held is the only room
// the next two sheets can take, and the store keeps its size.
TEST_F(AtlasReleaseTest, ReleasedRoomIsTakenBySheetsThatFitIt) {
    std::vector<std::vector<std::uint8_t>> sheets;
    for (int i = 0; i < 10; ++i) sheets.push_back(sheetPixels(64, 64, i + 1));

    Renderer             r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    std::vector<AtlasId> ids;
    for (int i = 0; i < 8; ++i)
        ids.push_back(r.uploadAtlas(sheets[static_cast<std::size_t>(i)].data(), 64, 64).atlasId);
    const int width  = r.renderStats().atlasStoreWidth;
    const int height = r.renderStats().atlasStoreHeight;

    r.releaseAtlas(ids[1]);
    r.releaseAtlas(ids[2]);
    (void)r.uploadAtlas(sheets[8].data(), 64, 64);
    (void)r.uploadAtlas(sheets[9].data(), 64, 64);
    EXPECT_EQ(r.renderStats().atlasStoreWidth, width);
    EXPECT_EQ(r.renderStats().atlasStoreHeight, height);
}

// Eight 64×64 sheets fill the store; with all eight released, a sheet wider than every released room has no
// shelf room left either, and is placed by laying the live sheets out afresh at the store's size: the store
// keeps its size, and the sheet draws exactly as it does alone.
TEST_F(AtlasReleaseTest, ASheetThatFitsNoReleasedRoomIsPlacedByARepackAtTheStoresSize) {
    std::vector<std::vector<std::uint8_t>> sheets;
    for (int i = 0; i < 8; ++i) sheets.push_back(sheetPixels(64, 64, i + 1));
    const std::vector<std::uint8_t> wide = sheetPixels(128, 64, /*seed=*/9);

    Renderer             r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    std::vector<AtlasId> ids;
    for (int i = 0; i < 8; ++i)
        ids.push_back(r.uploadAtlas(sheets[static_cast<std::size_t>(i)].data(), 64, 64).atlasId);
    const int width  = r.renderStats().atlasStoreWidth;
    const int height = r.renderStats().atlasStoreHeight;

    for (const AtlasId id : ids) r.releaseAtlas(id);
    const AtlasId   wideId = r.uploadAtlas(wide.data(), 128, 64).atlasId;
    const PaletteId p      = r.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    EXPECT_EQ(r.renderStats().atlasStoreWidth, width);
    EXPECT_EQ(r.renderStats().atlasStoreHeight, height);
    const std::vector<Rgba8> shared = renderSheet(r, wideId, p);

    Renderer        alone{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const PaletteId ap = alone.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    const AtlasId   aw = alone.uploadAtlas(wide.data(), 128, 64).atlasId;

    EXPECT_TRUE(carriesArt(shared));
    EXPECT_TRUE(sameImage(shared, renderSheet(alone, aw, ap)));
}

// A sheet taller than the store can be is refused, and no handle is spent on it: the next upload takes the
// handle the refused one would have had.
TEST_F(AtlasReleaseTest, ASheetPastTheCapIsRefusedWithNothingMinted) {
    const std::vector<std::uint8_t> small = sheetPixels(16, 16, /*seed=*/1);
    const std::vector<std::uint8_t> tall(static_cast<std::size_t>(8) * 16400u);

    Renderer      r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId a = r.uploadAtlas(small.data(), 16, 16).atlasId;
    EXPECT_THROW((void)r.uploadAtlas(tall.data(), 8, 16400), std::runtime_error);
    const AtlasId b = r.uploadAtlas(small.data(), 16, 16).atlasId;
    EXPECT_EQ(static_cast<int>(b), static_cast<int>(a) + 1);
}

// A wide sheet and a tall one share the store, and each draws exactly as it does alone.
TEST_F(AtlasReleaseTest, AWideSheetAndATallSheetShareTheStore) {
    const std::vector<std::uint8_t> wide = sheetPixels(512, 8, /*seed=*/1);
    const std::vector<std::uint8_t> tall = sheetPixels(8, 512, /*seed=*/2);

    Renderer        r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const PaletteId p        = r.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    const AtlasId   wideId   = r.uploadAtlas(wide.data(), 512, 8).atlasId;
    const AtlasId   tallId   = r.uploadAtlas(tall.data(), 8, 512).atlasId;
    const std::vector<Rgba8> wideShared = renderSheet(r, wideId, p);
    const std::vector<Rgba8> tallShared = renderSheet(r, tallId, p);

    Renderer        wideAlone{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const PaletteId wp = wideAlone.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    const AtlasId   wa = wideAlone.uploadAtlas(wide.data(), 512, 8).atlasId;

    Renderer        tallAlone{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const PaletteId tp = tallAlone.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    const AtlasId   ta = tallAlone.uploadAtlas(tall.data(), 8, 512).atlasId;

    EXPECT_TRUE(carriesArt(wideShared));
    EXPECT_TRUE(carriesArt(tallShared));
    EXPECT_TRUE(sameImage(wideShared, renderSheet(wideAlone, wa, wp)));
    EXPECT_TRUE(sameImage(tallShared, renderSheet(tallAlone, ta, tp)));
}

// Releasing a handle no upload minted is refused, and so is releasing the same sheet a second time.
TEST_F(AtlasReleaseTest, ReleasingAHandleNobodyMintedIsRefused) {
    const std::vector<std::uint8_t> sheet = sheetPixels(16, 16, /*seed=*/1);

    Renderer      r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId a = r.uploadAtlas(sheet.data(), 16, 16).atlasId;
    EXPECT_THROW(r.releaseAtlas(static_cast<AtlasId>(40)), std::invalid_argument);
    r.releaseAtlas(a);
    EXPECT_THROW(r.releaseAtlas(a), std::invalid_argument);
}

// A palette that fits a released palette's entries takes them; one that does not lands past the end.
TEST_F(AtlasReleaseTest, AReleasedPalettesEntriesAreTakenByTheNextPaletteThatFits) {
    const std::vector<std::uint8_t> sheet = sheetPixels(16, 16, /*seed=*/1);

    Renderer        r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const AtlasId   atlas = r.uploadAtlas(sheet.data(), 16, 16).atlasId;
    (void)r.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    const PaletteId b = r.uploadPalette(std::span<const Rgba8>(paletteColors(1)));
    const PaletteId c = r.uploadPalette(std::span<const Rgba8>(paletteColors(2)));

    r.releasePalette(b);
    const PaletteId d = r.uploadPalette(std::span<const Rgba8>(paletteColors(3)));
    EXPECT_EQ(d, b);

    std::vector<Rgba8> eight = paletteColors(4);
    const std::vector<Rgba8> more = paletteColors(5);
    eight.insert(eight.end(), more.begin(), more.end());
    const PaletteId e = r.uploadPalette(std::span<const Rgba8>(eight));
    EXPECT_EQ(static_cast<int>(e), static_cast<int>(c) + 4) << "past the end, not in the released entries";

    const std::vector<Rgba8> throughD = renderSheet(r, atlas, d);
    EXPECT_TRUE(carriesArt(throughD));
    EXPECT_FALSE(sameImage(throughD, renderSheet(r, atlas, c)));
}

// Releasing the palette at the end of the store gives its entries back to the end, so the next palette
// begins where it began; a handle that begins no palette is refused.
TEST_F(AtlasReleaseTest, ReleasingTheLastPaletteShrinksTheMirrorAndAnUnknownIdIsRefused) {
    Renderer        r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    (void)r.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    const PaletteId b = r.uploadPalette(std::span<const Rgba8>(paletteColors(1)));

    r.releasePalette(b);
    std::vector<Rgba8> six = paletteColors(2);
    six.push_back(Rgba8{.r = 10, .g = 10, .b = 10, .a = 255});
    six.push_back(Rgba8{.r = 250, .g = 250, .b = 250, .a = 255});
    const PaletteId c = r.uploadPalette(std::span<const Rgba8>(six));
    EXPECT_EQ(c, b);
    EXPECT_THROW(r.releasePalette(static_cast<PaletteId>(2)), std::invalid_argument);
}

// A store that grows is one size larger than its sheets need, so the sheet uploaded next is written into the
// room it has and the store keeps its size; the sheet after that grows it again.
TEST_F(AtlasReleaseTest, AGrownStoreHasRoomForTheNextSheet) {
    const std::vector<std::uint8_t> sheet = sheetPixels(16, 16, /*seed=*/1);

    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    (void)r.uploadAtlas(sheet.data(), 16, 16);
    EXPECT_EQ(r.renderStats().atlasStoreWidth, 32);
    EXPECT_EQ(r.renderStats().atlasStoreHeight, 16);

    (void)r.uploadAtlas(sheet.data(), 16, 16);
    EXPECT_EQ(r.renderStats().atlasStoreWidth, 32) << "written into the room the store has";
    EXPECT_EQ(r.renderStats().atlasStoreHeight, 16);

    (void)r.uploadAtlas(sheet.data(), 16, 16);
    EXPECT_EQ(r.renderStats().atlasStoreWidth, 64) << "laid out at 32×32, then one size larger";
    EXPECT_EQ(r.renderStats().atlasStoreHeight, 32);
}

// The store holds 16384 sheets at once. The one past that is refused with nothing minted and nothing left
// to write; a released handle is minted again, and its sheet draws.
TEST_F(AtlasReleaseTest, TheStoreHoldsAtMost16384SheetsAtOnce) {
    Renderer        r{device_, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
    const PaletteId p = r.uploadPalette(std::span<const Rgba8>(paletteColors(0)));
    AtlasId         last{};
    for (int i = 0; i < 16384; ++i) {
        const std::vector<std::uint8_t> sheet = sheetPixels(8, 8, i);
        last = r.uploadAtlas(sheet.data(), 8, 8).atlasId;
    }
    const int width  = r.renderStats().atlasStoreWidth;
    const int height = r.renderStats().atlasStoreHeight;

    const std::vector<std::uint8_t> extra = sheetPixels(8, 8, /*seed=*/1);
    EXPECT_THROW((void)r.uploadAtlas(extra.data(), 8, 8), std::runtime_error);
    EXPECT_EQ(r.renderStats().atlasStoreWidth, width);
    EXPECT_EQ(r.renderStats().atlasStoreHeight, height);

    r.releaseAtlas(last);
    EXPECT_EQ(r.uploadAtlas(extra.data(), 8, 8).atlasId, last);
    // An 8×8 sheet is one cell, so its art is cell 0.
    EXPECT_TRUE(carriesArt(renderSheet(r, last, p, /*tile=*/0)));
}

}  // namespace

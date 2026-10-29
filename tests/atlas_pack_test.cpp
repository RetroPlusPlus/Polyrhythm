// Where a sheet lands in the atlas store. A sheet goes on the first shelf tall enough with room left, or
// opens a new shelf below the last; a released sheet's room is taken by the next sheet that fits it, and
// what is left of that room stays free; a repack lays every sheet out tallest first, and lays the same
// sheets out the same way every time; a store grows by its shorter side, up to the most a side can be; and
// whatever fits a store fits it grown.
//
// Device-free: a placement that is off by a shelf writes correct pixels to the wrong texels and nothing
// fails, so the arithmetic is exercised on its own.

#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include <retropp/atlas_pack.h>

namespace {
using retropp::detail::AtlasPlacement;
using retropp::detail::AtlasSheetSize;
using retropp::detail::AtlasShelves;
using retropp::detail::AtlasStoreSize;
using retropp::detail::firstAtlasStore;
using retropp::detail::grownAtlasStore;
using retropp::detail::packAtlasSheets;

TEST(AtlasShelves, AFirstSheetOpensAShelfAtTheOrigin) {
    AtlasShelves s(64, 64);
    EXPECT_EQ(s.place(16, 8), (AtlasPlacement{.x = 0, .y = 0}));
}

TEST(AtlasShelves, ASheetNoTallerThanTheShelfSitsToItsRight) {
    AtlasShelves s(64, 64);
    ASSERT_EQ(s.place(16, 8), (AtlasPlacement{.x = 0, .y = 0}));
    EXPECT_EQ(s.place(8, 8), (AtlasPlacement{.x = 16, .y = 0}));
    EXPECT_EQ(s.place(8, 4), (AtlasPlacement{.x = 24, .y = 0}));
}

TEST(AtlasShelves, ATallerSheetOpensANewShelfBelow) {
    AtlasShelves s(64, 64);
    ASSERT_EQ(s.place(16, 8), (AtlasPlacement{.x = 0, .y = 0}));
    EXPECT_EQ(s.place(16, 16), (AtlasPlacement{.x = 0, .y = 8}));
}

TEST(AtlasShelves, AShelfFullToTheRightOpensANewOne) {
    AtlasShelves s(32, 64);
    ASSERT_EQ(s.place(16, 8), (AtlasPlacement{.x = 0, .y = 0}));
    ASSERT_EQ(s.place(16, 8), (AtlasPlacement{.x = 16, .y = 0}));
    EXPECT_EQ(s.place(8, 8), (AtlasPlacement{.x = 0, .y = 8}));
}

TEST(AtlasShelves, ASheetWithNoRoomBelowIsNotPlaced) {
    AtlasShelves s(32, 16);
    ASSERT_EQ(s.place(32, 16), (AtlasPlacement{.x = 0, .y = 0}));
    EXPECT_EQ(s.place(8, 8), std::nullopt);
    EXPECT_EQ(s.place(40, 8), std::nullopt) << "a sheet wider than the store";
    EXPECT_EQ(s.place(0, 8), std::nullopt) << "a sheet with no area";
}

TEST(AtlasShelves, AReleasedSheetsRoomIsTakenByTheNextSheetThatFitsIt) {
    AtlasShelves s(64, 64);
    ASSERT_EQ(s.place(16, 8), (AtlasPlacement{.x = 0, .y = 0}));
    ASSERT_EQ(s.place(16, 8), (AtlasPlacement{.x = 16, .y = 0}));
    s.release(AtlasPlacement{.x = 0, .y = 0}, 16, 8);
    EXPECT_EQ(s.place(16, 8), (AtlasPlacement{.x = 0, .y = 0})) << "the released room";
    EXPECT_EQ(s.place(16, 8), (AtlasPlacement{.x = 32, .y = 0})) << "the shelf again";
}

TEST(AtlasShelves, WhatIsLeftOfAReleasedSheetsRoomStaysFree) {
    AtlasShelves s(64, 64);
    ASSERT_EQ(s.place(16, 16), (AtlasPlacement{.x = 0, .y = 0}));
    s.release(AtlasPlacement{.x = 0, .y = 0}, 16, 16);
    EXPECT_EQ(s.place(8, 8), (AtlasPlacement{.x = 0, .y = 0}));
    EXPECT_EQ(s.place(8, 8), (AtlasPlacement{.x = 8, .y = 0})) << "to its right";
    EXPECT_EQ(s.place(16, 8), (AtlasPlacement{.x = 0, .y = 8})) << "below";
}

TEST(AtlasPack, APackIsTallestFirstAndInputOrderOnATie) {
    const std::vector<AtlasSheetSize> sizes{{.width = 16, .height = 8},
                                            {.width = 16, .height = 16},
                                            {.width = 16, .height = 8}};
    const auto placed = packAtlasSheets(sizes, AtlasStoreSize{.width = 48, .height = 16});
    ASSERT_TRUE(placed.has_value());
    EXPECT_EQ(*placed, (std::vector<AtlasPlacement>{{.x = 16, .y = 0}, {.x = 0, .y = 0}, {.x = 32, .y = 0}}))
        << "the placements are in input order";
}

TEST(AtlasPack, APackIsTheSameForTheSameSheets) {
    const std::vector<AtlasSheetSize> sizes{{.width = 24, .height = 8},
                                            {.width = 8, .height = 32},
                                            {.width = 40, .height = 16},
                                            {.width = 16, .height = 16}};
    const AtlasStoreSize store{.width = 64, .height = 64};
    const auto           first  = packAtlasSheets(sizes, store);
    const auto           second = packAtlasSheets(sizes, store);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *second);
}

TEST(AtlasPack, APackThatDoesNotFitIsNothing) {
    const std::vector<AtlasSheetSize> two{{.width = 32, .height = 32}, {.width = 32, .height = 32}};
    EXPECT_EQ(packAtlasSheets(two, AtlasStoreSize{.width = 32, .height = 32}), std::nullopt);
    const std::vector<AtlasSheetSize> wide{{.width = 40, .height = 8}};
    EXPECT_EQ(packAtlasSheets(wide, AtlasStoreSize{.width = 32, .height = 32}), std::nullopt);
}

TEST(AtlasPack, AFirstStoreIsTheSmallestPowerOfTwoSquareThatHoldsTheSheet) {
    EXPECT_EQ(firstAtlasStore(16, 1024), (AtlasStoreSize{.width = 1024, .height = 1024}));
    EXPECT_EQ(firstAtlasStore(1000, 8), (AtlasStoreSize{.width = 1024, .height = 1024}));
    EXPECT_EQ(firstAtlasStore(20000, 8), (AtlasStoreSize{.width = 16384, .height = 16384}));
}

TEST(AtlasPack, AStoreGrowsByItsShorterSideToTheCap) {
    EXPECT_EQ(grownAtlasStore(AtlasStoreSize{.width = 256, .height = 256}),
              (AtlasStoreSize{.width = 512, .height = 256}))
        << "the width on a tie";
    EXPECT_EQ(grownAtlasStore(AtlasStoreSize{.width = 512, .height = 256}),
              (AtlasStoreSize{.width = 512, .height = 512}));
    EXPECT_EQ(grownAtlasStore(AtlasStoreSize{.width = 16384, .height = 8192}),
              (AtlasStoreSize{.width = 16384, .height = 16384}));
    EXPECT_EQ(grownAtlasStore(AtlasStoreSize{.width = 16384, .height = 16384}), std::nullopt);
}

// A store that grows is laid out at the size past the first that holds its sheets, so whatever fits a store
// must fit it grown.
TEST(AtlasPack, WhatFitsAStoreFitsItGrown) {
    const std::vector<AtlasSheetSize> five(5, AtlasSheetSize{.width = 16, .height = 8});
    const AtlasStoreSize              narrow{.width = 48, .height = 16};
    ASSERT_EQ(grownAtlasStore(narrow), (AtlasStoreSize{.width = 48, .height = 32}));
    EXPECT_TRUE(packAtlasSheets(five, narrow).has_value());
    EXPECT_TRUE(packAtlasSheets(five, *grownAtlasStore(narrow)).has_value());

    const std::vector<AtlasSheetSize> two(2, AtlasSheetSize{.width = 32, .height = 32});
    const AtlasStoreSize              wide{.width = 64, .height = 32};
    ASSERT_EQ(grownAtlasStore(wide), (AtlasStoreSize{.width = 64, .height = 64}));
    EXPECT_TRUE(packAtlasSheets(two, wide).has_value());
    EXPECT_TRUE(packAtlasSheets(two, *grownAtlasStore(wide)).has_value());
}

}  // namespace

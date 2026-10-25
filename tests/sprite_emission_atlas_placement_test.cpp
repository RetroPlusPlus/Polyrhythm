// Placing a sprite's emission raster in the atlas — the footprint it covers, and the record that draws it
// somewhere else.
//
// A layer's emission instances draw in one instanced pass, each into its own rect. Nothing about that is a
// new mechanism: the record already carries a map ending in screen→clip, so re-expressing it against the
// atlas grid with the rect's offset in between moves the quad. These tests pin that the moved quad lands
// exactly where the offset says, and that the fragment's inverse map moves with it. A field is a viewport-
// space image, so the offset is already a whole number of viewport pixels and the crisp snap survives it.
//
// Device-free: the arithmetic decides where a halo appears, so the arithmetic is asserted.

#include <gtest/gtest.h>

#include <retropp/draw_state.h>
#include <retropp/emission_atlas.h>
#include <retropp/postprocess.h>
#include <retropp/transform.h>

using namespace retropp;

namespace {

Sprite spriteAt(float x, float y, int w, int h, Transform t = Transform{}) {
    Sprite s{.key = "emitter"};
    s.x         = x;
    s.y         = y;
    s.size      = AssetDimensions{.width = w, .height = h};
    s.transform = t;
    return s;
}

// A point through a homography, perspective divide included — what both the vertex stage and the
// fragment's analytic branch do with these rows.
struct Pt {
    float x, y;
};
Pt through(const Transform& m, Pt p) {
    const float w = m.m20 * p.x + m.m21 * p.y + m.m22;
    return Pt{(m.m00 * p.x + m.m01 * p.y + m.m02) / w, (m.m10 * p.x + m.m11 * p.y + m.m12) / w};
}

}  // namespace

// ── The footprint ───────────────────────────────────────────────────────────────────────────

TEST(EmissionAtlasPlacement, AnUntransformedSpriteCoversItsOwnRectangle) {
    const GpuSprite rec = makeGpuSprite(spriteAt(40.0f, 24.0f, 32, 16), 160, 144, 0.0f, 0.0f);

    EXPECT_EQ(spriteFootprint(rec, 160, 144), (PixelBox{.x = 40, .y = 24, .w = 32, .h = 16}));
}

TEST(EmissionAtlasPlacement, TheFootprintIsMeasuredOnWhicheverGridItIsGiven) {
    // The record maps to clip, which is resolution-free; the box it covers is the grid it is measured on. A
    // field lives on the VIEWPORT grid, which is why the renderer measures there — measuring on the compose
    // grid instead would make a field's memory follow the window's scale.
    const GpuSprite rec = makeGpuSprite(spriteAt(40.0f, 24.0f, 32, 16), 160, 144, 0.0f, 0.0f);

    EXPECT_EQ(spriteFootprint(rec, 320, 288), (PixelBox{.x = 80, .y = 48, .w = 64, .h = 32}));
}

TEST(EmissionAtlasPlacement, ATransformedSpriteCoversTheQuadItActuallyDraws) {
    // Rotated 45°, a 32×32 quad spans at least its diagonal. The box is read off the record's own forward
    // map, so it covers the quad as DRAWN — makeGpuSprite widens that quad past the corners so a transformed
    // sprite is not clipped at its static rectangle, and a field has to cover the widened one, since that is
    // the extent the raster fills.
    const GpuSprite rec =
        makeGpuSprite(spriteAt(60.0f, 60.0f, 32, 32, Transform::rotation(45.0f, 16.0f, 16.0f)), 160, 144,
                      0.0f, 0.0f);

    const PixelBox box  = spriteFootprint(rec, 160, 144);
    const float    diag = 32.0f * 1.41421356f;
    EXPECT_GE(static_cast<float>(box.w), diag);
    EXPECT_GE(static_cast<float>(box.h), diag);
    EXPECT_LE(box.x, 60);
    EXPECT_GE(box.x + box.w, 60 + 32);
}

TEST(EmissionAtlasPlacement, OffScreenDoesNotEnlargeTheBox) {
    // Position never costs atlas area: a sprite hanging off the edge asks for exactly what an on-screen one
    // does, so the box is never clipped and its overhang can never raster into a neighbor's rect.
    const GpuSprite inside  = makeGpuSprite(spriteAt(40.0f, 24.0f, 32, 16), 160, 144, 0.0f, 0.0f);
    const GpuSprite hanging = makeGpuSprite(spriteAt(-20.0f, 24.0f, 32, 16), 160, 144, 0.0f, 0.0f);

    const PixelBox a = spriteFootprint(inside, 160, 144);
    const PixelBox b = spriteFootprint(hanging, 160, 144);
    EXPECT_EQ(a.w, b.w);
    EXPECT_EQ(a.h, b.h);
    EXPECT_EQ(b.x, -20);
}

// ── The retargeted instance ─────────────────────────────────────────────────────────────────

TEST(EmissionAtlasPlacement, TheInstanceLandsAtTheOffsetItWasGiven) {
    const GpuSprite rec = makeGpuSprite(spriteAt(40.0f, 24.0f, 32, 16), 160, 144, 0.0f, 0.0f);
    const GpuSprite at  = spriteAtlasInstance(rec, 160, 144, 256, 256, 100, 7);

    // Measured against the ATLAS grid, the quad sits exactly the offset away from where it sat.
    EXPECT_EQ(spriteFootprint(at, 256, 256), (PixelBox{.x = 140, .y = 31, .w = 32, .h = 16}));
}

TEST(EmissionAtlasPlacement, AZeroOffsetIntoAnEquallySizedAtlasChangesNothing) {
    const GpuSprite rec = makeGpuSprite(spriteAt(40.0f, 24.0f, 32, 16), 160, 144, 0.0f, 0.0f);
    const GpuSprite at  = spriteAtlasInstance(rec, 160, 144, 160, 144, 0, 0);

    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(at.row0[i], rec.row0[i], 1e-5f) << "row0 lane " << i;
        EXPECT_NEAR(at.row1[i], rec.row1[i], 1e-5f) << "row1 lane " << i;
        EXPECT_NEAR(at.row2[i], rec.row2[i], 1e-5f) << "row2 lane " << i;
        EXPECT_NEAR(at.inv0[i], rec.inv0[i], 1e-5f) << "inv0 lane " << i;
    }
}

TEST(EmissionAtlasPlacement, TheAtlasGridDoesNotStretchTheQuad) {
    // The atlas is a different size from the grid the record was built for, and the quad must not scale with
    // it — a rect is sized in viewport pixels, so the art keeps that size wherever it is packed.
    const GpuSprite rec  = makeGpuSprite(spriteAt(40.0f, 24.0f, 32, 16), 160, 144, 0.0f, 0.0f);
    const GpuSprite wide = spriteAtlasInstance(rec, 160, 144, 1024, 512, 0, 0);

    const PixelBox box = spriteFootprint(wide, 1024, 512);
    EXPECT_EQ(box.w, 32);
    EXPECT_EQ(box.h, 16);
}

TEST(EmissionAtlasPlacement, TheInverseMapMovesWithTheQuad) {
    // The analytic branch rebuilds its viewport position from SV_Position, which in the atlas is the moved
    // position. The inverse pre-subtracts the offset, so the same art texel is read wherever the rect sits —
    // without that, a retargeted sprite would radiate the wrong part of its own art.
    const GpuSprite rec = makeGpuSprite(spriteAt(40.0f, 24.0f, 32, 16), 160, 144, 0.0f, 0.0f);
    const int       dx = 96, dy = 12;
    const GpuSprite at = spriteAtlasInstance(rec, 160, 144, 512, 512, dx, dy);

    const Transform before = spriteInverseMap(rec);
    const Transform after   = spriteInverseMap(at);
    for (const Pt viewportPos : {Pt{40.5f, 24.5f}, Pt{55.5f, 31.5f}, Pt{71.5f, 39.5f}}) {
        const Pt want = through(before, viewportPos);
        const Pt got  = through(after, Pt{viewportPos.x + static_cast<float>(dx),
                                          viewportPos.y + static_cast<float>(dy)});
        EXPECT_NEAR(got.x, want.x, 1e-4f);
        EXPECT_NEAR(got.y, want.y, 1e-4f);
    }
}

#include "retropp/draw_state.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

// Device-free coverage of frame assembly: N-layer composition order across role-free mixed-content
// layers, and the carriage of the frame's whole-frame effect lists. Whole-frame color (day/night,
// fades, flash, tints) is a screen-space effect (a ColorFill region with a blend mode), covered by
// colorfill_effect_test / blend_mode_test; here it rides as data on FrameDrawState like any effect.

namespace retropp {
namespace {

// ── N-layer composition: role-free, mixed content, ordered back-to-front ──────────────

// A layer carries either tile or sprite content — there is NO engine role. Any content can sit
// at any z; "the layer above the sprite layer" (the priority realization) is just the highest-z
// layer, whatever it holds. This hardens the ordering across 4+ mixed-kind layers.
TEST(FrameFinish, MixedContentLayersOrderBackToFrontRoleFree) {
    std::vector<DrawLayer> layers;
    // Submission order deliberately scrambled; content kinds deliberately mixed at every z.
    layers.push_back(DrawLayer{.key = "foreground", .z = 30, .content = TileContent{}});    // top: TILES foreground
    layers.push_back(DrawLayer{.key = "farBackground", .z = 0, .content = TileContent{}});  // far bg
    layers.push_back(DrawLayer{.key = "characters", .z = 20, .content = SpriteContent{}});  // characters
    layers.push_back(DrawLayer{.key = "spriteBand", .z = 10, .content = SpriteContent{}});  // a sprite-content bg band

    const auto order = layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw);
    ASSERT_EQ(order.size(), 4u);
    EXPECT_EQ(layers[order[0]].z, 0);    // composited first (back)
    EXPECT_EQ(layers[order[1]].z, 10);
    EXPECT_EQ(layers[order[2]].z, 20);
    EXPECT_EQ(layers[order[3]].z, 30);   // composited last (front) — the foreground-over-sprites layer

    // The front layer being TILES and a mid layer being SPRITES proves no role is fixed to a slot.
    EXPECT_EQ(contentKind(layers[order[3]].content), LayerContentKind::Tiles);
    EXPECT_EQ(contentKind(layers[order[2]].content), LayerContentKind::Sprites);
}

// Z values are ARBITRARY and need not be sequential or contiguous: a game can keep its sprite
// layers in a z-band like 40..60, drop a layer at z=255 right next to one at z=15, and use gaps
// or negatives freely. There is no "z=1 must exist before z=2" rule — the only constraint is that
// z be unique within a frame so the front-to-back order is unambiguous. Higher z is nearer the
// top; the compositor just sorts ascending and paints back-to-front, so gaps cost nothing. (Ids
// are equally free — non-sequential here too — and the renderer keys GPU resources by submission
// position, never by id or z value.)
TEST(FrameFinish, ArbitrarySparseZComposesByDepth) {
    std::vector<DrawLayer> layers{
        DrawLayer{.key = "hud",             .z = 255, .content = TileContent{}},    // a far-foreground HUD at z=255
        DrawLayer{.key = "background",       .z = 15,  .content = TileContent{}},    // a background at z=15
        DrawLayer{.key = "bottomSprites",    .z = 45,  .content = SpriteContent{}},  // sprite layer in the 40..60 band
        DrawLayer{.key = "topSprites",       .z = 55,  .content = SpriteContent{}},  // sprite layer in the 40..60 band
    };
    const auto order = layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw);
    ASSERT_EQ(order.size(), 4u);
    // Pure z-ascending (back-to-front) order, regardless of how sparse / unordered the z's are.
    EXPECT_EQ(layers[order[0]].z, 15);   // bottom / back
    EXPECT_EQ(layers[order[1]].z, 45);   // sprite band
    EXPECT_EQ(layers[order[2]].z, 55);   // sprite band
    EXPECT_EQ(layers[order[3]].z, 255);  // top / front
}

// A layer's label is its human-readable name with no depth role: depth follows z alone, never the
// name's lexical order. The name is stored verbatim and a duplicate name is rejected (the label must
// be unique).
TEST(FrameFinish, LabelIsANameWithNoDepthRole) {
    // Names sort OPPOSITE to z ("aaa" < "zzz" lexically, but z puts zzz on top): order must follow z.
    const std::vector<DrawLayer> layers{
        DrawLayer{.key = "zzzTopLayer",    .z = 100},
        DrawLayer{.key = "aaaBottomLayer", .z = 0},
    };
    const auto order = layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw);
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(std::string_view{layers[order[0]].key}, "aaaBottomLayer");  // z=0 → back, despite name sorting last
    EXPECT_EQ(std::string_view{layers[order[1]].key}, "zzzTopLayer");     // z=100 → front (key preserved verbatim)

    // A duplicate key is a DuplicateKey collision — the key must be unambiguous.
    const std::vector<DrawLayer> dup{
        DrawLayer{.key = "player", .z = 0},
        DrawLayer{.key = "player", .z = 10},
    };
    EXPECT_THROW((void)layerDrawOrder(dup, LayerKeyCollisionPolicy::Throw), std::invalid_argument);
}

TEST(FrameFinish, ManyLayerStackStillTripsDuplicateZ) {
    constexpr std::array<const char*, 6> names{"l0", "l1", "l2", "l3", "l4", "l5"};
    std::vector<DrawLayer> layers;
    for (int i = 0; i < 6; ++i) {
        layers.push_back(DrawLayer{.key = names[static_cast<std::size_t>(i)], .z = i});
    }
    layers.push_back(DrawLayer{.key = "extra", .z = 3});  // z collides with l3
    EXPECT_THROW((void)layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw), std::invalid_argument);
}

// ── Whole-frame effect carriage ───────────────────────────────────────────────────────

// The frame carries a whole-frame postEffects list and a confined-effect regions list. Whole-frame
// color is just an effect here: a Multiply ColorFill region is a day/night tint. The frame's own
// `blend` (the container blend mode) defaults to Normal. The two lists are carried verbatim and are
// independent of each other.
TEST(FrameFinish, PostEffectsAndColourRegionsAreCarriedIndependently) {
    FrameDrawState frame;
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement,
                                                  .amplitude = 2.0f});
    frame.regions.push_back(Region{.key = "dim",
                                   .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                                                 .fill = Rgba8{128, 128, 160, 255}}},
                                   .blend = BlendMode::Multiply});

    ASSERT_EQ(frame.postEffects.size(), 1u);
    EXPECT_EQ(frame.postEffects[0].kind, ScreenSpaceEffectKind::RowDisplacement);
    ASSERT_EQ(frame.regions.size(), 1u);
    EXPECT_EQ(frame.regions[0].blend, BlendMode::Multiply);
    ASSERT_EQ(frame.regions[0].effects.size(), 1u);
    EXPECT_EQ(frame.regions[0].effects[0].kind, ScreenSpaceEffectKind::ColorFill);
    EXPECT_EQ(frame.blend, BlendMode::Normal);  // frame container blend defaults to Normal
}

}  // namespace
}  // namespace retropp

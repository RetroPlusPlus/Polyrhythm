// Articulation showcase — sprite anchors, origin/pivot placement, and per-sprite z-order, all on ONE layer:
//
//   • A CRAB-BOT whose body, upper arm, forearm, and claw are separate sprites chained purely by
//     re-feeding anchor queries: each child's x/y is the parent's anchor(joint, Space::Layer) and each
//     child's origin and pivot are its own anchor(socket, Space::Quad) — the mount and the hinge are the
//     same point, so every joint rotates about its socket by construction. The body rocks gently about
//     its center (origin = pivot = center(Space::Quad)) and the whole creature rides the rock coherently,
//     because every child re-resolves its parent's anchor each tick.
//   • PER-SPRITE Z within the one layer: the parts are deliberately submitted front-first (claw first,
//     body last) and their z keys restore the stacking. Toggle the keys off (Z) and the same submission
//     stacks backward — the claw vanishes behind the arm.
//   • ANCHORS FOLLOW THE ART: flip the facing (X) and every socket mirrors with its pixels — the chain
//     math just negates its angles; no hand-mirrored anchor tables.
//   • A CURVE PINNED TO AN ANCHOR: a trail of dots rides a quadratic Curve whose origin is the body's
//     tail anchor — the path swings with the body's rock because its origin is re-read per tick.
//
// Motion is slow and same-direction, and interpolation (default-on) eases every part between ticks —
// watch the joints: parent and child stay glued mid-ease because matrix lerp is linear.
//
// Like the other example hosts this instantiates SdlPlatform + Renderer in a real run, so it keeps the
// live sprite path compiling + linking on every CI platform even though CI never opens the window.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "retropp/clock.h"
#include "retropp/curve.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/transform.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 160, kViewH = 144;
constexpr int kBgMapW = 20, kBgMapH = 18;  // 20×18 8px tiles cover 160×144 exactly

// The demo's input vocabulary: the showcase toggles + the window/render dev controls.
enum class Action : std::uint8_t {
    FlipFacing, ZKeysToggle, Fullscreen, SamplingToggle, WindowScale,
};

// ── The creature's anchor tables — art-space points, named where the pixels are. ─────────────

constexpr Anchor kBodyAnchors[] = {
    {.label = "shoulder", .x = 14.0f, .y = 8.0f},  // right edge, mid — the arm's socket
    {.label = "tail",     .x = 1.5f,  .y = 9.0f},  // left edge — the trail curve's origin
};
static_assert(!findDuplicateAnchorLabel(kBodyAnchors));

constexpr Anchor kArmAnchors[] = {
    {.label = "root",  .x = 2.0f,  .y = 4.0f},  // socket end — pivots on the parent
    {.label = "elbow", .x = 13.0f, .y = 4.0f},  // far end — the next segment's socket
};
static_assert(!findDuplicateAnchorLabel(kArmAnchors));

constexpr Anchor kForearmAnchors[] = {
    {.label = "root",  .x = 2.0f,  .y = 4.0f},
    {.label = "wrist", .x = 13.0f, .y = 4.0f},
};
static_assert(!findDuplicateAnchorLabel(kForearmAnchors));

constexpr Anchor kClawAnchors[] = {
    {.label = "hinge", .x = 2.0f, .y = 4.0f},
};
static_assert(!findDuplicateAnchorLabel(kClawAnchors));

// ── The creature's art: one 32×32 indexed atlas, painted here (no asset files). ──────────────
// Cells on the 8px grid (atlas is 4 cells wide): body 16×16 at tile 0, upper arm 16×8 at tile 2,
// forearm 16×8 at tile 6, claw 8×8 at tile 8, trail dot 8×8 at tile 9.
// Index 0 = transparent hole, 1 = fill, 2 = rim, 3 = socket rivet, 4 = accent.
constexpr int kAtlasW = 32, kAtlasH = 32;

std::array<std::uint8_t, kAtlasW * kAtlasH> makeCreatureAtlas() {
    std::array<std::uint8_t, kAtlasW * kAtlasH> px{};
    auto set = [&](int x, int y, std::uint8_t v) {
        px[static_cast<std::size_t>(y) * kAtlasW + static_cast<std::size_t>(x)] = v;
    };
    auto rect = [&](int x0, int y0, int x1, int y1, std::uint8_t fill, std::uint8_t rim) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                set(x, y, (y == y0 || y == y1 || x == x0 || x == x1) ? rim : fill);
    };

    // Body (px 0,0 .. 15,15): a riveted shell with an eye so the rock and the flip read clearly.
    rect(1, 2, 14, 13, 1, 2);
    set(3, 4, 3);  set(12, 4, 3);  set(3, 11, 3);  set(12, 11, 3);   // corner rivets
    set(10, 6, 4); set(11, 6, 4);  set(10, 7, 4);  set(11, 7, 4);    // the eye — right side, so a flip shows
    set(14, 8, 3);                                                    // the shoulder socket rivet
    // Upper arm (px 16,0 .. 31,7): a beam with a rivet at each socket.
    rect(17, 2, 30, 6, 1, 2);
    set(18, 4, 3); set(29, 4, 3);
    // Forearm (px 16,8 .. 31,15): a thinner beam.
    rect(17, 11, 30, 13, 1, 2);
    set(18, 12, 3); set(29, 12, 3);
    // Claw (px 0,16 .. 7,23): a C opening away from its hinge, accent tips.
    rect(1, 17, 2, 22, 1, 2);          // the back bar (hinge side)
    rect(3, 17, 6, 18, 1, 2);          // top finger
    rect(3, 21, 6, 22, 1, 2);          // bottom finger
    set(6, 17, 4); set(6, 22, 4);      // tips
    set(2, 20, 3);                     // hinge rivet
    // Trail dot (px 8,16 .. 15,23): a 2×2 accent dot, centered.
    set(11, 19, 4); set(12, 19, 4); set(11, 20, 4); set(12, 20, 4);
    return px;
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Articulation Demo"},
        .window = {.title = "Polyrhythm — articulation: anchors + pivots + per-sprite z on one layer"}};

    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::FlipFacing,     {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::ZKeysToggle,    {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::Fullscreen,     {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
        {Action::SamplingToggle, {SDL_SCANCODE_RETURN, PadButton::Start}},
        {Action::WindowScale,    {SDL_SCANCODE_C, PadButton::FaceWest}},
    };
    platform.actions(map);

    int windowScale = config.enhancements.windowScale;

    // ── Background checkerboard (tile path) so the motion reads against a grid. ──────────────
    std::array<std::uint8_t, 8 * 8> bgPx{};
    bgPx.fill(1);
    const AtlasId bgAtlas = renderer.uploadAtlas(bgPx.data(), 8, 8).atlasId;
    const std::array<Rgba8, 2> palBgA{{{0, 0, 0}, {40, 44, 58}}};
    const std::array<Rgba8, 2> palBgB{{{0, 0, 0}, {30, 33, 44}}};
    const PaletteId bgA = renderer.uploadPalette(std::span<const Rgba8>(palBgA));
    const PaletteId bgB = renderer.uploadPalette(std::span<const Rgba8>(palBgB));
    std::vector<TileCell> bgCells(static_cast<std::size_t>(kBgMapW) * kBgMapH);
    for (int y = 0; y < kBgMapH; ++y)
        for (int x = 0; x < kBgMapW; ++x)
            bgCells[static_cast<std::size_t>(y) * kBgMapW + x] =
                TileCell{.atlas = bgAtlas, .tile = 0, .palette = ((x + y) & 1) ? bgB : bgA};

    // ── The creature's atlas + palette. ───────────────────────────────────────────────────────
    const auto creaturePx = makeCreatureAtlas();
    const AtlasId creature = renderer.uploadAtlas(creaturePx.data(), kAtlasW, kAtlasH,
                                                  TransparentIndices::GameBoy).atlasId;
    const std::array<Rgba8, 5> palCreature{{{0, 0, 0},          // 0: hole
                                            {196, 144, 84},     // 1: shell fill
                                            {92, 58, 34},       // 2: rim
                                            {58, 46, 60},       // 3: socket rivets
                                            {240, 220, 120}}};  // 4: accents (eye, tips, trail)
    const PaletteId palC = renderer.uploadPalette(std::span<const Rgba8>(palCreature));

    bool facingLeft = false;  // X: flip the whole creature — anchors mirror with the art
    bool zKeys      = true;   // Z: per-sprite z keys on/off (off = raw submission order, claw hides)

    // The chain state the render reads — recomputed whole each tick (positions round to ints there;
    // the interpolator eases them between ticks by each part's stable key).
    std::vector<Sprite> parts;      // submitted FRONT-FIRST on purpose; z keys restore the stacking
    std::vector<Sprite> trailDots;

    int tick = 0;
    loop.simTick([&](const InputState& in) {
        ++tick;
        if (in.justPressed(Action::FlipFacing)) {
            facingLeft = !facingLeft;
            std::printf("[dev] facing: %s — anchors mirror with the art; the chain only negates its "
                        "angles\n", facingLeft ? "left" : "right");
        }
        if (in.justPressed(Action::ZKeysToggle)) {
            zKeys = !zKeys;
            std::printf("[dev] per-sprite z keys: %s — %s\n", zKeys ? "ON" : "OFF",
                        zKeys ? "stacking follows z (claw in front)"
                              : "raw submission order (front-first submission stacks backwards)");
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
        if (in.justPressed(Action::SamplingToggle)) {
            const bool toBilinear = renderer.samplingMode() == SamplingMode::Nearest;
            renderer.samplingMode(toBilinear ? SamplingMode::Bilinear : SamplingMode::Nearest);
            std::printf("[dev] sampling: %s\n", toBilinear ? "bilinear" : "nearest");
        }
        if (in.justPressed(Action::WindowScale)) {
            windowScale = (windowScale >= 8) ? 1 : windowScale + 1;
            const PixelSize vp{kViewW, kViewH};
            const int eff = fitWindowScale(vp, platform.usableDisplaySize(), windowScale);
            if (!platform.window().fullscreen()) platform.window().size(PixelSize{vp.width * eff, vp.height * eff});
        }

        // ── The chain: trivial forward kinematics, game-side — walk outward, angles accumulate. ──
        const float t   = static_cast<float>(tick);
        const float dir = facingLeft ? -1.0f : 1.0f;
        const float rock     = 6.0f * std::sin(t * 0.010f) * dir;           // the body's gentle rock
        const float shoulder = rock + 30.0f * std::sin(t * 0.020f) * dir;   // each joint adds its swing
        const float elbow    = shoulder + 40.0f * std::sin(t * 0.017f + 1.3f) * dir;
        const float wrist    = elbow + 24.0f * std::sin(t * 0.050f) * dir;  // the claw's grip flutter

        Sprite body{.key = "body", .size = AssetDimensions{16, 16}, .atlas = creature, .tile = 0,
                    .palette = palC, .flipX = facingLeft};
        body.origin    = body.center(Space::Quad);      // x/y place the shell's center…
        body.pivot     = body.center(Space::Quad);      // …and it rocks about that same point
        body.x         = 76;
        body.y         = 84;
        body.transform = Transform::rotation(rock);
        body.anchors   = kBodyAnchors;

        // Each child mounts by its own socket: origin = pivot = anchor(socket, Space::Quad), so the
        // placement handle and the hinge are the SAME point (a joint). x/y then put that socket on the
        // parent's anchor in layer space (Space::Layer — the joint), and rotation about the socket is
        // rotation about the joint by construction. Both origin and pivot ride the art's flip. Rotation
        // is the accumulated angle.
        auto attach = [](Sprite& child, const char* socket, const Sprite& parent, const char* joint,
                         float degrees) {
            child.origin  = child.anchor(socket, Space::Quad);
            child.pivot   = child.anchor(socket, Space::Quad);
            const Point p = parent.anchor(joint, Space::Layer);
            child.x       = static_cast<int>(std::lround(p.x));
            child.y       = static_cast<int>(std::lround(p.y));
            child.transform = Transform::rotation(degrees);
        };

        Sprite upper{.key = "upper", .size = AssetDimensions{16, 8}, .atlas = creature, .tile = 2,
                     .palette = palC, .flipX = facingLeft};
        upper.anchors = kArmAnchors;
        attach(upper, "root", body, "shoulder", shoulder);

        Sprite fore{.key = "fore", .size = AssetDimensions{16, 8}, .atlas = creature, .tile = 6,
                    .palette = palC, .flipX = facingLeft};
        fore.anchors = kForearmAnchors;
        attach(fore, "root", upper, "elbow", elbow);

        Sprite claw{.key = "claw", .size = AssetDimensions{8, 8}, .atlas = creature, .tile = 8,
                    .palette = palC, .flipX = facingLeft};
        claw.anchors = kClawAnchors;
        attach(claw, "hinge", fore, "wrist", wrist);

        // Deliberately FRONT-FIRST submission; the z keys are what put the claw in front of the arm
        // and the arm in front of the body. Toggling them off shows the raw order stacking backward.
        claw.z  = zKeys ? 30 : 0;
        fore.z  = zKeys ? 20 : 0;
        upper.z = zKeys ? 10 : 0;
        body.z  = 0;
        parts.assign({claw, fore, upper, body});

        // ── The trail: a quadratic Curve PINNED to the tail anchor — its start point is re-read each
        // tick, so the whole path rides the body's rock. Dots sit at equal arc lengths; each places by
        // its center (origin = the dot's center, so x/y IS the curve point) and stacks BEHIND the body
        // (negative z).
        const Point tail = body.anchor("tail", Space::Layer);
        const float sway = 10.0f * std::sin(t * 0.013f);
        const Curve trail = Curve::quadratic(
            Vec2{tail.x, tail.y},
            Vec2{tail.x - dir * 18.0f, tail.y - 6.0f + sway},
            Vec2{tail.x - dir * 34.0f, tail.y + 10.0f - sway});
        const float len = trail.length();
        trailDots.clear();
        for (int i = 0; i < 5; ++i) {
            const Vec2 p = trail.atDistance(len * (static_cast<float>(i) + 1.0f) / 5.0f);
            Sprite dot{.key = std::string("dot") + std::to_string(i),
                       .z = zKeys ? -10 : 0, .size = AssetDimensions{8, 8},
                       .atlas = creature, .tile = 9, .palette = palC};
            dot.origin = dot.center(Space::Quad);  // x/y place the dot's center — the curve point itself
            dot.pivot  = dot.center(Space::Quad);  // spin center matches (the dot carries no transform)
            dot.x     = static_cast<int>(std::lround(p.x));
            dot.y     = static_cast<int>(std::lround(p.y));
            trailDots.push_back(std::move(dot));
        }
        for (const Sprite& d : trailDots) parts.push_back(d);
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();

        DrawLayer bg{.key = "bg"};
        bg.z       = 0;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles  = kBgMapW,
                                 .heightInTiles = kBgMapH,
                                 .cells         = std::span<const TileCell>(bgCells)};
        frame.layers.push_back(bg);

        // ONE sprite layer — the whole creature and its trail; stacking comes from Sprite::z alone.
        DrawLayer creatureLayer{.key = "creature"};
        creatureLayer.z       = 10;
        creatureLayer.size    = PixelSize{kViewW, kViewH};
        creatureLayer.content = SpriteContent{.sprites = std::span<const Sprite>(parts)};
        frame.layers.push_back(creatureLayer);

        renderer.renderFrame(frame);
    });

    std::printf("Articulation showcase — a crab-bot chained by anchor re-feeds on ONE sprite layer: "
                "each joint pivots on its socket, the claw stacks by per-sprite z, and a curve trail "
                "rides the tail anchor.\n");
    std::printf("[dev] X = flip facing (anchors mirror with the art), Z = per-sprite z keys on/off "
                "(off = submission order, the claw hides), Backspace = fullscreen, Return = sampling, "
                "C = window scale.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}

// Sprite-blend demo — a runnable host for per-sprite `Sprite::blend`. A hero disc bobs across a scrolling
// textured scene carrying two blended decals, and a probe disc lets you cycle one sprite through every
// blend mode over the same moving background:
//
//   • SHADOW (Multiply, alpha 0.7) — a soft grey decal rides at the hero's feet, one z below it. Multiply
//     darkens the scene under it, most where the decal is darkest (its centre), so it reads as a shadow
//     that tracks the hero over whatever tile it crosses.
//   • FLARE (Add) — a warm radial decal pulses one z above the hero. Add lifts the scene toward light,
//     brightest at the decal's centre — a glow that never darkens.
//   • PROBE — a single disc mid-screen. Press A to cycle its blend (Normal → Multiply → Add → Screen →
//     Subtract → Half → HalfSubtract) and watch the SAME sprite grade the scene beneath it differently at
//     each mode.
//
// Every sprite here rides one ordinary sprite layer over the scene, so each grades the accumulated scene
// beneath it (the container rule for a direct-to-accumulator layer).
//
// Motion advances on the sim tick (RunLoop::tickCount), so it runs the same on any display. A = cycle the
// probe's blend mode; Backspace = fullscreen; close to quit.
//
// Modes:
//   (no args)   the window
//   --verify    headless, on a GPU device with no window: draws the demo's own first frame with the probe in
//               each of the seven modes and requires every pixel the probe covers to match the CPU mirror,
//               applyBlendMode(scene, probe color, mode), within 2 8-bit steps, and every pixel off the probe
//               to equal the frame drawn without it. Exits nonzero on any miss (CI runs this on every
//               platform); on Windows on ARM, whose CI runner has no GPU device, exits 77 — a counted skip.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/image.h"  // TransparentIndices
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/postprocess.h"  // applyBlendMode
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/viewport.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;  // 20×18 tiles cover the 160×144 viewport
constexpr int kProbeX = 112;           // the probe's fixed column

struct ModeEntry {
    BlendMode   mode;
    const char* name;
};
constexpr std::array<ModeEntry, 7> kModes{{{BlendMode::Normal, "Normal (replace)"},
                                           {BlendMode::Multiply, "Multiply (shadow)"},
                                           {BlendMode::Add, "Add (glow)"},
                                           {BlendMode::Screen, "Screen (bloom)"},
                                           {BlendMode::Subtract, "Subtract"},
                                           {BlendMode::Half, "Half"},
                                           {BlendMode::HalfSubtract, "HalfSubtract"}}};

enum class Action : std::uint8_t { CycleMode, Fullscreen };

// A 16×16 solid disc: index 1 inside radius 7, index 0 (a GameBoy hole) outside — a round, opaque sprite.
[[nodiscard]] std::array<std::uint8_t, 16 * 16> discArt() {
    std::array<std::uint8_t, 16 * 16> a{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const float dx = x - 7.5f, dy = y - 7.5f;
            a[static_cast<std::size_t>(y) * 16 + x] = (dx * dx + dy * dy <= 7.0f * 7.0f) ? 1 : 0;
        }
    return a;
}

// A 16×16 radial falloff: index 3 at the centre grading out to 1 at the rim, 0 (hole) beyond — three
// concentric bands. Paired with a dark-centre palette it is a Multiply shadow; with a bright-centre one an
// Add flare (the SAME art, the owning sprite's blend decides which).
[[nodiscard]] std::array<std::uint8_t, 16 * 16> softArt() {
    std::array<std::uint8_t, 16 * 16> a{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const float d = std::sqrt((x - 7.5f) * (x - 7.5f) + (y - 7.5f) * (y - 7.5f));
            std::uint8_t idx = 0;                         // outside → hole
            if (d < 2.5f)      idx = 3;                   // centre
            else if (d < 5.0f) idx = 2;                   // mid
            else if (d < 7.5f) idx = 1;                   // rim
            a[static_cast<std::size_t>(y) * 16 + x] = idx;
        }
    return a;
}

// Everything the frame draws from: the scene's sheet and cells, the disc and soft sheets, and their palettes.
struct Art {
    AtlasId               sceneAtlas{};
    PaletteId             scenePalId{};
    std::vector<TileCell> sceneCells;
    AtlasId               discAtlas{};
    AtlasId               softAtlas{};
    PaletteId             heroPalId{};
    PaletteId             probePalId{};
    PaletteId             shadowPalId{};
    PaletteId             flarePalId{};
};

[[nodiscard]] Art uploadArt(Renderer& renderer) {
    Art art;

    // The opaque scene the sprites grade over: a soft four-tone diagonal pattern, muted and close in value so
    // the blends read against a calm backdrop rather than a busy one.
    std::array<std::uint8_t, 64> sceneArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            sceneArt[static_cast<std::size_t>(y) * 8 + x] = static_cast<std::uint8_t>(((x + y) / 2) % 4);
    art.sceneAtlas = renderer.uploadAtlas(sceneArt.data(), 8, 8).atlasId;  // opaque (default None)
    const std::array<Rgba8, 4> scenePal{{{150, 158, 150}, {150, 150, 162}, {162, 156, 146}, {144, 156, 156}}};
    art.scenePalId = renderer.uploadPalette(std::span<const Rgba8>(scenePal));
    art.sceneCells.assign(static_cast<std::size_t>(kMapW) * kMapH,
                          TileCell{.atlas = art.sceneAtlas, .tile = 0, .palette = art.scenePalId});

    // Disc + soft sheets (index 0 = a GameBoy hole on both, so the round silhouette is the blend's mask).
    const auto disc = discArt();
    const auto soft = softArt();
    art.discAtlas = renderer.uploadAtlas(disc.data(), 16, 16, TransparentIndices::GameBoy).atlasId;
    art.softAtlas = renderer.uploadAtlas(soft.data(), 16, 16, TransparentIndices::GameBoy).atlasId;

    // Palettes. Disc index 1 is the body color; soft indices 1..3 are the rim→center falloff.
    const std::array<Rgba8, 2> heroPal{{{0, 0, 0}, {240, 240, 255}}};    // a bright opaque hero
    const std::array<Rgba8, 2> probePal{{{0, 0, 0}, {130, 170, 225}}};   // a mid blue probe body
    // Shadow (Multiply): darkest at the center so Multiply bites hardest there, lightest at the rim.
    const std::array<Rgba8, 4> shadowPal{{{0, 0, 0}, {150, 150, 160}, {90, 90, 100}, {45, 45, 55}}};
    // Flare (Add): brightest at the center so Add lifts hardest there, dim at the rim.
    const std::array<Rgba8, 4> flarePal{{{0, 0, 0}, {24, 18, 6}, {84, 66, 22}, {168, 132, 52}}};
    art.heroPalId   = renderer.uploadPalette(std::span<const Rgba8>(heroPal));
    art.probePalId  = renderer.uploadPalette(std::span<const Rgba8>(probePal));
    art.shadowPalId = renderer.uploadPalette(std::span<const Rgba8>(shadowPal));
    art.flarePalId  = renderer.uploadPalette(std::span<const Rgba8>(flarePal));
    return art;
}

// The frame at tick `t`: the drifting scene, the hero with its shadow and flare, and — when `withProbe` — the
// probe disc under `probeMode`. `sprites` holds the sprite layer's span and outlives the frame.
void buildFrame(FrameDrawState& frame, std::vector<Sprite>& sprites, const Art& art, double t,
                BlendMode probeMode, bool withProbe) {
    frame.layers.clear();

    // The opaque scene, drifting slowly sideways so the moving decals cross a little variety.
    DrawLayer scene{.key = "scene"};
    scene.z       = 0;
    scene.size    = PixelSize{kViewW, kViewH};
    scene.scroll  = LayerScroll{static_cast<int>(t) / 16 % (kMapW * 8), 0};
    scene.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                .cells = std::span<const TileCell>(art.sceneCells)};
    frame.layers.push_back(scene);

    // The hero drifts gently left↔right with a shallow bob; its shadow/flare track it. Integer placement,
    // motion off the tick.
    const int   heroX  = 24 + static_cast<int>(44.0 + 44.0 * std::sin(t * 0.010));
    const int   heroY  = 64 + static_cast<int>(3.0 * std::sin(t * 0.024));
    const float flareA = 0.5f;  // a steady glow

    sprites.clear();
    // Shadow decal — Multiply, below the hero (z 0), offset to its feet, translucent.
    sprites.push_back(Sprite{.key = "shadow", .x = heroX, .y = heroY + 10, .z = 0,
                             .size = AssetDimensions::Snes16x16, .atlas = art.softAtlas, .tile = 0,
                             .palette = art.shadowPalId, .alpha = 0.7f, .blend = BlendMode::Multiply});
    // Hero — opaque, Normal, over the shadow (z 1).
    sprites.push_back(Sprite{.key = "hero", .x = heroX, .y = heroY, .z = 1,
                             .size = AssetDimensions::Snes16x16, .atlas = art.discAtlas, .tile = 0,
                             .palette = art.heroPalId});
    // Flare — Add, above the hero (z 2), alpha pulsing.
    sprites.push_back(Sprite{.key = "flare", .x = heroX, .y = heroY - 8, .z = 2,
                             .size = AssetDimensions::Snes16x16, .atlas = art.softAtlas, .tile = 0,
                             .palette = art.flarePalId, .alpha = flareA, .blend = BlendMode::Add});
    // Probe — one disc at a fixed spot, cycling its blend so you can compare modes over the moving scene.
    if (withProbe)
        sprites.push_back(Sprite{.key = "probe", .x = kProbeX, .y = heroY, .z = 1,
                                 .size = AssetDimensions::Snes16x16, .atlas = art.discAtlas, .tile = 0,
                                 .palette = art.probePalId, .blend = probeMode});

    DrawLayer actors{.key = "actors"};
    actors.z       = 10;
    actors.size    = PixelSize{kViewW, kViewH};
    actors.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
    frame.layers.push_back(actors);
}

// ── --verify ──────────────────────────────────────────────────────────────────────────────────────

[[nodiscard]] Vec4 norm(Rgba8 c) {
    return Vec4{static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
                static_cast<float>(c.b) / 255.0f, static_cast<float>(c.a) / 255.0f};
}
[[nodiscard]] std::uint8_t quant(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}
[[nodiscard]] bool exactEq(Rgba8 a, Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
[[nodiscard]] int  chDelta(Rgba8 a, Rgba8 b) {
    return std::max({std::abs(int(a.r) - int(b.r)), std::abs(int(a.g) - int(b.g)),
                     std::abs(int(a.b) - int(b.b)), std::abs(int(a.a) - int(b.a))});
}

int runVerify() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("sprite-blend verify: SDL_Init(SDL_INIT_VIDEO) failed: %s\n", SDL_GetError());
        return kDeviceOptional ? 77 : 1;
    }
    SDL_GPUDevice* device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
        /*debug_mode=*/false, /*name=*/nullptr);
    if (!device) {
        std::printf("sprite-blend verify: no GPU device (%s)%s\n", SDL_GetError(),
                    kDeviceOptional ? " — skipped on this platform" : "");
        SDL_Quit();
        return kDeviceOptional ? 77 : 1;
    }

    int        failures = 0;
    const auto check    = [&](bool ok, const std::string& what) {
        if (!ok) {
            std::printf("  FAIL  %s\n", what.c_str());
            ++failures;
        }
    };

    {
        Renderer r{device, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
        r.automaticInterpolation(false);
        const Art           art = uploadArt(r);
        FrameDrawState      frame;
        std::vector<Sprite> sprites;
        constexpr double    kT = 0.0;  // the first frame: the hero sits at x 68–83, clear of the probe

        const auto capture = [&](BlendMode mode, bool withProbe) {
            buildFrame(frame, sprites, art, kT, mode, withProbe);
            return r.captureViewport(frame);
        };
        const std::vector<Rgba8> B = capture(BlendMode::Normal, false);  // the frame without the probe
        const std::vector<Rgba8> N = capture(BlendMode::Normal, true);   // the probe's own color where it covers
        check(N.size() == B.size(), "the two captures are one size");

        // The probe's pixels are the ones a Normal probe changed; they all lie in the probe's own columns, so
        // the hero, its shadow and its flare are the same in every capture and cancel out.
        int covered = 0, strays = 0;
        for (std::size_t i = 0; i < B.size(); ++i) {
            if (exactEq(N[i], B[i])) continue;
            ++covered;
            const int x = static_cast<int>(i) % kViewW;
            if (x < kProbeX || x >= kProbeX + 16) ++strays;
        }
        check(covered > 0, "the probe covers pixels");
        check(strays == 0, "every pixel the probe changes lies in its columns (" + std::to_string(strays) +
                               " outside)");

        for (const ModeEntry& m : kModes) {
            const std::vector<Rgba8> G = capture(m.mode, true);
            int mismatched = 0, leaked = 0, graded = 0;
            for (std::size_t i = 0; i < B.size(); ++i) {
                if (!exactEq(N[i], B[i])) {
                    const Vec4  src   = norm(N[i]);
                    const Vec4  want4 = applyBlendMode(norm(B[i]), Vec4{src.x, src.y, src.z, 1.0f}, m.mode);
                    const Rgba8 want{quant(want4.x), quant(want4.y), quant(want4.z), quant(want4.w)};
                    if (chDelta(G[i], want) > 2) ++mismatched;
                    else if (chDelta(G[i], B[i]) > 2) ++graded;
                } else if (!exactEq(G[i], B[i])) {
                    ++leaked;
                }
            }
            check(mismatched == 0, std::string(m.name) + ": " + std::to_string(mismatched) +
                                       " covered pixels differ from the mirror by more than 2 steps");
            check(leaked == 0, std::string(m.name) + ": " + std::to_string(leaked) + " pixels changed off the probe");
            if (m.mode != BlendMode::Normal)
                check(graded > 0, std::string(m.name) + ": the probe moved no pixel off the scene");
        }

        std::printf("sprite-blend verify: the probe in %zu modes over %d pixels; %s\n", kModes.size(), covered,
                    failures == 0 ? "PASS" : "FAIL");
    }

    SDL_DestroyGPUDevice(device);
    SDL_Quit();
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--verify") == 0) {
        return runVerify();
    }

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Sprite Blend Demo"},
        .window   = {.title = "Polyrhythm — sprite-blend demo (per-sprite shadow / flare / probe)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::CycleMode, {SDL_SCANCODE_A, PadButton::FaceSouth}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    const Art art = uploadArt(renderer);

    int  modeIdx = 0;
    auto announce = [&]() {
        std::printf("sprite-blend demo — probe blend: %s. The hero carries a Multiply shadow and an Add "
                    "flare; the probe (right of centre) cycles every mode over the same scene. A = next "
                    "mode; Backspace = fullscreen; close to quit.\n",
                    kModes[static_cast<std::size_t>(modeIdx)].name);
    };
    announce();

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::CycleMode)) {
            modeIdx = (modeIdx + 1) % static_cast<int>(kModes.size());
            announce();
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
    });

    FrameDrawState      frame;
    std::vector<Sprite> sprites;
    loop.renderLoop([&]() {
        buildFrame(frame, sprites, art, static_cast<double>(loop.tickCount()),
                   kModes[static_cast<std::size_t>(modeIdx)].mode, /*withProbe=*/true);
        renderer.renderFrame(frame);
    });

    WindowedHost host{loop, platform};
    host.run();
    return 0;
}

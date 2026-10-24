#include <gtest/gtest.h>

#include "retropp/animation.h"      // AnimationPlayer::defaultTiming
#include "retropp/path_walker.h"    // PathWalker::defaultTiming
#include "retropp/sprite_path.h"    // SpritePath::defaultTiming
#include "retropp/tween.h"          // TweenPlayer<T>::defaultTiming (float/Vec2/Vec3/Vec4)
#include "retropp/vibration.h"      // VibrationPlayer::defaultTiming
#include "retropp/engine_config.h"
#include "retropp/renderer.h"       // Renderer::defaultViewport (read only — no GPU construction)
#include "retropp/run_loop.h"       // RunLoop::defaultTiming + the inherited-ctor check
#include "retropp/save_store.h"     // SaveStore::defaultIdentity
#include "manual_clock.h"           // retropp::test::ManualClock

namespace retropp {
namespace {

// EngineConfig is the single startup bundle. Its defining property: a default-constructed
// config reproduces the faithful Game Boy Color baseline, and every field is independently
// overridable via designated initializers. All checks are headless value-type comparisons.

TEST(EngineConfig, DefaultReproducesTheFaithfulGameBoyColorBaseline) {
    // const, not constexpr: WindowConfig holds a std::string, so a constexpr EngineConfig is
    // not portable (libstdc++ rejects a constexpr object with a std::string member). The
    // defaults are runtime values; that is all this asserts.
    const EngineConfig cfg{};

    // Internal viewport: the original 160×144 (ViewportResolution has no operator==, so compare
    // fields against the GameBoyColor preset).
    EXPECT_EQ(cfg.viewport.width, ViewportResolution::GameBoyColor.width);
    EXPECT_EQ(cfg.viewport.height, ViewportResolution::GameBoyColor.height);
    EXPECT_EQ(cfg.viewport.width, 160);
    EXPECT_EQ(cfg.viewport.height, 144);

    // Render timing: the GBC cadence (TimingProfile has a defaulted operator==).
    EXPECT_EQ(cfg.timing, TimingProfile::GameBoyColor);

    // Enhancements: faithful sampling/fullscreen baseline + the factory window scale.
    EXPECT_FALSE(cfg.enhancements.fullscreen);
    EXPECT_EQ(cfg.enhancements.windowScale, 4);  // window = 4× viewport (clamped to the display)
    EXPECT_EQ(cfg.enhancements.sampling, SamplingMode::Nearest);

    // Window: the default title (the size derives from windowScale × viewport, not WindowConfig).
    EXPECT_EQ(cfg.window.title, "Polyrhythm");

    // Evaluation grid: Viewport (crisp) — the analytic paths evaluate on the viewport grid by default.
    EXPECT_EQ(cfg.evaluationGrid, EvaluationGrid::Viewport);
}

TEST(EngineConfig, EachFieldIsIndependentlyOverridable) {
    const EngineConfig cfg{
        .window       = {.title = "Demo"},
        .viewport     = ViewportResolution::Snes,
        .timing       = TimingProfile::GameBoy,
        .enhancements = {.windowScale = 6, .fullscreen = true},
    };

    EXPECT_EQ(cfg.window.title, "Demo");

    EXPECT_EQ(cfg.viewport.width, 256);   // SNES internal resolution
    EXPECT_EQ(cfg.viewport.height, 224);

    EXPECT_EQ(cfg.timing, TimingProfile::GameBoy);

    EXPECT_TRUE(cfg.enhancements.fullscreen);
    EXPECT_EQ(cfg.enhancements.windowScale, 6);
}

TEST(EngineConfig, PartialOverrideLeavesOtherFieldsAtTheBaseline) {
    // Overriding only the timing must not disturb viewport / enhancements / window.
    const EngineConfig cfg{.timing = TimingProfile::GameBoy};

    EXPECT_EQ(cfg.timing, TimingProfile::GameBoy);
    EXPECT_EQ(cfg.viewport.width, 160);
    EXPECT_FALSE(cfg.enhancements.fullscreen);
    EXPECT_EQ(cfg.window.title, "Polyrhythm");
}

TEST(WindowConfig, DefaultHasTheConventionalTitle) {
    const WindowConfig w{};  // const, not constexpr — see the note above (std::string member)
    EXPECT_EQ(w.title, "Polyrhythm");  // the window size derives from windowScale × viewport, not WindowConfig
}

TEST(EnhancementToggles, DefaultsAreFactory) {
    constexpr EnhancementToggles e{};
    EXPECT_EQ(e.windowScale, 4);   // factory window scale
    EXPECT_FALSE(e.fullscreen);
    EXPECT_EQ(e.sampling, SamplingMode::Nearest);
}

// ── EngineConfig::setActive fan-out ──────────────────────────────────────────────
// setActive() requires an identity, assigns the active config, AND fans its fields into the
// per-type SDL-free static defaults so bare ctors inherit them. These statics are process-global,
// so the fixture restores the faithful Game Boy Color baseline after every case — case ordering
// can't leak state. (The baseline config still carries an identity: setActive refuses an
// anonymous config, so "default" here means every field but the required identity.)
class EngineConfigActive : public ::testing::Test {
protected:
    static EngineConfig identified() {
        EngineConfig cfg{};
        cfg.identity = {.organization = "Retro++", .application = "EngineConfigTest"};
        return cfg;
    }
    void TearDown() override { EngineConfig::setActive(identified()); }
};

TEST_F(EngineConfigActive, SetActiveRefusesAnAnonymousConfig) {
    // Every program declares who it is before it starts — no platform lets a project exist
    // without an identity, and the engine enforces the same at its one startup call.
    EXPECT_THROW(EngineConfig::setActive(EngineConfig{}), std::invalid_argument);
    EngineConfig halfSet{};
    halfSet.identity = {.organization = "Retro++", .application = ""};
    EXPECT_THROW(EngineConfig::setActive(halfSet), std::invalid_argument);
    halfSet.identity = {.organization = "", .application = "EngineConfigTest"};
    EXPECT_THROW(EngineConfig::setActive(halfSet), std::invalid_argument);
}

TEST_F(EngineConfigActive, SetActiveFansOutToThePerTypeDefaults) {
    EngineConfig cfg = identified();
    cfg.timing                = TimingProfile{TickPeriodNs::Hz60};  // non-default cadence
    cfg.viewport              = ViewportResolution::Snes;           // non-default resolution
    cfg.enhancements.sampling = SamplingMode::Bilinear;            // non-default sampling
    cfg.evaluationGrid        = EvaluationGrid::Output;            // non-default evaluation grid
    EngineConfig::setActive(cfg);

    // The active config holds the assigned values.
    EXPECT_EQ(EngineConfig::active.timing, cfg.timing);
    EXPECT_EQ(EngineConfig::active.viewport.width, cfg.viewport.width);
    EXPECT_EQ(EngineConfig::active.viewport.height, cfg.viewport.height);

    // The per-type SDL-free defaults were fanned out (ViewportResolution has no operator==).
    EXPECT_EQ(RunLoop::defaultTiming, cfg.timing);
    EXPECT_EQ(Renderer::defaultViewport.width, cfg.viewport.width);
    EXPECT_EQ(Renderer::defaultViewport.height, cfg.viewport.height);
    EXPECT_EQ(Renderer::defaultSamplingMode, cfg.enhancements.sampling);  // sampling rides the fan-out
    EXPECT_EQ(Renderer::defaultEvaluationGrid, cfg.evaluationGrid);       // evaluation grid rides it too
    EXPECT_EQ(AnimationPlayer::defaultTiming, cfg.timing);
    // Every interpolable tween type's cadence is fanned out too (per-T template static).
    EXPECT_EQ(TweenPlayer<float>::defaultTiming, cfg.timing);
    EXPECT_EQ(TweenPlayer<Vec2>::defaultTiming, cfg.timing);
    EXPECT_EQ(TweenPlayer<Vec3>::defaultTiming, cfg.timing);
    EXPECT_EQ(TweenPlayer<Vec4>::defaultTiming, cfg.timing);
    EXPECT_EQ(PathWalker::defaultTiming, cfg.timing);  // path-walker cadence rides the fan-out too
    EXPECT_EQ(SpritePath::defaultTiming, cfg.timing);  // sprite-path cadence rides it too
    EXPECT_EQ(VibrationPlayer::defaultTiming, cfg.timing);  // vibration cadence rides it too
    // The application identity rides the fan-out too (SaveStore resolves its directory from it).
    EXPECT_EQ(SaveStore::defaultIdentity.organization, cfg.identity.organization);
    EXPECT_EQ(SaveStore::defaultIdentity.application, cfg.identity.application);
}

TEST_F(EngineConfigActive, BareRunLoopInheritsTheFannedTimingAndExplicitOverrideStillWins) {
    EngineConfig cfg = identified();
    cfg.timing = TimingProfile{TickPeriodNs::Hz60};
    EngineConfig::setActive(cfg);

    test::ManualClock clock;
    // Inherited: a bare RunLoop picks up the fanned default through its ctor default argument.
    RunLoop inherited{clock};
    EXPECT_EQ(inherited.timing(), cfg.timing);

    // Override: an explicitly-passed profile still wins over the default.
    RunLoop overridden{clock, TimingProfile::GameBoyColor};
    EXPECT_EQ(overridden.timing(), TimingProfile::GameBoyColor);
}

TEST_F(EngineConfigActive, FaithfulDefaultIsPreservedByADefaultConfig) {
    // A default config (identity aside — setActive requires one) fans the GBC values back into
    // every per-type default, proving the defaults themselves reproduce the original behavior.
    EngineConfig::setActive(identified());

    EXPECT_EQ(RunLoop::defaultTiming, TimingProfile::GameBoyColor);
    EXPECT_EQ(Renderer::defaultViewport.width, ViewportResolution::GameBoyColor.width);
    EXPECT_EQ(Renderer::defaultViewport.height, ViewportResolution::GameBoyColor.height);
    EXPECT_EQ(Renderer::defaultSamplingMode, SamplingMode::Nearest);  // faithful crisp-pixel default
    EXPECT_EQ(Renderer::defaultEvaluationGrid, EvaluationGrid::Viewport);  // crisp evaluation default
    EXPECT_EQ(AnimationPlayer::defaultTiming, TimingProfile::GameBoyColor);
    EXPECT_EQ(TweenPlayer<float>::defaultTiming, TimingProfile::GameBoyColor);
    EXPECT_EQ(TweenPlayer<Vec3>::defaultTiming, TimingProfile::GameBoyColor);
    EXPECT_EQ(PathWalker::defaultTiming, TimingProfile::GameBoyColor);
    EXPECT_EQ(SpritePath::defaultTiming, TimingProfile::GameBoyColor);
    EXPECT_EQ(VibrationPlayer::defaultTiming, TimingProfile::GameBoyColor);
}

}  // namespace
}  // namespace retropp

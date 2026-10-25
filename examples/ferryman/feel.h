#pragma once

// Ferryman — the GAME-FEEL layer: the "juice" over the plain sim, plus the shared animation
// cursors. All of it is game-owned state the engine never ticks: this layer advances the tween
// and animation cursors each sim tick and the renderer reads the resolved values into draw state:
//   • score popups    — a "+N" that rises, shrinks, AND fades (per-sprite alpha) where the points
//                       landed: the banked haul at the sanctuary, a foiled abduction, a downed
//                       enemy.
//   • screen shake    — a gentle, brief RowDisplacement wobble on FERRY DEATH only (the one real
//                       down-beat; the frequent events never shake).
//   • the beam breath — the abductor's tractor beam pulses via a looping tween the renderer
//                       reads only while a beam is lit.
//   • the home glow   — the sanctuary-edge glow breathes, scaled by how many souls ride the deck
//                       (the greed dial made visible).
//   • the WAVE banner — the arcade round card: "CROSSING N" slides in under an OutBack overshoot,
//                       holds, and departs — restarted whenever the wave number changes.
//   • pooled booms    — 3-frame explosion clips played single() per instance, reaped when done.
//   • the cursors     — AnimationPlayers over the assets' shared clips: thruster flicker, the
//                       three colonist bobs, abductor wings, mutant pulse, the beacon glow and
//                       water shimmer (palette cycling), and the running-light metronome.

#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

#include "retropp/animation.h"   // AnimationPlayer — the shared clip cursors
#include "retropp/draw_state.h"  // ScreenSpaceEffect
#include "retropp/tween.h"       // Tween / TweenPlayer / PlaybackMode

#include "assets.h"
#include "game.h"

namespace ferryman {

// A floating "+N" spawned where points landed. `progress` runs 0→1 over the popup's life; the
// renderer derives the rise, the shrink, and the per-sprite alpha fade from it.
struct ScorePopup {
    float                       x = 0.0f;
    float                       y = 0.0f;
    int                         points = 0;
    int                         id = 0;      // per-spawn identity — the stable key its glyphs use
    retropp::TweenPlayer<float> progress{};  // 0→1, Single; points at the shared popup tween
};

// A pooled explosion instance: position + identity + its own clip cursor.
struct Boom {
    float                    x = 0.0f;
    float                    y = 0.0f;
    int                      id = 0;
    retropp::AnimationPlayer player{};
};

class FerrymanFeel {
public:
    // Wires the animation cursors to the assets' shared clips (they must outlive this object —
    // both live for the whole program in main).
    explicit FerrymanFeel(const FerrymanAssets& assets);

    // React to a tick's game events: spawn popups (Bank / Foil / EnemyDown), booms
    // (FerryDeath / EnemyDown / Foil), kick the shake (FerryDeath).
    void onEvent(const GameEvent& e);

    // Advance every cursor one tick; watch the wave number for the round card; reap the
    // finished popups and booms. Call once per tick.
    void tick(const FerrymanGame& game);

    // ── Read by the renderer ───────────────────────────────────────────────────────────────────
    [[nodiscard]] const std::vector<ScorePopup>& popups() const { return popups_; }
    [[nodiscard]] const std::vector<Boom>&       booms() const { return booms_; }
    [[nodiscard]] const retropp::AnimationFrame& thrusterFrame() const { return thruster_.current(); }
    [[nodiscard]] const retropp::AnimationFrame& bobFrame(int look) const {
        return bobs_[static_cast<std::size_t>(look)].current();
    }
    [[nodiscard]] const retropp::AnimationFrame& wingsFrame() const { return wings_.current(); }
    [[nodiscard]] const retropp::AnimationFrame& pulseFrame() const { return pulse_.current(); }
    // The beacon's current glow phase (palette animation: the renderer reads .palette).
    [[nodiscard]] const retropp::AnimationFrame& beaconFrame() const { return beacon_.current(); }
    // The SEA's animation: the frame's index picks each water variant's phase slot (the swell
    // rolls, the dashes march) and its palette carries the a/b breathing over it.
    [[nodiscard]] const retropp::AnimationFrame& waterFrame() const { return water_.current(); }
    [[nodiscard]] std::size_t waterPhase() const { return water_.currentIndex(); }
    // The running-light metronome: which blink phase (0/1) every livery is in right now.
    [[nodiscard]] std::size_t lightsPhase() const { return lights_.currentIndex(); }
    // The beam's breathing alpha (read only while an abductor's beam is lit).
    [[nodiscard]] float beamAlpha() const { return beam_.value(); }
    // The sanctuary glow's breath (the renderer scales it by the deck's load).
    [[nodiscard]] float glowAlpha() const { return glow_.value(); }
    // The PRESS ENTER prompt's breath — the classic attract-mode pulse, via per-sprite alpha.
    [[nodiscard]] float promptAlpha() const { return prompt_.value(); }
    // The title's stadium-wave phase (radians, tick-advanced): each FERRYMAN glyph scales by
    // sin(phase − letterIndex · offset), so a crest rolls across the word.
    [[nodiscard]] float titleWavePhase() const { return wavePhase_; }
    // The mutant's reality-warp ripple phase (radians, tick-advanced) — the custom wake shader
    // emanates its rings off this.
    [[nodiscard]] float warpPhase() const { return warpPhase_; }
    // A mutant's path history (newest at the front), sampled per tick — the renderer traces the
    // comet-tail warp along it. Null if that mutant has no trail yet.
    [[nodiscard]] const std::deque<retropp::Vec2>* mutantTrail(int id) const {
        const auto it = mutantTrails_.find(id);
        return it == mutantTrails_.end() ? nullptr : &it->second;
    }
    // The shake as a frame-level effect, or nullopt while idle (no post-process pass when quiet).
    [[nodiscard]] std::optional<retropp::ScreenSpaceEffect> screenShake() const;
    // The round card: live while its track plays. bannerX01 is the card center's x as a fraction
    // of the viewport (−0.35 offscreen-left → 0.5 center → 1.35 offscreen-right).
    [[nodiscard]] bool  bannerActive() const { return !banner_.finished(); }
    [[nodiscard]] float bannerX01() const { return banner_.value(); }
    [[nodiscard]] int   bannerWave() const { return bannerWave_; }

    static constexpr std::uint64_t kPopupLifeMs = 700;  // popup rise/shrink/fade duration

private:
    const FerrymanAssets&       assets_;
    retropp::TweenPlayer<float> shakeAmp_{};  // amplitude: gentle → 0
    bool  shakeActive_ = false;
    float shakePhase_  = 0.0f;  // advances while a shake runs, for the wobble

    float wavePhase_ = 0.0f;  // the title wave (advances every tick; only the title reads it)
    float warpPhase_ = 0.0f;  // the mutant wake's refraction phase (advances every tick)
    // Per-mutant path history (id → newest-first samples), sampled each tick for the comet tail.
    std::unordered_map<int, std::deque<retropp::Vec2>> mutantTrails_;
    int   bannerWave_ = 0;    // which wave the round card announces
    int   lastWave_   = 0;    // watches game.wave for the restart

    retropp::TweenPlayer<float> beam_{};     // the tractor beam's breath (loops forever)
    retropp::TweenPlayer<float> glow_{};     // the sanctuary glow's breath (loops forever)
    retropp::TweenPlayer<float> prompt_{};   // the PRESS ENTER pulse (loops forever)
    retropp::TweenPlayer<float> banner_{};   // the round card's slide (Single per wave)
    retropp::AnimationPlayer    thruster_{};
    std::array<retropp::AnimationPlayer, 3> bobs_{};
    retropp::AnimationPlayer    wings_{};
    retropp::AnimationPlayer    pulse_{};
    retropp::AnimationPlayer    beacon_{};
    retropp::AnimationPlayer    water_{};
    retropp::AnimationPlayer    lights_{};

    std::vector<ScorePopup> popups_;
    std::vector<Boom>       booms_;
    int                     nextSpawnId_ = 1;  // monotonic id shared by popups + booms
};

}  // namespace ferryman

#pragma once

// Polterball — the GAME-FEEL layer: the "juice" over the plain sim, plus the two shared animation
// cursors. All of it is game-owned state the engine never ticks: this layer advances the tween and
// animation cursors each sim tick and the renderer reads the resolved values into draw state.
//   • score popups   — a "+N" that rises and shrinks away where a ghost was smashed.
//   • paddle squash  — the paddle dips its height briefly on a bounce (Tween → scaleY).
//   • screen shake   — a gentle, brief RowDisplacement wobble on a LOST ball only (swallowed or
//                      dropped — the rare down-beats, never the constant events).
//   • the pulse + the walk — AnimationPlayers over the assets' shared clips: the power pellet's
//                      palette pulse and the ghosts' two-frame skirt wave. Presentation state, so it
//                      lives here, not in the sim.

#include <cstdint>
#include <optional>
#include <vector>

#include "retropp/animation.h"   // AnimationPlayer — the pulse + skirt cursors
#include "retropp/draw_state.h"  // ScreenSpaceEffect
#include "retropp/tween.h"       // Tween / TweenPlayer / PlaybackMode

#include "assets.h"
#include "game.h"

namespace polter {

// A floating "+N" spawned where a ghost went down. `progress` runs 0→1 over the popup's life; the
// renderer derives the rise and the shrink from it.
struct ScorePopup {
    float                       x = 0.0f;  // spawn origin (the ghost's center), viewport px
    float                       y = 0.0f;
    int                         points = 0;
    int                         id = 0;    // per-spawn identity — the stable key its glyph sprites use
    retropp::TweenPlayer<float> progress{};  // 0→1, Single; points at the shared popup tween
};

class PolterFeel {
public:
    // Wires the animation cursors to the assets' shared clips (they must outlive this object —
    // both live for the whole program in main).
    explicit PolterFeel(const PolterAssets& assets);

    // React to a tick's game events: spawn a popup (GhostDown), kick the squash (PaddleBounce),
    // kick the shake (BallSwallowed / BallLost).
    void onEvent(const GameEvent& e);

    // Advance every cursor one tick and reap finished popups. Call once per tick.
    void tick();

    // ── Read by the renderer ───────────────────────────────────────────────────────────────────
    [[nodiscard]] float paddleScaleY() const { return paddle_.value(); }  // ~1.0 idle, dips on a bounce
    [[nodiscard]] const std::vector<ScorePopup>& popups() const { return popups_; }
    // The power pellet's current pulse frame (palette cycling: the palette changes, the art holds).
    [[nodiscard]] const retropp::AnimationFrame& powerFrame() const { return power_.current(); }
    // Which skirt frame the ghosts are on (0/1) — the renderer maps it to S_GHOST_A / S_GHOST_B.
    [[nodiscard]] std::size_t ghostStep() const { return walk_.currentIndex(); }
    // The shake as a frame-level effect, or nullopt while idle (no post-process pass when quiet).
    [[nodiscard]] std::optional<retropp::ScreenSpaceEffect> screenShake() const;

    static constexpr std::uint64_t kPopupLifeMs = 600;  // popup rise/shrink duration

private:
    retropp::TweenPlayer<float> paddle_{};    // scaleY: 1 → dip → 1
    retropp::TweenPlayer<float> shakeAmp_{};  // amplitude: gentle → 0
    bool  shakeActive_ = false;
    float shakePhase_  = 0.0f;  // advances while a shake runs, for the wobble

    retropp::AnimationPlayer power_{};  // the pellet pulse (loops forever)
    retropp::AnimationPlayer walk_{};   // the skirt wave (loops forever)

    std::vector<ScorePopup> popups_;
    int                     nextPopupId_ = 1;  // monotonic id handed to each spawned popup
};

}  // namespace polter

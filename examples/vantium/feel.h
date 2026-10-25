#pragma once

// Vantium — the GAME-FEEL layer + the shared animation cursors. All game-owned state the engine
// never ticks: this layer advances every tween/animation cursor each sim tick and the renderer
// reads the resolved values.
//   • explosions   — pooled AnimationPlayers over the 4-frame boom clip, single() mode, reaped on
//                    finished(); spawned wherever something dies.
//   • score popups — a "+N" that rises and shrinks away (kills, pods, bonuses).
//   • shakes       — one brief jolt on the player's death; a long gentle rumble while a scuttled
//                    dreadnought tears itself apart.
//   • the pod glow + the star twinkle — palette-cycling AnimationPlayers (the deck consumes the
//                    current palette each tick).
//   • the destruct dim — a slow Multiply pulse over the frame during the destruct sequence.

#include <cstdint>
#include <optional>
#include <vector>

#include "retropp/animation.h"   // AnimationPlayer / AnimationFrame
#include "retropp/draw_state.h"  // ScreenSpaceEffect
#include "retropp/tween.h"       // Tween / TweenPlayer / PlaybackMode

#include "assets.h"
#include "layout.h"

namespace vant {

struct Boom {
    float                    x = 0, y = 0;   // center, world px
    int                      id = 0;
    retropp::AnimationPlayer player{};
};

struct ScorePopup {
    float                       x = 0, y = 0;
    int                         points = 0;
    int                         id = 0;
    retropp::TweenPlayer<float> progress{};
};

class VantFeel {
public:
    explicit VantFeel(const VantAssets& assets);

    void onEvent(const GameEvent& e);
    void tick();

    // ── Read by the renderer / the deck's palette passes ──────────────────────────────────────
    [[nodiscard]] const std::vector<Boom>&       booms() const { return booms_; }
    [[nodiscard]] const std::vector<ScorePopup>& popups() const { return popups_; }
    [[nodiscard]] const retropp::AnimationFrame& podFrame() const { return pod_.current(); }
    [[nodiscard]] const retropp::AnimationFrame& starFrame() const { return star_.current(); }
    [[nodiscard]] std::optional<retropp::ScreenSpaceEffect> screenShake() const;
    [[nodiscard]] float destructDim() const;   // 1 = no dim; dips gently during the destruct

    static constexpr std::uint64_t kPopupLifeMs = 700;

private:
    const retropp::Animation*   boomClip_;
    retropp::TweenPlayer<float> shakeAmp_{};
    retropp::TweenPlayer<float> dim_{};
    bool  shakeActive_ = false;
    float shakePhase_  = 0.0f;

    retropp::AnimationPlayer pod_{};
    retropp::AnimationPlayer star_{};

    std::vector<Boom>       booms_;
    std::vector<ScorePopup> popups_;
    int                     nextId_ = 1;
};

}  // namespace vant

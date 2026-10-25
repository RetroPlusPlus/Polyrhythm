#include "feel.h"

#include <chrono>

#include "retropp/timing.h"  // TimingProfile / TickPeriodNs

namespace ferryman {

using namespace retropp;

namespace {
using namespace std::chrono_literals;

constexpr float kShakeAmp = 3.5f;  // px — a gentle jolt

// Shared tween tracks — static storage, so every TweenPlayer that points at one outlives nothing
// it shouldn't. The game runs at 60 Hz; durations resolve to ticks via kProfile.
const Tween<float> kShakeTween =
    Tween<float>::of(kShakeAmp, 0.0f, 180ms, Easing::OutQuad);
const Tween<float> kPopupTween =
    Tween<float>::of(0.0f, 1.0f, std::chrono::milliseconds(FerrymanFeel::kPopupLifeMs),
                     Easing::OutQuad);
// The tractor beam's breath: a quick 0.8 s pulse loop, urgent but not strobing.
const Tween<float> kBeamTween =
    Tween<float>::of(0.30f, 0.55f, 400ms, Easing::InOutSine).then(0.30f, 400ms, Easing::InOutSine);
// The sanctuary glow's breath: a slow 2 s swell.
const Tween<float> kGlowTween =
    Tween<float>::of(0.35f, 0.8f, 1s, Easing::InOutSine).then(0.35f, 1s, Easing::InOutSine);
// The PRESS ENTER pulse (1.5 s) — the slow attract-mode swell.
const Tween<float> kPromptTween =
    Tween<float>::of(1.0f, 0.45f, 750ms, Easing::InOutSine).then(1.0f, 750ms, Easing::InOutSine);
// The round card: slide in with the arcade overshoot, hold center-stage, depart right.
const Tween<float> kBannerTween =
    Tween<float>::of(-0.35f, 0.5f, 500ms, Easing::OutBack)
        .then(0.5f, 1400ms, Easing::Linear)
        .then(1.35f, 400ms, Easing::InQuad);

const TimingProfile kProfile{TickPeriodNs::Hz60};
constexpr std::uint64_t kRested = 1'000'000;  // an elapsed count safely past any track's end
}  // namespace

FerrymanFeel::FerrymanFeel(const FerrymanAssets& assets) : assets_(assets) {
    // The one-shot cursors start RESTED (past their end), so they only run when restart()ed.
    shakeAmp_.tween        = &kShakeTween;
    shakeAmp_.profile      = kProfile;
    shakeAmp_.elapsedTicks = kRested;
    shakeAmp_.advance(PlaybackMode::single());
    banner_.tween        = &kBannerTween;
    banner_.profile      = kProfile;
    banner_.elapsedTicks = kRested;
    banner_.advance(PlaybackMode::single());
    // The looping cursors run from tick 0 for the whole program (bare advance() loops forever).
    beam_.tween    = &kBeamTween;
    beam_.profile  = kProfile;
    glow_.tween    = &kGlowTween;
    glow_.profile  = kProfile;
    prompt_.tween  = &kPromptTween;
    prompt_.profile = kProfile;
    thruster_.animation = &assets.thrusterClip;
    thruster_.profile   = kProfile;
    for (std::size_t look = 0; look < bobs_.size(); ++look) {
        bobs_[look].animation = &assets.bobClips[look];
        bobs_[look].profile   = kProfile;
        // Staggered starts so the shore's crowd doesn't bob in lockstep.
        bobs_[look].elapsedTicks = look * 8;
    }
    wings_.animation   = &assets.wingsClip;
    wings_.profile     = kProfile;
    pulse_.animation   = &assets.pulseClip;
    pulse_.profile     = kProfile;
    beacon_.animation = &assets.beaconClip;
    beacon_.profile   = kProfile;
    water_.animation  = &assets.waterClip;
    water_.profile    = kProfile;
    lights_.animation = &assets.lightsClip;
    lights_.profile   = kProfile;
}

void FerrymanFeel::onEvent(const GameEvent& e) {
    switch (e.kind) {
        case GameEventKind::Bank:
        case GameEventKind::Foil:
        case GameEventKind::EnemyDown: {
            // The payoff moments get a rising number…
            ScorePopup p;
            p.x = e.x;
            p.y = e.y;
            p.points = e.points;
            p.id = nextSpawnId_++;
            p.progress.tween   = &kPopupTween;
            p.progress.profile = kProfile;
            p.progress.restart();
            popups_.push_back(p);
            if (e.kind == GameEventKind::Bank) break;
            [[fallthrough]];  // …and the violent ones explode too
        }
        case GameEventKind::FerryDeath: {
            Boom b;
            b.x = e.x;
            b.y = e.y;
            b.id = nextSpawnId_++;
            b.player.animation = &assets_.boomClip;
            b.player.profile   = kProfile;
            b.player.restart();
            booms_.push_back(b);
            if (e.kind == GameEventKind::FerryDeath) {
                // The one real down-beat gets the one jolt; frequent events never shake —
                // constant wobble would be fatiguing.
                shakeAmp_.restart();
                shakeActive_ = true;
                shakePhase_  = 0.0f;
            }
            break;
        }
        default:
            break;  // hops / pickups / alarms / losses carry no feel beyond their SFX
    }
}

void FerrymanFeel::tick(const FerrymanGame& game) {
    wavePhase_ += 0.06f;  // the title wave rolls at ~0.6 Hz per letter
    warpPhase_ += 0.40f;  // the mutant's surface-warp rings emanate (a brisk shimmer)

    // The mutant comet-tail path history: drop trails whose mutant is gone, then sample each live
    // mutant's position (newest at the front, length-capped). The renderer traces the tapering warp
    // tail along this — the tail curves because it follows the creature's real path (the
    // trailing-object idiom).
    constexpr std::size_t kMutantTrailLen = 26;
    for (auto it = mutantTrails_.begin(); it != mutantTrails_.end();) {
        bool alive = false;
        for (const Enemy& e : game.enemies)
            if (e.kind == EK_MUTANT && e.id == it->first) {
                alive = true;
                break;
            }
        if (alive)
            ++it;
        else
            it = mutantTrails_.erase(it);
    }
    for (const Enemy& e : game.enemies) {
        if (e.kind != EK_MUTANT) continue;
        std::deque<retropp::Vec2>& tr = mutantTrails_[e.id];
        tr.push_front(retropp::Vec2{e.x, e.y});
        while (tr.size() > kMutantTrailLen) tr.pop_back();
    }
    // The round card: restart whenever the wave number changes (game start included). Back to
    // the title resets the watcher so a fresh game announces CROSSING 1 again.
    if (game.state == GameState::Playing && game.wave != lastWave_) {
        lastWave_   = game.wave;
        bannerWave_ = game.wave;
        banner_.restart();
    } else if (game.state == GameState::Title) {
        lastWave_ = 0;
    }
    banner_.advance(PlaybackMode::single());
    shakeAmp_.advance(PlaybackMode::single());
    beam_.advance();     // loops by default
    glow_.advance();
    prompt_.advance();
    thruster_.advance();
    for (AnimationPlayer& b : bobs_) b.advance();
    wings_.advance();
    pulse_.advance();
    beacon_.advance();
    water_.advance();
    lights_.advance();
    if (shakeActive_) {
        shakePhase_ += 0.85f;  // advance the wobble while the shake runs
        if (shakeAmp_.finished()) shakeActive_ = false;
    }
    for (ScorePopup& p : popups_) p.progress.advance(PlaybackMode::single());
    std::erase_if(popups_, [](const ScorePopup& p) { return p.progress.finished(); });
    for (Boom& b : booms_) b.player.advance(PlaybackMode::single());
    std::erase_if(booms_, [](const Boom& b) { return b.player.finished(); });
}

std::optional<ScreenSpaceEffect> FerrymanFeel::screenShake() const {
    if (!shakeActive_) return std::nullopt;
    ScreenSpaceEffect e{};
    e.kind      = ScreenSpaceEffectKind::RowDisplacement;
    e.amplitude = shakeAmp_.value();        // tween-decayed magnitude
    e.frequency = 3.0f;
    e.phase     = shakePhase_;
    e.axis      = Axis::Horizontal;         // rows slide horizontally → a brief side-to-side judder
    e.edge      = DisplacementEdge::Blank;  // exposed strip shows the backdrop, not a smear
    return e;
}

}  // namespace ferryman

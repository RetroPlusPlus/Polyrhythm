#pragma once

// Ferryman — the SIMULATION, its own translation unit. Owns the ferry, the colonists, the enemy
// craft and their bullets, the abductors' contact rules, and the score/lives/wave state, and
// advances them one 60 Hz tick from the input. It draws nothing and makes no sound: it NAMES
// what happened each tick as a GameEvent stream, which the audio + feel layers consume. Pure
// game model.
//
// THE LOOP (the hybrid's emergent core):
//   • THE CARGO IS THE DIFFICULTY — AND THE ARSENAL. You sail an open sea rescuing souls from
//     islets and carrying them home to the sanctuary band. Every colonist aboard slows the ferry
//     (speed 2.6 − 0.35n px/tick) — and each one aboard occasionally FIRES BACK at the nearest
//     enemy. Delivering n at once pays escalating (slot i pays i × 50), so the greed dial is how
//     heavy — how slow, how armed — you dare get. There is NO fire button: the crew fights, the
//     ferryman sails.
//   • A TONED-DOWN BULLET HELL. Three enemy craft classes cross the field firing straight,
//     readable bullets — an aimed bolt (corsair), a 6-ring (warden), a 3-fan (dreadnought) — and
//     a colonist the abductor carries off comes back as a MUTANT contact-hunter. Waves ramp
//     density and cadence, never randomness: the hell stays readable. (The full game grows this
//     into the build-dependent bullet hell ↔ bullet heaven balance.)
//   • One identity for life: a colonist's id (and its sprite key) rides with it through waiting →
//     aboard → stunned. Only true teleports (an abduction, a mutant return) re-mint.
//   • THE ISLANDS ARE SOLID. The hull collides with every islet and with the sanctuary is where
//     it banks; a soul boards by DOCKING against its islet's coast (no button, never by sailing
//     THROUGH the colonist), and the only place cargo leaves the deck is the sanctuary band.

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "retropp/input.h"  // InputState

#include "abductor.h"
#include "layout.h"

namespace ferryman {

// The game's input vocabulary — the semantic actions the sim reads. main.cpp binds each action to
// its physical sources (keys, d-pad, face buttons, shoulders) in one ActionMap; the analog stick is
// read raw beside these.
enum class Action : std::uint8_t {
    SailUp,
    SailDown,
    SailLeft,
    SailRight,
    Menu,        // starts a game at the title; opens the pause menu and confirms its choice in play
    Fullscreen,  // host-level toggle, read in main.cpp
};

// What happened this tick — the sim's output channel beyond the field state. Audio cues off
// `kind`; the feel layer reads `x`/`y` (where it happened) and `points` (a popup's number).
enum class GameEventKind : std::uint8_t {
    Pickup,        // a colonist boarded (by docking against its islet — no button)
    Bank,          // the whole deck delivered at the sanctuary (points = the escalating pay)
    BeamLock,      // an abductor's beam lit — the you-are-being-robbed alarm
    Foil,          // the abduction stopped — body-block or a cargo bolt (points = the bounty)
    ColonistLost,  // carried off the top — a mutant is coming
    MutantSpawn,   // it arrived
    CargoFire,     // a passenger fired the gold return bolt (the most frequent cue)
    EnemyDown,     // a craft or mutant destroyed by cargo fire (points = the bounty)
    FerryDeath,
    WaveClear,
};
struct GameEvent {
    GameEventKind kind;
    float         x      = 0.0f;
    float         y      = 0.0f;
    int           points = 0;
};

// A colonist's one identity for its whole life. Free (x, y) on the open field; `state` moves it
// between an islet, the deck, and a stunned drop.
enum class ColonistState : std::uint8_t { Waiting, Aboard, Stunned };
struct Colonist {
    int           id   = 0;
    int           look = 0;  // hood / pack / cap — the art variant, chosen at spawn
    ColonistState state = ColonistState::Waiting;
    float         x = 0.0f, y = 0.0f;
    int           stunnedLeft = 0;  // Stunned: ticks until it recovers to Waiting
};

// An enemy craft (or mutant). Motion state is per-kind: a corsair holds a crossing velocity and
// sway phase, a warden an orbit anchor and angle, a dreadnought a horizontal direction, a mutant
// nothing (it just hunts).
struct Enemy {
    int   id   = 0;
    int   kind = EK_CORSAIR;
    float x = 0.0f, y = 0.0f;
    int   hp        = 1;
    int   fireTimer = 0;   // ticks until the next volley
    int   hitFlash  = 0;   // ticks of the just-hit dim (per-sprite alpha)
    bool  leaving   = false;  // a mutant whose ferry died: it flees off-screen (vx/vy) then despawns
    float vx = 0.0f, vy = 0.0f;   // corsair / dreadnought course
    float sway = 0.0f;            // corsair flight bob
    float anchorX = 0.0f, anchorY = 0.0f, orbit = 0.0f;  // warden patrol
};

struct Bolt {
    int   id = 0;
    float x = 0.0f, y = 0.0f, vx = 0.0f, vy = 0.0f;
    bool  friendly = false;  // gold (the cargo's) vs magenta (theirs)
};

// The game model. State is public so the sim-free renderer reads it directly (a demo, not a
// library surface — clarity over encapsulation); the sim mutates it through tick().
struct FerrymanGame {
    FerrymanGame();

    GameState state = GameState::Title;

    int score = 0, lives = kLives, wave = 0;
    int rescued = 0;   // toward this wave's quota
    [[nodiscard]] int quota() const { return std::min(kQuotaBase + wave, kQuotaCap); }

    // The ferry: free center-based position. lifeNum keys the sprite per life (a respawn is a
    // teleport home — the fresh key mount-snaps it).
    float  ferryX = kViewW / 2.0f, ferryY = kViewH - 80.0f;
    int    lifeNum = 0;
    int    invulnLeft = 0;   // ticks of respawn invulnerability (the alpha breath while > 0)
    Facing ferryFacing = Facing::East;  // heading — picks the sprite AND the collision hull
    bool   ferryMoving = false;         // moved this tick → trail a wake behind the hull

    // The collision hull turns with the heading: the wide side view (E/W) vs the narrow bow/stern
    // view (N/S) that threads a one-block channel. (The forgiving COMBAT box stays fixed.)
    [[nodiscard]] bool facingVertical() const {
        return ferryFacing == Facing::North || ferryFacing == Facing::South;
    }
    [[nodiscard]] float hullW() const { return facingVertical() ? kFerryNSW : kFerryW; }
    [[nodiscard]] float hullH() const { return facingVertical() ? kFerryNSH : kFerryH; }

    // The pause menu (Playing only): the sim + feel + parallax freeze while `paused`; START opens
    // it and confirms the highlighted choice, up/down move the selection.
    bool  paused = false;
    int   pauseChoice = 0;      // 0 = RESUME, 1 = QUIT TO TITLE

    std::vector<int>       deck;       // colonist ids aboard, in boarding order (cap kDeckCap)
    std::vector<Colonist>  colonists;  // every alive, un-delivered colonist, in EVERY state
    std::vector<Enemy>     enemies;
    std::vector<Bolt>      bolts;
    std::vector<IsletSpec> islets;     // THIS run's archipelago — rolled fresh at newGame()

    std::vector<int>                 mutantQueue;    // countdowns to each pending mutant
    std::vector<std::pair<int, int>> respawnQueue;   // {kind, ticks} for downed craft re-entries
    int   waveLull = 0;   // ticks until the next wave once the quota is met
    float fireRateMul = 1.0f;  // the per-wave cadence multiplier (denser, never random)

    // Two abductors; the second activates from kSecondAbductorWave. Each may hold the colonist
    // it grabbed (removed from `colonists` at the grab, re-inserted intact on a foil — the
    // identity, and so the sprite key, survives the rescue).
    std::array<Abductor, 2>                abductors;
    std::array<std::optional<Colonist>, 2> carried;
    [[nodiscard]] int abductorCount() const { return wave >= kSecondAbductorWave ? 2 : 1; }

    // What delivering the current deck would pay — the HUD's greed dial.
    [[nodiscard]] int haulPays() const {
        const int n = static_cast<int>(deck.size());
        return kBankPerSlot * n * (n + 1) / 2;
    }
    [[nodiscard]] const Colonist* colonistById(int id) const;
    [[nodiscard]] bool anyBeamLit() const {
        for (int i = 0; i < abductorCount(); ++i)
            if (abductors[static_cast<std::size_t>(i)].beamLit()) return true;
        return false;
    }
    [[nodiscard]] bool anyMutant() const {
        for (const Enemy& e : enemies)
            if (e.kind == EK_MUTANT) return true;
        return false;
    }

    // Advance one sim tick; refills the per-tick event list (cleared at entry).
    void tick(const retropp::InputState& in);

    // The events emitted THIS tick (valid until the next tick()).
    [[nodiscard]] std::span<const GameEvent> events() const { return events_; }

private:
    std::uint32_t rng;
    int           nextId_ = 1;
    int           colonistSpawnTimer_ = 0;
    int           cargoFireTimer_ = 0;
    std::vector<GameEvent> events_;

    void emit(GameEventKind kind, float x = 0.0f, float y = 0.0f, int points = 0) {
        events_.push_back(GameEvent{kind, x, y, points});
    }

    void newGame();
    void randomizeIslets();           // roll this run's archipelago (count/positions/widths/props)
    void spawnWave();                 // ++wave, ramps cadence, tops the enemy roster up
    void spawnEnemy(int kind);        // a fresh craft entering from a field edge
    void respawnFerry();
    void moveInput(const retropp::InputState& in);
    void colonistFlow();              // islet trickle + stun recovery
    void abductorPhase();             // targets, grabs, foils, losses, the mutant queue
    void enemyPhase();                // motion + firing + the respawn queue
    void cargoFire();                 // the crew's return volleys
    void boltPhase();                 // advance bolts; bolt-vs-ferry / -enemy / -abductor
    void resolveContacts();           // enemy contact, pickups, banking
    void enemyDown(std::size_t idx);  // score + boom + respawn booking
    void ferryDeath();
    void placeGrounded(Colonist c, float px, float py, int stunTicks);

    // The solid islands: the hull collides with these, and their coasts are the docks.
    [[nodiscard]] bool ferryBoxHitsIslet(float cx, float cy) const;      // hull overlaps an islet
    [[nodiscard]] float isletPenetration(float cx, float cy) const;      // deepest hull-vs-islet overlap
    [[nodiscard]] bool ferryTouchesIslet(const IsletSpec& s) const;      // hull is docked at a coast
    [[nodiscard]] static bool colonistOnIslet(const Colonist& c, const IsletSpec& s);
};

}  // namespace ferryman

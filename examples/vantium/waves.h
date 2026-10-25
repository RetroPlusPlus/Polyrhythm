#pragma once

// Vantium — the ENEMY layer, its own translation unit: fighter squadrons, homing mines, and the
// enemy shots. It moves enemies and emits events; the ball-park contact rules (what a hit costs)
// stay in the sim, which reads positions from here.
//
//   • A SQUADRON is five fighters in single file along a smooth Catmull-Rom curve spanning the
//     view — authored as waypoints per spawn, baked ONCE into an ArcLengthTable, and walked at
//     constant speed (members trail the leader by a fixed arc length: the classic conga swoop).
//   • Each wave rotates its LIVERY: one dart shape, four 8-color palettes.
//   • Fighters on screen fire aimed shots on independent cooldowns.
//   • MINES rise from authored deck spawn points when on screen (max 2 alive) and home slowly.
//   • Clearing the wave QUOTA raises the LAND NOW prompt; killing all five of a squadron pays
//     the wave bonus.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "retropp/curve.h"     // Curve / ArcLengthTable — the squadron paths
#include "retropp/geometry.h"  // Vec2

#include "deck.h"
#include "layout.h"

namespace vant {

struct Fighter {
    bool  alive = false;
    int   fireCooldown = 0;
};

struct Wave {
    int   id = 0;              // monotonic — the stable-key root for its fighters
    bool  running = false;
    float headS = 0.0f;        // the leader's arc-length along the path
    float pathLen = 0.0f;
    bool  headsRight = true;   // travel direction (drives the sprite flip)
    int   livery = 0;          // PAL_FIGHTER_0 + livery
    int   killed = 0;          // 5 kills = the wave bonus
    std::optional<retropp::ArcLengthTable> path;
    std::array<Fighter, kWaveSize> members{};
};

struct EnemyShot {
    int   id = 0;
    bool  alive = false;
    float x = 0, y = 0, vx = 0, vy = 0;
};

struct Mine {
    int   id = 0;
    bool  alive = false;
    float x = 0, y = 0;
    int   cooldown = 0;        // per-slot respawn delay after death
};

class WaveSystem {
public:
    void reset(int shipNum);   // new ship / new life wave state (quota progress persists per ship)
    void resetQuota(int shipNum);  // full reset for a NEW ship

    void tick(const Deck& deck, retropp::Vec2 ship, bool shipAlive, float camX, int facing,
              std::uint32_t& rng, std::vector<GameEvent>& events);

    // The sim's collision queries / mutations.
    [[nodiscard]] Wave&                    wave() { return wave_; }
    [[nodiscard]] const Wave&              wave() const { return wave_; }
    [[nodiscard]] retropp::Vec2            fighterPos(int i) const;  // center, world px
    void killFighter(int i, std::vector<GameEvent>& events);
    [[nodiscard]] std::vector<Mine>&       mines() { return mines_; }
    [[nodiscard]] const std::vector<Mine>& mines() const { return mines_; }
    [[nodiscard]] std::vector<EnemyShot>&       shots() { return eshots_; }
    [[nodiscard]] const std::vector<EnemyShot>& shots() const { return eshots_; }

    [[nodiscard]] int  wavesCleared() const { return wavesCleared_; }
    [[nodiscard]] int  quota() const { return quota_; }
    [[nodiscard]] bool quotaMet() const { return wavesCleared_ >= quota_; }

private:
    void spawnWave(retropp::Vec2 ship, float camX, int facing, std::uint32_t& rng);

    Wave                   wave_{};
    std::vector<Mine>      mines_;
    std::vector<EnemyShot> eshots_;
    float                  waveSpeed_ = kWaveSpeedBase;
    int                    wavesCleared_ = 0;
    int                    quota_ = kQuotaBase;
    int                    lull_ = kWaveLull;
    int                    nextWaveId_ = 1;
    int                    nextShotId_ = 1;
    int                    nextMineId_ = 1;
    bool                   landNowSent_ = false;
};

}  // namespace vant

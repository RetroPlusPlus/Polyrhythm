#include "game.h"

#include <chrono>
#include <cmath>
#include <cstdio>

#include "retropp/timing.h"  // TimingProfile / TickPeriodNs — durations → ticks

namespace ferryman {

using namespace retropp;

namespace {
using namespace std::chrono_literals;

const TimingProfile kProfile{TickPeriodNs::Hz60};
const std::uint64_t kInvulnTicks      = kProfile.ticksForDuration(2500ms);
const std::uint64_t kWaveLullTicks    = kProfile.ticksForDuration(2200ms);
const std::uint64_t kColonistSpawnGap = kProfile.ticksForDuration(4s);
const std::uint64_t kStunTicks        = kProfile.ticksForDuration(4s);
const std::uint64_t kAbductorFirst    = kProfile.ticksForDuration(8s);
const std::uint64_t kAbductorReentry  = kProfile.ticksForDuration(10s);
const std::uint64_t kMutantDelay      = kProfile.ticksForDuration(5s);

constexpr float kDegToRad = 3.14159265f / 180.0f;
constexpr float kFieldTopF = static_cast<float>(kFieldTop);

// A tiny clock-seeded LCG (variety per run; native game, no VM involved).
std::uint32_t nextRand(std::uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

float frand(std::uint32_t& s) { return static_cast<float>(nextRand(s) % 10000u) / 10000.0f; }

float distTo(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

// Center-based AABB overlap with per-object box sizes (full widths/heights).
bool boxesOverlap(float ax, float ay, float aw, float ah,
                  float bx, float by, float bw, float bh) {
    return std::abs(ax - bx) < (aw + bw) / 2.0f && std::abs(ay - by) < (ah + bh) / 2.0f;
}

// The per-wave enemy roster — density ramps, geometry never randomizes.
int corsairTarget(int wave) { return std::min(2 + (wave - 1) / 2, 5); }
int wardenTarget(int wave) { return std::min(1 + (wave - 1) / 3, 3); }
int dreadTarget(int wave) {
    return wave >= kDreadFirstWave ? std::min(1 + (wave - kDreadFirstWave) / 3, 2) : 0;
}

}  // namespace

FerrymanGame::FerrymanGame()
    : rng(static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())) {}

const Colonist* FerrymanGame::colonistById(int id) const {
    for (const Colonist& c : colonists)
        if (c.id == id) return &c;
    return nullptr;
}

void FerrymanGame::newGame() {
    score   = 0;
    lives   = kLives;
    wave    = 0;
    rescued = 0;
    fireRateMul = 1.0f;
    waveLull = 0;
    deck.clear();
    colonists.clear();
    enemies.clear();
    bolts.clear();
    mutantQueue.clear();
    respawnQueue.clear();
    carried[0].reset();
    carried[1].reset();
    colonistSpawnTimer_ = 0;
    cargoFireTimer_     = 0;
    randomizeIslets();
    respawnFerry();
    // A couple of souls already wait on the islets, so the first run out has a purpose.
    for (int i = 0; i < 2; ++i) {
        const IsletSpec& islet = islets[static_cast<std::size_t>(nextRand(rng) % islets.size())];
        Colonist c;
        c.id   = nextId_++;
        c.look = c.id % kColonistLooks;
        c.x    = isletCenterX(islet) + (frand(rng) - 0.5f) * 20.0f;
        c.y    = isletCenterY(islet) + (frand(rng) - 0.5f) * 10.0f;
        colonists.push_back(c);
    }
    spawnWave();
}

void FerrymanGame::randomizeIslets() {
    // Roll this run's archipelago — the random-tilemap demo's ergonomic applied to geography:
    // sample within the valid vocabulary (field rows, 1- or 2-block widths, the prop set) and
    // reject anything violating the spacing constraint, so sea lanes always stay open. Rejection
    // sampling with a guard: a crowded roll just yields a slightly sparser sea, never a spin.
    islets.clear();
    const int want = kIsletCountMin +
                     static_cast<int>(nextRand(rng) %
                                      static_cast<unsigned>(kIsletCountMax - kIsletCountMin + 1));
    for (int guard = 0; static_cast<int>(islets.size()) < want && guard < 1500; ++guard) {
        IsletSpec s{};
        // Roll a shape: single (1×1), horizontal (2×1), VERTICAL (1×2), or a small block (2×2) —
        // vertical islets are now as common as horizontal ones.
        switch (nextRand(rng) % 5u) {
            case 0:  s.tilesW = 2; s.tilesH = 1; break;  // horizontal
            case 1:  s.tilesW = 1; s.tilesH = 2; break;  // vertical
            case 2:  s.tilesW = 2; s.tilesH = 2; break;  // a small block
            default: s.tilesW = 1; s.tilesH = 1; break;  // single (the common case)
        }
        // Place the WHOLE footprint inside the edge-inset bounds — never on the map border.
        const int colSpan = kIsletColMax - kIsletColMin - s.tilesW + 2;  // # of legal blockX values
        const int rowSpan = kIsletRowMax - kIsletRowMin - s.tilesH + 2;  // # of legal blockY values
        s.blockX = kIsletColMin + static_cast<int>(nextRand(rng) % static_cast<unsigned>(colSpan));
        s.blockY = kIsletRowMin + static_cast<int>(nextRand(rng) % static_cast<unsigned>(rowSpan));
        bool clear = true;
        for (const IsletSpec& o : islets) {
            // Block gap between the two rectangles on each axis (0 = touching or overlapping).
            const int gapX = std::max({o.blockX - (s.blockX + s.tilesW - 1),
                                       s.blockX - (o.blockX + o.tilesW - 1), 0});
            const int gapY = std::max({o.blockY - (s.blockY + s.tilesH - 1),
                                       s.blockY - (o.blockY + o.tilesH - 1), 0});
            if (std::max(gapX, gapY) < kIsletSpacing) {
                clear = false;
                break;
            }
        }
        if (!clear) continue;
        if (s.tilesW * s.tilesH > 1) {  // multi-block islets may carry a buoy / mooring / lamp
            switch (nextRand(rng) % 4u) {
                case 0:  s.prop = T_BUOY; break;
                case 1:  s.prop = T_MOORING; break;
                case 2:  s.prop = T_LAMP; break;
                default: s.prop = 0; break;
            }
        }
        islets.push_back(s);
    }
    if (islets.empty())  // unreachable in practice; the guarantee costs one line
        islets.push_back(IsletSpec{.blockX = 9, .blockY = 7, .tilesW = 2, .tilesH = 1, .prop = 0});
}

void FerrymanGame::respawnFerry() {
    ++lifeNum;  // the ferry teleports home — a fresh key makes it mount-snap, not streak
    ferryX      = kViewW / 2.0f;
    ferryY      = kViewH - 80.0f;
    ferryFacing = Facing::East;  // reset heading (the wide hull) before the clear-spot search
    ferryMoving = false;
    // The archipelago is random — never surface INSIDE an island. Nudge up into open water.
    for (int guard = 0; guard < 16 && ferryBoxHitsIslet(ferryX, ferryY); ++guard)
        ferryY -= static_cast<float>(kBlock) / 2.0f;
    invulnLeft = static_cast<int>(kInvulnTicks);
}

void FerrymanGame::spawnEnemy(int kind) {
    Enemy e;
    e.id   = nextId_++;
    e.kind = kind;
    e.hp   = kEnemyHp[static_cast<std::size_t>(kind)];
    e.fireTimer =
        60 + static_cast<int>(frand(rng) * 120.0f);  // staggered first volleys, never a chorus
    switch (kind) {
        case EK_CORSAIR: {
            // A straight crossing run: enter on a side edge, fly a gentle diagonal.
            const bool fromLeft = nextRand(rng) % 2u == 0u;
            e.x  = fromLeft ? -kEnemyW[EK_CORSAIR] : kViewW + kEnemyW[EK_CORSAIR];
            e.y  = kFieldTopF + 60.0f + frand(rng) * (kViewH - kFieldTopF - 160.0f);
            e.vx = (fromLeft ? 1.0f : -1.0f) * kCorsairSpeed;
            e.vy = (frand(rng) - 0.5f) * 0.6f;
            break;
        }
        case EK_WARDEN: {
            // A patrol orbit around a mid-sea anchor; it drifts in from above its station.
            e.anchorX = 100.0f + frand(rng) * (kViewW - 200.0f);
            e.anchorY = kFieldTopF + 120.0f + frand(rng) * (kViewH - kFieldTopF - 220.0f);
            e.orbit   = frand(rng) * 6.28f;
            e.x       = e.anchorX;
            e.y       = kFieldTopF - 30.0f;
            break;
        }
        case EK_DREAD: {
            // The ponderous horizontal crossing, upper-mid sea.
            const bool fromLeft = nextRand(rng) % 2u == 0u;
            e.x  = fromLeft ? -kEnemyW[EK_DREAD] : kViewW + kEnemyW[EK_DREAD];
            e.y  = kFieldTopF + 90.0f + frand(rng) * 120.0f;
            e.vx = (fromLeft ? 1.0f : -1.0f) * kDreadSpeed;
            break;
        }
        default: {  // EK_MUTANT — arrives at the top, where its abductor left
            e.x = 40.0f + frand(rng) * (kViewW - 80.0f);
            e.y = kFieldTopF + 10.0f;
            break;
        }
    }
    enemies.push_back(e);
}

void FerrymanGame::spawnWave() {
    ++wave;
    rescued     = 0;
    fireRateMul = std::max(std::pow(kFireRateGrow, static_cast<float>(wave - 1)), kFireRateFloor);
    // Top the roster up to this wave's targets (survivors keep flying; newcomers enter from the
    // edges, so the field thickens without anything popping into view).
    int have[3] = {0, 0, 0};
    for (const Enemy& e : enemies)
        if (e.kind < 3) ++have[e.kind];
    for (int k = have[EK_CORSAIR]; k < corsairTarget(wave); ++k) spawnEnemy(EK_CORSAIR);
    for (int k = have[EK_WARDEN]; k < wardenTarget(wave); ++k) spawnEnemy(EK_WARDEN);
    for (int k = have[EK_DREAD]; k < dreadTarget(wave); ++k) spawnEnemy(EK_DREAD);
    const int base = static_cast<int>(kAbductorFirst);
    abductors[0].reset(base);
    abductors[1].reset(base + base / 2);  // the second (once active) arrives on its own beat
}

void FerrymanGame::moveInput(const InputState& in) {
    // 8-direction sailing: each sail action gathers all of its sources (arrows, WASD, d-pad, the
    // pad aliases — see main.cpp's ActionMap), and the analog stick adds its vector beside them.
    ferryMoving = false;  // no input below → stays false (no wake, no heading change)
    float dx = 0.0f, dy = 0.0f;
    if (in.isHeld(Action::SailLeft))  dx -= 1.0f;
    if (in.isHeld(Action::SailRight)) dx += 1.0f;
    if (in.isHeld(Action::SailUp))    dy -= 1.0f;
    if (in.isHeld(Action::SailDown))  dy += 1.0f;
    const Vec2 stick = in.stick(Stick::Left);
    if (std::abs(stick.x) > 0.25f) dx += stick.x;
    if (std::abs(stick.y) > 0.25f) dy += stick.y;
    const float mag = std::hypot(dx, dy);
    if (mag < 0.01f) return;
    ferryMoving = true;
    // The heading follows the DOMINANT axis: mostly-vertical input turns the boat to its narrow
    // bow/stern hull (so it threads a one-block channel); mostly-horizontal keeps the wide side
    // hull. The hull dims (hullW/hullH) below then follow the new heading.
    if (std::abs(dy) > std::abs(dx))
        ferryFacing = dy < 0.0f ? Facing::North : Facing::South;
    else
        ferryFacing = dx < 0.0f ? Facing::West : Facing::East;
    // THE WEIGHT RULE: every soul aboard slows the ferry — cargo is the difficulty.
    const float speed =
        kFerrySpeedBase - kFerrySpeedPerPax * static_cast<float>(deck.size());
    // Resolve one axis at a time so the hull SLIDES along an island coast instead of sticking.
    // The field edges clamp (no wrap — the sea has a shore); the ISLANDS ARE SOLID — the hull
    // stops at the shore rather than sailing through, and that same coast is the jetty where
    // souls come aboard.
    // Move each axis only if the step does not push DEEPER into an islet. A clear step (penetration
    // 0) always passes; a step that REDUCES an existing overlap passes too; only a deepening step is
    // blocked. That lets a hull the heading change just WIDENED into a coast always back out again,
    // instead of deadlocking (an all-or-nothing test traps it — open water beside it, but every
    // nearby position still overlaps). The sanctuary is the top LAND band — the hull stops at its
    // coast (banks on touch, below); the bottom/sides are the open-water edge.
    const float nx =
        std::clamp(ferryX + dx / mag * speed, hullW() / 2.0f, kViewW - hullW() / 2.0f);
    if (isletPenetration(nx, ferryY) <= isletPenetration(ferryX, ferryY)) ferryX = nx;
    const float ny = std::clamp(ferryY + dy / mag * speed, kSanctuaryBottom + hullH() / 2.0f,
                                kViewH - hullH() / 2.0f);
    if (isletPenetration(ferryX, ny) <= isletPenetration(ferryX, ferryY)) ferryY = ny;
}

// The deepest the hull (centered at cx,cy, at the current heading) penetrates any islet it overlaps
// — the minimum push-out distance, 0 when clear or merely touching. moveInput permits a step only
// when it does not INCREASE this, so the hull can always slide or back out of an overlap.
float FerrymanGame::isletPenetration(float cx, float cy) const {
    const float hw = hullW(), hh = hullH();
    float       worst = 0.0f;
    for (const IsletSpec& s : islets) {
        const float iw = isletRightX(s) - isletLeftX(s);
        const float ih = isletBotY(s) - isletTopY(s);
        const float ix = isletLeftX(s) + iw / 2.0f;
        const float iy = isletTopY(s) + ih / 2.0f;
        const float ox = (hw + iw) / 2.0f - std::abs(cx - ix);  // X overlap (>0 = overlapping in X)
        const float oy = (hh + ih) / 2.0f - std::abs(cy - iy);  // Y overlap
        if (ox > 0.0f && oy > 0.0f) worst = std::max(worst, std::min(ox, oy));
    }
    return worst;
}

// Does the ferry's FULL hull, centered at (cx, cy), overlap any islet's solid rectangle? Land
// collision uses the whole rendered footprint (not the forgiving combat box) so the visible boat
// never climbs onto land — it only ever sails in the water.
bool FerrymanGame::ferryBoxHitsIslet(float cx, float cy) const {
    for (const IsletSpec& s : islets) {
        const float iw = isletRightX(s) - isletLeftX(s);
        const float ih = isletBotY(s) - isletTopY(s);
        const float ix = isletLeftX(s) + iw / 2.0f;
        const float iy = isletTopY(s) + ih / 2.0f;
        if (boxesOverlap(cx, cy, hullW(), hullH(), ix, iy, iw, ih)) return true;
    }
    return false;
}

// Is the hull pressed against this islet's coast? A slightly inflated hull counts as docked (the
// island itself is solid, so the hulls never truly overlap — they meet at the shore).
bool FerrymanGame::ferryTouchesIslet(const IsletSpec& s) const {
    const float iw = isletRightX(s) - isletLeftX(s);
    const float ih = isletBotY(s) - isletTopY(s);
    const float ix = isletLeftX(s) + iw / 2.0f;
    const float iy = isletTopY(s) + ih / 2.0f;
    return boxesOverlap(ferryX, ferryY, hullW() + 2.0f * kCoastTouch,
                        hullH() + 2.0f * kCoastTouch, ix, iy, iw, ih);
}

// Is this colonist standing on the islet (spawn jitter + a small dock margin)?
bool FerrymanGame::colonistOnIslet(const Colonist& c, const IsletSpec& s) {
    constexpr float m = 12.0f;
    return c.x >= isletLeftX(s) - m && c.x <= isletRightX(s) + m && c.y >= isletTopY(s) - m &&
           c.y <= isletBotY(s) + m;
}

void FerrymanGame::colonistFlow() {
    // The islet trickle: a new soul every few seconds while fewer than the cap wait.
    int waiting = 0;
    for (const Colonist& c : colonists)
        if (c.state != ColonistState::Aboard) ++waiting;
    if (waiting < kMaxWaiting && --colonistSpawnTimer_ <= 0) {
        colonistSpawnTimer_ = static_cast<int>(kColonistSpawnGap);
        const IsletSpec& islet = islets[static_cast<std::size_t>(nextRand(rng) % islets.size())];
        Colonist c;
        c.id   = nextId_++;
        c.look = c.id % kColonistLooks;
        c.x    = isletCenterX(islet) + (frand(rng) - 0.5f) * 20.0f;
        c.y    = isletCenterY(islet) + (frand(rng) - 0.5f) * 10.0f;
        colonists.push_back(c);
    }
    // Stun recovery: a dropped soul dusts itself off and waits where it landed.
    for (Colonist& c : colonists)
        if (c.state == ColonistState::Stunned && --c.stunnedLeft <= 0)
            c.state = ColonistState::Waiting;
}

void FerrymanGame::placeGrounded(Colonist c, float px, float py, int stunTicks) {
    c.state       = ColonistState::Stunned;
    c.stunnedLeft = stunTicks;
    // A soul always comes to rest on LAND: snap the drop to the nearest islet's coast. Open water
    // has no dock, so a colonist stranded there could never be recovered under the touch rule —
    // washing survivors ashore keeps every waiting soul reachable and the model coherent (people
    // live on islands). The requested (px, py) clamps INTO that islet, so several drops spread
    // across its width rather than stacking on one pixel.
    const IsletSpec* best  = nullptr;
    float            bestD = 1.0e9f;
    for (const IsletSpec& s : islets) {
        const float d = distTo(px, py, isletCenterX(s), isletCenterY(s));
        if (d < bestD) {
            bestD = d;
            best  = &s;
        }
    }
    if (best != nullptr) {
        px = std::clamp(px, isletLeftX(*best) + 8.0f, isletRightX(*best) - 8.0f);
        py = std::clamp(py, isletTopY(*best) + 8.0f, isletBotY(*best) - 8.0f);
    }
    c.x = std::clamp(px, 10.0f, kViewW - 10.0f);
    c.y = std::clamp(py, kFieldTopF + 10.0f, kViewH - 10.0f);
    colonists.push_back(c);
}

void FerrymanGame::abductorPhase() {
    for (int i = 0; i < abductorCount(); ++i) {
        auto&      abd = abductors[static_cast<std::size_t>(i)];
        const Vec2 ap  = abd.pos();

        // The sim picks WHAT it wants (its knowledge); the abductor flies. Prey = the nearest
        // grounded colonist — never one aboard the ferry.
        std::optional<Vec2> target;
        float best = 1.0e9f;
        for (const Colonist& c : colonists) {
            if (c.state == ColonistState::Aboard) continue;
            const float d = distTo(ap.x, ap.y, c.x, c.y);
            if (d < best) {
                best   = d;
                target = Vec2{c.x, c.y};
            }
        }
        abd.tick(target, rng);

        if (abd.beamJustLit()) emit(GameEventKind::BeamLock, ap.x, ap.y);

        // The grab: a descending saucer within reach of a grounded colonist swallows it whole —
        // the colonist LEAVES the field vector and rides in the saucer's stomach (`carried`),
        // identity intact, until a foil returns it or the top of the field loses it.
        if (abd.state() == AbductorState::Descending) {
            for (std::size_t k = 0; k < colonists.size(); ++k) {
                const Colonist& c = colonists[k];
                if (c.state == ColonistState::Aboard) continue;
                if (distTo(ap.x, ap.y + kAbductorH / 2.0f, c.x, c.y) <= kGrabDist) {
                    carried[static_cast<std::size_t>(i)] = c;
                    colonists.erase(colonists.begin() + static_cast<std::ptrdiff_t>(k));
                    abd.grab();
                    break;
                }
            }
        }

        // The body-block foil: the ferry rams a lit beam — the colonist (if already grabbed)
        // lands stunned below, SAME identity, and the thief flees. Pays the bounty. (A cargo
        // bolt strike foils it too — boltPhase handles that path.)
        if (abd.beamLit() &&
            distTo(ferryX, ferryY, ap.x, ap.y) <= kFoilDist + kFerryBoxW / 2.0f) {
            if (auto& held = carried[static_cast<std::size_t>(i)]) {
                placeGrounded(*held, ap.x, ap.y + 20.0f, static_cast<int>(kStunTicks));
                held.reset();
            }
            abd.foil(static_cast<int>(kAbductorReentry));
            score += kFoilScore;
            emit(GameEventKind::Foil, ap.x, ap.y, kFoilScore);
        }

        // Carried off the top: the colonist is LOST — and something is coming back.
        if (abd.state() == AbductorState::Carrying && ap.y < kFieldTopF - 30.0f) {
            carried[static_cast<std::size_t>(i)].reset();
            abd.leftWithColonist(static_cast<int>(kAbductorReentry));
            emit(GameEventKind::ColonistLost, ap.x, kFieldTopF);
            mutantQueue.push_back(static_cast<int>(kMutantDelay));
        }
    }

    // The mutant queue: each loss matures into a hunter (capped; the rest wait their turn).
    int mutantsAlive = 0;
    for (const Enemy& e : enemies)
        if (e.kind == EK_MUTANT) ++mutantsAlive;
    for (std::size_t q = 0; q < mutantQueue.size();) {
        if (mutantQueue[q] > 0) --mutantQueue[q];
        if (mutantQueue[q] == 0 && mutantsAlive < kMaxMutants) {
            spawnEnemy(EK_MUTANT);
            ++mutantsAlive;
            emit(GameEventKind::MutantSpawn, enemies.back().x, enemies.back().y);
            mutantQueue.erase(mutantQueue.begin() + static_cast<std::ptrdiff_t>(q));
        } else {
            ++q;
        }
    }
}

void FerrymanGame::enemyPhase() {
    // The respawn queue: a downed craft re-enters from an edge after its delay (a fresh id —
    // the population holds, and the interpolator mount-snaps the arrival).
    for (std::size_t q = 0; q < respawnQueue.size();) {
        if (--respawnQueue[q].second <= 0) {
            spawnEnemy(respawnQueue[q].first);
            respawnQueue.erase(respawnQueue.begin() + static_cast<std::ptrdiff_t>(q));
        } else {
            ++q;
        }
    }

    for (Enemy& e : enemies) {
        if (e.hitFlash > 0) --e.hitFlash;
        switch (e.kind) {
            case EK_CORSAIR: {
                e.sway += 0.05f;
                e.x += e.vx;
                e.y += e.vy + std::sin(e.sway) * 0.5f;
                // Off either side: turn the run around (a new pass, same craft — continuous
                // motion, no teleport, so the key needs no epoch).
                if ((e.vx > 0 && e.x > kViewW + kEnemyW[EK_CORSAIR]) ||
                    (e.vx < 0 && e.x < -kEnemyW[EK_CORSAIR])) {
                    e.vx = -e.vx;
                    e.y  = kFieldTopF + 60.0f + frand(rng) * (kViewH - kFieldTopF - 160.0f);
                }
                e.y = std::clamp(e.y, kFieldTopF + 20.0f, kViewH - 20.0f);
                // The aimed bolt: one straight, readable line at where you ARE — dodge by moving.
                if (--e.fireTimer <= 0) {
                    e.fireTimer = static_cast<int>(kCorsairFireEvery * fireRateMul);
                    const float d = distTo(e.x, e.y, ferryX, ferryY);
                    if (d > 1.0f && e.x > 0 && e.x < kViewW) {
                        bolts.push_back(Bolt{.id = nextId_++, .x = e.x, .y = e.y,
                                             .vx = (ferryX - e.x) / d * kEnemyBoltSpeed,
                                             .vy = (ferryY - e.y) / d * kEnemyBoltSpeed});
                    }
                }
                break;
            }
            case EK_WARDEN: {
                e.orbit += kWardenOrbit;
                const float tx = e.anchorX + std::cos(e.orbit) * kWardenRadius;
                const float ty = e.anchorY + std::sin(e.orbit) * kWardenRadius;
                e.x += std::clamp(tx - e.x, -1.2f, 1.2f);
                e.y += std::clamp(ty - e.y, -1.2f, 1.2f);
                // The 6-bolt ring: pure geometry, phase-locked to its orbit — read it and slip
                // between the spokes.
                if (--e.fireTimer <= 0) {
                    e.fireTimer = static_cast<int>(kWardenFireEvery * fireRateMul);
                    for (int k = 0; k < 6; ++k) {
                        const float a = e.orbit + static_cast<float>(k) * 60.0f * kDegToRad;
                        bolts.push_back(Bolt{.id = nextId_++, .x = e.x, .y = e.y,
                                             .vx = std::cos(a) * kEnemyBoltSpeed * 0.9f,
                                             .vy = std::sin(a) * kEnemyBoltSpeed * 0.9f});
                    }
                }
                break;
            }
            case EK_DREAD: {
                e.x += e.vx;
                if ((e.vx > 0 && e.x > kViewW + kEnemyW[EK_DREAD]) ||
                    (e.vx < 0 && e.x < -kEnemyW[EK_DREAD])) {
                    e.vx = -e.vx;
                    e.y  = kFieldTopF + 90.0f + frand(rng) * 120.0f;
                }
                // The 3-bolt fan at the ferry: a center aim flanked by fixed spreads.
                if (--e.fireTimer <= 0) {
                    e.fireTimer = static_cast<int>(kDreadFireEvery * fireRateMul);
                    const float d = distTo(e.x, e.y, ferryX, ferryY);
                    if (d > 1.0f && e.x > 0 && e.x < kViewW) {
                        const float base = std::atan2(ferryY - e.y, ferryX - e.x);
                        for (int k = -1; k <= 1; ++k) {
                            const float a = base + static_cast<float>(k) * kFanSpreadDeg * kDegToRad;
                            bolts.push_back(Bolt{.id = nextId_++, .x = e.x, .y = e.y,
                                                 .vx = std::cos(a) * kEnemyBoltSpeed,
                                                 .vy = std::sin(a) * kEnemyBoltSpeed});
                        }
                    }
                }
                break;
            }
            default: {  // EK_MUTANT: the contact hunter — no bullets, no mercy, no lanes
                if (e.leaving) {           // the ferry died: flee off-screen on the baked velocity
                    e.x += e.vx;
                    e.y += e.vy;
                } else {
                    const float dx = ferryX - e.x, dy = ferryY - e.y;
                    const float d  = std::hypot(dx, dy);
                    if (d > 0.5f) {
                        e.x += dx / d * kMutantSpeed;
                        e.y += dy / d * kMutantSpeed;
                    }
                }
                break;
            }
        }
    }
    // Reap fleeing mutants once they are fully off the side of the screen.
    std::erase_if(enemies, [](const Enemy& e) {
        return e.kind == EK_MUTANT && e.leaving &&
               (e.x < -kEnemyW[EK_MUTANT] - 20.0f || e.x > kViewW + kEnemyW[EK_MUTANT] + 20.0f);
    });
}

void FerrymanGame::cargoFire() {
    // THE SEED OF THE ARSENAL: one volley clock whose period divides by the souls aboard — each
    // passenger is another gunner on the rail. The bolt aims at the nearest enemy; the player
    // has no fire button. (The full game grows this into the whole build system.)
    if (deck.empty()) {
        cargoFireTimer_ = kCargoFirePeriod;
        return;
    }
    if (--cargoFireTimer_ > 0) return;
    cargoFireTimer_ = kCargoFirePeriod / static_cast<int>(deck.size());

    float best = 1.0e9f;
    const Enemy* target = nullptr;
    for (const Enemy& e : enemies) {
        const float d = distTo(ferryX, ferryY, e.x, e.y);
        if (d < best) {
            best   = d;
            target = &e;
        }
    }
    if (target == nullptr) return;
    bolts.push_back(Bolt{.id = nextId_++, .x = ferryX, .y = ferryY - 8.0f,
                         .vx = (target->x - ferryX) / best * kCargoBoltSpeed,
                         .vy = (target->y - ferryY + 8.0f) / best * kCargoBoltSpeed,
                         .friendly = true});
    emit(GameEventKind::CargoFire, ferryX, ferryY);
}

void FerrymanGame::enemyDown(std::size_t idx) {
    const Enemy e = enemies[idx];
    enemies.erase(enemies.begin() + static_cast<std::ptrdiff_t>(idx));
    const int pts = kEnemyScore[static_cast<std::size_t>(e.kind)];
    score += pts;
    emit(GameEventKind::EnemyDown, e.x, e.y, pts);
    if (e.kind != EK_MUTANT)  // craft re-enter; a downed mutant is simply gone
        respawnQueue.emplace_back(e.kind, kEnemyRespawnTicks);
}

void FerrymanGame::boltPhase() {
    for (std::size_t b = 0; b < bolts.size();) {
        Bolt& bolt = bolts[b];
        bolt.x += bolt.vx;
        bolt.y += bolt.vy;
        bool spent = bolt.x < -20.0f || bolt.x > kViewW + 20.0f || bolt.y < kFieldTopF - 20.0f ||
                     bolt.y > kViewH + 20.0f;

        if (!spent && bolt.friendly) {
            // The crew's gold bolts: down a craft or mutant…
            for (std::size_t k = 0; k < enemies.size(); ++k) {
                Enemy& e = enemies[k];
                if (boxesOverlap(bolt.x, bolt.y, kBoltBox, kBoltBox, e.x, e.y,
                                 kEnemyW[static_cast<std::size_t>(e.kind)],
                                 kEnemyH[static_cast<std::size_t>(e.kind)])) {
                    e.hitFlash = 8;
                    if (--e.hp <= 0) enemyDown(k);
                    spent = true;
                    break;
                }
            }
            // …and a strike on a beam-lit abductor is a foil too (the crew defends its own).
            if (!spent) {
                for (int i = 0; i < abductorCount(); ++i) {
                    auto&      abd = abductors[static_cast<std::size_t>(i)];
                    const Vec2 ap  = abd.pos();
                    if (abd.state() == AbductorState::Away ||
                        abd.state() == AbductorState::Fleeing) {
                        continue;
                    }
                    if (boxesOverlap(bolt.x, bolt.y, kBoltBox, kBoltBox, ap.x, ap.y, kAbductorW,
                                     kAbductorH)) {
                        if (auto& held = carried[static_cast<std::size_t>(i)]) {
                            placeGrounded(*held, ap.x, ap.y + 20.0f, static_cast<int>(kStunTicks));
                            held.reset();
                        }
                        abd.foil(static_cast<int>(kAbductorReentry));
                        score += kFoilScore;
                        emit(GameEventKind::Foil, ap.x, ap.y, kFoilScore);
                        spent = true;
                        break;
                    }
                }
            }
        } else if (!spent && invulnLeft == 0 &&
                   boxesOverlap(bolt.x, bolt.y, kBoltBox, kBoltBox, ferryX, ferryY, kFerryBoxW,
                                kFerryBoxH)) {
            ferryDeath();
            return;  // the tick's remaining contacts are moot (respawn or title)
        }

        if (spent) {
            bolts.erase(bolts.begin() + static_cast<std::ptrdiff_t>(b));
        } else {
            ++b;
        }
    }
}

void FerrymanGame::ferryDeath() {
    emit(GameEventKind::FerryDeath, ferryX, ferryY);
    // The passengers spill where the ferry went down — stunned, treading water, recoverable,
    // and exposed. Identity (and so each sprite key) survives the swim.
    int spread = 0;
    for (int id : deck) {
        for (std::size_t k = 0; k < colonists.size(); ++k) {
            if (colonists[k].id != id) continue;
            Colonist c = colonists[k];
            colonists.erase(colonists.begin() + static_cast<std::ptrdiff_t>(k));
            const int sx = spread % 3 - 1, sy = spread / 3;
            placeGrounded(c, ferryX + static_cast<float>(sx) * 22.0f,
                          ferryY + static_cast<float>(sy) * 18.0f, static_cast<int>(kStunTicks));
            ++spread;
            break;
        }
    }
    deck.clear();
    bolts.clear();  // the moment of the sinking clears the air — a fair restart, not a trap
    // The mutant's hunt ends with the ferry it hunted: on ANY death it turns and FLEES off the
    // NEAREST side toward a random Y ("my job here is done"), then despawns once fully off-screen
    // (enemyPhase). It stops hunting immediately. (Pending mutants in the queue still mature.)
    for (Enemy& e : enemies) {
        if (e.kind != EK_MUTANT || e.leaving) continue;
        e.leaving          = true;
        const float exitX  = e.x < kViewW / 2.0f ? -kEnemyW[EK_MUTANT] - 30.0f
                                                 : kViewW + kEnemyW[EK_MUTANT] + 30.0f;
        const float exitY  = kFieldTopF + frand(rng) * (kViewH - kFieldTopF);
        const float ex     = exitX - e.x, ey = exitY - e.y;
        const float ed     = std::hypot(ex, ey);
        e.vx = ed > 0.5f ? ex / ed * kMutantFleeSpeed : -kMutantFleeSpeed;
        e.vy = ed > 0.5f ? ey / ed * kMutantFleeSpeed : 0.0f;
    }
    if (--lives <= 0) {
        std::printf("game over — score %d — back to title\n", score);
        state = GameState::Title;
        return;
    }
    respawnFerry();
}

void FerrymanGame::resolveContacts() {
    // Enemy contact — only the MUTANT is lethal to touch (a water hunter). The flying craft pass
    // OVER the boat: they never collide, only their bullets bite (handled in boltPhase).
    if (invulnLeft == 0) {
        for (const Enemy& e : enemies) {
            if (e.kind != EK_MUTANT || e.leaving) continue;  // a fleeing mutant is done — harmless
            if (boxesOverlap(ferryX, ferryY, kFerryBoxW, kFerryBoxH, e.x, e.y,
                             kEnemyW[static_cast<std::size_t>(e.kind)] * 0.85f,
                             kEnemyH[static_cast<std::size_t>(e.kind)] * 0.85f)) {
                ferryDeath();
                return;
            }
        }
    }

    // Pickup: DOCK against an island and its waiting souls come aboard — no button, and never by
    // sailing THROUGH a colonist (the island is solid; its coast is the jetty). A boarding soul
    // takes the rail immediately: the first aboard fires within half a second, and every boarding
    // pulls the next volley forward to the heavier deck's cadence (never waits out the old timer).
    if (deck.size() < static_cast<std::size_t>(kDeckCap)) {
        for (const IsletSpec& islet : islets) {
            if (!ferryTouchesIslet(islet)) continue;
            for (Colonist& c : colonists) {
                if (deck.size() >= static_cast<std::size_t>(kDeckCap)) break;
                if (c.state != ColonistState::Waiting) continue;
                if (!colonistOnIslet(c, islet)) continue;
                c.state = ColonistState::Aboard;
                deck.push_back(c.id);
                const int cadence = kCargoFirePeriod / static_cast<int>(deck.size());
                cargoFireTimer_ =
                    std::min(cargoFireTimer_, deck.size() == 1 ? kFirstVolleyTicks : cadence);
                emit(GameEventKind::Pickup, c.x, c.y);
            }
        }
    }

    // Banking: DOCK against the sanctuary's coast with souls aboard and the whole deck delivers —
    // slot i pays i × 50, so the heavy crossing is the rich one. (The hull can't enter the band;
    // it presses the coast, so bank on touch — the same dock-to-transfer rule as the islets.)
    if (ferryY <= kSanctuaryBottom + hullH() / 2.0f + kCoastTouch && !deck.empty()) {
        const int pts = haulPays();
        const int n   = static_cast<int>(deck.size());
        score += pts;
        rescued += n;
        for (int id : deck)
            std::erase_if(colonists, [id](const Colonist& c) { return c.id == id; });
        deck.clear();
        emit(GameEventKind::Bank, ferryX, kSanctuaryBottom - 20.0f, pts);
    }
}

void FerrymanGame::tick(const InputState& in) {
    events_.clear();

    // Title: the Menu action (Return / numpad Enter / gamepad Start or west face) starts a
    // fresh game.
    if (state == GameState::Title) {
        if (in.justPressed(Action::Menu)) {
            state = GameState::Playing;
            newGame();
        }
        return;
    }

    // The pause menu. Menu opens it and, once open, confirms the highlighted choice; up/down move
    // the selection. While paused the whole sim is frozen (main also freezes feel + the parallax),
    // so reusing the sail actions for menu navigation is harmless.
    const bool menuButton = in.justPressed(Action::Menu);
    if (paused) {
        if (in.justPressed(Action::SailUp))   pauseChoice = 0;
        if (in.justPressed(Action::SailDown)) pauseChoice = 1;
        if (menuButton) {
            paused = false;                                   // RESUME, or…
            if (pauseChoice == 1) state = GameState::Title;   // …QUIT TO TITLE (the score carries)
        }
        return;  // frozen while the menu is up
    }
    if (menuButton) {
        paused      = true;
        pauseChoice = 0;
        return;
    }

    moveInput(in);
    // No drop button: souls board by docking at an islet and leave the deck only at the sanctuary.

    colonistFlow();
    abductorPhase();
    enemyPhase();
    cargoFire();
    boltPhase();
    if (state == GameState::Title) return;  // a bolt on the last life ended the game
    resolveContacts();
    if (state == GameState::Title) return;

    // Quota met → a short lull (the round card holds the stage) → the next, denser crossing.
    // Waiting colonists persist — the sea's souls carry over.
    if (rescued >= quota() && waveLull == 0) {
        waveLull = static_cast<int>(kWaveLullTicks);
        emit(GameEventKind::WaveClear, kViewW / 2.0f, kSanctuaryBottom);
        std::printf("wave %d cleared — score %d\n", wave, score);
    }
    if (waveLull > 0 && --waveLull == 0) spawnWave();

    if (invulnLeft > 0) --invulnLeft;
}

}  // namespace ferryman

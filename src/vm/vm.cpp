// The generic VM host. System-agnostic: it owns one VmBackend chosen by VMPlatform and
// drives it through the abstract seam. No SM83 / Game Boy / SameBoy idiom appears here — that lives
// in the concrete backend (src/vm/sameboy_backend.cpp). Adding a system is adding a backend + a
// factory case; this file does not change.
#include "retropp/vm.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "retropp/asset_policy.h"      // resolveAssetPolicy
#include "retropp/asset_registry.h"    // assetRoot — the single project-relative resource root (no routine root)
#include "retropp/routine_registry.h"  // detail::findEmbeddedRoutine
#include "retropp/user_files.h"        // UserFiles — where a keyed machine's own files live
#include "src/vm/run_governor.h"       // RunGovernor — what a running cartridge owes the wall clock
#include "src/vm/save_writer.h"        // SaveWriter — the thread a keyed machine's bytes reach disk on
#include "src/vm/vm_backend.h"
#include "src/vm/vm_runner.h"          // VmRunner — the thread a running cartridge steps on
#include "src/vm/vm_testing.h"         // VmTestAccess — the deterministic seam, defined at file end

namespace retropp {

// How a registered routine is called. A routine the engine placed gets a frame of the engine's own —
// the register file replaced, the stack seated at a scratch top — which is why it cannot run while a
// guest frame is live. A routine bound where it already sits runs in whatever context the machine is
// already in, and gives that context back.
enum class CallForm { OwnFrame, InContext };

// A registration resolved at registerRoutine time: the entry address its bytes were placed at, and
// the per-input / output locations + widths the call path marshals against.
struct ResolvedRoutine {
    std::uint32_t           entry;
    std::vector<Location>   inputs;
    std::vector<int>        inputWidths;
    std::optional<Location> output;
    int                     outputWidth;
    Throttle                throttle;  // HostSpeed = called for a value; HardwareSpeed = driven (audio)
    CallForm                form = CallForm::OwnFrame;
};

// The resolved state of the resident driver hosted on a VM (set by hostDriver): where the per-frame
// tick lives and the declared slots readSlot indexes. The images live in the backend's cartridge image.
struct HostedDriverState {
    bool                  hosted = false;
    std::uint32_t         tickEntry = 0;
    std::vector<SlotSpec> slots;
};

// One declared place's berth in the run publish: where it lives in the machine and where its bytes
// land in the published block.
struct PublishedRegion {
    MemoryRegion where;
    std::size_t  offset;
};

// A write waiting to cross to the running machine's own thread. Validated against the declared
// place at the call, applied in issue order at the next step boundary.
struct PendingRegionWrite {
    MemoryRegion              where;
    std::uint32_t             index;
    std::vector<std::uint8_t> bytes;
};

// Everything Vm::run() stands up: the governor owed cycles accrue against, the seqlock publish of
// the declared places, the write channel, and the runner whose thread steps the machine. The runner
// exists exactly while the machine runs — stop() takes the thread down with it — so a machine with
// no runner is quiescent and every direct-access path is safe.
struct RomRunState {
    bool booted = false;  // the image booted once; run() after stop() resumes rather than re-boots

    std::optional<vm::RunGovernor> governor;  // survives across run/stop episodes (factor + carry)
    vm::MachineClock governorClock{};  // the clock `governor` was built from

    // The seqlock publish (the DriverSnapshot idiom): even = stable, odd = a publish is in flight.
    // `published` is laid out by `table` and sized once per run(), written in place by the stepping
    // thread after each step, read wait-free by the game thread.
    std::atomic<std::uint32_t>   seq{0};
    std::vector<std::uint8_t>    published;
    std::vector<PublishedRegion> table;

    std::mutex                      writeMx;
    std::vector<PendingRegionWrite> pendingWrites;

    // The thread stepping the machine, recorded by the machine's own step. A call into the guest is
    // made from there or not at all — the game thread's verbs cross to it, and a call does not.
    std::atomic<std::thread::id> steppingThread{};

    std::unique_ptr<vm::VmRunner> runner;
};

// A replacement's argument count is bounded so the fire path allocates nothing: sixteen is far past
// any register file's worth of distinct argument homes, and registration refuses a longer binding.
constexpr std::size_t kMaxReplacementInputs = 16;

// One escape as the host layer holds it: the game's key, what runs (a handler, or a native routine
// answering by its binding), and the address the backend watches. The key owns its bytes, so a key
// built at runtime outlives the call that declared it.
struct DeclaredEscape {
    std::string   key;
    std::uint32_t at = 0;
    EscapeHandler handler;
    NativeRoutine replaces;
    bool          armed = true;
};

// A change to the escape table issued from the game thread while the machine runs. Switching an
// escape and dropping one both change the machine's own code — the backend watches an armed
// address, and an answering escape holds a return at it — so the change crosses to the thread that
// owns the machine and lands at the next step boundary, in issue order, exactly as a declared write
// does. Issued from the stepping thread itself, inside a handler, it applies at once instead: that
// thread already owns the machine.
struct PendingEscapeChange {
    enum class Kind : std::uint8_t { Arm, Disarm, Remove };

    std::string key;
    Kind        kind = Kind::Arm;
};

// One watch as the host layer holds it: the game's key, the place the backend watches, what answers
// in each direction, and whose accesses fire it. The key owns its bytes, so a key built at runtime
// outlives the call that declared it.
struct DeclaredWatch {
    std::string  key;
    MemoryRegion at{};
    WatchHandler onRead;
    WatchHandler onWrite;
    AccessSource from  = AccessSource::Guest;
    bool         armed = true;
};

// A change to the watch table issued from the game thread while the machine runs. Switching a watch
// and dropping one both change what the machine's own memory path tests on every access, so the
// change crosses to the thread that owns the machine and lands at the next step boundary, in issue
// order, exactly as a declared write does. Issued from the stepping thread itself, inside a handler,
// it applies at once instead: that thread already owns the machine.
struct PendingWatchChange {
    enum class Kind : std::uint8_t { Arm, Disarm, Remove };

    std::string key;
    Kind        kind = Kind::Arm;
};

// Records that a guest hook — an escape, or a watch — holds the machine, for as long as it does,
// and gives back whatever was true before. Scoped rather than counted: a hook reached from inside a
// hook still holds the machine, so the answer is the same at every depth.
//
// What it decides is whose stack a call into the guest pushes on. Both kinds park the guest
// mid-instruction, with a live context to give back to, so both answer the same way.
class GuestHookScope {
public:
    explicit GuestHookScope(bool& flag) noexcept : flag_(flag), previous_(flag) { flag_ = true; }
    ~GuestHookScope() { flag_ = previous_; }
    GuestHookScope(const GuestHookScope&)            = delete;
    GuestHookScope& operator=(const GuestHookScope&) = delete;

private:
    bool& flag_;
    bool  previous_;
};

// A call in the guest's context runs until the routine returns; this is the runaway guard for one
// that never does, in the shape and at the size the machine's own run-to-return carries.
constexpr std::size_t kMaxContextInstructions = 1'000'000;

// ── How often a keyed machine's data is taken from it ─────────────────────────────────────────────
//
// The longest the data stays unwritten while the guest keeps changing it. What this bounds is the
// SNAPSHOT — a copy of the machine's own memory, taken on the machine's thread — and not the write,
// which costs that thread nothing (save_writer.h). So the interval is picked against the copy: a
// guest that scribbles on its save memory every frame is snapshotted once a second rather than sixty
// times, and a player who is interrupted loses at most this much of what they had just done.
constexpr std::chrono::milliseconds kSaveInterval{1000};

// Where a keyed machine's files live, inside the game's own per-user data directory:
//
//     VM/<the machine's key>/<the save's name>
//
// The default file name says which KIND of save it is — the one a cartridge's battery holds, not a
// snapshot of the machine — and a machine that plays more than one cartridge replaces it per image
// through Vm::batterySave.
constexpr std::string_view kSaveDirectory       = "VM";
constexpr std::string_view kDefaultSaveDocument = "battery";

// Both names a machine carries — the machine's own, and its save's — are ONE path component, never a
// path: the rule SaveStore's document names carry, for the same reason. A name that could be a path is
// a name that could name somewhere else. Refusing it here puts the error where the name was given
// rather than at the first write.
void requireFlatName(std::string_view name, const char* what) {
    if (name.empty()) {
        throw std::invalid_argument(std::string(what) + " is empty");
    }
    if (name == "." || name == "..") {
        throw std::invalid_argument(std::string(what) + " is \"" + std::string(name) +
                                    "\", which names a directory rather than a thing in one");
    }
    if (name.find_first_of("/\\:") != std::string_view::npos) {
        throw std::invalid_argument(std::string(what) + " \"" + std::string(name) +
                                    "\" contains a path separator — it is one name, not a path");
    }
}

struct Vm::Impl {
    VMPlatform                   platform;
    TimingProfile                timing;  // the profile this Vm was constructed with; nothing here reads it
    std::unique_ptr<vm::VmBackend> backend;
    std::vector<ResolvedRoutine> routines;
    HostedDriverState            driver;

    // The Vm this Impl belongs to, kept current across a move so an escape handler is handed the
    // machine at its present address rather than where it was declared.
    Vm* owner = nullptr;

    // Declared escapes, in declaration order. The backend watches the armed ones' addresses and
    // reports each fire back here; the keys, the handlers and the arming all live at this layer.
    //
    // The declarations keep their addresses for as long as they are declared, which is what lets a
    // running escape declare another one: the code being run lives in the entry that is running, and
    // a container that relocated its elements would free it mid-call.
    //
    // `escapeMx` guards the list — its structure and each entry's `armed` — and the pending changes
    // below, against the one thread that can contend for them: the machine's own, while it runs. The
    // dispatch path takes it to FIND the entry that fired and lets it go before running the game's
    // code, because a handler is free to ask the machine about its escapes and a lock held across
    // the call would meet itself. `registerEscapes` takes none of it — that verb refuses a running
    // machine, so no stepping thread exists for any of it to race with.
    std::mutex                       escapeMx;
    std::list<DeclaredEscape>        escapes;
    std::vector<PendingEscapeChange> pendingEscapes;
    bool                             escapeSinkInstalled = false;

    // Declared watches, on exactly the terms above. The list keeps its elements where they are for
    // as long as they are declared, which is what lets a running handler declare another watch: the
    // code being run lives in the entry that is running, and a container that relocated its elements
    // would free it mid-call.
    //
    // `watchMx` guards the list and the pending changes against the one thread that can contend for
    // them, and the dispatch path takes it to FIND the entry that fired and lets it go before
    // running the game's code — a handler is free to ask the machine about its watches, and a lock
    // held across the call would meet itself. registerWatches takes none of it: that verb refuses a
    // running machine.
    std::mutex                      watchMx;
    std::list<DeclaredWatch>        declaredWatches;
    std::vector<PendingWatchChange> pendingWatches;
    bool                            watchSinkInstalled = false;

    // The machine's picture, in three buffers so the thread that draws and the thread that composes
    // never touch one at the same time:
    //
    //   filling  the frame sink's own, written wherever the machine is stepped
    //   ready    the hand-off slot, exchanged under `mx`
    //   shown    the game thread's own — what video() answers with
    //
    // A completed frame goes filling → ready under the lock; a latch takes ready → shown under it. The
    // lock is never held across a copy, only across a swap of vector handles, so a machine drawing on
    // its own thread never waits on the game's compose and the game never waits on the machine.
    //
    // WHERE THE LATCH HAPPENS IS WHAT EACH CLOCK OFFERS. A tick-advanced machine latches at the tick
    // boundary, so a frame finishing part-way through a tick waits there the way a queued write does. A
    // machine on a clock of its own has no such boundary — it does not take advanceTick at all — so it
    // latches when the game asks, and what it answers with is the newest frame it has finished. Nothing
    // downstream tells the two apart: both report a generation, and that is the whole signal.
    struct Video {
        std::mutex                mx;       // guards ready + its dimensions + `completed`, nothing else
        std::vector<std::uint8_t> filling;  // the machine's own; no lock, one thread
        std::vector<std::uint8_t> ready;    // the hand-off slot
        int               readyWidth  = 0;
        int               readyHeight = 0;
        RasterPixelFormat readyFormat = RasterPixelFormat::Rgba8888;
        bool              readyHeld   = false;  // `ready` holds a frame nothing has latched yet

        std::vector<std::uint8_t> shown;  // the game thread's own
        int               width  = 0;
        int               height = 0;
        RasterPixelFormat format = RasterPixelFormat::Rgba8888;
        // How many frames the machine has finished. Written by the drawing thread under `mx` and read
        // into `shownGeneration` at a latch, so the game sees it advance only where it latches — by one
        // per frame, or by several when several landed between two latches.
        std::uint64_t completed = 0;
        std::uint64_t shownGeneration = 0;
        // Whether the machine draws. Read on the game thread by video(), written on whichever thread
        // applies the change — the game's own for a parked machine, the machine's own for a running one.
        std::atomic<bool> on{false};
        // A video(bool, options) issued while the machine runs, waiting for a step boundary to be applied
        // on the machine's own thread. Guarded by `mx`. Two calls before one boundary merge: a member the
        // later call sets wins, one it leaves unset keeps the earlier call's.
        bool         pendingChange     = false;
        bool         pendingChangeWant = false;
        VideoOptions pendingOptions;
        // The machine's video settings, as the last video(bool, options) left them. Written only where
        // the sink is installed — the thread that owns the machine — and read by the sink on that same
        // thread, so nothing guards them.
        bool  interlacing = false;
        Weave weave       = Weave::Straight;
        // The woven picture while interlacing is on: twice a field's height, each field's lines at their
        // own parity. `wovenSeen` says a field has landed in it since it was built or dropped. The sink's
        // own, one thread.
        std::vector<std::uint8_t> woven;
        int  wovenWidth  = 0;
        int  wovenHeight = 0;      // a field's height; the picture is twice this
        bool wovenSeen   = false;
    };
    Video video;

    // Button state the game submitted, waiting for a step boundary. A level rather than a queue: a
    // second submission before the boundary replaces the first, because what the guest reads is what
    // is held now and never the order it was pressed in.
    struct Held {
        std::mutex    mx;
        std::uint64_t latest  = 0;
        bool          pending = false;
    };
    Held buttons;

    // The step boundary's input step: hand the machine the buttons held as of now.
    void drainButtons() {
        std::uint64_t held = 0;
        {
            const std::lock_guard guard{buttons.mx};
            if (!buttons.pending) {
                return;
            }
            held            = buttons.latest;
            buttons.pending = false;
        }
        backend->setButtons(held);
    }

    // Install or detach the frame sink and turn the backend's pixel output on or off. Touches the
    // machine, so it runs on whichever thread owns it: the game's for a parked machine, and the
    // machine's own — from a step boundary — for a running one. Installing a sink from the game thread
    // while the machine's own thread is calling it is a race, which is the whole reason for the queue.
    void applyVideo(bool drawing, const VideoOptions& options) {
        // The settings first, on the owning thread, whether or not the machine draws: a setting given
        // with the switch off is held for when it is next on.
        if (options.interlacing.on) {
            video.interlacing = *options.interlacing.on;
        }
        if (options.interlacing.type) {
            video.weave = *options.interlacing.type;
        }
        if (!drawing) {
            backend->setVideoEnabled(false);
            backend->setFrameSink({});
            video.on.store(false, std::memory_order_relaxed);
            video.wovenSeen = false;  // the fields held describe a picture that is over
            return;
        }
        // The sink fires on whatever thread steps the machine — the game's own under Advance::OnTick,
        // the machine's own under Advance::Continuously. It copies the finished frame into the buffer
        // it owns and hands it over; the hand-off is the only thing the two threads share.
        backend->setFrameSink([impl = this](std::span<const std::uint8_t> pixels, int width, int height,
                                            RasterPixelFormat format, vm::FrameField field) {
            impl->takeFrame(pixels, width, height, format, field);
        });
        backend->setVideoEnabled(true);
        video.on.store(true, std::memory_order_relaxed);
    }

    // The frame sink's body, on the thread that steps the machine. A whole frame, or a field while
    // interlacing is off, is copied as it is; a field while interlacing is on lands at its parity in the
    // woven picture, and the whole picture is what is handed over. Either way the hand-off is one swap
    // under the lock.
    void takeFrame(std::span<const std::uint8_t> pixels, int width, int height, RasterPixelFormat format,
                   vm::FrameField field) {
        int shownHeight = height;
        if (!video.interlacing || field == vm::FrameField::Whole) {
            video.wovenSeen = false;  // the fields held do not describe this picture
            video.filling.assign(pixels.begin(), pixels.end());
        } else {
            weaveField(pixels, width, height, format, field);
            shownHeight = 2 * height;
        }
        const std::lock_guard guard{video.mx};
        video.filling.swap(video.ready);
        video.readyWidth  = width;
        video.readyHeight = shownHeight;
        video.readyFormat = format;
        video.readyHeld   = true;
        ++video.completed;
    }

    // Land one field in the woven picture and produce the picture to hand over into `filling`. Line y of
    // the field of parity p is line 2y + p of the picture. The first field after the picture is built or
    // dropped fills both parities, so no line shows nothing until the other field arrives; a field whose
    // size differs from the picture's rebuilds it.
    void weaveField(std::span<const std::uint8_t> pixels, int width, int height, RasterPixelFormat format,
                    vm::FrameField field) {
        const std::size_t row    = static_cast<std::size_t>(width) * bytesPerPixel(format);
        const std::size_t parity = field == vm::FrameField::Odd ? 1 : 0;
        const bool rebuild = !video.wovenSeen || video.wovenWidth != width || video.wovenHeight != height;
        if (rebuild) {
            video.woven.assign(row * static_cast<std::size_t>(height) * 2, 0);
            video.wovenWidth  = width;
            video.wovenHeight = height;
            video.wovenSeen   = true;
        }
        for (std::size_t y = 0; y < static_cast<std::size_t>(height); ++y) {
            const std::uint8_t* line = pixels.data() + y * row;
            std::copy_n(line, row, video.woven.data() + (2 * y + parity) * row);
            if (rebuild) {
                std::copy_n(line, row, video.woven.data() + (2 * y + 1 - parity) * row);
            }
        }
        if (video.weave == Weave::Straight) {
            video.filling.assign(video.woven.begin(), video.woven.end());
            return;
        }
        // Blend: each line averaged with the line below it, byte by byte; the last line with itself.
        const std::size_t rows = static_cast<std::size_t>(height) * 2;
        video.filling.resize(video.woven.size());
        for (std::size_t r = 0; r < rows; ++r) {
            const std::uint8_t* a   = video.woven.data() + r * row;
            const std::uint8_t* b   = video.woven.data() + (r + 1 < rows ? r + 1 : r) * row;
            std::uint8_t*       out = video.filling.data() + r * row;
            for (std::size_t i = 0; i < row; ++i) {
                out[i] = static_cast<std::uint8_t>((static_cast<unsigned>(a[i]) + b[i]) / 2u);
            }
        }
    }

    // Whether this machine has been asked to draw — counting a request that has not reached a step
    // boundary yet. A game that asks a running machine to draw and reads its picture in the same breath
    // is early, not wrong: it gets the empty picture it would get before the first frame, and the answer
    // fills in once the machine has drawn one.
    [[nodiscard]] bool videoOrAsked() {
        if (video.on.load(std::memory_order_relaxed)) {
            return true;
        }
        const std::lock_guard guard{video.mx};
        return video.pendingChange && video.pendingChangeWant;
    }

    // The step boundary's video step: apply a change the game asked for while the machine was
    // running, on the thread that owns the machine.
    void drainVideoChange() {
        bool         wanted = false;
        VideoOptions options;
        {
            const std::lock_guard guard{video.mx};
            if (!video.pendingChange) {
                return;
            }
            wanted  = video.pendingChangeWant;
            options = video.pendingOptions;
        }
        applyVideo(wanted, options);
        // Cleared only AFTER the change has landed, so a reader is never caught between the request
        // being taken and the machine reporting that it draws — it sees one or the other, never a gap.
        const std::lock_guard guard{video.mx};
        video.pendingChange  = false;
        video.pendingOptions = VideoOptions{};
    }

    // Take the newest completed frame, if one is waiting. Called at the tick boundary for a machine the
    // game advances, and from video() for one advancing on a clock of its own.
    void latchVideo() {
        if (!video.on.load(std::memory_order_relaxed)) {
            return;
        }
        const std::lock_guard guard{video.mx};
        if (!video.readyHeld) {
            return;
        }
        video.shown.swap(video.ready);
        video.width           = video.readyWidth;
        video.height          = video.readyHeight;
        video.format          = video.readyFormat;
        video.shownGeneration = video.completed;
        video.readyHeld       = false;
    }

    // Whether a guest hook holds the machine right now — set for the length of a dispatch, restored
    // afterwards. A call into the guest asks it whose stack to use: an escape or a watch holds a
    // guest that is mid-instruction, with a live stack to push onto.
    bool inGuestHook = false;
    // Registered region batches, in registration order; a RegionMapId holds an index into this.
    std::vector<std::vector<DeclaredRegion>> regionBatches;
    // Sub-cycle remainder carried between advanceTick calls, so a machine whose clock does not
    // divide the tick period stays exact over any number of ticks.
    std::uint64_t cycleCarryNs = 0;

    // The speed factor's own remainder, carried on the same terms: scaling an exact cycle count by a
    // rational leaves a fraction of a cycle behind, and a later tick spends it rather than rounding
    // it away. It is held against kCarryScale × the denominator it accrued under, so a change of
    // factor re-denominates it for a millionth of a cycle instead of a fraction of one.
    static constexpr std::uint64_t kCarryScale = 1'000'000;
    std::uint64_t                  factorCarry    = 0;
    std::uint32_t                  factorCarryDen = 1;

    // What one tick of `enginePeriod` is worth to a running cartridge, at the factor as it stands.
    // Two exact conversions, each carrying its own remainder: the period into this machine's cycles,
    // then those cycles scaled by the factor. The machine's own clock answers the first, and at the
    // machine's own frame period it hands back the exact frame count — the exact hardware fact, where
    // deriving a count from the rounded period would lose a cycle every tick. At {1, 1} the scaling
    // is the identity and the budget is the draw.
    std::uint64_t tickBudget(std::chrono::nanoseconds enginePeriod) {
        const std::optional<vm::MachineClock> clock = backend->clock();
        if (!clock) {
            return 0;  // a core that keeps no clock has nothing a tick is worth
        }
        const CycleDraw draw = clock->cyclesFor(enginePeriod, cycleCarryNs);
        cycleCarryNs         = draw.carryNs;
        const std::pair<std::uint32_t, std::uint32_t> f =
            romRun.governor ? romRun.governor->factor()
                            : std::pair<std::uint32_t, std::uint32_t>{1u, 1u};
        if (f.second != factorCarryDen) {
            factorCarry    = factorCarry * f.second / factorCarryDen;
            factorCarryDen = f.second;
        }
        const std::uint64_t scale = kCarryScale * f.second;
        const std::uint64_t acc   = factorCarry + draw.cycles * f.first * kCarryScale;
        factorCarry               = acc % scale;
        return acc / scale;
    }

    bool romHosted = false;  // hostRom has run: the machine holds a game's own cartridge

    // ── Save data ────────────────────────────────────────────────────────────────────────────────
    // What a keyed machine keeps for the player. The bytes belong to the machine and are never read
    // here — this layer owns only WHEN they move and WHERE they land.
    //
    // Every field is touched on the thread that owns the machine: the game's own while it is parked
    // (construction, hostRom, stop), and the machine's own at a step boundary while it runs. There is
    // no moment when both could reach it, which is why none of it is guarded.
    struct SaveData {
        std::string key;  // empty: this machine has no key and keeps nothing

        // The file inside the machine's directory, replaced per cartridge by a machine that plays
        // more than one.
        std::string document{kDefaultSaveDocument};

        // Set to root the files at an explicit directory instead of the player's data directory —
        // what VmTestAccess uses to keep a case hermetic, the same seam UserFiles::atPath is.
        std::optional<std::filesystem::path> root;

        bool dirty = false;  // the guest has touched the data since the last snapshot
        std::chrono::steady_clock::time_point lastSnapshot{};

        // What the file already holds. A machine is asked whether its guest TOUCHED the data, which
        // is not the same question as whether the data is different: a guest that stores the same
        // checksum every frame touches it constantly and changes nothing. Keeping the last copy turns
        // the first answer into the second, so a file is rewritten only when rewriting it would
        // change it — which is what keeps a large save off the disk when nothing is happening to it.
        std::vector<std::uint8_t> lastWritten;
    };
    SaveData save;

    // Whether this machine keeps anything: it was named, and its core has somewhere to keep it.
    [[nodiscard]] bool keepsSave() const { return !save.key.empty() && backend->keepsSaveData(); }

    [[nodiscard]] UserFiles saveFiles() const {
        return save.root ? UserFiles::atPath(*save.root) : UserFiles();
    }

    // Where this machine's data goes: VM/<the machine's key>/<the file's name>.<the core's extension>.
    //
    // The extension is the CORE's, and it always ends the file — a name that already carries it is
    // already right and is left alone, and a name carrying some other extension keeps that and gets
    // this one after it. So a developer can spell it out or ignore it entirely and land in the same
    // place, and a file of a console's format is never called something that console's other programs
    // would not recognize.
    [[nodiscard]] std::string saveDocument() const {
        const std::string_view extension = backend->saveDataExtension();
        std::string            file      = save.document;
        if (!extension.empty()) {
            const std::string suffix = "." + std::string(extension);
            if (file.size() < suffix.size() ||
                file.compare(file.size() - suffix.size(), suffix.size(), suffix) != 0) {
                file += suffix;
            }
        }
        return std::string(kSaveDirectory) + "/" + save.key + "/" + file;
    }

    // Read the stored copy back into the machine. Called where the image becomes known — hosting one
    // is what decides how much this machine keeps — so the size is the loaded cartridge's own.
    //
    // A stored copy that is not the size this image keeps is left alone rather than forced in: it was
    // written by a different cartridge under the same name, and padding or truncating it would hand
    // the guest a corrupt save instead of a missing one.
    void loadSaveData() {
        if (!keepsSave()) {
            return;
        }
        const std::size_t size = backend->saveDataSize();
        if (size == 0) {
            return;  // this image keeps nothing
        }
        const std::optional<std::vector<std::byte>> stored = saveFiles().read(saveDocument());
        if (!stored || stored->size() != size) {
            return;
        }
        std::vector<std::uint8_t> bytes(stored->size());
        std::transform(stored->begin(), stored->end(), bytes.begin(),
                       [](std::byte b) { return static_cast<std::uint8_t>(b); });
        backend->writeSaveData(bytes);
        // The file and the machine now agree, so nothing is owed until the guest changes something.
        // Placing the bytes is not the guest touching them, so the signal that says it did is taken
        // and dropped here rather than being left to read as a change on the first step.
        save.lastWritten = std::move(bytes);
        static_cast<void>(backend->takeSaveDataChanged());
        save.dirty = false;
    }

    // Take the machine's own copy and hand it over to be written. The copy is taken HERE, on the
    // thread that owns the machine, because that is the only thread allowed to reach into it; the
    // write is somebody else's job (save_writer.h), so this costs a step the copy and nothing more.
    //
    // Bytes identical to the ones the file already holds are not handed over at all. The machine is
    // asked whether its guest TOUCHED the data, and a guest that stores the same value touched it
    // without changing it — so without this compare a cartridge nothing is happening to would have
    // its whole file rewritten on every interval, for as long as the game ran.
    //
    // `blocking` is the shutdown path: the caller may be about to go away, so the bytes have to be on
    // disk before this returns rather than with a thread that has not run yet.
    void handOverSaveData(bool blocking) {
        std::vector<std::uint8_t> bytes = backend->readSaveData();
        if (bytes.empty() || bytes == save.lastWritten) {
            return;
        }
        if (blocking) {
            if (!vm::SaveWriter::shared().writeNow(saveFiles(), saveDocument(), bytes)) {
                return;  // the file still holds what it held; the machine still holds the newer bytes
            }
        } else {
            vm::SaveWriter::shared().queue(saveFiles(), saveDocument(), bytes);
        }
        save.lastWritten = std::move(bytes);
    }

    // The step boundary's save step: notice that the guest touched the data, and take a copy no more
    // often than the interval allows. Runs after the step's own work, on the machine's own thread.
    void stepSaveData() {
        if (!keepsSave()) {
            return;
        }
        if (backend->takeSaveDataChanged()) {
            save.dirty = true;
        }
        if (!save.dirty) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - save.lastSnapshot < kSaveInterval) {
            return;
        }
        save.lastSnapshot = now;
        save.dirty        = false;
        handOverSaveData(/*blocking=*/false);
    }

    // Everything the guest changed, on disk before this returns, whatever the interval would have
    // said. This is what putting a machine away means for its data: a save the guest finished a
    // moment ago is not lost because the machine stopped before the interval was up.
    void flushSaveData() {
        if (!keepsSave()) {
            return;
        }
        if (backend->takeSaveDataChanged()) {
            save.dirty = true;
        }
        if (save.dirty) {
            save.dirty = false;
            handOverSaveData(/*blocking=*/true);
        }
        // What the guest changed MOST RECENTLY is now on disk — but a snapshot taken earlier in the run
        // went to the writer without blocking, and that thread may not have run yet. Putting a machine
        // away means its data is ON DISK, not merely handed over, so this waits for the rest of it: the
        // program may be about to end, and a machine that has been parked can be read back at once.
        vm::SaveWriter::shared().settle(saveFiles(), saveDocument());
    }

    // Declared after `backend` on purpose: members destroy in reverse order, so the runner (and its
    // thread) is gone before the machine it steps.
    RomRunState romRun;

    Impl(detail::CoreFactory core, VMPlatform p, TimingProfile t)
        : platform(p), timing(t), backend(core(p)) {}

    // Whether the hosted cartridge is running — the gate every machine-mutating verb checks, since
    // a running machine belongs to its own thread.
    [[nodiscard]] bool running() const noexcept { return romRun.runner != nullptr; }

    void requireNotRunning(const char* verb) const {
        if (running()) {
            throw std::logic_error(std::string(verb) +
                                   ": the machine is running its hosted cartridge; stop() first");
        }
    }

    // ── Escapes ──────────────────────────────────────────────────────────────────────────────────
    //
    // Every function in this block reads or writes the escape list, so every one of them is called
    // with `escapeMx` held.

    // Whether a change issued right now has to cross to the machine's own thread: the machine is
    // running, and this is not the thread stepping it. The rule is one rule — escapes and watches
    // both change what the running machine does, so both cross on the same terms.
    [[nodiscard]] bool changeCrosses() const noexcept {
        return running() &&
               std::this_thread::get_id() != romRun.steppingThread.load(std::memory_order_relaxed);
    }

    // Arm or disarm one entry against the backend. Doing nothing when it already stands that way is
    // what keeps a repeated switch from re-patching the machine's code.
    void applyEscapeArmed(DeclaredEscape& e, bool on) {
        if (e.armed == on) {
            return;
        }
        e.armed = on;
        if (on) {
            backend->armEscape(e.at, static_cast<bool>(e.replaces));
        } else {
            backend->disarmEscape(e.at);
        }
    }

    // Stepping thread, before each step: land the changes queued so far, in issue order. A change
    // whose escape is no longer declared has nothing left to do — it is skipped rather than reported,
    // because this runs where an exception has nowhere to go.
    void drainEscapeChanges() {
        const std::lock_guard<std::mutex> lock(escapeMx);
        if (pendingEscapes.empty()) {
            return;
        }
        std::vector<PendingEscapeChange> changes;
        changes.swap(pendingEscapes);
        for (const PendingEscapeChange& c : changes) {
            const auto at = std::find_if(escapes.begin(), escapes.end(),
                                         [&c](const DeclaredEscape& e) { return e.key == c.key; });
            if (at == escapes.end()) {
                continue;
            }
            if (c.kind == PendingEscapeChange::Kind::Remove) {
                applyEscapeArmed(*at, false);
                escapes.erase(at);
            } else {
                applyEscapeArmed(*at, c.kind == PendingEscapeChange::Kind::Arm);
            }
        }
    }

    [[nodiscard]] DeclaredEscape* findEscape(std::string_view key) noexcept {
        for (DeclaredEscape& e : escapes) {
            if (e.key == key) {
                return &e;
            }
        }
        return nullptr;
    }

    [[nodiscard]] DeclaredEscape& requireEscape(std::string_view key) {
        if (DeclaredEscape* e = findEscape(key)) {
            return *e;
        }
        throw std::out_of_range("escapes: this machine declares no escape named '" +
                                std::string(key) + "'");
    }

    // Which entry answers for the address that fired, or null if none is armed there. The lock is
    // held for the search alone: an entry stays where it is for as long as it is declared, so the
    // pointer outlives the lock, and the game's code runs without holding it.
    [[nodiscard]] DeclaredEscape* escapeArmedAt(std::uint32_t firedAt) {
        const std::lock_guard<std::mutex> lock(escapeMx);
        for (DeclaredEscape& e : escapes) {
            if (e.armed && e.at == firedAt) {
                return &e;
            }
        }
        return nullptr;
    }

    // Stepping thread: an armed address is about to execute. Runs the handler to completion before
    // the instruction executes; the guest's clock does not advance for it.
    //
    // The answering kind marshals synchronously against the PARKED machine — this thread owns it and
    // nothing moves while the escape runs, so reading the register file and live memory here is
    // coherent by construction. The guest's own calling code loaded the bound inputs before its call;
    // the bound output is where its callers read the answer; the instruction that executes on return
    // is the backend's own return, sending control straight back to the caller.
    void dispatchEscape(std::uint32_t firedAt) {
        if (DeclaredEscape* found = escapeArmedAt(firedAt)) {
            DeclaredEscape&   e = *found;
            const GuestHookScope holding{inGuestHook};
            if (e.replaces) {
                const NativeRoutine& r = e.replaces;
                std::array<std::uint64_t, kMaxReplacementInputs> values{};
                for (std::size_t i = 0; i < r.inputs.size(); ++i) {
                    const Location& in = r.inputs[i];
                    values[i] = in.kind() == Location::Kind::Register
                                    ? backend->readRegister(in.registerId())
                                    : backend->readMemory(in.address(), r.inputWidths[i]);
                }
                // Where the answer goes is taken before the game's function runs: that function may
                // declare escapes of its own, and what it declares is not required to leave this
                // declaration's fields where they were read from.
                const std::optional<Location> output      = r.output;
                const int                     outputWidth = r.outputWidth;

                const std::uint64_t result = r.fn(values.data());
                if (output) {
                    if (output->kind() == Location::Kind::Register) {
                        backend->writeLiveRegister(output->registerId(), result, outputWidth);
                    } else {
                        backend->writeMemory(output->address(), result, outputWidth);
                    }
                }
            } else if (e.handler) {
                e.handler(*owner, firedAt);
            }
        }
    }

    // Call a routine the machine already holds, in whatever context the machine is already in. The
    // marshalling is invoke's — memory inputs written live, register inputs applied over the guest's
    // own file, the output read where the routine left it — and the backend owns the frame.
    std::uint64_t invokeInContext(const ResolvedRoutine& routine,
                                  std::span<const CallValue> inputs) {
        if (running() &&
            std::this_thread::get_id() != romRun.steppingThread.load(std::memory_order_relaxed)) {
            throw std::logic_error(
                "this machine is running its hosted cartridge on a thread of its own, and its own "
                "code runs there: call the routine from an escape handler, or stop() the machine "
                "first");
        }
        std::vector<vm::ResidentRegister> presets;
        presets.reserve(inputs.size());
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const Location& loc = routine.inputs[i];
            if (loc.kind() == Location::Kind::Register) {
                presets.push_back({loc.registerId(), inputs[i].value});
            } else {
                backend->writeMemory(loc.address(), inputs[i].value, inputs[i].width);
            }
        }
        // The guest's own stack is the right one exactly when there is a guest to give back to: an
        // escape holds one mid-instruction, and a booted cartridge is parked in the middle of its
        // own program. A machine whose image has never been booted, and one holding placed routines
        // at rest, have no such context — their call goes on the engine's own scratch stack.
        const vm::CallStack stack =
            (inGuestHook || romRun.booted) ? vm::CallStack::Guest : vm::CallStack::Scratch;

        std::uint64_t out = 0;
        backend->callInContext(routine.entry, std::span<const vm::ResidentRegister>(presets), stack,
                               kMaxContextInstructions, [&] {
                                   if (!routine.output.has_value()) {
                                       return;
                                   }
                                   const Location& o = *routine.output;
                                   out = (o.kind() == Location::Kind::Register)
                                             ? backend->readRegister(o.registerId())
                                             : backend->readMemory(o.address(), routine.outputWidth);
                               });
        return out;
    }

    // Install the sink once, the first time this machine declares an escape.
    void ensureEscapeSink() {
        if (escapeSinkInstalled) {
            return;
        }
        backend->setEscapeSink([this](std::uint32_t firedAt) { dispatchEscape(firedAt); });
        escapeSinkInstalled = true;
    }

    // ── Watches ──────────────────────────────────────────────────────────────────────────────────
    //
    // Every function in this block reads or writes the watch list, so every one of them is called
    // with `watchMx` held — except the dispatch, which says where it lets go.

    // Arm or disarm one entry against the backend. Doing nothing when it already stands that way is
    // what keeps a repeated switch from re-walking the machine's watched set.
    void applyWatchArmed(DeclaredWatch& w, bool on) {
        if (w.armed == on) {
            return;
        }
        w.armed = on;
        const bool onRead  = static_cast<bool>(w.onRead);
        const bool onWrite = static_cast<bool>(w.onWrite);
        if (on) {
            backend->armWatch(w.at, onRead, onWrite);
        } else {
            backend->disarmWatch(w.at, onRead, onWrite);
        }
    }

    // Stepping thread, before each step: land the changes queued so far, in issue order. A change
    // naming a watch that has since been removed has nothing left to do — it is skipped rather than
    // reported, because this runs where an exception has nowhere to go.
    void drainWatchChanges() {
        const std::lock_guard<std::mutex> lock(watchMx);
        if (pendingWatches.empty()) {
            return;
        }
        std::vector<PendingWatchChange> changes;
        changes.swap(pendingWatches);
        for (const PendingWatchChange& c : changes) {
            const auto at = std::find_if(declaredWatches.begin(), declaredWatches.end(),
                                         [&c](const DeclaredWatch& w) { return w.key == c.key; });
            if (at == declaredWatches.end()) {
                continue;
            }
            if (c.kind == PendingWatchChange::Kind::Remove) {
                applyWatchArmed(*at, false);
                declaredWatches.erase(at);
            } else {
                applyWatchArmed(*at, c.kind == PendingWatchChange::Kind::Arm);
            }
        }
    }

    [[nodiscard]] DeclaredWatch* findWatch(std::string_view key) noexcept {
        for (DeclaredWatch& w : declaredWatches) {
            if (w.key == key) {
                return &w;
            }
        }
        return nullptr;
    }

    [[nodiscard]] DeclaredWatch& requireWatch(std::string_view key) {
        if (DeclaredWatch* w = findWatch(key)) {
            return *w;
        }
        throw std::out_of_range("watches: this machine declares no watch named '" +
                                std::string(key) + "'");
    }

    // Which entry answers for the place that fired, or null if none is armed there. The lock is held
    // for the search alone: an entry stays where it is for as long as it is declared, so the pointer
    // outlives the lock, and the game's code runs without holding it.
    [[nodiscard]] DeclaredWatch* watchArmedAt(std::uint32_t armedBase) {
        const std::lock_guard<std::mutex> lock(watchMx);
        for (DeclaredWatch& w : declaredWatches) {
            if (w.armed && w.at.at == armedBase) {
                return &w;
            }
        }
        return nullptr;
    }

    // Stepping thread: the guest touched a watched byte, and the machine is parked at the access
    // waiting to be told what it does. The handler runs to completion first; the access then does
    // whatever the handler answered.
    [[nodiscard]] AccessVerdict dispatchWatch(std::uint32_t armedBase, std::uint32_t at,
                                              vm::VmBackend::AccessKind kind, std::uint8_t value) {
        DeclaredWatch* found = watchArmedAt(armedBase);
        if (found == nullptr) {
            return AccessVerdict::proceed();
        }
        // Called where it lives. The list keeps its elements at their addresses for as long as they
        // are declared, so a handler that declares or drops OTHER watches while this one runs leaves
        // this entry where it is — and this path runs on every watched access, so a copy of the
        // handler here would be an allocation in the machine's hottest loop.
        //
        // A handler that removes its OWN watch destroys the code it is running in, which is the same
        // term an escape carries and is stated at the surface.
        const WatchHandler& handler =
            kind == vm::VmBackend::AccessKind::Read ? found->onRead : found->onWrite;
        if (!handler) {
            return AccessVerdict::proceed();
        }
        const GuestHookScope holding{inGuestHook};
        return handler(*owner, at, value);
    }

    // Install the sink once, the first time this machine declares a watch.
    void ensureWatchSink() {
        if (watchSinkInstalled) {
            return;
        }
        backend->setWatchSink([this](std::uint32_t armedBase, std::uint32_t at,
                                     vm::VmBackend::AccessKind kind,
                                     std::uint8_t value) -> AccessVerdict {
            return dispatchWatch(armedBase, at, kind, value);
        });
        watchSinkInstalled = true;
    }

    // The game's own read or write, offered to the watches that asked for it. Only watches declaring
    // AccessSource::GuestAndGame see these — the cartridge's own code is always watched, the game's
    // own verbs are not unless the game says so.
    //
    // The address arithmetic here is flat, which is exactly why a GuestAndGame watch may not be
    // bank-qualified (registration refuses one): a place that banks is not `base + n` in the space
    // the CPU sees, and the engine does not do stride arithmetic on an encoded address. A banked
    // place being accessed therefore matches no such watch, which is correct — a byte in bank 1 is
    // not the byte a bank-0 declaration named.
    [[nodiscard]] bool anyGameWatch() {
        const std::lock_guard<std::mutex> lock(watchMx);
        for (const DeclaredWatch& w : declaredWatches) {
            if (w.armed && w.from == AccessSource::GuestAndGame) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] DeclaredWatch* gameWatchAt(std::uint32_t address) {
        const std::lock_guard<std::mutex> lock(watchMx);
        for (DeclaredWatch& w : declaredWatches) {
            if (!w.armed || w.from != AccessSource::GuestAndGame || (w.at.at >> 16) != 0) {
                continue;
            }
            const std::uint64_t span = w.at.totalBytes();
            if (address >= w.at.at && address - w.at.at < span) {
                return &w;
            }
        }
        return nullptr;
    }

    // Run the game's own access past its watches, byte by byte, and report what each one decided.
    // `bytes` is edited in place: a read's answer, or the values a write actually lands.
    void offerGameAccess(const MemoryRegion& where, std::uint32_t index,
                         std::span<std::uint8_t> bytes, vm::VmBackend::AccessKind kind,
                         std::vector<bool>& vetoed) {
        vetoed.assign(bytes.size(), false);
        if ((where.at >> 16) != 0 || !anyGameWatch()) {
            return;  // a banked place matches no GuestAndGame watch, and no watch asked at all
        }
        const std::uint64_t base =
            static_cast<std::uint64_t>(where.at) + static_cast<std::uint64_t>(index) * where.size;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            const auto     address = static_cast<std::uint32_t>(base + i);
            DeclaredWatch* w       = gameWatchAt(address);
            if (w == nullptr) {
                continue;
            }
            const WatchHandler& declared =
                kind == vm::VmBackend::AccessKind::Read ? w->onRead : w->onWrite;
            if (!declared) {
                continue;
            }
            const GuestHookScope holding{inGuestHook};
            const AccessVerdict  verdict = declared(*owner, address, bytes[i]);
            switch (verdict.kind()) {
                case AccessVerdict::Kind::Proceed:
                    break;
                case AccessVerdict::Kind::Veto:
                    // A read cannot be prevented; on a write the byte keeps what it had.
                    vetoed[i] = kind == vm::VmBackend::AccessKind::Write;
                    break;
                case AccessVerdict::Kind::Instead:
                    bytes[i] = verdict.value();
                    break;
            }
        }
    }

    // The governor, created on first need and built from the machine's own clock. A machine whose
    // clock differs from the one its governor was built from — a cartridge of the other region
    // hosted between runs — gets a governor at the new rate carrying the factor it had. A machine
    // whose clock never changes keeps the one governor, and its sub-cycle carry, for life. The clock
    // of a running machine does not change: hosting a cartridge refuses a running machine.
    vm::RunGovernor& ensureGovernor() {
        const std::optional<vm::MachineClock> clock = backend->clock();
        if (!clock) {
            throw std::logic_error(
                "run/speed: this machine's core keeps no clock of its own, so its speed is "
                "undefined");
        }
        if (!romRun.governor || romRun.governorClock != *clock) {
            const std::pair<std::uint32_t, std::uint32_t> factor =
                romRun.governor ? romRun.governor->factor()
                                : std::pair<std::uint32_t, std::uint32_t>{1u, 1u};
            romRun.governor.emplace(clock->hertzNumerator, clock->hertzDivisor);
            romRun.governor->setFactor(factor.first, factor.second);
            romRun.governorClock = *clock;
        }
        return *romRun.governor;
    }

    // Lay the declared places out into one published block. Rebuilt at each run(), so places
    // registered between episodes join the observable set.
    void buildPublishTable() {
        romRun.table.clear();
        std::size_t offset = 0;
        for (const std::vector<DeclaredRegion>& batch : regionBatches) {
            for (const DeclaredRegion& d : batch) {
                romRun.table.push_back(PublishedRegion{.where = d.where, .offset = offset});
                offset += static_cast<std::size_t>(d.where.size) * d.where.count;
            }
        }
        romRun.published.assign(offset, 0);
        romRun.seq.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] const PublishedRegion* publishedEntry(const MemoryRegion& where) const {
        for (const PublishedRegion& p : romRun.table) {
            if (p.where.at == where.at && p.where.size == where.size &&
                p.where.count == where.count) {
                return &p;
            }
        }
        return nullptr;
    }

    // The publish's capture: every declared place's bytes into the published block. Only ever runs
    // between the seqlock's odd and even edges.
    void capturePublished() {
        for (const PublishedRegion& p : romRun.table) {
            for (std::uint32_t i = 0; i < p.where.count; ++i) {
                const std::size_t at = p.offset + static_cast<std::size_t>(i) * p.where.size;
                backend->readRegion(p.where, i,
                                    std::span<std::uint8_t>(romRun.published.data() + at,
                                                            p.where.size));
            }
        }
    }

    // Stepping thread, after each step: capture every declared place as one coherent set.
    void publishStep() {
        romRun.seq.fetch_add(1, std::memory_order_release);  // -> odd (writing)
        capturePublished();
        romRun.seq.fetch_add(1, std::memory_order_release);  // -> even (stable)
    }

    // Game thread, while running: the latest coherent capture of one declared place. Retries only
    // while a publish is mid-flight (bounded — a publish is one block of reads).
    [[nodiscard]] std::vector<std::uint8_t> readPublished(const MemoryRegion& where,
                                                          std::uint32_t index) const {
        const PublishedRegion* entry = publishedEntry(where);
        if (entry == nullptr) {
            throw std::logic_error(
                "read: the machine is running and this place is not among the declared regions — "
                "register it before run(), or stop() the machine to read a place built on the spot");
        }
        if (index >= where.count) {
            throw std::out_of_range("read: index " + std::to_string(index) +
                                    " is out of range (the place declares " +
                                    std::to_string(where.count) + " entries)");
        }
        const std::size_t at = entry->offset + static_cast<std::size_t>(index) * where.size;
        std::vector<std::uint8_t> out(where.size);
        for (;;) {
            const std::uint32_t before = romRun.seq.load(std::memory_order_acquire);
            if (before & 1u) {
                continue;  // a publish is in flight — wait for it to finish
            }
            std::copy_n(romRun.published.data() + at, where.size, out.data());
            std::atomic_thread_fence(std::memory_order_acquire);
            if (romRun.seq.load(std::memory_order_acquire) == before) {
                return out;
            }
        }
    }

    // Game thread, while running: queue a write for the stepping thread. Validated here in full —
    // the apply on the stepping thread cannot throw.
    void queueRegionWrite(const MemoryRegion& where, std::span<const std::uint8_t> bytes,
                          std::uint32_t index) {
        if (publishedEntry(where) == nullptr) {
            throw std::logic_error(
                "write: the machine is running and this place is not among the declared regions — "
                "register it before run(), or stop() the machine to write a place built on the spot");
        }
        if (bytes.size() != where.size) {
            throw std::invalid_argument("write: " + std::to_string(bytes.size()) +
                                        " bytes is not one entry (the place's entries are " +
                                        std::to_string(where.size) + " bytes)");
        }
        if (index >= where.count) {
            throw std::out_of_range("write: index " + std::to_string(index) +
                                    " is out of range (the place declares " +
                                    std::to_string(where.count) + " entries)");
        }
        const std::lock_guard<std::mutex> lock(romRun.writeMx);
        romRun.pendingWrites.push_back(PendingRegionWrite{
            .where = where, .index = index,
            .bytes = std::vector<std::uint8_t>(bytes.begin(), bytes.end())});
    }

    // Stepping thread, before each step: land the writes queued so far, in issue order.
    void drainRegionWrites() {
        std::vector<PendingRegionWrite> writes;
        {
            const std::lock_guard<std::mutex> lock(romRun.writeMx);
            writes.swap(romRun.pendingWrites);
        }
        for (const PendingRegionWrite& w : writes) {
            applyGameWrite(w.where, w.index, w.bytes);
        }
    }

    // The game's own write, landing. Offered to the watches that asked for the game's own verbs
    // first, then written as they left it — a vetoed byte keeps the value it already had, which is
    // read back and put in place of the one the game supplied.
    void applyGameWrite(const MemoryRegion& where, std::uint32_t index,
                        std::span<const std::uint8_t> bytes) {
        if ((where.at >> 16) != 0 || bytes.size() != where.size || !anyGameWatch()) {
            backend->writeRegion(where, index, bytes);
            return;
        }
        std::vector<std::uint8_t> value(bytes.begin(), bytes.end());
        std::vector<bool>         vetoed;
        offerGameAccess(where, index, value, vm::VmBackend::AccessKind::Write, vetoed);
        if (std::find(vetoed.begin(), vetoed.end(), true) != vetoed.end()) {
            std::vector<std::uint8_t> held(where.size);
            backend->readRegion(where, index, held);
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (vetoed[i]) {
                    value[i] = held[i];
                }
            }
        }
        backend->writeRegion(where, index, value);
    }

    // Stand the run up. Boot (first episode) and the first publish happen on the calling thread —
    // the machine is quiescent until start(), and thread creation orders these writes before the
    // stepping thread's first look. The threaded mode is the public one; Inline is the
    // deterministic seam device-free tests step through (vm_testing.h).
    void startRun(Vm* self, vm::VmRunner::Mode mode) {
        if (!romHosted) {
            throw std::logic_error("run: no cartridge is hosted on this VM (hostRom first)");
        }
        if (running()) {
            throw std::logic_error("run: the machine is already running");
        }
        vm::RunGovernor& gov = ensureGovernor();
        if (romRun.governorClock.cyclesPerFrame == 0) {
            throw std::logic_error("run: the machine's frame is zero cycles long");
        }
        buildPublishTable();
        if (!romRun.booted) {
            backend->bootHostedRom();
            romRun.booted = true;
        }
        publishStep();
        auto runner = std::make_unique<vm::VmRunner>(self, vm::VmRunner::StepKind::Started,
                                                     romRun.governorClock.cyclesPerFrame, mode);
        runner->beforeEachStep([this] {
            // The machine's own step says which thread it belongs to; a call into the guest is made
            // from there or not at all.
            romRun.steppingThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
            drainRegionWrites();
            drainEscapeChanges();
            drainWatchChanges();
            drainVideoChange();
            drainButtons();
        });
        runner->afterEachStep([this] {
            publishStep();
            stepSaveData();
        });
        romRun.runner = std::move(runner);
        if (mode == vm::VmRunner::Mode::Threaded) {
            // The pace: step while cycles run lag cycles owed, park otherwise. Owed advances with
            // the wall clock at the platform's speed times the factor; both sides are read on the
            // stepping thread, so the closure is race-free by construction. A machine the engine's
            // tick advances owes the wall clock nothing, so it anchors to nothing.
            gov.restart(std::chrono::steady_clock::now());
            vm::VmRunner*    raw = romRun.runner.get();
            vm::RunGovernor* g   = &gov;
            romRun.runner->start(
                [g, raw] {
                    const std::uint64_t owed =
                        g->owedThrough(std::chrono::steady_clock::now());
                    const std::uint64_t ran = raw->cyclesRun();
                    return static_cast<std::size_t>(ran > owed ? ran - owed : 0);
                },
                /*highWater=*/1,
                // The wait, from the same two numbers: how long until the wall clock owes the
                // machine everything it has already run. One step is a whole guest frame, so a park
                // measured any other way lands the next frame late by whatever the park had left to
                // run; measured this way it lands when the frame is due.
                [g, raw] { return g->timeUntilOwed(raw->cyclesRun()); });
        }
    }
};

namespace {
// A generous one-time runaway cap for the engine-run .init gesture at host() (no frame budget applies).
constexpr std::uint64_t kInitCycleCap = 1u << 24;  // ~4 s of SM83 time — an init returns in far less
}  // namespace

Vm::Vm(detail::CoreFactory core, VMPlatform platform, TimingProfile timing, VmConfig config) {
    if (core == nullptr) {
        throw std::runtime_error("VMPlatform (" + std::to_string(static_cast<int>(platform)) +
                                 "): no backend built in v1 (only GameBoy / GameBoyColor / Snes)");
    }
    impl_ = std::make_unique<Impl>(core, platform, timing);
    impl_->owner = this;
    if (!config.key.empty()) {
        requireFlatName(config.key, "the machine's key");
        if (!impl_->backend->keepsSaveData()) {
            throw std::logic_error(
                "Vm: this machine's core keeps nothing when the power goes off, so a key here would "
                "stand for nothing");
        }
        impl_->save.key = std::move(config.key);
    }
    // Declaring an output and switching it on are the same setting reached two ways, so this is the
    // verb — a machine declared here is producing before it hosts anything, which is the whole
    // difference from asking for it later. Nothing runs yet, so each lands directly.
    if (config.video) {
        video(true);
    }
}

void Vm::batterySave(std::string_view name) {
    impl_->requireNotRunning("batterySave");
    requireFlatName(name, "the battery save's name");
    if (impl_->save.key.empty()) {
        throw std::logic_error(
            "batterySave: this machine has no key, so there is no directory for the file to be in — "
            "give it VmConfig::key at construction");
    }
    // The file being left is already complete: this refuses a running machine, and parking one is what
    // writes out everything it owed.
    impl_->save.document = std::string(name);
    // A different file: nothing is known about what it holds, so the next hand-over compares against
    // nothing and writes.
    impl_->save.lastWritten.clear();
    impl_->save.dirty = false;
    // Set after the cartridge arrived, this is where that file's stored copy is read back. Set
    // before, there is no image yet and this does nothing — hostRom does it instead.
    impl_->loadSaveData();
}


// Parking the machine is what writes its data out, and a machine that is dropped without being parked
// is still being put away — so this does what stop() does, for the case where nothing else will. A
// destructor cannot throw, and a save that cannot be written is not worth taking a program down over.
Vm::~Vm() {
    if (impl_ == nullptr) {
        return;  // moved from: the machine lives somewhere else now
    }
    try {
        stop();
    } catch (...) {  // NOLINT(bugprone-empty-catch) — see above
    }
}

// The move operations re-point the Impl at its new owner: an escape handler is handed the machine, so
// the machine it is handed must be where the machine now lives.
Vm::Vm(Vm&& other) noexcept : impl_(std::move(other.impl_)) {
    if (impl_) {
        impl_->owner = this;
    }
}

Vm& Vm::operator=(Vm&& other) noexcept {
    impl_ = std::move(other.impl_);
    if (impl_) {
        impl_->owner = this;
    }
    return *this;
}

VMPlatform Vm::platform() const noexcept { return impl_->platform; }

void Vm::reset() {
    impl_->requireNotRunning("reset");
    impl_->backend->reset();
    impl_->romRun.booted = false;  // a reset machine boots fresh on the next run()
}

void Vm::advanceClock(std::uint64_t cycles) {
    impl_->requireNotRunning("advanceClock");
    impl_->backend->advanceClock(cycles);
}

void Vm::advanceTick(std::chrono::nanoseconds enginePeriod) {
    if (impl_->running()) {
        if (impl_->romRun.runner->mode() != vm::VmRunner::Mode::Inline) {
            throw std::logic_error(
                "advanceTick: the machine runs its hosted cartridge on a clock of its own; "
                "run(Advance::OnTick) to advance it by the engine's tick instead");
        }
        // The step: the queued writes and switches land, the machine runs the tick's worth, and the
        // declared regions publish — the same step boundary the other clock steps through.
        impl_->romRun.runner->stepOnce(impl_->tickBudget(enginePeriod));
        impl_->latchVideo();
        return;
    }
    // The carry rides on the VM, so consecutive ticks compose: the fraction of a cycle this tick
    // leaves behind is spent by a later one, and the running total never drifts from the machine's
    // true rate however the tick period relates to it. A core that keeps no clock advances nothing;
    // its picture still latches.
    if (const std::optional<vm::MachineClock> clock = impl_->backend->clock()) {
        const CycleDraw draw = clock->cyclesFor(enginePeriod, impl_->cycleCarryNs);
        impl_->cycleCarryNs  = draw.carryNs;
        if (draw.cycles != 0) {
            impl_->backend->advanceClock(draw.cycles);
        }
    }
    impl_->latchVideo();
}

// One tick of a machine is one frame of its own clock. A core that keeps no clock has no frame to
// spend, and its picture still latches.
void Vm::advanceTick() {
    const std::optional<vm::MachineClock> clock = impl_->backend->clock();
    advanceTick(clock ? clock->framePeriod() : std::chrono::nanoseconds::zero());
}

void Vm::enableAudio(unsigned sampleRate,
                     std::function<void(std::int16_t, std::int16_t)> onSample) {
    impl_->requireNotRunning("enableAudio");
    impl_->backend->enableAudio(sampleRate, std::move(onSample));
}

void Vm::startDriver(const Routine<void()>& driver) {
    if (driver.vm_ != this) {
        throw std::invalid_argument("startDriver: the routine was not registered on this Vm");
    }
    const ResolvedRoutine& routine = impl_->routines[driver.handle_];
    if (routine.throttle != Throttle::HardwareSpeed) {
        throw std::invalid_argument(
            "startDriver: only a Throttle::HardwareSpeed routine can be driven as an audio driver");
    }
    impl_->backend->beginContinuous(routine.entry);
}

std::uint64_t Vm::stepDriver(std::uint64_t cpuCycles) {
    return impl_->backend->runForCycles(cpuCycles);
}

// ── Resident driver ─────────────────────────────────────────────────────────────────────────────

std::uint64_t Vm::performInstruction(const Instruction& instruction, std::uint64_t cycleCap) {
    const std::uint64_t value = instruction.valueFor(0);
    if (instruction.kind() == Instruction::Kind::Write) {
        impl_->backend->writeMemory(instruction.location().address(), value, instruction.width());
        return 0;
    }
    // A call: apply the declared fixed register presets, then the folded argument in its register.
    std::vector<vm::ResidentRegister> presets;
    presets.reserve(instruction.presets().size() + 1);
    for (const RegisterPreset& p : instruction.presets()) {
        presets.push_back({p.reg.registerId(), p.value});
    }
    presets.push_back({instruction.location().registerId(), value});
    return impl_->backend->callResident(instruction.entry(),
                                        std::span<const vm::ResidentRegister>(presets), cycleCap);
}

void Vm::hostRom(std::span<const std::uint8_t> rom) {
    impl_->requireNotRunning("hostRom");
    impl_->backend->loadRom(rom);
    impl_->romHosted = true;
    impl_->romRun.booted = false;  // a fresh image boots fresh
    // The image decides how much this machine keeps, so its stored copy is read back here rather than
    // at construction. It survives the boot that run() performs — keeping its memory through a reset
    // is what a cartridge's own battery does.
    impl_->loadSaveData();
}

void Vm::run(Advance how) {
    impl_->startRun(this, how == Advance::OnTick ? vm::VmRunner::Mode::Inline
                                                 : vm::VmRunner::Mode::Threaded);
}

void Vm::buttons(GuestButtons held) {
    if (!impl_->backend->takesButtons()) {
        throw std::logic_error("buttons: this machine's core takes no button state");
    }
    // Held on the game's thread and applied on the machine's, so a submission never reaches the core
    // while its own thread is mid-step. A machine that is not running has no such thread, so the state
    // lands where it was asked for.
    if (impl_->running()) {
        const std::lock_guard guard{impl_->buttons.mx};
        impl_->buttons.latest  = held.held;
        impl_->buttons.pending = true;
        return;
    }
    impl_->backend->setButtons(held.held);
}

void Vm::video(bool drawing) { video(drawing, VideoOptions{}); }

void Vm::video(bool drawing, VideoOptions options) {
    if (impl_->running()) {
        // The machine owns itself while it runs: installing the sink here would be writing the very
        // function its own thread is calling. Queue it for the next step boundary, where every other
        // change issued to a running machine lands; the settings ride with the switch.
        const std::lock_guard guard{impl_->video.mx};
        impl_->video.pendingChange     = true;
        impl_->video.pendingChangeWant = drawing;
        if (options.interlacing.on) {
            impl_->video.pendingOptions.interlacing.on = options.interlacing.on;
        }
        if (options.interlacing.type) {
            impl_->video.pendingOptions.interlacing.type = options.interlacing.type;
        }
        return;
    }
    impl_->applyVideo(drawing, options);
}

RasterContent Vm::video() const {
    if (!impl_->videoOrAsked()) {
        throw std::logic_error("video: this machine draws nothing — video(true) first");
    }
    // A machine on a clock of its own has no tick boundary to have latched at, so asking is the latch.
    // One advanced by the game latched at its boundary and this changes nothing — which is what makes
    // two reads in one frame answer the same on either clock.
    if (impl_->running() && impl_->romRun.runner->mode() != vm::VmRunner::Mode::Inline) {
        impl_->latchVideo();
    }
    return RasterContent{
        .pixels     = impl_->video.shown,
        .width      = impl_->video.width,
        .height     = impl_->video.height,
        .format     = impl_->video.format,
        .generation = impl_->video.shownGeneration,
    };
}

void Vm::speed(std::uint32_t num, std::uint32_t den) {
    impl_->ensureGovernor().setFactor(num, den);
    if (impl_->running()) {
        impl_->romRun.runner->wake();  // a parked machine reacts to the new pace now, not next park
    }
}

std::pair<std::uint32_t, std::uint32_t> Vm::speed() const {
    if (!impl_->romRun.governor) {
        return {1u, 1u};  // the platform's own speed — nothing has been steered yet
    }
    return impl_->romRun.governor->factor();
}

void Vm::stop() {
    if (!impl_->running()) {
        return;
    }
    impl_->romRun.runner->requestStop();
    impl_->romRun.runner->wake();
    impl_->romRun.runner.reset();  // joins: the thread is gone by return, the machine parked
    // A verb issued in the last moments of the run still has a step boundary to land at: this one,
    // in the order a step boundary lands them. Parking is not a reason for a verb the game already
    // issued to go missing.
    impl_->drainRegionWrites();
    impl_->drainEscapeChanges();
    impl_->drainWatchChanges();
    impl_->drainVideoChange();
    impl_->flushSaveData();
}

std::vector<std::uint8_t> Vm::read(const MemoryRegion& where, std::uint32_t index) {
    if (impl_->running()) {
        // The publish is a copy the machine's own step took, not an access to the machine, so no
        // watch fires for it. A running machine's own reads are watched where they happen.
        return impl_->readPublished(where, index);
    }
    std::vector<std::uint8_t> bytes(where.size);
    impl_->backend->readRegion(where, index, bytes);
    std::vector<bool> vetoed;
    impl_->offerGameAccess(where, index, bytes, vm::VmBackend::AccessKind::Read, vetoed);
    return bytes;
}

void Vm::write(const MemoryRegion& where, std::span<const std::uint8_t> bytes, std::uint32_t index) {
    if (impl_->running()) {
        // Queued here, offered to the watches and landed at the next step boundary — on the
        // machine's own thread, which is where a handler belongs.
        impl_->queueRegionWrite(where, bytes, index);
        return;
    }
    impl_->applyGameWrite(where, index, bytes);
}

std::size_t Vm::registerRegionsResolved(std::span<const DeclaredRegion> declared) {
    impl_->requireNotRunning("registerRegions");
    if (declared.empty()) {
        throw std::invalid_argument("registerRegions: the batch declares no places");
    }
    // Check every entry before reporting any. The whole point of handing the batch over is that a
    // two-hundred-entry table is answered once — a report that stops at the first bad entry turns
    // one registration into as many rounds as there are mistakes.
    std::string failures;
    std::size_t failed = 0;
    for (const DeclaredRegion& d : declared) {
        if (impl_->backend->regionIsAddressable(d.where)) {
            continue;
        }
        ++failed;
        failures += "\n  ";
        failures += d.name.empty() ? "(unnamed)" : std::string(d.name);
        failures += " at " + std::to_string(d.where.at) + ", " + std::to_string(d.where.size) +
                    " bytes x " + std::to_string(d.where.count);
    }
    if (failed != 0) {
        throw std::invalid_argument(
            "registerRegions: " + std::to_string(failed) + " of " +
            std::to_string(declared.size()) +
            " declared places are not reachable on this machine (host the cartridge before "
            "registering places inside it):" + failures);
    }
    impl_->regionBatches.emplace_back(declared.begin(), declared.end());
    return impl_->regionBatches.size() - 1;
}

// ── Escapes ─────────────────────────────────────────────────────────────────────────────────────

void Vm::registerEscapes(const EscapeMap& map) {
    impl_->requireNotRunning("registerEscapes");
    if (map.declarations.empty()) {
        throw std::invalid_argument("registerEscapes: the batch declares no escapes");
    }

    // Every entry is checked before any is reported, for the same reason a region batch is: a
    // generated table is answered once, not once per mistake.
    std::string failures;
    std::size_t failed = 0;
    const auto fail = [&](std::string_view key, const std::string& why) {
        ++failed;
        failures += "\n  ";
        failures += key.empty() ? "(unnamed)" : std::string(key);
        failures += ": " + why;
    };

    // One binding location checked against this machine: a register must exist here at the width the
    // signature carries; a memory home must be reachable for that many bytes. Reports through `fail`
    // and answers whether the location passed.
    const auto locationFits = [&](std::string_view key, const std::string& what, const Location& loc,
                                  int width) -> bool {
        if (loc.kind() == Location::Kind::Register) {
            const int regWidth = impl_->backend->registerWidthBytes(loc.registerId());
            if (regWidth == 0) {
                fail(key, what + " names a register this machine does not have");
                return false;
            }
            if (regWidth != width) {
                fail(key, what + " binds a " + std::to_string(width) + "-byte value to a " +
                              std::to_string(regWidth) + "-byte register");
                return false;
            }
            return true;
        }
        if (!impl_->backend->regionIsAddressable(MemoryRegion{
                .at = loc.address(), .size = static_cast<std::uint32_t>(width)})) {
            fail(key, what + " names memory this machine cannot reach");
            return false;
        }
        return true;
    };

    for (std::size_t i = 0; i < map.declarations.size(); ++i) {
        const GuestEscape&     e   = map.declarations[i];
        const std::string_view key = e.key;
        if (key.empty()) {
            fail(key, "the key is empty");
            continue;  // nothing else about this entry can be reported against a name
        }
        if (!e.handler && !e.replaces) {
            fail(key, "neither a handler nor a replacement — a declared escape with nothing to run "
                      "would never do anything");
        }
        if (e.handler && e.replaces) {
            fail(key, "both a handler and a replacement — one escape does one or the other");
        }
        if (e.replaces) {
            const NativeRoutine& r = e.replaces;
            if (r.inputs.size() != r.inputWidths.size()) {
                fail(key, "the binding declares " + std::to_string(r.inputs.size()) +
                              " input(s) for a function taking " +
                              std::to_string(r.inputWidths.size()));
            }
            if (r.inputs.size() > kMaxReplacementInputs) {
                fail(key, "a replacement takes at most " + std::to_string(kMaxReplacementInputs) +
                              " inputs");
            }
            if (r.output && r.outputWidth == 0) {
                fail(key, "the binding declares an output for a function returning nothing");
            }
            if (!r.output && r.outputWidth != 0) {
                fail(key, "the function returns a value the binding gives no home — its answer "
                          "would vanish");
            }
            if (r.declaredEntryOffset != 0 || r.declaredHardwarePacing) {
                fail(key, "a replacement binding names no pacing and no entry offset — the guest's "
                          "own call decides both");
            }
            for (std::size_t j = 0; j < r.inputs.size() && j < r.inputWidths.size(); ++j) {
                locationFits(key, "input " + std::to_string(j), r.inputs[j], r.inputWidths[j]);
            }
            if (r.output && r.outputWidth != 0) {
                locationFits(key, "the output", *r.output, r.outputWidth);
            }
        }
        if (!impl_->backend->addressIsAccessible(e.at)) {
            fail(key, "address " + std::to_string(e.at) + " is not reachable on this machine");
        }
        if (impl_->findEscape(key) != nullptr) {
            fail(key, "this machine already declares an escape by that name");
        }
        for (const DeclaredEscape& already : impl_->escapes) {
            if (already.at == e.at) {
                fail(key, "address " + std::to_string(e.at) + " already escapes, as '" +
                              already.key + "'");
                break;
            }
        }
        for (std::size_t j = 0; j < i; ++j) {
            const GuestEscape& earlier = map.declarations[j];
            if (std::string_view(earlier.key) == key) {
                fail(key, "the batch declares that name twice");
            }
            if (earlier.at == e.at) {
                fail(key, "the batch declares address " + std::to_string(e.at) + " twice");
            }
        }
    }
    if (failed != 0) {
        throw std::invalid_argument("registerEscapes: " + std::to_string(failed) + " of " +
                                    std::to_string(map.declarations.size()) +
                                    " declared escapes did not pass (host the cartridge before "
                                    "declaring escapes inside it):" + failures);
    }

    // Arm before recording, and undo what was armed if the backend refuses one — so a machine that
    // cannot watch addresses at all is left exactly as it was rather than half-declared.
    impl_->ensureEscapeSink();
    std::vector<std::uint32_t> armedHere;
    try {
        for (const GuestEscape& e : map.declarations) {
            if (e.armed) {
                impl_->backend->armEscape(e.at, static_cast<bool>(e.replaces));
                armedHere.push_back(e.at);
            }
        }
    } catch (...) {
        for (const std::uint32_t address : armedHere) {
            impl_->backend->disarmEscape(address);
        }
        throw;
    }

    // No lock here, and none over the checks above: this verb refuses a running machine, so the
    // thread that would contend for the table does not exist while any of it runs.
    for (const GuestEscape& e : map.declarations) {
        impl_->escapes.push_back(DeclaredEscape{.key      = std::string(std::string_view(e.key)),
                                                .at       = e.at,
                                                .handler  = e.handler,
                                                .replaces = e.replaces,
                                                .armed    = e.armed});
    }
}

EscapeTable Vm::escapes() noexcept { return EscapeTable(*this); }

bool Vm::escapeArmed(std::string_view key) const {
    const std::lock_guard<std::mutex> lock(impl_->escapeMx);
    return impl_->requireEscape(key).armed;
}

void Vm::setEscapeArmed(std::string_view key, bool on) {
    const std::lock_guard<std::mutex> lock(impl_->escapeMx);
    // The key is answered here either way, so a name this machine does not declare throws at the
    // call that made the mistake rather than going quiet in a queue.
    DeclaredEscape& e = impl_->requireEscape(key);
    if (impl_->changeCrosses()) {
        impl_->pendingEscapes.push_back(PendingEscapeChange{
            .key  = std::string(key),
            .kind = on ? PendingEscapeChange::Kind::Arm : PendingEscapeChange::Kind::Disarm});
        return;
    }
    impl_->applyEscapeArmed(e, on);
}

void Vm::removeEscape(std::string_view key) {
    const std::lock_guard<std::mutex> lock(impl_->escapeMx);
    const auto at = std::find_if(impl_->escapes.begin(), impl_->escapes.end(),
                                 [key](const DeclaredEscape& e) { return e.key == key; });
    if (at == impl_->escapes.end()) {
        throw std::out_of_range("escapes: this machine declares no escape named '" +
                                std::string(key) + "'");
    }
    if (impl_->changeCrosses()) {
        impl_->pendingEscapes.push_back(
            PendingEscapeChange{.key = std::string(key), .kind = PendingEscapeChange::Kind::Remove});
        return;
    }
    impl_->applyEscapeArmed(*at, false);
    impl_->escapes.erase(at);
}

bool Vm::hasEscape(std::string_view key) const {
    const std::lock_guard<std::mutex> lock(impl_->escapeMx);
    return impl_->findEscape(key) != nullptr;
}

std::size_t Vm::escapeCount() const {
    const std::lock_guard<std::mutex> lock(impl_->escapeMx);
    return impl_->escapes.size();
}

bool EscapeRef::armed() const { return machine_->escapeArmed(key_); }
void EscapeRef::armed(bool on) { machine_->setEscapeArmed(key_, on); }
void EscapeRef::remove() { machine_->removeEscape(key_); }

EscapeRef EscapeTable::operator[](std::string_view key) const {
    if (!machine_->hasEscape(key)) {
        throw std::out_of_range("escapes: this machine declares no escape named '" +
                                std::string(key) + "'");
    }
    return EscapeRef(*machine_, ObjectKey(key));
}

bool EscapeTable::contains(std::string_view key) const { return machine_->hasEscape(key); }

std::size_t EscapeTable::size() const { return machine_->escapeCount(); }

// ── Watches ─────────────────────────────────────────────────────────────────────────────────────

void Vm::registerWatches(const WatchMap& map) {
    impl_->requireNotRunning("registerWatches");
    if (map.declarations.empty()) {
        throw std::invalid_argument("registerWatches: the batch declares no watches");
    }

    // Every entry is checked before any is reported, for the same reason a region batch is: a
    // generated table is answered once, not once per mistake.
    std::string failures;
    std::size_t failed = 0;
    const auto  fail   = [&](std::string_view key, const std::string& why) {
        ++failed;
        failures += "\n  ";
        failures += key.empty() ? "(unnamed)" : std::string(key);
        failures += ": " + why;
    };

    // Two places overlap when they name a byte in common. Compared in the machine's own encoded
    // space, where a bank rides the high bits — so places in different banks never collide, which is
    // right, because a byte in bank 1 is not the byte a bank-0 declaration named.
    const auto overlaps = [](const MemoryRegion& a, const MemoryRegion& b) {
        const std::uint64_t aStart = a.at;
        const std::uint64_t bStart = b.at;
        return aStart < bStart + b.totalBytes() && bStart < aStart + a.totalBytes();
    };

    for (std::size_t i = 0; i < map.declarations.size(); ++i) {
        const GuestWatch&      w   = map.declarations[i];
        const std::string_view key = w.key;
        if (key.empty()) {
            fail(key, "the key is empty");
            continue;  // nothing else about this entry can be reported against a name
        }
        if (!w.onRead && !w.onWrite) {
            fail(key, "neither a read handler nor a write handler — a declared watch with nothing "
                      "to run would never do anything");
        }
        if (w.at.totalBytes() == 0) {
            fail(key, "the place spans no bytes");
        }
        if (!impl_->backend->regionIsAddressable(w.at)) {
            fail(key, "the place at " + std::to_string(w.at.at) + " (" + std::to_string(w.at.size) +
                          " bytes x " + std::to_string(w.at.count) +
                          ") is not reachable on this machine");
        }
        // A watch on the game's own verbs is answered by flat address arithmetic, which a banked
        // place does not obey — its bytes are not `base + n` in the space the CPU sees.
        if (w.from == AccessSource::GuestAndGame && (w.at.at >> 16) != 0) {
            fail(key, "a watch on the game's own reads and writes cannot name a bank-qualified "
                      "place; declare it where the CPU sees it, or watch the guest alone");
        }
        if (impl_->findWatch(key) != nullptr) {
            fail(key, "this machine already declares a watch by that name");
        }
        for (const DeclaredWatch& already : impl_->declaredWatches) {
            if (overlaps(already.at, w.at)) {
                fail(key, "this place overlaps the one '" + already.key +
                              "' already watches; one watch owns a place, and a place whose reads "
                              "and writes are both wanted declares both handlers");
                break;
            }
        }
        for (std::size_t j = 0; j < i; ++j) {
            const GuestWatch& earlier = map.declarations[j];
            if (std::string_view(earlier.key) == key) {
                fail(key, "the batch declares that name twice");
            }
            if (overlaps(earlier.at, w.at)) {
                fail(key, "the batch declares two watches over the same place");
            }
        }
    }
    if (failed != 0) {
        throw std::invalid_argument("registerWatches: " + std::to_string(failed) + " of " +
                                    std::to_string(map.declarations.size()) +
                                    " declared watches did not pass (host the cartridge before "
                                    "declaring watches inside it):" + failures);
    }

    // Arm before recording, and undo what was armed if the backend refuses one — so a machine with
    // no per-access hook at all is left exactly as it was rather than half-declared.
    impl_->ensureWatchSink();
    std::vector<const GuestWatch*> armedHere;
    try {
        for (const GuestWatch& w : map.declarations) {
            if (w.armed) {
                impl_->backend->armWatch(w.at, static_cast<bool>(w.onRead),
                                         static_cast<bool>(w.onWrite));
                armedHere.push_back(&w);
            }
        }
    } catch (...) {
        for (const GuestWatch* w : armedHere) {
            impl_->backend->disarmWatch(w->at, static_cast<bool>(w->onRead),
                                        static_cast<bool>(w->onWrite));
        }
        throw;
    }

    // No lock here, and none over the checks above: this verb refuses a running machine, so the
    // thread that would contend for the table does not exist while any of it runs.
    for (const GuestWatch& w : map.declarations) {
        impl_->declaredWatches.push_back(
            DeclaredWatch{.key     = std::string(std::string_view(w.key)),
                          .at      = w.at,
                          .onRead  = w.onRead,
                          .onWrite = w.onWrite,
                          .from    = w.from,
                          .armed   = w.armed});
    }
}

WatchTable Vm::watches() noexcept { return WatchTable(*this); }

bool Vm::watchArmed(std::string_view key) const {
    const std::lock_guard<std::mutex> lock(impl_->watchMx);
    return impl_->requireWatch(key).armed;
}

void Vm::setWatchArmed(std::string_view key, bool on) {
    const std::lock_guard<std::mutex> lock(impl_->watchMx);
    // Named first so a mistyped key throws whether or not the change has to cross.
    DeclaredWatch& w = impl_->requireWatch(key);
    if (impl_->changeCrosses()) {
        impl_->pendingWatches.push_back(PendingWatchChange{
            .key  = std::string(key),
            .kind = on ? PendingWatchChange::Kind::Arm : PendingWatchChange::Kind::Disarm});
        return;
    }
    impl_->applyWatchArmed(w, on);
}

void Vm::removeWatch(std::string_view key) {
    const std::lock_guard<std::mutex> lock(impl_->watchMx);
    const auto at = std::find_if(impl_->declaredWatches.begin(), impl_->declaredWatches.end(),
                                 [key](const DeclaredWatch& w) { return w.key == key; });
    if (at == impl_->declaredWatches.end()) {
        throw std::out_of_range("watches: this machine declares no watch named '" +
                                std::string(key) + "'");
    }
    if (impl_->changeCrosses()) {
        impl_->pendingWatches.push_back(
            PendingWatchChange{.key = std::string(key), .kind = PendingWatchChange::Kind::Remove});
        return;
    }
    impl_->applyWatchArmed(*at, false);
    impl_->declaredWatches.erase(at);
}

bool Vm::hasWatch(std::string_view key) const {
    const std::lock_guard<std::mutex> lock(impl_->watchMx);
    return impl_->findWatch(key) != nullptr;
}

std::size_t Vm::watchCount() const {
    const std::lock_guard<std::mutex> lock(impl_->watchMx);
    return impl_->declaredWatches.size();
}

bool WatchRef::armed() const { return machine_->watchArmed(key_); }
void WatchRef::armed(bool on) { machine_->setWatchArmed(key_, on); }
void WatchRef::remove() { machine_->removeWatch(key_); }

WatchRef WatchTable::operator[](std::string_view key) const {
    if (!machine_->hasWatch(key)) {
        throw std::out_of_range("watches: this machine declares no watch named '" +
                                std::string(key) + "'");
    }
    return WatchRef(*machine_, ObjectKey(key));
}

bool WatchTable::contains(std::string_view key) const { return machine_->hasWatch(key); }

std::size_t WatchTable::size() const { return machine_->watchCount(); }

void Vm::hostDriver(const DriverBinding& binding) {
    if (binding.isa != isaFor(impl_->platform)) {
        throw std::invalid_argument(
            "hostDriver: the binding's ISA does not match this VM's platform");
    }
    // The image a driver is hosted in is the platform's own — it writes the header and places every
    // byte. There is nothing in it that belongs to the player, so a machine keyed to keep the player's
    // data has been asked for two different things. Refused rather than silently kept empty, the same
    // way hosting a cartridge and hosting a driver refuse each other.
    if (!impl_->save.key.empty()) {
        throw std::logic_error(
            "hostDriver: this machine carries a key, and a key is for keeping the data a GAME's own "
            "cartridge holds; the image a driver runs in belongs to the platform");
    }
    // Configure the cartridge image (place + validate); stackTop 0 = the backend's default scratch top.
    impl_->backend->configureResidentImage(std::span<const DriverImage>(binding.images),
                                           binding.mapper, binding.stackTop.value_or(0));
    impl_->driver.hosted = true;
    impl_->driver.tickEntry = binding.tickEntry;
    impl_->driver.slots = binding.slots;
    // Perform the declared .init once — the engine runs it at host time (Gap 3: no call site exists).
    if (binding.init.has_value()) {
        performInstruction(*binding.init, kInitCycleCap);
    }
}

std::uint64_t Vm::tickDriver(std::span<const Instruction> queued, std::uint64_t cyclesPerFrame) {
    if (!impl_->driver.hosted) {
        throw std::logic_error("tickDriver: no driver is hosted on this VM (call hostDriver first)");
    }
    std::uint64_t spent = 0;
    // Perform queued gestures in submission order (mailbox writes and entry calls).
    for (const Instruction& ins : queued) {
        const std::uint64_t cap = (cyclesPerFrame > spent) ? (cyclesPerFrame - spent) : 1;
        spent += performInstruction(ins, cap);
    }
    // Call the per-frame tick entry to its return (no register presets).
    const std::uint64_t tickCap = (cyclesPerFrame > spent) ? (cyclesPerFrame - spent) : 1;
    spent += impl_->backend->callResident(impl_->driver.tickEntry, {}, tickCap);
    // Idle the machine for the remainder so the APU synthesizes at the hardware cadence.
    if (spent < cyclesPerFrame) {
        impl_->backend->advanceClock(cyclesPerFrame - spent);
    }
    return spent;
}

std::uint64_t Vm::readSlot(std::size_t index) {
    if (!impl_->driver.hosted) {
        throw std::logic_error("readSlot: no driver is hosted on this VM");
    }
    if (index >= impl_->driver.slots.size()) {
        throw std::out_of_range("readSlot: slot index " + std::to_string(index) +
                                " is out of range (" + std::to_string(impl_->driver.slots.size()) +
                                " slots declared)");
    }
    const SlotSpec& s = impl_->driver.slots[index];
    return impl_->backend->readMemory(s.address, s.width);
}

namespace {

// Validate a binding location against the width the signature gives that slot. Registers must match
// the backend's register width; an unknown register id is rejected. Memory accepts any width.
void validateLocation(const vm::VmBackend& backend, const Location& loc, int valueWidth,
                      const char* role, std::size_t index) {
    if (loc.kind() == Location::Kind::Register) {
        const int regWidth = backend.registerWidthBytes(loc.registerId());
        if (regWidth == 0) {
            throw std::invalid_argument(std::string(role) + " " + std::to_string(index) +
                                        ": register id " + std::to_string(loc.registerId()) +
                                        " is not a register on this system");
        }
        if (regWidth != valueWidth) {
            throw std::invalid_argument(
                std::string(role) + " " + std::to_string(index) + ": a " +
                std::to_string(valueWidth * 8) + "-bit value cannot bind to a " +
                std::to_string(regWidth * 8) + "-bit register");
        }
    } else if (!backend.addressIsAccessible(loc.address())) {
        throw std::invalid_argument(std::string(role) + " " + std::to_string(index) +
                                    ": address " + std::to_string(loc.address()) +
                                    " is not directly accessible on this system");
    }
}

}  // namespace

std::size_t Vm::registerResolved(std::span<const std::uint8_t> routineBytes,
                                 const RoutineBinding& binding,
                                 std::span<const int> inputWidths,
                                 int outputWidth, int instances) {
    // `instances > 1` is a declared seam (multi-instance routing for anti-channel-stealing audio) —
    // not built yet, so it throws. A HardwareSpeed routine is NOT a seam: it registers like any other
    // and is driven via startDriver / stepDriver instead of being called for a value.
    if (instances != 1) {
        throw std::logic_error(
            "multi-instance routing (anti-channel-stealing audio) is not built yet");
    }

    // Arity + width/location validation.
    if (binding.inputs.size() != inputWidths.size()) {
        throw std::invalid_argument(
            "RoutineBinding.inputs has " + std::to_string(binding.inputs.size()) +
            " entries but the routine signature has " + std::to_string(inputWidths.size()) +
            " argument(s)");
    }
    for (std::size_t i = 0; i < binding.inputs.size(); ++i) {
        validateLocation(*impl_->backend, binding.inputs[i], inputWidths[i], "argument", i);
    }
    if (outputWidth == 0 && binding.output.has_value()) {
        throw std::invalid_argument("a void routine signature cannot bind an output location");
    }
    if (outputWidth != 0 && !binding.output.has_value()) {
        throw std::invalid_argument("a value-returning routine signature requires binding.output");
    }
    if (binding.output.has_value()) {
        validateLocation(*impl_->backend, *binding.output, outputWidth, "return value", 0);
    }

    if (routineBytes.empty()) {
        throw std::invalid_argument("routine has no bytes");
    }
    if (binding.entryOffset >= routineBytes.size()) {
        throw std::invalid_argument("entryOffset is past the end of the routine bytes");
    }

    // Inject the bytes into the backend's code space; entry is the placement base + the binding's
    // offset within those bytes.
    const std::uint32_t base = impl_->backend->placeRoutine(routineBytes);

    ResolvedRoutine resolved;
    resolved.entry = base + binding.entryOffset;
    resolved.inputs.assign(binding.inputs.begin(), binding.inputs.end());
    resolved.inputWidths.assign(inputWidths.begin(), inputWidths.end());
    resolved.output = binding.output;
    resolved.outputWidth = outputWidth;
    resolved.throttle = binding.throttle;
    impl_->routines.push_back(std::move(resolved));
    return impl_->routines.size() - 1;
}

std::size_t Vm::bindRoutineResolved(std::uint32_t at, const RoutineBinding& binding,
                                    std::span<const int> inputWidths, int outputWidth) {
    // Declared against the machine as it stands, like every other declaration — so host the
    // cartridge first, and a machine running on its own thread is not one to declare against.
    impl_->requireNotRunning("bindRoutine");

    if (binding.entryOffset != 0 || binding.throttle != Throttle::HostSpeed) {
        throw std::invalid_argument(
            "bindRoutine: a routine already in place names no pacing and no entry offset — the "
            "address it is bound at is the entry, and a call is not paced");
    }
    if (binding.inputs.size() != inputWidths.size()) {
        throw std::invalid_argument(
            "RoutineBinding.inputs has " + std::to_string(binding.inputs.size()) +
            " entries but the routine signature has " + std::to_string(inputWidths.size()) +
            " argument(s)");
    }
    for (std::size_t i = 0; i < binding.inputs.size(); ++i) {
        validateLocation(*impl_->backend, binding.inputs[i], inputWidths[i], "argument", i);
    }
    if (outputWidth == 0 && binding.output.has_value()) {
        throw std::invalid_argument("a void routine signature cannot bind an output location");
    }
    if (outputWidth != 0 && !binding.output.has_value()) {
        throw std::invalid_argument("a value-returning routine signature requires binding.output");
    }
    if (binding.output.has_value()) {
        validateLocation(*impl_->backend, *binding.output, outputWidth, "return value", 0);
    }
    // Code in work RAM or high RAM is as callable as code in a cartridge — a game that copies its
    // display-transfer routine into high RAM and calls it there is doing what the hardware asks.
    if (!impl_->backend->addressIsAccessible(at)) {
        throw std::invalid_argument("bindRoutine: address " + std::to_string(at) +
                                    " is not reachable on this machine (host the cartridge before "
                                    "binding a routine inside it)");
    }

    ResolvedRoutine resolved;
    resolved.entry = at;
    resolved.inputs.assign(binding.inputs.begin(), binding.inputs.end());
    resolved.inputWidths.assign(inputWidths.begin(), inputWidths.end());
    resolved.output      = binding.output;
    resolved.outputWidth = outputWidth;
    resolved.throttle    = binding.throttle;
    resolved.form        = CallForm::InContext;
    impl_->routines.push_back(std::move(resolved));
    return impl_->routines.size() - 1;
}

std::vector<std::uint8_t> Vm::assemble(std::string_view source) {
    // The VM's platform (fixed at construction) selects the backend, which selects the ISA's assembler —
    // so "which ISA" is never ambiguous. A pure source → bytes transform; placement is a separate step.
    return impl_->backend->assemble(std::string(source)).bytes;
}

std::size_t Vm::registerRoutineResolvingPolicy(std::string_view logicalPath,
                                               const RoutineBinding& binding,
                                               std::optional<AssetPolicy> policy,
                                               std::span<const int> inputWidths, int outputWidth,
                                               int instances) {
    // Embed (default): the build scan baked the assembled bytes into the routine registry, keyed by the
    // logical path; place them directly. If none were baked (the target was not run through the scan)
    // fall through to the on-disk read so the literal path still resolves during development.
    if (resolveAssetPolicy(policy, AssetPolicy::Embed) == AssetPolicy::Embed) {
        if (const std::span<const std::uint8_t> baked = detail::findEmbeddedRoutine(logicalPath);
            !baked.empty()) {
            return registerResolved(baked, binding, inputWidths, outputWidth, instances);
        }
        detail::warnEmbedNotBaked("routine", logicalPath);
    }
    // LoadFromPath (or an un-baked Embed): resolve the full project-relative logical path against the
    // engine's single assetRoot(), read it, assemble it in this VM's ISA, and register the resulting bytes
    // exactly as the byte form does — registerResolved copies them into the code arena, so the temporary
    // buffer's lifetime is fine.
    const std::filesystem::path full = assetRoot() / std::filesystem::path(logicalPath);
    std::ifstream in{full, std::ios::binary};
    if (!in) {
        throw std::runtime_error("VM: cannot open routine .asm file: " + full.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const vm::AssembledRoutine assembled = impl_->backend->assemble(ss.str());
    return registerResolved(std::span<const std::uint8_t>(assembled.bytes), binding, inputWidths,
                            outputWidth, instances);
}

std::uint64_t Vm::invoke(std::size_t handle, std::span<const CallValue> inputs) {
    const ResolvedRoutine& routine = impl_->routines[handle];
    vm::VmBackend& backend = *impl_->backend;

    if (routine.form == CallForm::InContext) {
        return impl_->invokeInContext(routine, inputs);
    }

    backend.beginCall(routine.entry);
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const Location& loc = routine.inputs[i];
        if (loc.kind() == Location::Kind::Register) {
            backend.writeRegister(loc.registerId(), inputs[i].value, inputs[i].width);
        } else {
            backend.writeMemory(loc.address(), inputs[i].value, inputs[i].width);
        }
    }
    backend.run();

    if (!routine.output.has_value()) {
        return 0;
    }
    const Location& out = *routine.output;
    if (out.kind() == Location::Kind::Register) {
        return backend.readRegister(out.registerId());
    }
    return backend.readMemory(out.address(), routine.outputWidth);
}

// ── The deterministic seam (vm_testing.h) ───────────────────────────────────────────────────────

namespace vm {

std::unique_ptr<VmBackend> VmTestAccess::substituteBackend(Vm& v,
                                                           std::unique_ptr<VmBackend> backend) {
    std::unique_ptr<VmBackend> previous = std::move(v.impl_->backend);
    v.impl_->backend = std::move(backend);
    // The sink belongs to the machine that holds it, so the replacement is handed its own on the
    // next declaration rather than inheriting one installed elsewhere.
    v.impl_->escapeSinkInstalled = false;
    v.impl_->watchSinkInstalled  = false;
    return previous;
}

void VmTestAccess::runInline(Vm& v) { v.impl_->startRun(&v, VmRunner::Mode::Inline); }

std::uint64_t VmTestAccess::tickBudget(Vm& v, std::chrono::nanoseconds period) {
    return v.impl_->tickBudget(period);
}

std::uint64_t VmTestAccess::stepOnce(Vm& v) { return v.impl_->romRun.runner->stepOnce(); }

void VmTestAccess::tornPublishBegin(Vm& v) {
    v.impl_->romRun.seq.fetch_add(1, std::memory_order_release);  // -> odd (mid-flight)
}

void VmTestAccess::tornPublishEnd(Vm& v) {
    v.impl_->capturePublished();
    v.impl_->romRun.seq.fetch_add(1, std::memory_order_release);  // -> even (stable)
}

bool VmTestAccess::readIsStable(const Vm& v) {
    return (v.impl_->romRun.seq.load(std::memory_order_acquire) & 1u) == 0;
}

std::uint32_t VmTestAccess::publishSeq(const Vm& v) {
    return v.impl_->romRun.seq.load(std::memory_order_acquire);
}

void VmTestAccess::saveFilesAt(Vm& v, std::filesystem::path base) {
    v.impl_->save.root = std::move(base);
}

void VmTestAccess::flushSaveData(Vm& v) { v.impl_->flushSaveData(); }

bool VmTestAccess::saveDataPending(const Vm& v) { return v.impl_->save.dirty; }

}  // namespace vm

}  // namespace retropp

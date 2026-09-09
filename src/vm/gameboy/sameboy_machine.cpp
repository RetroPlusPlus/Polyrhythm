// SameBoy backend implementation. The only engine TU that includes
// gb.h; everything SameBoy is contained here (the header is pimpl'd).
#include "src/vm/gameboy/sameboy_machine.h"

#include <algorithm>
#include <csetjmp>
#include <utility>
#include <vector>

#include "gb.h"

// The instruction stepper GB_run wraps, declared here — the vendor's sm83_cpu.h declares it behind
// GB_INTERNAL, which this TU does not define, and the vendor's own sources stay as they are.
//
// It is what a call in the guest's context steps through. GB_run holds a running-context assertion
// (GB_ASSERT_NOT_RUNNING, gb.c:1192) and resets the cycle tally it returns (gb.c:1211); the stepper
// holds neither, and the cycles it advances accumulate into the tally of whatever GB_run encloses
// it — which is where a guest's own time belongs.
extern "C" void GB_cpu_run(GB_gameboy_t* gb);

namespace retropp::vm {

namespace {
// Defined below; named here so the Impl can install and remove them in one place.
void executionCallback(GB_gameboy_t* gb, std::uint16_t address, std::uint8_t opcode);
std::uint8_t readMemoryCallback(GB_gameboy_t* gb, std::uint16_t address, std::uint8_t data);
bool writeMemoryCallback(GB_gameboy_t* gb, std::uint16_t address, std::uint8_t value);

// The watched set, one bit per address over the whole 16-bit space. 8 KiB, allocated the first time
// an address is watched in that direction — the test the hook runs on every access is then one load
// and a mask.
constexpr std::size_t kWatchBitmapBytes = 0x10000 / 8;

void setWatchBit(std::vector<std::uint8_t>& bits, std::uint16_t address) {
    if (bits.empty()) {
        bits.assign(kWatchBitmapBytes, 0);
    }
    bits[address >> 3] |= static_cast<std::uint8_t>(1u << (address & 7u));
}

void clearWatchBit(std::vector<std::uint8_t>& bits, std::uint16_t address) {
    if (!bits.empty()) {
        bits[address >> 3] &= static_cast<std::uint8_t>(~(1u << (address & 7u)));
    }
}

[[nodiscard]] bool watchBitSet(const std::vector<std::uint8_t>& bits, std::uint16_t address) {
    return !bits.empty() &&
           (bits[address >> 3] & static_cast<std::uint8_t>(1u << (address & 7u))) != 0;
}
}  // namespace

// Holds the engine's own stores out of the watch path for as long as one is in flight, and gives
// back whatever was true before. Every store the engine makes through the bus — seeding a booted
// image, planting a return landing, realizing a substituted write — is the engine acting on the
// machine, and the last of those re-enters its own handler unless it is held out.
class SuppressAccessScope {
public:
    explicit SuppressAccessScope(bool& flag) noexcept : flag_(flag), previous_(flag) {
        flag_ = true;
    }
    ~SuppressAccessScope() { flag_ = previous_; }
    SuppressAccessScope(const SuppressAccessScope&)            = delete;
    SuppressAccessScope& operator=(const SuppressAccessScope&) = delete;

private:
    bool& flag_;
    bool  previous_;
};

// One call in the guest's context, while it is in flight: where it leaves, how far it may go, and
// the way out of the stepper. The frames live in the native call stack and link to each other, so a
// call made from inside a call is the same call one level in, at whatever depth the host reaches.
struct RunFrame {
    std::uint16_t landing = 0;
    std::size_t   maxInstructions = 0;
    std::size_t   instructions = 0;  // written by the execution callback
    RunFrame*     enclosing = nullptr;
    std::jmp_buf  out{};
};

// Embeds the SameBoy instance plus the run-to-return bookkeeping the execution
// callback writes. Heap-held behind the pimpl so the header pulls no GB_* type.
struct SameBoyMachine::Impl {
    GB_gameboy_t gb{};
    ConsoleModel model;

    // run-to-return state, read/written by the static execution callback below.
    std::uint16_t returnAddress = 0;
    std::size_t instructionCount = 0;
    bool reachedReturn = false;
    bool running = false;

    // Audio: the sink the APU sample callback forwards each frame to.
    SameBoyMachine::SampleSink sampleSink;

    // Picture: the buffer the PPU draws into (one RGBA word per pixel, screen-sized once drawing is
    // on, empty while it is off), the sink each finished frame is reported to, and whether drawing
    // was turned on at all.
    std::vector<std::uint32_t> pixels;
    SameBoyMachine::FrameSink  frameSink;
    bool                       pictureOn = false;

    // Watched addresses, sorted so the per-instruction check is a binary search, and the sink each
    // one reports to. Both are read by the execution callback below.
    std::vector<std::uint16_t> armedEscapes;
    SameBoyMachine::EscapeSink escapeSink;

    // The call in the guest's context currently in flight, innermost first. Null when none is.
    RunFrame* innermost = nullptr;

    // Whether the per-instruction hook is needed right now: a run-to-return is watching for its
    // landing, a call in the guest's context is watching for its own, or there is at least one
    // watched address to report.
    [[nodiscard]] bool hookNeeded() const {
        return running || innermost != nullptr ||
               (!armedEscapes.empty() && escapeSink != nullptr);
    }

    bool hookOn = false;

    // Bring the installed hook into line with whether anything needs it. The ONE place the callback
    // is installed or removed, so every path that changes what is watched ends here and no path can
    // leave a machine carrying a hook nothing asked for.
    void syncHook() {
        const bool want = hookNeeded();
        if (want == hookOn) {
            return;
        }
        GB_set_execution_callback(&gb, want ? &executionCallback : nullptr);
        hookOn = want;
    }

    // Watches: the bitmap per direction, how many addresses each holds (what decides whether its
    // hook is installed), and the sink each asks. Read by the per-access callbacks below.
    std::vector<std::uint8_t>        readWatchBits;
    std::vector<std::uint8_t>        writeWatchBits;
    std::size_t                      readWatchCount  = 0;
    std::size_t                      writeWatchCount = 0;
    SameBoyMachine::ReadAccessSink   onReadAccess;
    SameBoyMachine::WriteAccessSink  onWriteAccess;

    // True while a store the ENGINE makes is in flight, so that store is not itself watched.
    bool accessSuppressed = false;

    bool readHookOn  = false;
    bool writeHookOn = false;

    // The same discipline as syncHook, per direction: each hook exists exactly while something is
    // watched in that direction and a sink is there to answer. Detaching a callback asserts the
    // machine is not running on another thread (memory.c), which holds — every path here runs on
    // the thread that owns the machine.
    void syncAccessHooks() {
        const bool wantRead = readWatchCount != 0 && onReadAccess != nullptr;
        if (wantRead != readHookOn) {
            GB_set_read_memory_callback(&gb, wantRead ? &readMemoryCallback : nullptr);
            readHookOn = wantRead;
        }
        const bool wantWrite = writeWatchCount != 0 && onWriteAccess != nullptr;
        if (wantWrite != writeHookOn) {
            GB_set_write_memory_callback(&gb, wantWrite ? &writeMemoryCallback : nullptr);
            writeHookOn = wantWrite;
        }
    }

    explicit Impl(ConsoleModel m) : model(m) {
        const GB_model_t sbModel =
            (m == ConsoleModel::GameBoyColor) ? GB_MODEL_CGB_E : GB_MODEL_DMG_B;
        GB_init(&gb, sbModel);
        GB_set_user_data(&gb, this);
        // The VM is headless — it never displays. Disable PPU rendering so running the CPU for
        // extended periods (e.g. ticking the divider between RNG calls) never drives the PPU to
        // invoke the pixel/colour-encode callbacks this surgical backend never sets (a null call
        // → crash). Short routine calls never render; sustained idle does.
        GB_set_rendering_disabled(&gb, true);
    }

    ~Impl() { GB_free(&gb); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
};

namespace {

GB_direct_access_t toDirectAccess(GbHardwareMemory region) {
    switch (region) {
        case GbHardwareMemory::Rom:     return GB_DIRECT_ACCESS_ROM;
        case GbHardwareMemory::VRam:    return GB_DIRECT_ACCESS_VRAM;
        case GbHardwareMemory::WorkRam: return GB_DIRECT_ACCESS_RAM;
        case GbHardwareMemory::Oam:     return GB_DIRECT_ACCESS_OAM;
        case GbHardwareMemory::Hram:    return GB_DIRECT_ACCESS_HRAM;
        case GbHardwareMemory::Io:      return GB_DIRECT_ACCESS_IO;
    }
    return GB_DIRECT_ACCESS_HRAM;  // unreachable; quiets -Wreturn-type
}

// Fires once per instruction at its start (sm83_cpu.c passes the opcode's own
// address). One GB_run == one GB_cpu_run == one instruction, so the count is
// exact. The one hook serves both callers: it watches for control reaching a
// run-to-return's landing, and reports a watched address to the escape sink. The
// instruction executes once this returns, whatever either arm did — so reporting
// an address observes it and never replaces it.
void executionCallback(GB_gameboy_t* gb, std::uint16_t address, std::uint8_t /*opcode*/) {
    auto* impl = static_cast<SameBoyMachine::Impl*>(GB_get_user_data(gb));
    if (impl == nullptr) {
        return;
    }
    // The landing watch is checked first: a run's own terminator is the machine's business and
    // settles before anything the game asked for. A call in the guest's context settles ahead of
    // even that, because its landing is left from HERE, at the fetch, before the stepper reaches the
    // instruction there. Its instructions count against its own cap alone.
    if (RunFrame* frame = impl->innermost) {
        if (address == frame->landing) {
            std::longjmp(frame->out, 1);  // does not return
        }
        ++frame->instructions;
    } else if (impl->running) {
        ++impl->instructionCount;
        if (address == impl->returnAddress) {
            impl->reachedReturn = true;
        }
    }
    if (!impl->armedEscapes.empty() && impl->escapeSink) {
        if (std::binary_search(impl->armedEscapes.begin(), impl->armedEscapes.end(), address)) {
            impl->escapeSink(address);
        }
    }
}

// Fires on every read the CPU makes once one address is watched for reading (memory.c:805), with
// `data` the byte the machine was about to answer with. The byte returned is the one the guest
// sees.
//
// EVERY read means every read: the opcode fetch is a read like any other (sm83_cpu.c:91), so an
// address holding code is answered here when it is fetched. This is the address bus, not a
// data-only hook, and the layer above states that.
std::uint8_t readMemoryCallback(GB_gameboy_t* gb, std::uint16_t address, std::uint8_t data) {
    auto* impl = static_cast<SameBoyMachine::Impl*>(GB_get_user_data(gb));
    if (impl == nullptr || impl->accessSuppressed || !impl->onReadAccess) {
        return data;
    }
    if (!watchBitSet(impl->readWatchBits, address)) {
        return data;
    }
    return impl->onReadAccess(address, data);
}

// Fires on every write the CPU makes once one address is watched for writing (memory.c:1822),
// BEFORE the store happens; returning false prevents it. Substitution is composed here because the
// hook cannot change the value: prevent the guest's store, then make our own through the same bus,
// with this machine's own stores held out of the watch path while it happens.
bool writeMemoryCallback(GB_gameboy_t* gb, std::uint16_t address, std::uint8_t value) {
    auto* impl = static_cast<SameBoyMachine::Impl*>(GB_get_user_data(gb));
    if (impl == nullptr || impl->accessSuppressed || !impl->onWriteAccess) {
        return true;
    }
    if (!watchBitSet(impl->writeWatchBits, address)) {
        return true;
    }
    std::uint8_t substitute = 0;
    switch (impl->onWriteAccess(address, value, substitute)) {
        case SameBoyMachine::WriteAction::Allow:
            return true;
        case SameBoyMachine::WriteAction::Prevent:
            return false;
        case SameBoyMachine::WriteAction::Substitute: {
            const SuppressAccessScope ours{impl->accessSuppressed};
            GB_write_memory(gb, address, substitute);
            return false;
        }
    }
    return true;
}

// Fires once per produced APU output sample. Forwards the stereo frame to the installed
// sink — the producer side of the audio ring buffer. GB_sample_t exposes int16 left/right whether or
// not GB_INTERNAL is defined (this TU compiles without it, so it is the plain struct form).
void sampleCallback(GB_gameboy_t* gb, GB_sample_t* sample) {
    auto* impl = static_cast<SameBoyMachine::Impl*>(GB_get_user_data(gb));
    if (impl != nullptr && impl->sampleSink) {
        impl->sampleSink(sample->left, sample->right);
    }
}

// How the PPU writes a colour into the pixel buffer: one word per pixel whose BYTES are red, green,
// blue, alpha in memory order — GuestPixelFormat::Rgba8888 — since every target is little-endian.
// Alpha is opaque, because a console's picture carries no transparency of its own.
std::uint32_t rgbEncodeCallback(GB_gameboy_t*, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) |
           (static_cast<std::uint32_t>(b) << 16) | 0xFF000000u;
}

// Fires when the PPU finishes a frame — the one moment the buffer holds a whole picture rather than a
// raster part-way down. A REPEAT frame is one the hardware would not have redrawn: the screen keeps
// what it is showing, so nothing is reported and nothing downstream re-uploads.
void vblankCallback(GB_gameboy_t* gb, GB_vblank_type_t type) {
    if (type == GB_VBLANK_TYPE_REPEAT) {
        return;
    }
    auto* impl = static_cast<SameBoyMachine::Impl*>(GB_get_user_data(gb));
    if (impl == nullptr || !impl->frameSink) {
        return;
    }
    const int         width  = static_cast<int>(GB_get_screen_width(&impl->gb));
    const int         height = static_cast<int>(GB_get_screen_height(&impl->gb));
    const std::size_t bytes  = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                              sizeof(std::uint32_t);
    if (bytes > impl->pixels.size() * sizeof(std::uint32_t)) {
        return;  // a screen larger than the buffer drawing was enabled for; nothing whole to report
    }
    impl->frameSink(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(impl->pixels.data()), bytes),
        width, height);
}

// Step the CPU until the frame's landing is fetched or its cap is spent. The jump back out lands
// here, which is why this is its own function: `frame` belongs to the caller and the reference to it
// never changes, so everything read after the jump belongs to the caller too.
//
// The stepper is driven rather than GB_run because the interrupted instruction needs its own fetch
// cycles still latched when it resumes, and every GB_cpu_run ends by spending them
// (flush_pending_cycles, sm83_cpu.c:1718). Leaving from inside the hook, at the landing's fetch, puts
// that fetch's cycles in place of the ones the first nested instruction spent early — so the
// interrupted instruction finds exactly what it left, and the routine costs the guest its own
// instructions plus the one fetch made for the landing.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4611)  // this frame holds nothing with a destructor to unwind
#endif
void stepUntilLanding(GB_gameboy_t* gb, RunFrame& frame) {
    if (setjmp(frame.out) != 0) {
        return;  // the landing was reached; the instruction there never executes
    }
    while (frame.instructions < frame.maxInstructions) {
        GB_cpu_run(gb);
    }
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace

SameBoyMachine::SameBoyMachine(ConsoleModel model)
    : impl_(std::make_unique<Impl>(model)) {}

SameBoyMachine::~SameBoyMachine() = default;
SameBoyMachine::SameBoyMachine(SameBoyMachine&&) noexcept = default;
SameBoyMachine& SameBoyMachine::operator=(SameBoyMachine&&) noexcept = default;

ConsoleModel SameBoyMachine::model() const { return impl_->model; }

void SameBoyMachine::loadRom(std::span<const std::uint8_t> rom) {
    GB_load_rom_from_buffer(&impl_->gb, rom.data(), rom.size());
}

void SameBoyMachine::reset() { GB_reset(&impl_->gb); }

Registers SameBoyMachine::registers() const {
    const GB_registers_t* r = GB_get_registers(&impl_->gb);
    return Registers{r->af, r->bc, r->de, r->hl, r->sp, r->pc};
}

void SameBoyMachine::setRegisters(const Registers& regs) {
    GB_registers_t* r = GB_get_registers(&impl_->gb);
    r->af = regs.af;
    r->bc = regs.bc;
    r->de = regs.de;
    r->hl = regs.hl;
    r->sp = regs.sp;
    r->pc = regs.pc;
}

std::span<std::uint8_t> SameBoyMachine::memory(GbHardwareMemory region) {
    std::size_t size = 0;
    std::uint16_t bank = 0;
    void* p = GB_get_direct_access(&impl_->gb, toDirectAccess(region), &size, &bank);
    if (p == nullptr || size == 0) {
        return {};
    }
    return {static_cast<std::uint8_t*>(p), size};
}

void SameBoyMachine::busWrite(std::uint16_t address, std::uint8_t value) {
    // The engine's own store, never a watched access: this is what seeds a booted image and plants
    // a return landing, and neither is something a game declared a watch over.
    const SuppressAccessScope ours{impl_->accessSuppressed};
    GB_write_memory(&impl_->gb, address, value);
}

std::size_t SameBoyMachine::runToReturn(std::uint16_t returnAddress,
                                        std::size_t maxInstructions) {
    impl_->returnAddress = returnAddress;
    impl_->instructionCount = 0;
    impl_->reachedReturn = false;
    impl_->running = true;
    impl_->syncHook();
    while (!impl_->reachedReturn && impl_->instructionCount < maxInstructions) {
        GB_run(&impl_->gb);
    }
    impl_->running = false;
    impl_->syncHook();
    return impl_->instructionCount;
}

std::size_t SameBoyMachine::runInContext(std::uint16_t landing, std::size_t maxInstructions) {
    RunFrame frame;
    frame.landing         = landing;
    frame.maxInstructions = maxInstructions;
    frame.enclosing       = impl_->innermost;
    impl_->innermost      = &frame;
    impl_->syncHook();

    stepUntilLanding(&impl_->gb, frame);

    impl_->innermost = frame.enclosing;
    impl_->syncHook();
    return frame.instructions;
}

std::uint64_t SameBoyMachine::runToReturnCycles(std::uint16_t returnAddress,
                                                std::uint64_t maxTicks8MHz) {
    // Same return-address watch as runToReturn (the execution callback sets reachedReturn when control
    // reaches returnAddress), but the loop bound and the return value are GB_run's 8 MHz tick returns,
    // not the instruction count — so a driver entry's cost pads the frame in the unit the APU runs in.
    impl_->returnAddress = returnAddress;
    impl_->instructionCount = 0;
    impl_->reachedReturn = false;
    impl_->running = true;
    impl_->syncHook();
    std::uint64_t ran = 0;
    while (!impl_->reachedReturn && ran < maxTicks8MHz) {
        ran += GB_run(&impl_->gb);
    }
    impl_->running = false;
    impl_->syncHook();
    return ran;
}

void SameBoyMachine::enableAudio(unsigned sampleRate) {
    GB_set_sample_rate(&impl_->gb, sampleRate);
    // Hardware-faithful DC-blocking highpass — matches the filter on real hardware (the faithful
    // default; GB_HIGHPASS_OFF would keep a DC offset, GB_HIGHPASS_REMOVE_DC_OFFSET is non-hardware).
    GB_set_highpass_filter_mode(&impl_->gb, GB_HIGHPASS_ACCURATE);
    GB_apu_set_sample_callback(&impl_->gb, &sampleCallback);
}

void SameBoyMachine::setSampleSink(SampleSink sink) { impl_->sampleSink = std::move(sink); }

void SameBoyMachine::setFrameSink(FrameSink sink) { impl_->frameSink = std::move(sink); }

void SameBoyMachine::setPictureEnabled(bool enabled) {
    if (!enabled) {
        // Suppress pixel output again. The buffer and the callbacks stay where they are, so turning
        // drawing back on costs nothing beyond the flag.
        GB_set_rendering_disabled(&impl_->gb, true);
        impl_->pictureOn = false;
        return;
    }
    if (impl_->pictureOn) {
        return;
    }
    // The encoding first: the PPU asks for it as it draws, and the palettes it has already resolved
    // are re-encoded through it here.
    GB_set_rgb_encode_callback(&impl_->gb, &rgbEncodeCallback);
    const std::size_t count = static_cast<std::size_t>(GB_get_screen_width(&impl_->gb)) *
                              static_cast<std::size_t>(GB_get_screen_height(&impl_->gb));
    impl_->pixels.assign(count, 0);
    GB_set_pixels_output(&impl_->gb, impl_->pixels.data());
    GB_set_vblank_callback(&impl_->gb, &vblankCallback);
    // Everything the PPU needs is in place, so the pixels the ctor suppressed can be produced. The
    // buffer, the encoding and this flag all live in the region GB_reset preserves, so a machine that
    // is reset — by hosting a cartridge, or on its own — keeps drawing.
    GB_set_rendering_disabled(&impl_->gb, false);
    impl_->pictureOn = true;
}

bool SameBoyMachine::pictureEnabled() const { return impl_->pictureOn; }

std::uint64_t SameBoyMachine::runForCycles(std::uint64_t ticks8MHz) {
    // Step the CPU in raw GB_run increments (each returns the 8 MHz ticks that instruction took)
    // until the budget is met. The per-instruction hook is installed only while an address is
    // watched: with none, the CPU runs with no hook at all and only the APU sample callback fires,
    // draining produced PCM to the sink. The headless machine has PPU rendering disabled (see the
    // ctor), so running for extended periods is safe.
    impl_->syncHook();
    std::uint64_t ran = 0;
    while (ran < ticks8MHz) {
        ran += GB_run(&impl_->gb);
    }
    impl_->syncHook();
    return ran;
}

void SameBoyMachine::setEscapeSink(EscapeSink sink) {
    impl_->escapeSink = std::move(sink);
    impl_->syncHook();
}

void SameBoyMachine::armEscape(std::uint16_t address) {
    const auto at = std::lower_bound(impl_->armedEscapes.begin(), impl_->armedEscapes.end(), address);
    if (at == impl_->armedEscapes.end() || *at != address) {
        impl_->armedEscapes.insert(at, address);
    }
    impl_->syncHook();
}

void SameBoyMachine::disarmEscape(std::uint16_t address) {
    const auto at = std::lower_bound(impl_->armedEscapes.begin(), impl_->armedEscapes.end(), address);
    if (at != impl_->armedEscapes.end() && *at == address) {
        impl_->armedEscapes.erase(at);
    }
    impl_->syncHook();
}

std::size_t SameBoyMachine::armedEscapeCount() const { return impl_->armedEscapes.size(); }

void SameBoyMachine::setAccessSinks(ReadAccessSink onRead, WriteAccessSink onWrite) {
    impl_->onReadAccess  = std::move(onRead);
    impl_->onWriteAccess = std::move(onWrite);
    impl_->syncAccessHooks();
}

void SameBoyMachine::armReadWatch(std::uint16_t address) {
    if (watchBitSet(impl_->readWatchBits, address)) {
        return;
    }
    setWatchBit(impl_->readWatchBits, address);
    ++impl_->readWatchCount;
    impl_->syncAccessHooks();
}

void SameBoyMachine::disarmReadWatch(std::uint16_t address) {
    if (!watchBitSet(impl_->readWatchBits, address)) {
        return;
    }
    clearWatchBit(impl_->readWatchBits, address);
    --impl_->readWatchCount;
    impl_->syncAccessHooks();
}

void SameBoyMachine::armWriteWatch(std::uint16_t address) {
    if (watchBitSet(impl_->writeWatchBits, address)) {
        return;
    }
    setWatchBit(impl_->writeWatchBits, address);
    ++impl_->writeWatchCount;
    impl_->syncAccessHooks();
}

void SameBoyMachine::disarmWriteWatch(std::uint16_t address) {
    if (!watchBitSet(impl_->writeWatchBits, address)) {
        return;
    }
    clearWatchBit(impl_->writeWatchBits, address);
    --impl_->writeWatchCount;
    impl_->syncAccessHooks();
}

bool SameBoyMachine::readWatchInstalled() const { return impl_->readHookOn; }
bool SameBoyMachine::writeWatchInstalled() const { return impl_->writeHookOn; }

bool SameBoyMachine::hookInstalled() const { return impl_->hookOn; }

std::uint16_t SameBoyMachine::mappedRomBank() const {
    std::size_t   size = 0;
    std::uint16_t bank = 0;
    GB_get_direct_access(&impl_->gb, GB_DIRECT_ACCESS_ROM, &size, &bank);
    return bank;
}

}  // namespace retropp::vm

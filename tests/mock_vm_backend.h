#ifndef RETROPP_TESTS_MOCK_VM_BACKEND_H
#define RETROPP_TESTS_MOCK_VM_BACKEND_H

// A machine that is not a machine: plain bytes and a scripted walk in place of a CPU.
//
// It implements the backend seam with a flat 64 KiB array for memory and a `walk` that names the
// addresses it "executes", in order. Everything a real console does — assembling, calling, producing
// audio — throws, because none of it is what this exists to answer.
//
// What it exists to answer is where the escape surface lives. Declaring escapes, validating a batch,
// dispatching a fire to the right handler, arming and disarming: if all of that works on a machine
// with no CPU and no per-instruction hook of its own beyond a for-loop, then it belongs to the host
// layer rather than to any one core's capabilities.
//
// TEST-ONLY — under tests/, never linked into the engine.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "src/vm/vm_backend.h"

namespace retropp::testing {

class MockVmBackend final : public vm::VmBackend {
public:
    // ── What this machine is for ─────────────────────────────────────────────────────────────────

    // Execute these addresses in order. Reaching a watched one reports it to the sink and then
    // carries on, exactly as a real instruction executes after its handler returns.
    void walk(std::span<const std::uint32_t> addresses) {
        for (const std::uint32_t address : addresses) {
            ++executed_;
            // Read the watched set fresh each step: a handler may declare or drop escapes, and the
            // surface has to tolerate that without the machine knowing anything about depth.
            if (escapeSink_ && std::find(armed_.begin(), armed_.end(), address) != armed_.end()) {
                escapeSink_(address);
            }
        }
    }

    [[nodiscard]] std::size_t armedCount() const noexcept { return armed_.size(); }
    [[nodiscard]] std::size_t executed() const noexcept { return executed_; }
    [[nodiscard]] bool        hasSink() const noexcept { return escapeSink_ != nullptr; }

    [[nodiscard]] bool isArmed(std::uint32_t address) const noexcept {
        return std::find(armed_.begin(), armed_.end(), address) != armed_.end();
    }

    // Refuse to watch anything, the way a core with no per-instruction hook must.
    void refuseEscapes(bool refuse) noexcept { acceptLimit_ = refuse ? 0 : kNoLimit; }

    // Accept this many addresses and refuse the rest — a machine that gives out partway through a
    // batch, which is what makes the caller's undo observable.
    void acceptAtMost(std::size_t count) noexcept { acceptLimit_ = count; }

    [[nodiscard]] std::span<std::uint8_t> bytes() noexcept { return memory_; }

    // ── Calling into the guest ───────────────────────────────────────────────────────────────────
    // This machine executes nothing, so a call is the ASK it was made with, recorded. What a routine
    // would have done to memory is supplied by the test through onContextCall, which runs where the
    // routine's own instructions would have — before the output is read.

    struct ContextCall {
        std::uint32_t                     entry = 0;
        std::vector<vm::ResidentRegister> presets;
        vm::CallStack                     stack = vm::CallStack::Scratch;
        std::size_t                       maxInstructions = 0;
    };

    void callInContext(std::uint32_t entry, std::span<const vm::ResidentRegister> presets,
                       vm::CallStack stack, std::size_t maxInstructions,
                       const std::function<void()>& readOutputs) override {
        calls_.push_back(ContextCall{.entry = entry,
                                     .presets = {presets.begin(), presets.end()},
                                     .stack = stack,
                                     .maxInstructions = maxInstructions});
        if (onCall_) {
            onCall_(entry);
        }
        if (readOutputs) {
            readOutputs();
        }
    }

    // What a called routine does. Runs on every call, with the entry it was called at.
    void onContextCall(std::function<void(std::uint32_t)> fn) { onCall_ = std::move(fn); }

    [[nodiscard]] const std::vector<ContextCall>& contextCalls() const noexcept { return calls_; }

    // ── The escape seam ──────────────────────────────────────────────────────────────────────────

    void setEscapeSink(EscapeSink sink) override { escapeSink_ = std::move(sink); }

    void armEscape(std::uint32_t address, bool replacesRoutine) override {
        if (armed_.size() >= acceptLimit_) {
            throw std::logic_error("this machine cannot watch that many addresses");
        }
        if (!isArmed(address)) {
            armed_.push_back(address);
        }
        // This machine executes no bytes, so "replacing" patches nothing — it records the ask, so a
        // test can pin that the host layer asked for the answering kind and released it again.
        if (replacesRoutine) {
            replaced_.push_back(address);
        }
    }

    void disarmEscape(std::uint32_t address) override {
        armed_.erase(std::remove(armed_.begin(), armed_.end(), address), armed_.end());
        replaced_.erase(std::remove(replaced_.begin(), replaced_.end(), address), replaced_.end());
    }

    [[nodiscard]] bool isReplaced(std::uint32_t address) const noexcept {
        return std::find(replaced_.begin(), replaced_.end(), address) != replaced_.end();
    }

    // ── The watch seam ───────────────────────────────────────────────────────────────────────────

    void setWatchSink(WatchSink sink) override { watchSink_ = std::move(sink); }

    void armWatch(const MemoryRegion& where, bool onRead, bool onWrite) override {
        if (watchLimit_ == 0) {
            throw std::logic_error("this machine cannot watch accesses");
        }
        for (const ArmedWatch& w : armedWatches_) {
            if (w.at == where.at && w.span == where.totalBytes()) {
                return;
            }
        }
        armedWatches_.push_back(ArmedWatch{.at      = where.at,
                                           .span    = where.totalBytes(),
                                           .onRead  = onRead,
                                           .onWrite = onWrite});
    }

    void disarmWatch(const MemoryRegion& where, bool /*onRead*/, bool /*onWrite*/) override {
        const std::uint64_t span = where.totalBytes();
        armedWatches_.erase(std::remove_if(armedWatches_.begin(), armedWatches_.end(),
                                           [&where, span](const ArmedWatch& w) {
                                               return w.at == where.at && w.span == span;
                                           }),
                            armedWatches_.end());
    }

    // Refuse to watch anything, the way a core with no per-access hook must.
    void refuseWatches(bool refuse) noexcept { watchLimit_ = refuse ? 0 : kNoLimit; }

    [[nodiscard]] std::size_t armedWatchCount() const noexcept { return armedWatches_.size(); }

    // The guest reads one byte: ask whatever is watching, then answer with what it said. A read
    // cannot be prevented, so a veto answers with the byte memory holds — exactly as a real core's
    // read hook must, since the instruction has already committed to reading something.
    [[nodiscard]] std::uint8_t guestRead(std::uint32_t at) {
        const std::uint8_t held = memory_.at(at);
        const AccessVerdict verdict = askWatch(at, AccessKind::Read, held);
        return verdict.kind() == AccessVerdict::Kind::Instead ? verdict.value() : held;
    }

    // The guest writes one byte: ask whatever is watching, then do what it said. The store that
    // realizes instead(v) is the engine's own and is not itself watched.
    void guestWrite(std::uint32_t at, std::uint8_t value) {
        const AccessVerdict verdict = askWatch(at, AccessKind::Write, value);
        switch (verdict.kind()) {
            case AccessVerdict::Kind::Proceed: memory_.at(at) = value; return;
            case AccessVerdict::Kind::Veto:    return;
            case AccessVerdict::Kind::Instead: memory_.at(at) = verdict.value(); return;
        }
    }

    // ── Memory ───────────────────────────────────────────────────────────────────────────────────

    // A flat machine: `at` is an offset into the one memory there is, and anything past the end of it
    // is not somewhere this machine can reach. The address is used as given: a machine that quietly
    // folded an out-of-range address back into its memory would answer yes to a question it refuses.
    [[nodiscard]] bool regionIsAddressable(const MemoryRegion& region) const override {
        return static_cast<std::uint64_t>(region.at) + region.totalBytes() <= memory_.size();
    }

    void readRegion(const MemoryRegion& region, std::uint32_t index,
                    std::span<std::uint8_t> out) override {
        const std::size_t at = entryOffset(region, index, out.size());
        std::copy_n(memory_.begin() + static_cast<std::ptrdiff_t>(at), out.size(), out.begin());
    }

    void writeRegion(const MemoryRegion& region, std::uint32_t index,
                     std::span<const std::uint8_t> in) override {
        const std::size_t at = entryOffset(region, index, in.size());
        std::copy_n(in.begin(), in.size(), memory_.begin() + static_cast<std::ptrdiff_t>(at));
    }

    // ── Everything a console does, which this is not ─────────────────────────────────────────────

    void reset() override { memory_.fill(0); }
    void advanceClock(std::uint64_t) override {}

    std::uint32_t placeRoutine(std::span<const std::uint8_t>) override { throw notAMachine(); }
    void          loadRom(std::span<const std::uint8_t>) override { throw notAMachine(); }
    void          bootHostedRom() override { throw notAMachine(); }

    [[nodiscard]] vm::AssembledRoutine assemble(std::string_view) const override {
        throw notAMachine();
    }
    [[nodiscard]] int registerWidthBytes(std::uint16_t) const override { return 0; }

    void beginCall(std::uint32_t) override { throw notAMachine(); }
    void writeRegister(std::uint16_t, std::uint64_t, int) override { throw notAMachine(); }
    void run() override { throw notAMachine(); }
    [[nodiscard]] std::uint64_t readRegister(std::uint16_t) override { throw notAMachine(); }
    void writeLiveRegister(std::uint16_t, std::uint64_t, int) override { throw notAMachine(); }

    // Live word access over the flat array — what a replacement's memory-convention marshalling
    // reads and writes. Little-endian, as every console this engine hosts is.
    [[nodiscard]] std::uint64_t readMemory(std::uint32_t address, int width) override {
        std::uint64_t value = 0;
        for (int i = 0; i < width; ++i) {
            value |= static_cast<std::uint64_t>(memory_.at(address + static_cast<std::size_t>(i)))
                     << (8 * i);
        }
        return value;
    }
    void writeMemory(std::uint32_t address, std::uint64_t value, int width) override {
        for (int i = 0; i < width; ++i) {
            memory_.at(address + static_cast<std::size_t>(i)) =
                static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
        }
    }

    // Guest input: recorded rather than realized, so a device-free case can assert what reached the
    // seam and in which step.
    [[nodiscard]] bool takesButtons() const override { return takesButtons_; }
    void setButtons(std::uint64_t held) override {
        buttonsSet.push_back(held);
    }
    bool                       takesButtons_ = true;  // flip to false to exercise the refusal
    std::vector<std::uint64_t> buttonsSet;            // every word that reached the seam, in order

    void enableAudio(unsigned, AudioSampleSink) override { throw notAMachine(); }
    void beginContinuous(std::uint32_t) override { throw notAMachine(); }
    std::uint64_t runForCycles(std::uint64_t) override { throw notAMachine(); }

    void configureResidentImage(std::span<const DriverImage>, Mapper, std::uint32_t) override {
        throw notAMachine();
    }
    std::uint64_t callResident(std::uint32_t, std::span<const vm::ResidentRegister>,
                               std::uint64_t) override {
        throw notAMachine();
    }

    // ── The video seam ─────────────────────────────────────────────────────────────────────────
    // This machine has no PPU either, so the frames come from the test: finishFrame is where a real
    // core's vblank is, and it reports only once drawing has been turned on — which is what lets a
    // case watch a machine that was never asked to draw stay silent.

    void setFrameSink(FrameSink sink) override { frameSink_ = std::move(sink); }

    void setVideoEnabled(bool enabled) override {
        if (enabled && refuseVideo_) {
            throw std::logic_error("MockVmBackend: this machine draws nothing");
        }
        videoOn_ = enabled;
    }

    // Draw nothing at all, the way a core with no video must.
    void refuseVideo(bool refuse) noexcept { refuseVideo_ = refuse; }

    [[nodiscard]] bool videoEnabled() const noexcept { return videoOn_; }

    // Finish one frame of `pixels` at these dimensions — where a real core's vblank fires.
    void finishFrame(std::span<const std::uint8_t> pixels, int width, int height) {
        if (videoOn_ && frameSink_) {
            frameSink_(pixels, width, height, RasterPixelFormat::Rgba8888);
        }
    }

    // ── The save-data seam ─────────────────────────────────────────────────────────────────────
    // This machine has no power to go off, so it keeps nothing: a core with no persistence model is a
    // thing the seam is built to allow, and this is one. The save-data behaviour itself is exercised
    // against the real core (vm_save_data_test.cpp), because the chain under test — a guest's store
    // raising the core's own change signal — only exists when the core is real.

    [[nodiscard]] bool keepsSaveData() const override { return false; }
    [[nodiscard]] std::string_view saveDataExtension() const override { return {}; }
    [[nodiscard]] std::size_t saveDataSize() const override { return 0; }
    [[nodiscard]] std::vector<std::uint8_t> readSaveData() override { return {}; }
    void writeSaveData(std::span<const std::uint8_t>) override { throw notAMachine(); }
    [[nodiscard]] bool takeSaveDataChanged() override { return false; }

private:
    [[nodiscard]] static std::logic_error notAMachine() {
        return std::logic_error("MockVmBackend: this machine has no CPU");
    }

    // One watched place, in the flat vocabulary this machine has: no banks, so `at` is the address.
    struct ArmedWatch {
        std::uint32_t at      = 0;
        std::uint64_t span    = 0;
        bool          onRead  = false;
        bool          onWrite = false;
    };

    // The first armed place covering the address in the direction asked, or proceed if none does.
    // Read fresh each access: a handler may declare or drop watches.
    [[nodiscard]] AccessVerdict askWatch(std::uint32_t at, AccessKind kind, std::uint8_t value) {
        if (!watchSink_) {
            return AccessVerdict::proceed();
        }
        for (const ArmedWatch& w : armedWatches_) {
            if (at < w.at || at - w.at >= w.span) {
                continue;
            }
            if (kind == AccessKind::Read ? !w.onRead : !w.onWrite) {
                continue;
            }
            const std::uint32_t base = w.at;
            return watchSink_(base, at, kind, value);
        }
        return AccessVerdict::proceed();
    }

    [[nodiscard]] std::size_t entryOffset(const MemoryRegion& region, std::uint32_t index,
                                          std::size_t bytes) const {
        if (!region.contains(index)) {
            throw std::out_of_range("MockVmBackend: no such entry");
        }
        if (bytes != region.size) {
            throw std::invalid_argument("MockVmBackend: not one entry wide");
        }
        return region.at + static_cast<std::size_t>(index) * region.size;
    }

    static constexpr std::size_t kNoLimit = static_cast<std::size_t>(-1);

    std::array<std::uint8_t, 0x10000> memory_{};
    std::vector<std::uint32_t>        armed_;
    std::vector<std::uint32_t>        replaced_;
    EscapeSink                        escapeSink_;
    std::vector<ContextCall>          calls_;
    std::function<void(std::uint32_t)> onCall_;
    std::vector<ArmedWatch>           armedWatches_;
    WatchSink                         watchSink_;
    FrameSink                         frameSink_;
    bool                              videoOn_     = false;
    bool                              refuseVideo_ = false;
    std::vector<std::uint8_t>         saveData_;
    bool                              saveDirty_     = false;
    bool                              keepsSaveData_ = true;
    std::size_t                       executed_    = 0;
    std::size_t                       acceptLimit_ = kNoLimit;
    std::size_t                       watchLimit_  = kNoLimit;
};

}  // namespace retropp::testing

#endif  // RETROPP_TESTS_MOCK_VM_BACKEND_H

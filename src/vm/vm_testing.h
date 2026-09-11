#ifndef RETROPP_SRC_VM_VM_TESTING_H
#define RETROPP_SRC_VM_VM_TESTING_H

// The deterministic seam device-free tests step a running cartridge through: the same machinery
// Vm::run() stands up, in the runner's Inline mode — no thread, every step on the calling thread —
// so a test orders boots, steps, writes and reads exactly. The torn-publish pair drives the
// publish's seqlock through its mid-flight state deterministically, which no thread schedule can
// be trusted to do.
//
// INTERNAL — under src/vm/, never include/retropp/; test TUs only.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>

#include "retropp/vm.h"
#include "src/vm/vm_backend.h"

namespace retropp::vm {

struct VmTestAccess {
    // Drive this Vm through a different machine: any VmBackend, including a test double with plain
    // arrays and a scripted walk in place of a CPU. What the host layer does — the declared tables,
    // the validation, the dispatch — is then exercised with no console core underneath it, which is
    // what proves those things belong to the host layer and not to one core's capabilities.
    //
    // Substitute before declaring anything on the Vm: declarations are validated and armed against
    // the machine that was there when they were made.
    static std::unique_ptr<VmBackend> substituteBackend(Vm& vm, std::unique_ptr<VmBackend> backend);

    // Stand the run up inline: boot (first episode), the first publish, both step hooks — and no
    // thread. The machine reports running (reads route to the publish, writes queue) and advances
    // only when stepOnce is called.
    static void runInline(Vm& vm);

    // One step on the calling thread: drain queued writes, advance one budget, publish. Returns the
    // CPU cycles the step consumed.
    static std::uint64_t stepOnce(Vm& vm);

    // What one tick of `period` is worth to this machine at the factor as it stands — the budget an
    // Advance::OnTick step runs. Calling it advances both carries, exactly as a tick does, which is
    // what lets a test walk the arithmetic one tick at a time.
    [[nodiscard]] static std::uint64_t tickBudget(Vm& vm, std::chrono::nanoseconds period);

    // Hold the publish mid-flight (sequence to odd), and complete it (capture + sequence to even).
    // Between the two, the machine's published set is officially unstable — what a reader must
    // refuse to return.
    static void tornPublishBegin(Vm& vm);
    static void tornPublishEnd(Vm& vm);

    // One seqlock attempt without the retry loop: whether a read taken now would be stable. The
    // real read() loops on exactly this condition.
    [[nodiscard]] static bool readIsStable(const Vm& vm);

    // The publish sequence word: even when stable, odd mid-flight, advanced by two per publish —
    // how a test pins that every step publishes exactly once.
    [[nodiscard]] static std::uint32_t publishSeq(const Vm& vm);

    // ── Save data ─────────────────────────────────────────────────────────────────────────────
    // Root this machine's own files at `base` rather than the player's data directory — the same
    // seam UserFiles::atPath is, reached for the same reason: a case that wrote into the real one
    // would depend on the machine it runs on and leave files behind. Set it before hosting an
    // image, which is where the stored copy is read back.
    static void saveFilesAt(Vm& vm, std::filesystem::path base);

    // Hand the machine's data over now, on the calling thread, and wait for it to be written —
    // what stop() does, without stopping. Lets a case assert the file's contents at a chosen
    // moment instead of against an interval.
    static void flushSaveData(Vm& vm);

    // Whether the machine is holding changes the interval has not let it hand over yet.
    [[nodiscard]] static bool saveDataPending(const Vm& vm);
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_VM_TESTING_H

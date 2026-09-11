#ifndef RETROPP_SRC_VM_SAVE_WRITER_H
#define RETROPP_SRC_VM_SAVE_WRITER_H

// Where a keyed machine's bytes reach the disk, off every thread that is being paced.
//
// A machine's own thread takes the snapshot — it must, since the bytes come out of the machine — and
// hands it here. Writing them is a flush to the device, which takes milliseconds and takes them
// unpredictably; on a machine advanced by the game's tick that thread IS the game's, so a write
// performed there would stall the frame the game is in the middle of, once per interval, for as long
// as the device felt like taking. Nothing about writing bytes that have already been copied out needs
// the machine, so nothing here happens on a thread that owes anyone a frame.
//
// ONE thread serves every machine, not one per machine. Three things follow from that and none of
// them would from the other arrangement: a game running many machines grows no threads; two machines
// that were given the same name cannot interleave their bytes into the same file, because one writer
// takes them in turn; and a machine that changes again while its last snapshot is still waiting
// replaces it rather than stacking a second write, since only the newest state is worth writing.
//
// INTERNAL — under src/vm/, never include/retropp/.

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "retropp/user_files.h"  // UserFiles — the store a machine's files are written through

namespace retropp::vm {

class SaveWriter {
public:
    // The one writer. Its thread starts with the first queued write and is joined at exit, after
    // whatever is still waiting has been written.
    static SaveWriter& shared();

    SaveWriter(const SaveWriter&)            = delete;
    SaveWriter& operator=(const SaveWriter&) = delete;

    // Hand over `bytes` to be written to `document`. Returns at once, having touched no disk: the
    // bytes are held until the writer reaches them. Anything still waiting for the same file is
    // replaced — a newer snapshot of a machine says everything an older one did.
    void queue(const UserFiles& files, std::string_view document, std::vector<std::uint8_t> bytes);

    // Write `bytes` to `document` on the CALLING thread and do not return until they are on disk,
    // after anything queued or in flight for the same file has settled. This is the shutdown path:
    // the caller may be about to go away, so the bytes cannot be left with a thread that has not run
    // yet. Returns whether the write completed.
    bool writeNow(const UserFiles& files, std::string_view document,
                  std::vector<std::uint8_t> bytes);

    // Wait until nothing is queued or being written for `document`, so a caller about to look at that
    // file — or about to go away — sees everything already handed over. Returns at once when the
    // writer has nothing for it.
    //
    // This is what makes putting a machine away mean its data is ON DISK rather than merely handed
    // over: a snapshot taken mid-run went to the writer without blocking, and the machine can be
    // parked and read back before that thread has run at all.
    void settle(const UserFiles& files, std::string_view document);

private:
    SaveWriter() = default;
    ~SaveWriter();

    // One file waiting to be written, keyed in `pending_` by where it lands.
    struct Waiting {
        UserFiles                 files;
        std::string               document;
        std::vector<std::uint8_t> bytes;
    };

    void ensureThread();  // called with mx_ held
    void loop();

    std::mutex              mx_;
    std::condition_variable work_;      // a file arrived, or the writer was asked to leave
    std::condition_variable settled_;   // the file that was in flight is written
    std::map<std::filesystem::path, Waiting> pending_;
    std::optional<std::filesystem::path>     inFlight_;
    bool                                     leaving_ = false;
    std::thread                              thread_;
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_SAVE_WRITER_H

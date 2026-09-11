#include "src/vm/save_writer.h"

#include <span>
#include <utility>

namespace retropp::vm {

SaveWriter& SaveWriter::shared() {
    static SaveWriter writer;
    return writer;
}

SaveWriter::~SaveWriter() {
    {
        const std::lock_guard<std::mutex> lock(mx_);
        leaving_ = true;
    }
    work_.notify_all();
    if (thread_.joinable()) {
        thread_.join();  // the loop writes what is still waiting before it leaves
    }
}

void SaveWriter::ensureThread() {
    if (!thread_.joinable()) {
        thread_ = std::thread([this] { loop(); });
    }
}

void SaveWriter::queue(const UserFiles& files, std::string_view document,
                       std::vector<std::uint8_t> bytes) {
    const std::filesystem::path at = files.pathFor(document);
    {
        const std::lock_guard<std::mutex> lock(mx_);
        // Replaced, never accumulated: the newest snapshot of a machine says everything an older one
        // did, so keeping both would write the same file twice to reach the same state.
        //
        // insert_or_assign, not operator[] — the latter DEFAULT-CONSTRUCTS the mapped value before
        // assigning over it, and a default-constructed UserFiles resolves the player's data directory
        // and throws when the game has not published an identity. The placeholder must never exist.
        pending_.insert_or_assign(
            at, Waiting{.files    = files,
                        .document = std::string(document),
                        .bytes    = std::move(bytes)});
        ensureThread();
    }
    work_.notify_one();
}

bool SaveWriter::writeNow(const UserFiles& files, std::string_view document,
                          std::vector<std::uint8_t> bytes) {
    const std::filesystem::path at = files.pathFor(document);
    {
        std::unique_lock<std::mutex> lock(mx_);
        // Whatever was waiting for this file is dropped in favour of the bytes handed over here: they
        // are the machine's final state, and the older snapshot would only overwrite them if it landed
        // second. A copy already IN FLIGHT is a different matter — it is being written right now, so
        // this waits for it rather than racing it into the same file.
        pending_.erase(at);
        settled_.wait(lock, [&] { return !inFlight_.has_value() || *inFlight_ != at; });
    }
    UserFiles store = files;  // write() is the store's own act, and the caller's copy is const here
    return store.write(document, std::as_bytes(std::span{bytes}));
}

void SaveWriter::loop() {
    std::unique_lock<std::mutex> lock(mx_);
    for (;;) {
        work_.wait(lock, [this] { return leaving_ || !pending_.empty(); });
        if (pending_.empty()) {
            return;  // asked to leave, and the queue is empty
        }
        const auto it = pending_.begin();
        const std::filesystem::path at = it->first;
        Waiting                     item = std::move(it->second);
        pending_.erase(it);
        inFlight_ = at;

        // The write itself runs with the lock RELEASED: it is the slow part, and holding the lock
        // across it would make a machine's hand-off wait on exactly the flush this thread exists to
        // keep off its thread.
        lock.unlock();
        item.files.write(item.document, std::as_bytes(std::span{item.bytes}));
        lock.lock();

        inFlight_.reset();
        settled_.notify_all();
    }
}

}  // namespace retropp::vm

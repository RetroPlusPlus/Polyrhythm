#include "src/net/in_process.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace retropp::net {

namespace detail {

// One direction of a pair. The bytes are a queue, so what goes in comes out in the order it went in,
// and a reader takes what it asked for or what there is.
struct Pipe {
    std::deque<std::byte> bytes;
    bool                  closed = false;  // the end that writes here is gone
};

struct InProcessPair {
    std::mutex              mutex;
    std::condition_variable arrived;
    Pipe                    toSecond;  // the first end writes here, the second reads
    Pipe                    toFirst;
    int                     ends = 0;
};

}  // namespace detail

namespace {

std::mutex& registryMutex() {
    static std::mutex mutex;
    return mutex;
}

// Held weakly, so a name releases itself once both its ends are gone and the same name opens a fresh
// pair afterwards.
using Registry = std::map<std::string, std::weak_ptr<detail::InProcessPair>, std::less<>>;

Registry& registry() {
    static Registry table;
    return table;
}

detail::Pipe& inbound(detail::InProcessPair& pair, bool second) {
    return second ? pair.toSecond : pair.toFirst;
}

detail::Pipe& outbound(detail::InProcessPair& pair, bool second) {
    return second ? pair.toFirst : pair.toSecond;
}

}  // namespace

InProcessStream::~InProcessStream() {
    if (pair_ == nullptr) return;
    {
        const std::lock_guard<std::mutex> guard(pair_->mutex);
        // The far side reads what is already queued and then reads Closed, which is what a peer sees
        // when a socket closes in an orderly way.
        outbound(*pair_, second_).closed = true;
        --pair_->ends;
    }
    pair_->arrived.notify_all();
}

InProcessStream::InProcessStream(InProcessStream&& other) noexcept
    : pair_(std::move(other.pair_)), second_(other.second_), closed_(other.closed_) {
    other.pair_ = nullptr;
}

InProcessStream& InProcessStream::operator=(InProcessStream&& other) noexcept {
    if (this != &other) {
        InProcessStream released;
        released.pair_   = std::move(pair_);
        released.second_ = second_;

        pair_       = std::move(other.pair_);
        second_     = other.second_;
        closed_     = other.closed_;
        other.pair_ = nullptr;
    }
    return *this;
}

Transfer InProcessStream::send(std::span<const std::byte> bytes) {
    if (pair_ == nullptr) return Transfer{.status = Status::Closed, .bytes = 0};

    {
        const std::lock_guard<std::mutex> guard(pair_->mutex);
        if (inbound(*pair_, second_).closed) {
            closed_ = true;
            return Transfer{.status = Status::Closed, .bytes = 0};
        }
        detail::Pipe& out = outbound(*pair_, second_);
        out.bytes.insert(out.bytes.end(), bytes.begin(), bytes.end());
    }
    pair_->arrived.notify_all();
    return Transfer{.status = Status::Ok, .bytes = bytes.size()};
}

Transfer InProcessStream::receive(std::span<std::byte> into) {
    if (closed_ || pair_ == nullptr) return Transfer{.status = Status::Closed, .bytes = 0};

    const std::lock_guard<std::mutex> guard(pair_->mutex);
    detail::Pipe&                     in = inbound(*pair_, second_);

    if (in.bytes.empty()) {
        // A closed peer is reported only once the bytes it left are drained — the close is a state
        // behind the data, never ahead of it.
        if (in.closed) {
            closed_ = true;
            return Transfer{.status = Status::Closed, .bytes = 0};
        }
        return Transfer{.status = Status::WouldBlock, .bytes = 0};
    }

    const std::size_t taken = into.size() < in.bytes.size() ? into.size() : in.bytes.size();
    for (std::size_t i = 0; i < taken; ++i) {
        into[i] = in.bytes.front();
        in.bytes.pop_front();
    }
    return Transfer{.status = Status::Ok, .bytes = taken};
}

Status InProcessStream::waitReadable(std::chrono::milliseconds timeout) {
    if (pair_ == nullptr) return Status::Closed;

    std::unique_lock<std::mutex> guard(pair_->mutex);
    // A closed peer counts as readable, the way a socket at end of stream selects readable: the caller
    // reads next and gets its answer there.
    const bool ready = pair_->arrived.wait_for(guard, timeout, [this] {
        detail::Pipe& in = inbound(*pair_, second_);
        return !in.bytes.empty() || in.closed;
    });
    return ready ? Status::Ok : Status::TimedOut;
}

bool InProcessStream::closed() const noexcept { return closed_; }

Status openInProcessStream(std::string_view pair, InProcessStream& out) {
    std::shared_ptr<detail::InProcessPair> joined;
    bool                                   second = false;

    {
        const std::lock_guard<std::mutex> guard(registryMutex());
        Registry&                         table = registry();

        const auto existing = table.find(pair);
        if (existing != table.end()) joined = existing->second.lock();
        if (joined == nullptr) {
            joined            = std::make_shared<detail::InProcessPair>();
            table[std::string(pair)] = joined;
        }

        const std::lock_guard<std::mutex> pairGuard(joined->mutex);
        if (joined->ends >= 2) return Status::Refused;
        second = joined->ends == 1;
        ++joined->ends;
    }

    out         = InProcessStream{};
    out.pair_   = std::move(joined);
    out.second_ = second;
    return Status::Ok;
}

}  // namespace retropp::net

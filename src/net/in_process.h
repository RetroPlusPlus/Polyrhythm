#pragma once

// A byte stream between two parties inside one process, satisfying the same contract a socket stream
// does with no OS underneath it.
//
// It is a transport a consumer selects, not a stand-in the tests keep to themselves: it compiles in
// Release, it lives beside the socket seam rather than under tests/, and it is what lets one declared
// map run in a device-free test and in a shipping build without changing a line of the map.
//
// Two ends meet by naming the same pair. The first end to name one creates it and waits; the second
// joins it; a third is refused, because a pair has two ends. An end whose name nobody else opens is a
// working stream with nobody on the other side — its reads answer WouldBlock, which is the same answer
// a quiet socket gives.

#include "src/net/socket.h"

#include <memory>
#include <string_view>

namespace retropp::net {

namespace detail {
struct InProcessPair;
}

class InProcessStream final : public Stream {
public:
    InProcessStream() = default;
    ~InProcessStream() override;

    InProcessStream(InProcessStream&& other) noexcept;
    InProcessStream& operator=(InProcessStream&& other) noexcept;

    [[nodiscard]] Transfer send(std::span<const std::byte> bytes) override;
    [[nodiscard]] Transfer receive(std::span<std::byte> into) override;
    [[nodiscard]] Status   waitReadable(std::chrono::milliseconds timeout) override;
    [[nodiscard]] bool     closed() const noexcept override;

    // True once this end is joined to a pair. An unopened stream is inert rather than invalid: it
    // answers every call the way a stream with nothing on the far side does.
    [[nodiscard]] bool open() const noexcept { return pair_ != nullptr; }

private:
    friend Status openInProcessStream(std::string_view pair, InProcessStream& out);

    std::shared_ptr<detail::InProcessPair> pair_;
    bool                                   second_ = false;  // which of the pair's two ends this is
    bool                                   closed_ = false;
};

// Take one end of a named pair. Ok for the first two ends of a name; Refused for a third, since both
// ends are already taken.
[[nodiscard]] Status openInProcessStream(std::string_view pair, InProcessStream& out);

}  // namespace retropp::net

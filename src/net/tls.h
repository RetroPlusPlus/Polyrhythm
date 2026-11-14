#pragma once

// The part of the TLS seam the platform keeps to itself.

#include "retropp/net.h"
#include "src/net/socket.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace retropp::net {

// When a certificate stops being valid, as a Unix timestamp. Every platform stack answers this, and the
// case that asserts a committed fixture still has life left is what reads it: an expiry nobody planned
// for surfaces years later as a handshake failure on every machine at once.
[[nodiscard]] Status certificateExpiry(std::span<const std::byte> der, std::int64_t& unixSeconds);

}  // namespace retropp::net

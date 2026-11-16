// The address helpers that need no OS: text to an endpoint, an interface's broadcast address, and the
// wait between attempts. Each answers from its arguments alone.

#include "retropp/net.h"

#include <array>

namespace retropp::net {
namespace {

// A run of one to `longest` decimal digits no greater than `most`, or nothing.
std::optional<std::uint32_t> decimal(std::string_view digits, std::uint32_t most, std::size_t longest) {
    if (digits.empty() || digits.size() > longest) return std::nullopt;
    std::uint32_t value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    if (value > most) return std::nullopt;
    return value;
}

// The four octets of a dotted v4 literal, or nothing.
std::optional<std::array<std::uint8_t, 4>> octets(std::string_view text) {
    std::array<std::uint8_t, 4> parts{};
    for (std::size_t i = 0; i < 4; ++i) {
        const std::size_t dot  = text.find('.');
        const bool        last = i == 3;
        if (last != (dot == std::string_view::npos)) return std::nullopt;

        const auto part = decimal(last ? text : text.substr(0, dot), 255, 3);
        if (!part) return std::nullopt;
        parts[i] = static_cast<std::uint8_t>(*part);
        if (!last) text.remove_prefix(dot + 1);
    }
    return parts;
}

// Thirty-two well-mixed bits from a seed and an attempt number.
std::uint32_t mixed(std::uint32_t seed, std::uint32_t attempt) {
    std::uint32_t x = seed ^ (attempt * 0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

}  // namespace

std::optional<Endpoint> parseEndpoint(std::string_view text) {
    std::string_view host;
    std::string_view port;

    if (!text.empty() && text.front() == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        if (close + 1 >= text.size() || text[close + 1] != ':') return std::nullopt;
        host = text.substr(1, close - 1);
        port = text.substr(close + 2);
        if (host.empty()) return std::nullopt;
    } else {
        const std::size_t colon = text.rfind(':');
        if (colon == std::string_view::npos) return std::nullopt;
        host = text.substr(0, colon);
        port = text.substr(colon + 1);
        // A v6 literal carries colons of its own, and goes in brackets.
        if (host.find(':') != std::string_view::npos) return std::nullopt;
    }

    const auto number = decimal(port, 65535, 5);
    if (!number) return std::nullopt;
    return Endpoint{.host = std::string(host), .port = static_cast<std::uint16_t>(*number)};
}

std::optional<std::string> broadcastAddress(const Interface& of) {
    if (of.prefixLength < 0 || of.prefixLength > 32) return std::nullopt;
    const auto parts = octets(of.address);
    if (!parts) return std::nullopt;

    const std::uint32_t address = (std::uint32_t{(*parts)[0]} << 24) | (std::uint32_t{(*parts)[1]} << 16) |
                                  (std::uint32_t{(*parts)[2]} << 8) | std::uint32_t{(*parts)[3]};
    const std::uint32_t past = of.prefixLength == 32 ? 0u : (~std::uint32_t{0} >> of.prefixLength);
    const std::uint32_t all  = address | past;

    return std::to_string(all >> 24) + "." + std::to_string((all >> 16) & 0xFFu) + "." +
           std::to_string((all >> 8) & 0xFFu) + "." + std::to_string(all & 0xFFu);
}

std::chrono::milliseconds backoffDelay(int attempt, const Backoff& policy) {
    // A quarter of the range stays clear, so nothing below can overflow.
    constexpr std::int64_t kCeiling = INT64_MAX / 4;

    std::int64_t most = policy.max.count() > 0 ? policy.max.count() : 0;
    if (most > kCeiling) most = kCeiling;
    std::int64_t wait = policy.base.count() > 0 ? policy.base.count() : 0;
    if (wait > most) wait = most;

    const int doublings = attempt > 0 ? attempt : 0;
    for (int i = 0; i < doublings && wait > 0 && wait < most; ++i) {
        wait = wait > most / 2 ? most : wait * 2;
    }

    const float        fraction = policy.jitter < 0.0f ? 0.0f : (policy.jitter > 1.0f ? 1.0f : policy.jitter);
    const std::int64_t permille = static_cast<std::int64_t>(fraction * 1000.0f + 0.5f);
    const std::int64_t span     = (wait / 1000) * permille + (wait % 1000) * permille / 1000;

    // The wait lands somewhere in [wait - span, wait + span], at the point the seed and the attempt name.
    const std::int64_t point = mixed(policy.seed, static_cast<std::uint32_t>(doublings)) & 0xFFFF;
    const std::int64_t above = (span / 65535) * 2 * point + (span % 65535) * 2 * point / 65535;

    std::int64_t delay = wait - span + above;
    if (delay > most) delay = most;
    return std::chrono::milliseconds{delay};
}

}  // namespace retropp::net

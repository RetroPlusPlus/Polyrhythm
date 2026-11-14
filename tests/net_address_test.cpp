// The address helpers in <retropp/net.h>: text read into an endpoint, the addresses this machine holds,
// an interface's broadcast address, and the wait between attempts.
//
// Every helper but localAddresses answers from its arguments alone, so those cases name exact values.
// localAddresses asks the OS, and its case asserts only what every machine has: the v4 loopback.

#include "retropp/net.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace net = retropp::net;

void expectEndpoint(std::string_view text, const std::string& host, std::uint16_t port) {
    const std::optional<net::Endpoint> read = net::parseEndpoint(text);
    ASSERT_TRUE(read.has_value()) << text;
    EXPECT_EQ(read->host, host) << text;
    EXPECT_EQ(read->port, port) << text;
}

std::optional<std::string> broadcastOf(const std::string& address, int prefixLength) {
    return net::broadcastAddress(
        net::Interface{.name = "test", .address = address, .prefixLength = prefixLength});
}

// The waits with no jitter, attempt 0 to 8, under the default base and most.
constexpr std::array<std::int64_t, 9> kRaw = {250, 500, 1000, 2000, 4000, 8000, 16000, 30000, 30000};

TEST(NetAddress, ParsesAV4HostAndPort) {
    const std::optional<net::Endpoint> peer = net::parseEndpoint("192.168.1.42:7654");
    ASSERT_TRUE(peer.has_value());
    EXPECT_EQ(peer->host, "192.168.1.42");
    EXPECT_EQ(peer->port, 7654);
}

TEST(NetAddress, ParsesABracketedV6HostAndPort) {
    expectEndpoint("[::1]:7654", "::1", 7654);
    expectEndpoint("[fe80::1%en0]:80", "fe80::1%en0", 80);
}

TEST(NetAddress, ParsesANameAndAnEmptyHost) {
    expectEndpoint("example.com:443", "example.com", 443);
    expectEndpoint(":7654", "", 7654);
    expectEndpoint("host:0", "host", 0);
    expectEndpoint("host:65535", "host", 65535);
}

TEST(NetAddress, AnswersNothingWithoutAUsablePort) {
    for (const std::string_view text :
         {"", "192.168.1.42", "host:", "host:65536", "host:123456", "host:8o", "host:-1", "host: 80"}) {
        EXPECT_FALSE(net::parseEndpoint(text).has_value()) << '"' << text << '"';
    }
}

TEST(NetAddress, AnswersNothingForAV6LiteralOutsideBrackets) {
    for (const std::string_view text : {"::1:80", "[::1]", "[::1", "[]:80", "[::1]80"}) {
        EXPECT_FALSE(net::parseEndpoint(text).has_value()) << '"' << text << '"';
    }
}

TEST(NetAddress, TheBroadcastAddressSetsEveryBitPastThePrefix) {
    const net::Interface             iface{.name = "en0", .address = "192.168.1.42", .prefixLength = 24};
    const std::optional<std::string> everyone = net::broadcastAddress(iface);
    EXPECT_EQ(everyone, "192.168.1.255");

    EXPECT_EQ(broadcastOf("192.168.1.42", 24), "192.168.1.255");
    EXPECT_EQ(broadcastOf("10.1.2.3", 8), "10.255.255.255");
    EXPECT_EQ(broadcastOf("172.16.5.9", 23), "172.16.5.255");
    EXPECT_EQ(broadcastOf("192.168.1.42", 31), "192.168.1.43");
    EXPECT_EQ(broadcastOf("192.168.1.42", 32), "192.168.1.42");
    EXPECT_EQ(broadcastOf("1.2.3.4", 0), "255.255.255.255");
}

TEST(NetAddress, ABroadcastAddressIsAV4Answer) {
    EXPECT_FALSE(broadcastOf("fe80::1", 64).has_value());
    EXPECT_FALSE(broadcastOf("192.168.1", 24).has_value());
    EXPECT_FALSE(broadcastOf("192.168.1.256", 24).has_value());
    EXPECT_FALSE(broadcastOf("192.168.1.42", 33).has_value());
    EXPECT_FALSE(broadcastOf("192.168.1.42", -1).has_value());
}

TEST(NetAddress, TheLocalAddressesIncludeTheLoopback) {
    const std::vector<net::Interface> addresses = net::localAddresses();

    const bool hasLoopback = std::any_of(addresses.begin(), addresses.end(), [](const net::Interface& entry) {
        return entry.address == "127.0.0.1" && entry.loopback && entry.prefixLength == 8;
    });
    EXPECT_TRUE(hasLoopback);

    for (const net::Interface& entry : addresses) {
        EXPECT_FALSE(entry.name.empty()) << entry.address;
        EXPECT_FALSE(entry.address.empty()) << entry.name;
    }
}

TEST(NetAddress, BackoffDoublesFromTheBaseAndHoldsAtTheMost) {
    const net::Backoff steady{.jitter = 0.0f};
    // Attempt -1 reads as attempt 0.
    const std::array<std::int64_t, 11> expected = {250,  250,  500,   1000,  2000, 4000,
                                                   8000, 16000, 30000, 30000, 30000};
    for (int attempt = -1; attempt <= 9; ++attempt) {
        EXPECT_EQ(net::backoffDelay(attempt, steady).count(), expected[static_cast<std::size_t>(attempt + 1)])
            << "attempt " << attempt;
    }
}

TEST(NetAddress, BackoffAnswersTheSameWaitForTheSameArguments) {
    for (const std::uint32_t seed : {0u, 1u, 12648430u}) {
        const net::Backoff policy{.seed = seed};
        for (int attempt = 0; attempt <= 8; ++attempt) {
            EXPECT_EQ(net::backoffDelay(attempt, policy), net::backoffDelay(attempt, policy))
                << "seed " << seed << ", attempt " << attempt;
        }
    }
    EXPECT_EQ(net::backoffDelay(1000000, {.base = 0ms}).count(), 0);
    EXPECT_EQ(net::backoffDelay(0, {.max = 100ms, .jitter = 0.0f}).count(), 100);
}

TEST(NetAddress, BackoffJitterStaysInsideItsFraction) {
    for (std::uint32_t seed = 0; seed < 100; ++seed) {
        const net::Backoff policy{.seed = seed};
        for (int attempt = 0; attempt <= 8; ++attempt) {
            const std::int64_t              raw  = kRaw[static_cast<std::size_t>(attempt)];
            const std::chrono::milliseconds wait = net::backoffDelay(attempt, policy);
            EXPECT_LE(wait.count(), raw + raw / 5) << "seed " << seed << ", attempt " << attempt;
            EXPECT_GE(wait.count(), raw - raw / 5) << "seed " << seed << ", attempt " << attempt;
            EXPECT_LE(wait.count(), 30000) << "seed " << seed << ", attempt " << attempt;
            EXPECT_GE(wait.count(), 0) << "seed " << seed << ", attempt " << attempt;
        }
    }
}

TEST(NetAddress, BackoffJitterIsDrawnFromTheSeed) {
    const std::array<std::int64_t, 9> seedZero = {200, 579, 1129, 1692, 4208, 8699, 19082, 30000, 30000};
    const std::array<std::int64_t, 9> seedOne  = {256, 507, 877, 1950, 4773, 8975, 14608, 29998, 30000};
    for (int attempt = 0; attempt <= 8; ++attempt) {
        const auto i = static_cast<std::size_t>(attempt);
        EXPECT_EQ(net::backoffDelay(attempt, {.seed = 0}).count(), seedZero[i])
            << "seed 0, attempt " << attempt;
        EXPECT_EQ(net::backoffDelay(attempt, {.seed = 1}).count(), seedOne[i])
            << "seed 1, attempt " << attempt;
    }
}

}  // namespace

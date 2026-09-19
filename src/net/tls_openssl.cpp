// TLS on Linux: the system's OpenSSL.
//
// The library is reached through a pair of memory buffers rather than a socket: one holds ciphertext
// already taken off the carrier, the other holds ciphertext not yet put on it. Everything between those
// buffers and the carrier is this file's work, and everything inside them is the library's — which is
// what lets the same code sit over a socket and over the in-process pair.
//
// A byte is reported sent only once its ciphertext has left this process. The library encrypts a whole
// record at a time and a non-blocking carrier can take part of one, so a record that is encrypted but
// still in hand is owed rather than sent: the count that goes back to the caller is one it can advance
// by, and retrying with the remainder never encrypts the same byte twice.
//
// A client always validates what its peer presents: it pins the certificates it will accept, compared
// byte for byte, or defers to the OS's own trust roots. The verify callback is the only place that
// answer is given.

#include "src/net/tls.h"

#if !defined(__APPLE__) && !defined(_WIN32)

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pkcs12.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <utility>

namespace retropp::net {
namespace {

// One hop between the carrier and the library's buffers. A TLS record tops out just above 16 KiB, so a
// whole record usually crosses in one pass.
constexpr std::size_t kHop = 16384;

// The most plaintext handed to the library at once, which is one record's worth. Writing a record at a
// time is what makes "encrypted but not yet gone" a bounded amount to keep track of.
constexpr std::size_t kRecord = 16384;

// Where a stream's own state hangs off its SSL, so the verify callback can reach the pinned list.
int pinnedSlot() {
    static const int slot = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return slot;
}

}  // namespace

struct TlsStream::State {
    std::unique_ptr<Stream> owned;
    SSL_CTX*                context  = nullptr;
    SSL*                    ssl      = nullptr;
    BIO*                    inbound  = nullptr;  // ciphertext taken off the carrier, not yet read
    BIO*                    outbound = nullptr;  // ciphertext the library produced, not yet sent

    std::vector<std::byte>   unsent;       // ciphertext the carrier would not take yet
    std::size_t              owed = 0;     // plaintext already encrypted, not yet reported sent
    std::vector<Certificate> pinned;
    bool                     settled = false;
    bool                     closed  = false;
    bool                     refused = false;  // the peer presented a certificate this side rejects

    ~State();
};

namespace {

// Put whatever the library has produced onto the carrier, keeping anything the carrier would not take.
Status flush(TlsStream::State& state) {
    for (;;) {
        std::array<char, kHop> hop{};
        const int              got = BIO_read(state.outbound, hop.data(), static_cast<int>(hop.size()));
        if (got <= 0) break;
        const auto* first = reinterpret_cast<const std::byte*>(hop.data());
        state.unsent.insert(state.unsent.end(), first, first + got);
    }

    while (!state.unsent.empty()) {
        const Transfer moved = state.owned->send(std::span<const std::byte>{state.unsent});
        if (moved.bytes > 0) {
            state.unsent.erase(state.unsent.begin(),
                               state.unsent.begin() + static_cast<std::ptrdiff_t>(moved.bytes));
        }
        if (moved.status == Status::Closed) {
            state.closed = true;
            return Status::Closed;
        }
        if (moved.status != Status::Ok) return moved.status;
        // Ok having moved nothing would otherwise spin here; treat it as the carrier being full.
        if (moved.bytes == 0) return Status::WouldBlock;
    }
    return Status::Ok;
}

// Take what the carrier has and hand it to the library. A record that arrived in pieces sits in the
// buffer until the rest of it does, which is the stream contract holding through the layer.
Transfer fill(TlsStream::State& state) {
    std::array<std::byte, kHop> hop{};
    const Transfer              moved = state.owned->receive(hop);
    if (moved.bytes > 0) {
        BIO_write(state.inbound, hop.data(), static_cast<int>(moved.bytes));
    }
    if (moved.status == Status::Closed) state.closed = true;
    return moved;
}

// What a failed library call means to a caller of this seam.
Status failureOf(TlsStream::State& state, int result) {
    const int reason = SSL_get_error(state.ssl, result);
    if (reason == SSL_ERROR_WANT_READ || reason == SSL_ERROR_WANT_WRITE) return Status::WouldBlock;
    if (reason == SSL_ERROR_ZERO_RETURN) return Status::Closed;
    if (state.closed) return Status::Closed;
    return Status::Other;
}

// The trust decision. A pinned certificate is compared byte for byte, which is what lets a certificate
// no public CA signed be accepted without a path that accepts anything; with nothing pinned, the chain
// the library built against the OS's roots is the answer.
int verifyPeer(int preverified, X509_STORE_CTX* store) {
    auto* ssl = static_cast<SSL*>(
        X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx()));
    auto* state =
        (ssl != nullptr) ? static_cast<TlsStream::State*>(SSL_get_ex_data(ssl, pinnedSlot())) : nullptr;
    if (state == nullptr || state->pinned.empty()) return preverified;

    // Only what the peer itself presented is pinned; an issuer above it is not the thing being named.
    if (X509_STORE_CTX_get_error_depth(store) != 0) return 1;

    X509*          presented = X509_STORE_CTX_get_current_cert(store);
    unsigned char* der       = nullptr;
    const int      length    = (presented != nullptr) ? i2d_X509(presented, &der) : -1;

    bool acceptable = false;
    if (length > 0 && der != nullptr) {
        const auto* first = reinterpret_cast<const std::byte*>(der);
        const auto  size  = static_cast<std::size_t>(length);
        acceptable = std::any_of(state->pinned.begin(), state->pinned.end(),
                                 [first, size](const Certificate& one) {
                                     return one.size() == size && std::equal(one.begin(), one.end(),
                                                                             first);
                                 });
    }
    if (der != nullptr) OPENSSL_free(der);

    if (!acceptable) state->refused = true;
    return acceptable ? 1 : 0;
}

// The context and the carrier. The side's own configuration goes on between this and attach.
Status openTls(std::unique_ptr<Stream> over, const SSL_METHOD* method, TlsStream::State& state) {
    if (over == nullptr) return Status::Closed;

    state.context = SSL_CTX_new(method);
    if (state.context == nullptr) return Status::Other;
    SSL_CTX_set_min_proto_version(state.context, TLS1_2_VERSION);

    state.owned = std::move(over);
    return Status::Ok;
}

// The connection itself, with a memory buffer either side of it.
Status attach(TlsStream::State& state) {
    state.ssl = SSL_new(state.context);
    if (state.ssl == nullptr) return Status::Other;

    state.inbound  = BIO_new(BIO_s_mem());
    state.outbound = BIO_new(BIO_s_mem());
    if (state.inbound == nullptr || state.outbound == nullptr) {
        BIO_free(state.inbound);
        BIO_free(state.outbound);
        state.inbound  = nullptr;
        state.outbound = nullptr;
        return Status::Other;
    }

    // An empty buffer means the peer owes bytes, not that it is gone.
    BIO_set_mem_eof_return(state.inbound, -1);
    BIO_set_mem_eof_return(state.outbound, -1);

    // Both buffers belong to the connection from here, and are freed with it.
    SSL_set_bio(state.ssl, state.inbound, state.outbound);
    return Status::Ok;
}

// Reads a PKCS#12 container and hands its certificate and key to the context. The key stays in this
// process: the library has no store of its own to put it in, which is the one place this platform's
// import asks less of the caller than the others do.
bool presentIdentity(TlsStream::State& state, const TlsServerConfig& presenting) {
    BIO* blob = BIO_new_mem_buf(presenting.identity.data(),
                                static_cast<int>(presenting.identity.size()));
    if (blob == nullptr) return false;

    PKCS12* container = d2i_PKCS12_bio(blob, nullptr);
    BIO_free(blob);
    if (container == nullptr) return false;

    EVP_PKEY*       key  = nullptr;
    X509*           leaf = nullptr;
    STACK_OF(X509)* rest = nullptr;
    const int parsed = PKCS12_parse(container, presenting.password.c_str(), &key, &leaf, &rest);
    PKCS12_free(container);

    bool accepted = false;
    if (parsed == 1 && key != nullptr && leaf != nullptr) {
        accepted = SSL_CTX_use_certificate(state.context, leaf) == 1 &&
                   SSL_CTX_use_PrivateKey(state.context, key) == 1;
    }

    if (leaf != nullptr) X509_free(leaf);
    if (key != nullptr) EVP_PKEY_free(key);
    if (rest != nullptr) sk_X509_pop_free(rest, X509_free);
    return accepted;
}

}  // namespace

// Defined here rather than in the class: closing tells the peer this side is done, and that needs the
// carrier and the flush above it.
TlsStream::State::~State() {
    if (ssl != nullptr) {
        if (settled) {
            SSL_shutdown(ssl);
            // A peer that does not get this learns from the carrier going away instead.
            const Status put = flush(*this);
            static_cast<void>(put);
        }
        SSL_free(ssl);
    }
    if (context != nullptr) SSL_CTX_free(context);
}

TlsStream::TlsStream()  = default;
TlsStream::~TlsStream() = default;

// Written out rather than defaulted: the base declares a destructor, which suppresses its own move
// operations, so a defaulted move here would resolve to the base's deleted copy. The base carries no
// state, so default-constructing it and moving the handshake state across is the whole job.
TlsStream::TlsStream(TlsStream&& other) noexcept : state_(std::move(other.state_)) {}

TlsStream& TlsStream::operator=(TlsStream&& other) noexcept {
    if (this != &other) state_ = std::move(other.state_);
    return *this;
}

Status openTlsClient(std::unique_ptr<Stream> over, const TlsClientConfig& accepting, TlsStream& out) {
    auto state = std::make_unique<TlsStream::State>();
    if (const Status opened = openTls(std::move(over), TLS_client_method(), *state);
        opened != Status::Ok) {
        return opened;
    }

    state->pinned = accepting.pinned;
    if (state->pinned.empty() && SSL_CTX_set_default_verify_paths(state->context) != 1) {
        // With nothing pinned the OS's roots are the whole answer, so not finding them is a failure to
        // bring the client up rather than something to discover at the handshake.
        return Status::Other;
    }

    if (const Status attached = attach(*state); attached != Status::Ok) return attached;
    SSL_set_connect_state(state->ssl);
    SSL_set_ex_data(state->ssl, pinnedSlot(), state.get());
    SSL_set_verify(state->ssl, SSL_VERIFY_PEER, verifyPeer);

    if (!accepting.hostname.empty()) {
        SSL_set_tlsext_host_name(state->ssl, accepting.hostname.c_str());
        // The name is checked against the certificate only where the certificate is not itself what is
        // pinned: an exact match on the bytes already says which peer this is.
        if (state->pinned.empty()) SSL_set1_host(state->ssl, accepting.hostname.c_str());
    }

    out.state_ = std::move(state);
    return Status::Ok;
}

Status openTlsServer(std::unique_ptr<Stream> over, const TlsServerConfig& presenting, TlsStream& out) {
    auto state = std::make_unique<TlsStream::State>();
    if (const Status opened = openTls(std::move(over), TLS_server_method(), *state);
        opened != Status::Ok) {
        return opened;
    }

    if (!presentIdentity(*state, presenting)) return Status::Other;
    if (const Status attached = attach(*state); attached != Status::Ok) return attached;
    SSL_set_accept_state(state->ssl);

    out.state_ = std::move(state);
    return Status::Ok;
}

Status TlsStream::handshake(std::chrono::milliseconds timeout) {
    if (state_ == nullptr) return Status::Closed;
    if (state_->settled) return Status::Ok;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        ERR_clear_error();
        const int step = SSL_do_handshake(state_->ssl);

        // Whatever that produced goes out before anything else: the peer cannot answer bytes it has not
        // been given.
        const Status put = flush(*state_);
        if (put == Status::Closed) return Status::Closed;

        if (step == 1) {
            // The last flight is still in hand, so the peer cannot settle yet either. Calling again
            // resumes from here.
            if (put != Status::Ok) return Status::WouldBlock;
            state_->settled = true;
            return Status::Ok;
        }

        const int reason = SSL_get_error(state_->ssl, step);
        if (reason == SSL_ERROR_WANT_WRITE) {
            if (put != Status::Ok) return Status::WouldBlock;
            continue;
        }
        if (reason != SSL_ERROR_WANT_READ) {
            if (state_->refused || SSL_get_verify_result(state_->ssl) != X509_V_OK) {
                // A verdict rather than a stall: it does not change on a later call.
                return Status::Untrusted;
            }
            if (reason == SSL_ERROR_ZERO_RETURN || state_->closed) return Status::Closed;
            return Status::Other;
        }

        const Transfer got = fill(*state_);
        if (got.status == Status::Closed) return Status::Closed;
        if (got.status != Status::Ok && got.status != Status::WouldBlock) return got.status;
        if (got.bytes > 0) continue;

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return Status::WouldBlock;
        const auto   remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const Status ready     = state_->owned->waitReadable(remaining);
        if (ready == Status::TimedOut) return Status::WouldBlock;
        if (ready != Status::Ok) return ready;
    }
}

bool TlsStream::settled() const noexcept { return state_ != nullptr && state_->settled; }

Transfer TlsStream::send(std::span<const std::byte> bytes) {
    if (state_ == nullptr) return Transfer{.status = Status::Closed, .bytes = 0};
    if (!state_->settled) return Transfer{.status = Status::WouldBlock, .bytes = 0};

    // Ciphertext from an earlier call goes out before any more plaintext is taken.
    const Status put = flush(*state_);
    if (put == Status::Closed) return Transfer{.status = Status::Closed, .bytes = 0};

    // Bytes encrypted on an earlier call are reported sent once, here, at the point their ciphertext has
    // actually left. The caller retried with them at the front of its remainder, which is why nothing
    // new is taken in the same breath.
    if (state_->owed > 0) {
        if (put != Status::Ok) return Transfer{.status = Status::WouldBlock, .bytes = 0};
        const std::size_t honored = state_->owed;
        state_->owed               = 0;
        return Transfer{.status = Status::Ok, .bytes = honored};
    }
    if (put != Status::Ok) return Transfer{.status = put, .bytes = 0};

    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const std::size_t chunk = std::min(kRecord, bytes.size() - sent);

        ERR_clear_error();
        std::size_t taken = 0;
        if (SSL_write_ex(state_->ssl, bytes.data() + sent, chunk, &taken) != 1) {
            const Status why = failureOf(*state_, 0);
            if (sent > 0) return Transfer{.status = Status::Ok, .bytes = sent};
            return Transfer{.status = why, .bytes = 0};
        }

        const Status pushed = flush(*state_);
        if (pushed == Status::Closed) return Transfer{.status = Status::Closed, .bytes = sent};
        if (pushed != Status::Ok) {
            // This chunk is encrypted but not yet gone, so it is owed rather than sent.
            state_->owed = taken;
            return Transfer{.status = sent > 0 ? Status::Ok : Status::WouldBlock, .bytes = sent};
        }
        sent += taken;
    }
    return Transfer{.status = Status::Ok, .bytes = sent};
}

Transfer TlsStream::receive(std::span<std::byte> into) {
    if (state_ == nullptr) return Transfer{.status = Status::Closed, .bytes = 0};
    if (!state_->settled) return Transfer{.status = Status::WouldBlock, .bytes = 0};

    for (;;) {
        ERR_clear_error();
        std::size_t moved = 0;
        if (SSL_read_ex(state_->ssl, into.data(), into.size(), &moved) == 1) {
            return Transfer{.status = Status::Ok, .bytes = moved};
        }

        const int reason = SSL_get_error(state_->ssl, 0);
        if (reason != SSL_ERROR_WANT_READ) {
            if (reason == SSL_ERROR_ZERO_RETURN || state_->closed) {
                return Transfer{.status = Status::Closed, .bytes = 0};
            }
            return Transfer{.status = Status::Other, .bytes = 0};
        }

        // The library wants bytes it has not been given. Take whatever the carrier has; if it has none,
        // say so rather than wait — a caller that wants to wait says so with waitReadable.
        const Transfer got = fill(*state_);
        if (got.status == Status::Closed) return Transfer{.status = Status::Closed, .bytes = 0};
        if (got.bytes == 0) {
            if (got.status == Status::Ok || got.status == Status::WouldBlock) {
                return Transfer{.status = Status::WouldBlock, .bytes = 0};
            }
            return Transfer{.status = got.status, .bytes = 0};
        }
    }
}

Status TlsStream::waitReadable(std::chrono::milliseconds timeout) {
    if (state_ == nullptr) return Status::Closed;
    return state_->owned->waitReadable(timeout);
}

bool TlsStream::closed() const noexcept {
    return state_ == nullptr || state_->closed || state_->owned->closed();
}

Status certificateExpiry(std::span<const std::byte> der, std::int64_t& unixSeconds) {
    const auto* cursor      = reinterpret_cast<const unsigned char*>(der.data());
    X509*       certificate = d2i_X509(nullptr, &cursor, static_cast<long>(der.size()));
    if (certificate == nullptr) return Status::Other;

    Status result = Status::Other;
    if (const ASN1_TIME* notAfter = X509_get0_notAfter(certificate); notAfter != nullptr) {
        std::tm when{};
        if (ASN1_TIME_to_tm(notAfter, &when) == 1) {
            // A certificate's validity is stated in UTC, so it converts without the local zone.
            unixSeconds = static_cast<std::int64_t>(timegm(&when));
            result      = Status::Ok;
        }
    }

    X509_free(certificate);
    return result;
}

}  // namespace retropp::net

#endif  // !__APPLE__ && !_WIN32

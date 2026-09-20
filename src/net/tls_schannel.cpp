// TLS on Windows: SChannel, through the SSPI calls.
//
// The provider is driven with buffers rather than a socket: one holds ciphertext taken off the carrier,
// one holds ciphertext not yet put on it, and one holds plaintext already decrypted but not yet asked
// for. Everything between those buffers and the carrier is this file's work, and everything inside them
// is the provider's — which is what lets the same code sit over a socket and over the in-process pair.
//
// A byte is reported sent only once its ciphertext has left this process. The provider encrypts a whole
// record at a time and a non-blocking carrier can take part of one, so a record that is encrypted but
// still in hand is owed rather than sent: the count that goes back to the caller is one it can advance
// by, and retrying with the remainder never encrypts the same byte twice.
//
// A client always validates what its peer presents. The provider is told to leave that decision here, so
// a pinned certificate is compared byte for byte and an unpinned one goes to the OS's own chain check —
// and there is no third answer that accepts whatever arrives.
//
// A server's private key is removed again when the connection that presented it goes away. The platform
// runs the server side of a handshake outside this process, and a key that was never written down cannot
// reach it — so the import writes one, and this file deletes the container it went into, which is the
// removal the platform prescribes for a key nobody wants kept.

#include "src/net/tls.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Asks <security.h> for the SSPI functions rather than only the types.
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
// What <schannel.h> requires before it declares SCH_CREDENTIALS, the current credential structure.
#ifndef SCHANNEL_USE_BLACKLISTS
#define SCHANNEL_USE_BLACKLISTS
#endif

#include <windows.h>
// schannel.h names types it does not declare: its credential structures carry certificate types from
// wincrypt.h and a counted string type from ntsecapi.h, so both must precede it.
#include <wincrypt.h>

#include <ntsecapi.h>

#include <ncrypt.h>
#include <schannel.h>
#include <security.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

namespace retropp::net {
namespace {

// One hop between the carrier and this file's buffers. A TLS record tops out just above 16 KiB, so a
// whole record usually crosses in one pass.
constexpr std::size_t kHop = 16384;

// Where a server's private key was written, so it can be removed once the connection is done with it.
struct KeyLocation {
    std::wstring  container;
    std::wstring  provider;
    unsigned long type  = 0;  // zero names a key storage provider; anything else a legacy provider
    unsigned long flags = 0;
};

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int wanted = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                             nullptr, 0);
    if (wanted <= 0) return {};

    std::wstring wide(static_cast<std::size_t>(wanted), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), wanted);
    return wide;
}

}  // namespace

struct TlsStream::State {
    std::unique_ptr<Stream> owned;

    CredHandle credentials{};
    CtxtHandle context{};
    bool       hasCredentials = false;
    bool       hasContext     = false;

    // The server's own certificate, the store the container was read into, and where its key landed. A
    // client has none of the three.
    HCERTSTORE     identityStore = nullptr;
    PCCERT_CONTEXT identity      = nullptr;
    KeyLocation    key;

    std::vector<std::byte> inbound;   // ciphertext taken off the carrier, not yet consumed
    std::vector<std::byte> unsent;    // ciphertext the provider produced, not yet put on the carrier
    std::vector<std::byte> plain;     // plaintext decrypted, not yet asked for
    std::size_t            owed = 0;  // plaintext already encrypted, not yet reported sent

    // How large a record's parts are, which the provider answers once the connection is up.
    SecPkgContext_StreamSizes sizes{};

    std::vector<Certificate> pinned;
    std::string              hostname;
    bool                     isClient = false;

    bool started   = false;  // the provider has been called at least once
    bool completed = false;  // the provider is done; the last flight may still be in hand
    bool vetted    = false;  // the peer's certificate has been judged, once
    bool settled   = false;
    bool closed    = false;
    bool refused   = false;  // the peer presented a certificate this side rejects

    ~State();
};

namespace {

// Put whatever the provider has produced onto the carrier, keeping anything the carrier would not take.
Status flush(TlsStream::State& state) {
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

// Take what the carrier has. A record that arrived in pieces sits in the buffer until the rest of it
// does, which is the stream contract holding through the layer.
Transfer fill(TlsStream::State& state) {
    std::array<std::byte, kHop> hop{};
    const Transfer              moved = state.owned->receive(hop);
    if (moved.bytes > 0) {
        state.inbound.insert(state.inbound.end(), hop.data(), hop.data() + moved.bytes);
    }
    if (moved.status == Status::Closed) state.closed = true;
    return moved;
}

// The provider's own answers that mean "the peer's certificate is not acceptable" rather than "come back
// later". The pinned comparison below reaches its verdict without them, but a peer that refuses this
// side's certificate reports one of these and it is the same answer either way.
bool isTrustFailure(SECURITY_STATUS status) {
    switch (status) {
        case SEC_E_UNTRUSTED_ROOT:
        case SEC_E_CERT_EXPIRED:
        case SEC_E_CERT_UNKNOWN:
        case SEC_E_WRONG_PRINCIPAL:
        case static_cast<SECURITY_STATUS>(CERT_E_UNTRUSTEDROOT):
        case static_cast<SECURITY_STATUS>(CERT_E_CN_NO_MATCH):
        case static_cast<SECURITY_STATUS>(CERT_E_EXPIRED):
        case static_cast<SECURITY_STATUS>(TRUST_E_CERT_SIGNATURE): return true;
        default:                                                   return false;
    }
}

// The trust decision, taken here rather than by the provider, which is what lets a certificate no public
// CA signed be accepted without a path that accepts anything.
bool peerIsAcceptable(TlsStream::State& state) {
    PCCERT_CONTEXT peer = nullptr;
    if (::QueryContextAttributesA(&state.context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &peer) != SEC_E_OK ||
        peer == nullptr) {
        return false;
    }

    bool acceptable = false;
    if (!state.pinned.empty()) {
        const auto* first = reinterpret_cast<const std::byte*>(peer->pbCertEncoded);
        const auto  size  = static_cast<std::size_t>(peer->cbCertEncoded);
        acceptable =
            std::any_of(state.pinned.begin(), state.pinned.end(), [first, size](const Certificate& one) {
                return one.size() == size && std::equal(one.begin(), one.end(), first);
            });
    } else {
        // Nothing pinned: the OS's own roots decide, which is what a client on the public internet wants.
        auto  serverAuth = const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
        CERT_CHAIN_PARA wanted{};
        wanted.cbSize                                    = sizeof(wanted);
        wanted.RequestedUsage.dwType                     = USAGE_MATCH_TYPE_AND;
        wanted.RequestedUsage.Usage.cUsageIdentifier     = 1;
        wanted.RequestedUsage.Usage.rgpszUsageIdentifier = &serverAuth;

        PCCERT_CHAIN_CONTEXT chain = nullptr;
        if (::CertGetCertificateChain(nullptr, peer, nullptr, peer->hCertStore, &wanted, 0, nullptr,
                                      &chain) != FALSE) {
            std::wstring name = widen(state.hostname);

            SSL_EXTRA_CERT_CHAIN_POLICY_PARA https{};
            https.cbSize         = sizeof(https);
            https.dwAuthType     = AUTHTYPE_SERVER;
            https.pwszServerName = name.empty() ? nullptr : name.data();

            CERT_CHAIN_POLICY_PARA policy{};
            policy.cbSize            = sizeof(policy);
            policy.pvExtraPolicyPara = &https;

            CERT_CHAIN_POLICY_STATUS verdict{};
            verdict.cbSize = sizeof(verdict);

            acceptable = ::CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy,
                                                            &verdict) != FALSE &&
                         verdict.dwError == 0;
            ::CertFreeCertificateChain(chain);
        }
    }

    ::CertFreeCertificateContext(peer);
    return acceptable;
}

// One pass through the provider's handshake. Any token it produces is queued for the carrier, and input
// it did not consume stays at the front of the buffer for the next pass.
SECURITY_STATUS carryHandshake(TlsStream::State& state, bool feedInput) {
    SecBuffer arriving[2]{};
    arriving[0].BufferType = SECBUFFER_TOKEN;
    arriving[0].pvBuffer   = state.inbound.empty() ? nullptr : state.inbound.data();
    arriving[0].cbBuffer   = static_cast<unsigned long>(state.inbound.size());
    arriving[1].BufferType = SECBUFFER_EMPTY;
    SecBufferDesc inputs{SECBUFFER_VERSION, 2, arriving};

    SecBuffer leaving[1]{};
    leaving[0].BufferType = SECBUFFER_TOKEN;
    SecBufferDesc outputs{SECBUFFER_VERSION, 1, leaving};

    unsigned long   granted = 0;
    SECURITY_STATUS rc      = SEC_E_INTERNAL_ERROR;

    if (state.isClient) {
        constexpr unsigned long kAsked = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                                         ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
                                         ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR |
                                         ISC_REQ_MANUAL_CRED_VALIDATION;
        // The name travels as the server name extension, so a host serving several names presents the
        // right certificate.
        auto* target = state.hostname.empty() ? nullptr : const_cast<SEC_CHAR*>(state.hostname.c_str());
        rc           = ::InitializeSecurityContextA(
            &state.credentials, state.hasContext ? &state.context : nullptr, target, kAsked, 0, 0,
            feedInput ? &inputs : nullptr, 0, state.hasContext ? nullptr : &state.context, &outputs,
            &granted, nullptr);
    } else {
        constexpr unsigned long kAsked = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT |
                                         ASC_REQ_CONFIDENTIALITY | ASC_REQ_ALLOCATE_MEMORY |
                                         ASC_REQ_STREAM | ASC_REQ_EXTENDED_ERROR;
        rc = ::AcceptSecurityContext(&state.credentials, state.hasContext ? &state.context : nullptr,
                                     &inputs, kAsked, 0, state.hasContext ? nullptr : &state.context,
                                     &outputs, &granted, nullptr);
    }

    if (!state.hasContext && (rc == SEC_E_OK || rc == SEC_I_CONTINUE_NEEDED)) state.hasContext = true;

    if (leaving[0].pvBuffer != nullptr) {
        if (leaving[0].cbBuffer > 0) {
            const auto* first = static_cast<const std::byte*>(leaving[0].pvBuffer);
            state.unsent.insert(state.unsent.end(), first, first + leaving[0].cbBuffer);
        }
        ::FreeContextBuffer(leaving[0].pvBuffer);
    }

    // A message that has not arrived whole is left alone; anything else is consumed except for what the
    // provider hands back, which is always at the end of what it was given.
    if (rc != SEC_E_INCOMPLETE_MESSAGE && !state.inbound.empty()) {
        std::size_t kept = 0;
        if (arriving[1].BufferType == SECBUFFER_EXTRA) {
            kept = std::min(static_cast<std::size_t>(arriving[1].cbBuffer), state.inbound.size());
        }
        state.inbound.erase(state.inbound.begin(),
                            state.inbound.end() - static_cast<std::ptrdiff_t>(kept));
    }
    return rc;
}

// A token the peer sends after the connection is up — a session ticket, a key update. It goes back
// through the provider until it is consumed; nothing about the settled connection changes.
Status carryPostHandshake(TlsStream::State& state) {
    for (;;) {
        const SECURITY_STATUS rc = carryHandshake(state, true);

        const Status put = flush(state);
        if (put == Status::Closed) return Status::Closed;

        if (rc == SEC_E_OK) return Status::Ok;
        if (rc == SEC_E_INCOMPLETE_MESSAGE) return Status::WouldBlock;
        if (rc != SEC_I_CONTINUE_NEEDED) return Status::Other;
        if (state.inbound.empty()) return Status::WouldBlock;
    }
}

// The credential handle, which is where the trust posture is declared.
Status acquire(TlsStream::State& state, unsigned long use) {
    SCH_CREDENTIALS credentials{};
    credentials.dwVersion = SCH_CREDENTIALS_VERSION;
    credentials.dwFlags   = SCH_USE_STRONG_CRYPTO;

    if (use == SECPKG_CRED_OUTBOUND) {
        // The trust decision belongs to this file. Without this the provider takes it first, and a
        // certificate no public CA signed would be refused before the pinned comparison ever ran.
        credentials.dwFlags |= SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    } else {
        credentials.cCreds = 1;
        credentials.paCred = &state.identity;
    }

    TimeStamp             expiry{};
    const SECURITY_STATUS rc =
        ::AcquireCredentialsHandleA(nullptr, const_cast<SEC_CHAR*>(UNISP_NAME_A), use, nullptr,
                                    &credentials, nullptr, nullptr, &state.credentials, &expiry);
    if (rc != SEC_E_OK) {
        std::fprintf(stderr, "retropp net: AcquireCredentialsHandle failed, status 0x%08lx\n",
                     static_cast<unsigned long>(rc));
        return Status::Other;
    }

    state.hasCredentials = true;
    return Status::Ok;
}

Status openTls(std::unique_ptr<Stream> over, bool asClient, TlsStream::State& state) {
    if (over == nullptr) return Status::Closed;
    state.owned    = std::move(over);
    state.isClient = asClient;
    return Status::Ok;
}

// Removes the key container a server identity was written into. Nothing to do for a client, which
// presents no identity and writes nothing.
void forgetKey(const KeyLocation& key) {
    if (key.container.empty()) return;

    if (key.type == 0) {
        NCRYPT_PROV_HANDLE provider = 0;
        const LPCWSTR      named    = key.provider.empty() ? nullptr : key.provider.c_str();
        if (::NCryptOpenStorageProvider(&provider, named, 0) == ERROR_SUCCESS) {
            NCRYPT_KEY_HANDLE handle = 0;
            if (::NCryptOpenKey(provider, &handle, key.container.c_str(), 0,
                                key.flags & NCRYPT_MACHINE_KEY_FLAG) == ERROR_SUCCESS) {
                // Deleting the key frees its handle, so there is nothing further to release here.
                ::NCryptDeleteKey(handle, 0);
            }
            ::NCryptFreeObject(provider);
        }
        return;
    }

    HCRYPTPROV    legacy = 0;
    const LPCWSTR named  = key.provider.empty() ? nullptr : key.provider.c_str();
    static_cast<void>(::CryptAcquireContextW(&legacy, key.container.c_str(), named, key.type,
                                             CRYPT_DELETEKEYSET | (key.flags & CRYPT_MACHINE_KEYSET)));
}

// Reads a PKCS#12 container, finds the certificate whose key came with it, and notes where that key was
// written so it can be removed again.
//
// The key has to be written somewhere: the platform runs the server side of a handshake outside this
// process, and a key kept only in this one cannot reach it. So the import puts it under the account that
// is running, which needs no special rights, and the connection deletes it on the way out — the removal
// the platform's own guidance names for a key that is not wanted beyond its use.
bool presentIdentity(TlsStream::State& state, const TlsServerConfig& presenting) {
    CRYPT_DATA_BLOB container{};
    container.cbData = static_cast<unsigned long>(presenting.identity.size());
    container.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(presenting.identity.data()));

    const std::wstring secret = widen(presenting.password);
    state.identityStore = ::PFXImportCertStore(&container, secret.c_str(), CRYPT_USER_KEYSET);
    if (state.identityStore == nullptr) {
        std::fprintf(stderr, "retropp net: PFXImportCertStore failed, error %lu\n", ::GetLastError());
        return false;
    }

    state.identity = ::CertFindCertificateInStore(state.identityStore,
                                                  X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                                  CERT_FIND_HAS_PRIVATE_KEY, nullptr, nullptr);
    if (state.identity == nullptr) {
        std::fprintf(stderr, "retropp net: no certificate with a key in the container, error %lu\n",
                     ::GetLastError());
        return false;
    }

    unsigned long size = 0;
    if (::CertGetCertificateContextProperty(state.identity, CERT_KEY_PROV_INFO_PROP_ID, nullptr,
                                            &size) != FALSE &&
        size > 0) {
        std::vector<std::byte> room(size);
        if (::CertGetCertificateContextProperty(state.identity, CERT_KEY_PROV_INFO_PROP_ID, room.data(),
                                                &size) != FALSE) {
            const auto* where = reinterpret_cast<const CRYPT_KEY_PROV_INFO*>(room.data());
            if (where->pwszContainerName != nullptr) state.key.container = where->pwszContainerName;
            if (where->pwszProvName != nullptr) state.key.provider = where->pwszProvName;
            state.key.type  = where->dwProvType;
            state.key.flags = where->dwFlags;
        }
    }
    return true;
}

}  // namespace

// Defined here rather than in the class: closing tells the peer this side is done, and that needs the
// carrier and the flush above it.
TlsStream::State::~State() {
    if (hasContext) {
        if (settled) {
            unsigned long instruction = SCHANNEL_SHUTDOWN;

            SecBuffer control{};
            control.BufferType = SECBUFFER_TOKEN;
            control.pvBuffer   = &instruction;
            control.cbBuffer   = sizeof(instruction);
            SecBufferDesc controls{SECBUFFER_VERSION, 1, &control};

            if (::ApplyControlToken(&context, &controls) == SEC_E_OK) {
                inbound.clear();
                static_cast<void>(carryHandshake(*this, false));
                // A peer that does not get this learns from the carrier going away instead.
                static_cast<void>(flush(*this));
            }
        }
        ::DeleteSecurityContext(&context);
    }
    if (hasCredentials) ::FreeCredentialsHandle(&credentials);
    // After the credential is released, so the container goes only once its last user has.
    forgetKey(key);
    if (identity != nullptr) ::CertFreeCertificateContext(identity);
    if (identityStore != nullptr) ::CertCloseStore(identityStore, 0);
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
    if (const Status opened = openTls(std::move(over), true, *state); opened != Status::Ok) {
        return opened;
    }

    state->pinned   = accepting.pinned;
    state->hostname = accepting.hostname;
    if (const Status held = acquire(*state, SECPKG_CRED_OUTBOUND); held != Status::Ok) return held;

    out.state_ = std::move(state);
    return Status::Ok;
}

Status openTlsServer(std::unique_ptr<Stream> over, const TlsServerConfig& presenting, TlsStream& out) {
    auto state = std::make_unique<TlsStream::State>();
    if (const Status opened = openTls(std::move(over), false, *state); opened != Status::Ok) {
        return opened;
    }

    if (!presentIdentity(*state, presenting)) return Status::Other;
    if (const Status held = acquire(*state, SECPKG_CRED_INBOUND); held != Status::Ok) return held;

    out.state_ = std::move(state);
    return Status::Ok;
}

Status TlsStream::handshake(std::chrono::milliseconds timeout) {
    if (state_ == nullptr) return Status::Closed;
    if (state_->settled) return Status::Ok;
    if (state_->refused) return Status::Untrusted;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (state_->completed) {
            // The provider is done. The peer's certificate is judged before anything else, so a refusal
            // is reached without this side's last flight ever going out.
            if (state_->isClient && !state_->vetted) {
                state_->vetted = true;
                if (!peerIsAcceptable(*state_)) {
                    state_->refused = true;
                    // A verdict rather than a stall: it does not change on a later call.
                    return Status::Untrusted;
                }
            }

            const Status put = flush(*state_);
            if (put == Status::Closed) return Status::Closed;
            // The last flight is still in hand, so the peer cannot settle yet either. Calling again
            // resumes from here.
            if (put != Status::Ok) return Status::WouldBlock;

            if (::QueryContextAttributesA(&state_->context, SECPKG_ATTR_STREAM_SIZES, &state_->sizes) !=
                SEC_E_OK) {
                return Status::Other;
            }
            state_->settled = true;
            return Status::Ok;
        }

        // A client opens the conversation; a server has nothing to say until the client has spoken.
        const bool canStep =
            state_->isClient ? (!state_->started || !state_->inbound.empty()) : !state_->inbound.empty();
        if (canStep) {
            const SECURITY_STATUS rc = carryHandshake(*state_, state_->started);
            state_->started          = true;

            if (rc == SEC_E_OK) {
                // What the provider produced stays in hand until the peer has been judged, which is what
                // the top of this loop does next.
                state_->completed = true;
                continue;
            }

            // Whatever that produced goes out before anything else: the peer cannot answer bytes it has
            // not been given.
            const Status put = flush(*state_);
            if (put == Status::Closed) return Status::Closed;

            if (rc == SEC_I_CONTINUE_NEEDED) {
                if (put != Status::Ok) return Status::WouldBlock;
                continue;
            }
            if (rc != SEC_E_INCOMPLETE_MESSAGE) {
                if (isTrustFailure(rc)) return Status::Untrusted;
                if (state_->closed) return Status::Closed;
                return Status::Other;
            }
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
    // actually left. The caller retried with them at the front of its remainder, which is why nothing new
    // is taken in the same breath.
    if (state_->owed > 0) {
        if (put != Status::Ok) return Transfer{.status = Status::WouldBlock, .bytes = 0};
        const std::size_t honored = state_->owed;
        state_->owed              = 0;
        return Transfer{.status = Status::Ok, .bytes = honored};
    }
    if (put != Status::Ok) return Transfer{.status = put, .bytes = 0};

    const std::size_t most = state_->sizes.cbMaximumMessage;
    if (most == 0) return Transfer{.status = Status::Other, .bytes = 0};

    // One record's worth of room, laid out the way the provider fills it: its header, the plaintext, its
    // trailer, end to end.
    std::vector<std::byte> record(static_cast<std::size_t>(state_->sizes.cbHeader) + most +
                                  state_->sizes.cbTrailer);

    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const std::size_t chunk = std::min(most, bytes.size() - sent);
        std::memcpy(record.data() + state_->sizes.cbHeader, bytes.data() + sent, chunk);

        SecBuffer parts[4]{};
        parts[0].BufferType = SECBUFFER_STREAM_HEADER;
        parts[0].pvBuffer   = record.data();
        parts[0].cbBuffer   = state_->sizes.cbHeader;
        parts[1].BufferType = SECBUFFER_DATA;
        parts[1].pvBuffer   = record.data() + state_->sizes.cbHeader;
        parts[1].cbBuffer   = static_cast<unsigned long>(chunk);
        parts[2].BufferType = SECBUFFER_STREAM_TRAILER;
        parts[2].pvBuffer   = record.data() + state_->sizes.cbHeader + chunk;
        parts[2].cbBuffer   = state_->sizes.cbTrailer;
        parts[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc message{SECBUFFER_VERSION, 4, parts};

        if (::EncryptMessage(&state_->context, 0, &message, 0) != SEC_E_OK) {
            if (sent > 0) return Transfer{.status = Status::Ok, .bytes = sent};
            return Transfer{.status = Status::Other, .bytes = 0};
        }

        const std::size_t produced = static_cast<std::size_t>(parts[0].cbBuffer) + parts[1].cbBuffer +
                                     parts[2].cbBuffer;
        state_->unsent.insert(state_->unsent.end(), record.data(), record.data() + produced);

        const Status pushed = flush(*state_);
        if (pushed == Status::Closed) return Transfer{.status = Status::Closed, .bytes = sent};
        if (pushed != Status::Ok) {
            // This chunk is encrypted but not yet gone, so it is owed rather than sent.
            state_->owed = chunk;
            return Transfer{.status = sent > 0 ? Status::Ok : Status::WouldBlock, .bytes = sent};
        }
        sent += chunk;
    }
    return Transfer{.status = Status::Ok, .bytes = sent};
}

Transfer TlsStream::receive(std::span<std::byte> into) {
    if (state_ == nullptr) return Transfer{.status = Status::Closed, .bytes = 0};
    if (!state_->settled) return Transfer{.status = Status::WouldBlock, .bytes = 0};

    for (;;) {
        if (!state_->plain.empty()) {
            const std::size_t moved = std::min(into.size(), state_->plain.size());
            std::memcpy(into.data(), state_->plain.data(), moved);
            state_->plain.erase(state_->plain.begin(),
                                state_->plain.begin() + static_cast<std::ptrdiff_t>(moved));
            return Transfer{.status = Status::Ok, .bytes = moved};
        }
        if (state_->closed && state_->inbound.empty()) {
            return Transfer{.status = Status::Closed, .bytes = 0};
        }

        if (!state_->inbound.empty()) {
            SecBuffer parts[4]{};
            parts[0].BufferType = SECBUFFER_DATA;
            parts[0].pvBuffer   = state_->inbound.data();
            parts[0].cbBuffer   = static_cast<unsigned long>(state_->inbound.size());
            parts[1].BufferType = SECBUFFER_EMPTY;
            parts[2].BufferType = SECBUFFER_EMPTY;
            parts[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc message{SECBUFFER_VERSION, 4, parts};

            const SECURITY_STATUS rc = ::DecryptMessage(&state_->context, &message, 0, nullptr);

            if (rc == SEC_E_OK || rc == SEC_I_RENEGOTIATE) {
                // Decryption happens in place, so the plaintext is taken out of the buffer before what
                // is left of it moves.
                std::size_t kept = 0;
                for (const SecBuffer& part : parts) {
                    if (part.BufferType == SECBUFFER_DATA && part.pvBuffer != nullptr) {
                        const auto* first = static_cast<const std::byte*>(part.pvBuffer);
                        state_->plain.insert(state_->plain.end(), first, first + part.cbBuffer);
                    } else if (part.BufferType == SECBUFFER_EXTRA) {
                        kept = std::min(static_cast<std::size_t>(part.cbBuffer), state_->inbound.size());
                    }
                }
                state_->inbound.erase(state_->inbound.begin(),
                                      state_->inbound.end() - static_cast<std::ptrdiff_t>(kept));

                if (rc == SEC_I_RENEGOTIATE) {
                    const Status carried = carryPostHandshake(*state_);
                    if (carried == Status::Closed) return Transfer{.status = Status::Closed, .bytes = 0};
                    if (carried == Status::Other) return Transfer{.status = Status::Other, .bytes = 0};
                }
                continue;
            }

            if (rc == SEC_I_CONTEXT_EXPIRED) {
                // The peer said it is done. Anything already decrypted is still owed to the caller, which
                // the top of this loop hands over before reporting the close.
                state_->closed = true;
                state_->inbound.clear();
                continue;
            }
            if (rc != SEC_E_INCOMPLETE_MESSAGE) return Transfer{.status = Status::Other, .bytes = 0};
        }

        // The provider wants bytes it has not been given. Take whatever the carrier has; if it has none,
        // say so rather than wait — a caller that wants to wait says so with waitReadable.
        const Transfer got = fill(*state_);
        if (got.bytes > 0) continue;
        if (got.status == Status::Closed) return Transfer{.status = Status::Closed, .bytes = 0};
        if (got.status == Status::Ok || got.status == Status::WouldBlock) {
            return Transfer{.status = Status::WouldBlock, .bytes = 0};
        }
        return Transfer{.status = got.status, .bytes = 0};
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
    PCCERT_CONTEXT certificate = ::CertCreateCertificateContext(
        X509_ASN_ENCODING, reinterpret_cast<const BYTE*>(der.data()),
        static_cast<unsigned long>(der.size()));
    if (certificate == nullptr) return Status::Other;

    ULARGE_INTEGER when{};
    when.LowPart  = certificate->pCertInfo->NotAfter.dwLowDateTime;
    when.HighPart = certificate->pCertInfo->NotAfter.dwHighDateTime;
    ::CertFreeCertificateContext(certificate);

    // A file time counts hundreds of nanoseconds from 1601; the caller wants whole seconds from 1970.
    constexpr std::uint64_t kEpochOffset = 116444736000000000ULL;
    constexpr std::uint64_t kPerSecond   = 10000000ULL;
    if (when.QuadPart < kEpochOffset) return Status::Other;

    unixSeconds = static_cast<std::int64_t>((when.QuadPart - kEpochOffset) / kPerSecond);
    return Status::Ok;
}

}  // namespace retropp::net

#endif  // _WIN32

// TLS on Apple platforms: SecureTransport.
//
// SecureTransport is used because it is a byte transformer — it moves bytes through IO callbacks the
// caller supplies, so it layers over whatever carrier it is handed. Network.framework, its replacement,
// owns its own connection: it creates and manages the socket itself, so it cannot sit on the seam this
// file takes as its input and could never carry the in-process pair. There is no system OpenSSL here to
// reach for instead.
//
// SecureTransport is deprecated, and the warning is suppressed around its declarations in this file
// alone — the project's warning bar is not lowered anywhere else for it. If Apple removes it, this
// becomes a transport of its own rather than a layer, and the Stream contract is what keeps that a
// contained change.

#include "src/net/tls.h"

#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <Security/SecCertificate.h>
#include <Security/SecCertificateOIDs.h>
#include <Security/SecImportExport.h>
#include <Security/SecTrust.h>
#include <Security/SecureTransport.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace retropp::net {
namespace {

// What the IO callbacks see. Kept separate from the stream's own state so the callbacks — which are
// file-local — never need to name a private nested type.
struct Carrier {
    Stream* stream = nullptr;
    bool    closed = false;
};

OSStatus carrierRead(SSLConnectionRef connection, void* data, size_t* dataLength) {
    auto* carrier = static_cast<Carrier*>(const_cast<void*>(connection));
    const std::size_t wanted = *dataLength;
    std::size_t       got    = 0;

    while (got < wanted) {
        const Transfer moved = carrier->stream->receive(
            std::span<std::byte>{static_cast<std::byte*>(data) + got, wanted - got});
        if (moved.status == Status::Ok) {
            got += moved.bytes;
            continue;
        }
        if (moved.status == Status::Closed) carrier->closed = true;
        break;
    }

    *dataLength = got;
    if (got == wanted) return noErr;
    // A partial read is ordinary: SecureTransport keeps what arrived and asks again.
    if (carrier->closed && got == 0) return errSSLClosedGraceful;
    return errSSLWouldBlock;
}

OSStatus carrierWrite(SSLConnectionRef connection, const void* data, size_t* dataLength) {
    auto* carrier = static_cast<Carrier*>(const_cast<void*>(connection));
    const std::size_t wanted = *dataLength;
    std::size_t       sent   = 0;

    while (sent < wanted) {
        const Transfer moved = carrier->stream->send(
            std::span<const std::byte>{static_cast<const std::byte*>(data) + sent, wanted - sent});
        if (moved.status == Status::Ok) {
            sent += moved.bytes;
            continue;
        }
        if (moved.status == Status::Closed) carrier->closed = true;
        break;
    }

    *dataLength = sent;
    if (sent == wanted) return noErr;
    if (carrier->closed && sent == 0) return errSSLClosedGraceful;
    return errSSLWouldBlock;
}

// The statuses that mean "the peer's certificate is not acceptable" rather than "come back later".
bool isTrustFailure(OSStatus status) {
    switch (status) {
        case errSSLXCertChainInvalid:
        case errSSLBadCert:
        case errSSLPeerBadCert:
        case errSSLUnknownRootCert:
        case errSSLNoRootCert:
        case errSSLCertExpired:
        case errSSLCertNotYetValid:
        case errSSLHostNameMismatch:
        case errSSLPeerCertExpired:
        case errSSLPeerCertRevoked:
        case errSSLPeerCertUnknown:
        case errSSLPeerUnsupportedCert:
        case errSSLPeerUnknownCA:            return true;
        default:                             return false;
    }
}

bool sameBytes(CFDataRef data, const Certificate& expected) {
    if (data == nullptr) return false;
    const auto length = static_cast<std::size_t>(CFDataGetLength(data));
    if (length != expected.size()) return false;
    return std::memcmp(CFDataGetBytePtr(data), expected.data(), length) == 0;
}

// Reads a PKCS#12 container into the identity array SSLSetCertificate wants. Returns nullptr on any
// failure; the caller reports it rather than guessing at a cause.
CFArrayRef importIdentity(const std::vector<std::byte>& container, const std::string& password) {
    CFDataRef blob = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(container.data()),
                                  static_cast<CFIndex>(container.size()));
    if (blob == nullptr) return nullptr;

    CFStringRef secret = CFStringCreateWithBytes(nullptr,
                                                 reinterpret_cast<const UInt8*>(password.data()),
                                                 static_cast<CFIndex>(password.size()),
                                                 kCFStringEncodingUTF8, false);
    const void* keys[]   = {kSecImportExportPassphrase};
    const void* values[] = {secret};
    CFDictionaryRef options =
        CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);

    CFArrayRef items  = nullptr;
    const OSStatus rc = SecPKCS12Import(blob, options, &items);

    CFArrayRef identities = nullptr;
    if (rc == errSecSuccess && items != nullptr && CFArrayGetCount(items) > 0) {
        auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(items, 0));
        auto identity =
            static_cast<SecIdentityRef>(const_cast<void*>(CFDictionaryGetValue(entry,
                                                                              kSecImportItemIdentity)));
        if (identity != nullptr) {
            const void* one[] = {identity};
            identities = CFArrayCreate(nullptr, one, 1, &kCFTypeArrayCallBacks);
        }
    }

    if (items != nullptr) CFRelease(items);
    if (options != nullptr) CFRelease(options);
    if (secret != nullptr) CFRelease(secret);
    CFRelease(blob);
    return identities;
}

}  // namespace

struct TlsStream::State {
    std::unique_ptr<Stream>  owned;
    Carrier                  carrier;
    SSLContextRef            context = nullptr;
    std::vector<Certificate> pinned;
    bool                     settled = false;

    ~State() {
        if (context != nullptr) {
            SSLClose(context);
            CFRelease(context);
        }
    }
};

namespace {

// The trust decision, taken here rather than by the library, which is what lets a certificate no public
// CA signed be accepted without a path that accepts anything.
bool peerIsAcceptable(TlsStream::State& state) {
    SecTrustRef trust = nullptr;
    if (SSLCopyPeerTrust(state.context, &trust) != errSecSuccess || trust == nullptr) return false;

    bool acceptable = false;
    if (state.pinned.empty()) {
        // Nothing pinned: the OS's own roots decide, which is what a client on the public internet wants.
        acceptable = SecTrustEvaluateWithError(trust, nullptr);
    } else if (SecTrustGetCertificateCount(trust) > 0) {
        SecCertificateRef leaf = SecTrustGetCertificateAtIndex(trust, 0);
        CFDataRef         der  = (leaf != nullptr) ? SecCertificateCopyData(leaf) : nullptr;
        acceptable = std::any_of(state.pinned.begin(), state.pinned.end(),
                                 [der](const Certificate& one) { return sameBytes(der, one); });
        if (der != nullptr) CFRelease(der);
    }

    CFRelease(trust);
    return acceptable;
}

Status openTls(std::unique_ptr<Stream> over, SSLProtocolSide side, TlsStream::State& state) {
    if (over == nullptr) return Status::Closed;

    state.context = SSLCreateContext(nullptr, side, kSSLStreamType);
    if (state.context == nullptr) return Status::Other;

    state.owned          = std::move(over);
    state.carrier.stream = state.owned.get();

    if (SSLSetIOFuncs(state.context, carrierRead, carrierWrite) != errSecSuccess) return Status::Other;
    if (SSLSetConnection(state.context, &state.carrier) != errSecSuccess) return Status::Other;
    return Status::Ok;
}

}  // namespace

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
    if (const Status opened = openTls(std::move(over), kSSLClientSide, *state); opened != Status::Ok) {
        return opened;
    }

    state->pinned = accepting.pinned;
    if (!accepting.hostname.empty()) {
        SSLSetPeerDomainName(state->context, accepting.hostname.c_str(), accepting.hostname.size());
    }
    // Stop at the trust point and decide here. Without this the library applies its own policy and a
    // pinned certificate no public CA signed would be refused before this code ever sees it.
    if (SSLSetSessionOption(state->context, kSSLSessionOptionBreakOnServerAuth, true) != errSecSuccess) {
        return Status::Other;
    }

    out.state_ = std::move(state);
    return Status::Ok;
}

Status openTlsServer(std::unique_ptr<Stream> over, const TlsServerConfig& presenting, TlsStream& out) {
    auto state = std::make_unique<TlsStream::State>();
    if (const Status opened = openTls(std::move(over), kSSLServerSide, *state); opened != Status::Ok) {
        return opened;
    }

    CFArrayRef identity = importIdentity(presenting.identity, presenting.password);
    if (identity == nullptr) return Status::Other;
    const OSStatus set = SSLSetCertificate(state->context, identity);
    CFRelease(identity);
    if (set != errSecSuccess) return Status::Other;

    out.state_ = std::move(state);
    return Status::Ok;
}

Status TlsStream::handshake(std::chrono::milliseconds timeout) {
    if (state_ == nullptr) return Status::Closed;
    if (state_->settled) return Status::Ok;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const OSStatus step = SSLHandshake(state_->context);

        if (step == errSecSuccess) {
            state_->settled = true;
            return Status::Ok;
        }
        if (step == errSSLPeerAuthCompleted) {
            // The trust point. A verdict either way — never a stall, and never revisited.
            if (!peerIsAcceptable(*state_)) return Status::Untrusted;
            continue;
        }
        if (step == errSSLWouldBlock) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return Status::WouldBlock;
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const Status ready = state_->carrier.stream->waitReadable(remaining);
            if (ready == Status::TimedOut) return Status::WouldBlock;
            if (ready != Status::Ok) return ready;
            continue;
        }
        if (isTrustFailure(step)) return Status::Untrusted;
        if (step == errSSLClosedGraceful || step == errSSLClosedAbort) return Status::Closed;
        return Status::Other;
    }
}

bool TlsStream::settled() const noexcept { return state_ != nullptr && state_->settled; }

Transfer TlsStream::send(std::span<const std::byte> bytes) {
    if (state_ == nullptr) return Transfer{.status = Status::Closed, .bytes = 0};
    if (!state_->settled) return Transfer{.status = Status::WouldBlock, .bytes = 0};

    std::size_t    moved = 0;
    const OSStatus rc    = SSLWrite(state_->context, bytes.data(), bytes.size(), &moved);
    if (rc == errSecSuccess) return Transfer{.status = Status::Ok, .bytes = moved};
    if (rc == errSSLWouldBlock) {
        return Transfer{.status = moved > 0 ? Status::Ok : Status::WouldBlock, .bytes = moved};
    }
    if (rc == errSSLClosedGraceful || rc == errSSLClosedAbort) {
        return Transfer{.status = Status::Closed, .bytes = moved};
    }
    return Transfer{.status = Status::Other, .bytes = moved};
}

Transfer TlsStream::receive(std::span<std::byte> into) {
    if (state_ == nullptr) return Transfer{.status = Status::Closed, .bytes = 0};
    if (!state_->settled) return Transfer{.status = Status::WouldBlock, .bytes = 0};

    std::size_t    moved = 0;
    const OSStatus rc    = SSLRead(state_->context, into.data(), into.size(), &moved);
    if (rc == errSecSuccess) return Transfer{.status = Status::Ok, .bytes = moved};
    if (rc == errSSLWouldBlock) {
        // A record that has not arrived whole yet reads as nothing available, not as a short record.
        return Transfer{.status = moved > 0 ? Status::Ok : Status::WouldBlock, .bytes = moved};
    }
    if (rc == errSSLClosedGraceful || rc == errSSLClosedAbort) {
        return Transfer{.status = moved > 0 ? Status::Ok : Status::Closed, .bytes = moved};
    }
    return Transfer{.status = Status::Other, .bytes = moved};
}

Status TlsStream::waitReadable(std::chrono::milliseconds timeout) {
    if (state_ == nullptr) return Status::Closed;
    return state_->carrier.stream->waitReadable(timeout);
}

bool TlsStream::closed() const noexcept {
    return state_ == nullptr || state_->carrier.closed || state_->carrier.stream->closed();
}

Status certificateExpiry(std::span<const std::byte> der, std::int64_t& unixSeconds) {
    CFDataRef blob = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(der.data()),
                                  static_cast<CFIndex>(der.size()));
    if (blob == nullptr) return Status::Other;

    SecCertificateRef certificate = SecCertificateCreateWithData(nullptr, blob);
    CFRelease(blob);
    if (certificate == nullptr) return Status::Other;

    const void* wanted[] = {kSecOIDX509V1ValidityNotAfter};
    CFArrayRef  keys     = CFArrayCreate(nullptr, wanted, 1, &kCFTypeArrayCallBacks);
    CFDictionaryRef values = SecCertificateCopyValues(certificate, keys, nullptr);
    CFRelease(keys);
    CFRelease(certificate);
    if (values == nullptr) return Status::Other;

    Status result = Status::Other;
    if (auto entry = static_cast<CFDictionaryRef>(
            CFDictionaryGetValue(values, kSecOIDX509V1ValidityNotAfter));
        entry != nullptr) {
        if (auto when =
                static_cast<CFNumberRef>(CFDictionaryGetValue(entry, kSecPropertyKeyValue));
            when != nullptr) {
            double absolute = 0.0;
            if (CFNumberGetValue(when, kCFNumberDoubleType, &absolute)) {
                // Core Foundation counts from 2001-01-01; the caller wants the Unix epoch.
                unixSeconds = static_cast<std::int64_t>(absolute + kCFAbsoluteTimeIntervalSince1970);
                result      = Status::Ok;
            }
        }
    }

    CFRelease(values);
    return result;
}

}  // namespace retropp::net

#pragma clang diagnostic pop

#endif  // __APPLE__

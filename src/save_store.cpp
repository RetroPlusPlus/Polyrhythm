#include "retropp/save_store.h"

// SDL_GetPrefPath — the platform per-user data directory; SDL_GetBasePath — the application's
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>      // SDL_free

#include "durable_file.h"  // detail::writeFileDurably / atomicReplace / syncDirectoryEntry
#include "save_store_testing.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace retropp {

namespace {

// The on-disk envelope, fixed little-endian so a file written on one platform reads on any
// other: a 4-byte magic tag, the container-format version (the store's own layout version,
// independent of the consumer schema version), the consumer schema version, and the payload
// length, followed by the payload bytes.
constexpr std::array<std::byte, 4> kMagic = {std::byte{'R'}, std::byte{'P'}, std::byte{'S'},
                                             std::byte{'V'}};
constexpr std::uint32_t kContainerVersion = 1;
constexpr std::size_t kHeaderSize = 4 + 4 + 4 + 8;  // magic + container + schema + length

void putU32(std::byte* out, std::uint32_t v) {
    out[0] = std::byte(v & 0xFF);
    out[1] = std::byte((v >> 8) & 0xFF);
    out[2] = std::byte((v >> 16) & 0xFF);
    out[3] = std::byte((v >> 24) & 0xFF);
}

void putU64(std::byte* out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out[i] = std::byte((v >> (8 * i)) & 0xFF);
}

std::uint32_t getU32(const std::byte* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t getU64(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}

// A document name is a flat identifier, never a path: rejecting separators (and the drive
// designator, and the two special directory names) guarantees a document cannot land
// outside the store's directory.
void validateName(std::string_view name) {
    const bool bad = name.empty() || name == "." || name == ".." ||
                     name.find('/') != std::string_view::npos ||
                     name.find('\\') != std::string_view::npos ||
                     name.find(':') != std::string_view::npos;
    if (bad) {
        throw std::invalid_argument("SaveStore: a document name must be a flat identifier "
                                    "(no path separators, drive designators, '.' or '..'): \"" +
                                    std::string(name) + "\"");
    }
}

// The file beside the application that turns the switch on when the placement allows it. Presence is the signal;
// the contents are never read.
constexpr const char* kPortableFile = "portable.txt";

// A directory path with any trailing separator dropped, so its last component is the directory's own name.
std::filesystem::path withoutTrailingSeparator(const std::filesystem::path& path) {
    return path.filename().empty() ? path.parent_path() : path;
}

// The application's directory as the platform reports it: SDL's base path (cached by SDL3, not freed), run through
// the bundle rule on Apple platforms. Empty when the platform supplies none.
std::filesystem::path applicationDir() {
    const char* base = SDL_GetBasePath();
    if (base == nullptr) return {};
#if defined(__APPLE__)
    return detail::applicationDirFrom(base);
#else
    return withoutTrailingSeparator(base);
#endif
}

}  // namespace

std::filesystem::path detail::applicationDirFrom(const std::filesystem::path& basePath) {
    const std::filesystem::path dir = withoutTrailingSeparator(basePath);
    // Walk up from the last component. `below` is the component under `here`, empty at the deepest one.
    std::filesystem::path below;
    for (std::filesystem::path here = dir; here.has_relative_path(); here = here.parent_path()) {
        if (here.filename().extension() == ".app") {
            return below.empty() || below == "Contents" ? here.parent_path() : dir;
        }
        below = here.filename();
    }
    return dir;
}

std::filesystem::path detail::userDataDirAt(const std::filesystem::path& applicationDir,
                                            const AppIdentity& identity, const PortableInstall& placement) {
    // No fallback name: a fallback would give every unconfigured game the same directory,
    // and their saves would collide. An unset identity is refused, loudly, on first run.
    if (identity.organization.empty() || identity.application.empty()) {
        throw SaveStoreError(
            "retropp: the application identity is not set — assign "
            "config.identity = {\"YourOrg\", \"YourGame\"} and call EngineConfig::setActive "
            "before asking for the user data directory (or root a store explicitly with atPath)");
    }
    const std::string& org = identity.organization;
    const std::string& app = identity.application;

    // The file is looked for only in an application directory the platform supplied; one that cannot be read
    // holds no file.
    std::error_code ec;
    const bool portable =
        placement.enabled || (placement.allowFile && !applicationDir.empty() &&
                              std::filesystem::is_regular_file(applicationDir / kPortableFile, ec));

    if (portable) {
        if (applicationDir.empty()) {
            throw SaveStoreError("retropp: the platform did not supply the application's directory, so there "
                                 "is nowhere beside it for \"" + app + "\"");
        }
        const std::filesystem::path dir = applicationDir / app;
        std::filesystem::create_directories(dir, ec);
        if (ec || !std::filesystem::is_directory(dir, ec)) {
            throw SaveStoreError("retropp: the portable directory \"" + dir.string() +
                                 "\" could not be created beside the application");
        }
        return dir;
    }

    char* pref = SDL_GetPrefPath(org.c_str(), app.c_str());
    if (pref == nullptr) {
        throw SaveStoreError("retropp: the platform did not supply a user data directory for \"" +
                             org + "/" + app + "\"");
    }
    std::filesystem::path dir(pref);
    SDL_free(pref);
    return dir;
}

std::filesystem::path userDataDir(const AppIdentity& identity, const PortableInstall& placement) {
    return detail::userDataDirAt(applicationDir(), identity, placement);
}

std::filesystem::path userDataDir(const AppIdentity& identity) {
    return userDataDir(identity, PortableInstall::active);
}

std::filesystem::path userDataDir() { return userDataDir(SaveStore::defaultIdentity, PortableInstall::active); }

// One resolution, one owner: the store roots itself where userDataDir says, so a game placing files
// beside its saves cannot end up beside a different directory than the saves themselves.
SaveStore::SaveStore() : base_(userDataDir(defaultIdentity)) {}

SaveStore SaveStore::atPath(std::filesystem::path base) { return SaveStore(std::move(base)); }

std::filesystem::path SaveStore::documentPath(std::string_view name) const {
    validateName(name);
    return base_ / std::filesystem::path(std::string(name));
}

bool SaveStore::write(std::string_view name, std::uint32_t schemaVersion,
                      std::span<const std::byte> payload) {
    const std::filesystem::path target = documentPath(name);

    std::error_code ec;
    std::filesystem::create_directories(base_, ec);
    if (ec) return false;

    std::array<std::byte, kHeaderSize> header{};
    std::copy(kMagic.begin(), kMagic.end(), header.begin());
    putU32(header.data() + 4, kContainerVersion);
    putU32(header.data() + 8, schemaVersion);
    putU64(header.data() + 12, payload.size());

    // A process-unique sibling temp name: the rename below is only atomic within one
    // filesystem, so the temp lives in the target's own directory.
    static std::atomic<unsigned> counter{0};
    const std::filesystem::path temp =
        base_ / (std::string(name) + "." + std::to_string(counter.fetch_add(1)) + ".tmp");

    if (!detail::writeFileDurably(temp, header, payload)) {
        std::filesystem::remove(temp, ec);
        return false;
    }

    if (preCommitHook_) {
        try {
            preCommitHook_();
        } catch (...) {
            std::filesystem::remove(temp, ec);
            return false;
        }
    }

    if (!detail::atomicReplace(temp, target)) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    detail::syncDirectoryEntry(base_);
    return true;
}

std::optional<SaveStore::Document> SaveStore::read(std::string_view name) const {
    const std::filesystem::path file = documentPath(name);

    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return std::nullopt;

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw SaveStoreError("SaveStore: document \"" + std::string(name) +
                             "\" exists but cannot be opened");
    }
    std::vector<std::byte> bytes(std::filesystem::file_size(file, ec));
    if (ec || !in.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()))) {
        throw SaveStoreError("SaveStore: document \"" + std::string(name) +
                             "\" could not be read");
    }

    if (bytes.size() < kHeaderSize) {
        throw SaveStoreError("SaveStore: document \"" + std::string(name) +
                             "\" is truncated (shorter than the envelope header)");
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        throw SaveStoreError("SaveStore: document \"" + std::string(name) +
                             "\" is not a save document (bad magic tag)");
    }
    const std::uint32_t container = getU32(bytes.data() + 4);
    if (container != kContainerVersion) {
        throw SaveStoreError("SaveStore: document \"" + std::string(name) +
                             "\" uses container format " + std::to_string(container) +
                             "; this store reads format " + std::to_string(kContainerVersion));
    }
    const std::uint64_t length = getU64(bytes.data() + 12);
    if (length != bytes.size() - kHeaderSize) {
        throw SaveStoreError("SaveStore: document \"" + std::string(name) +
                             "\" payload length does not match its envelope (corrupt or "
                             "truncated)");
    }

    Document doc;
    doc.schemaVersion = getU32(bytes.data() + 8);
    doc.payload.assign(bytes.begin() + kHeaderSize, bytes.end());
    return migrate(std::move(doc));
}

SaveStore::Document SaveStore::migrate(Document doc) const {
    if (!currentVersion_.has_value()) return doc;
    const std::uint32_t current = *currentVersion_;
    if (doc.schemaVersion > current) {
        throw SaveStoreError("SaveStore: document schema version " +
                             std::to_string(doc.schemaVersion) +
                             " is newer than the declared current version " +
                             std::to_string(current) + " — refusing to read it");
    }
    while (doc.schemaVersion < current) {
        const auto step = migrations_.find(doc.schemaVersion);
        if (step == migrations_.end()) {
            throw SaveStoreError("SaveStore: no migration registered from schema version " +
                                 std::to_string(doc.schemaVersion) +
                                 " (current version is " + std::to_string(current) + ")");
        }
        doc.payload = step->second(std::move(doc.payload));
        ++doc.schemaVersion;
    }
    return doc;
}

void SaveStore::setCurrentVersion(std::uint32_t version) { currentVersion_ = version; }

void SaveStore::registerMigration(std::uint32_t fromVersion, MigrationStep step) {
    migrations_[fromVersion] = std::move(step);
}

bool SaveStore::exists(std::string_view name) const {
    std::error_code ec;
    return std::filesystem::exists(documentPath(name), ec);
}

bool SaveStore::remove(std::string_view name) {
    std::error_code ec;
    return std::filesystem::remove(documentPath(name), ec);
}

}  // namespace retropp

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "retropp/app_identity.h"
#include "retropp/portable_install.h"

namespace retropp {

namespace detail {
struct SaveStoreTestAccess;
}

// Thrown when a stored document cannot be trusted: a corrupt or truncated envelope, a
// schema version newer than the code understands, or a gap in the registered migration
// chain. Deliberately distinct from an ABSENT document (read returns std::nullopt): a
// corrupt save surfacing as "no save" would silently discard the player's data, so the
// two conditions never share a signal.
class SaveStoreError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Where the player's files go for `identity` under `placement`: beside the application —
// `<the application's directory>/<application>/` — when the placement puts them there (portable_install.h), and
// otherwise the platform's per-user data directory — `%APPDATA%\<org>\<app>` on Windows,
// `~/Library/Application Support/<org>/<app>` on macOS, `$XDG_DATA_HOME/<org>/<app>` on Linux. This is the
// directory a default-constructed SaveStore and a default-constructed UserFiles write into, named so a game can
// place its other files beside its saves instead of resolving a second directory of its own. A placement of
// `{.allowFile = false}` names the per-user directory whatever is active, and `{.enabled = true}` the portable one,
// which is how a program that resolves before EngineConfig::setActive(), or one that needs both locations by name,
// asks.
//
// Throws SaveStoreError when either identity field is empty (an unconfigured identity has no correct directory,
// and a fallback would collide every unconfigured game into one) or when the platform cannot supply the per-user
// directory. The error type is the persistence one because this IS the persistence directory.
//
// When the portable directory cannot be created where the application is — a read-only location, a file standing
// in its place — or the platform does not report where the application is, the run ends: the player is shown an
// error box, "<application> cannot start: The portable installation directory (<directory>) cannot be created:
// <reason>", and the process exits with a failure status.
//
// Resolving the directory creates it if absent — the platform call does that for the per-user one and this
// function for the portable one — and it is what makes the path immediately writable. Nothing inside it is
// created. A portable resolution never asks the platform for the per-user directory.
[[nodiscard]] std::filesystem::path userDataDir(const AppIdentity& identity, const PortableInstall& placement);

// The same, under the placement EngineConfig::setActive() published (PortableInstall::active).
[[nodiscard]] std::filesystem::path userDataDir(const AppIdentity& identity);

// The same, for the identity and the placement EngineConfig::setActive() published — the common case.
[[nodiscard]] std::filesystem::path userDataDir();

// A durable, versioned store of named byte documents — the persistence primitive beneath
// game saves and settings. Each document is an opaque byte payload tagged with a
// consumer-defined schema version; the store never interprets the payload. What the store
// owns is everything around the bytes:
//
//   * WHERE — a default-constructed store resolves the player's directory — the platform-correct
//     per-user one for EngineConfig::active.identity (%APPDATA%\<org>\<app> on Windows,
//     ~/Library/Application Support/<org>/<app> on macOS, $XDG_DATA_HOME/<org>/<app> on
//     Linux), or a directory named for the application beside the application under
//     EngineConfig::portable — once, at construction. SaveStore::atPath roots a store at an explicit
//     directory instead — the seam tests use to stay hermetic, and the way to place
//     documents anywhere else deliberately.
//
//   * DURABILITY — write() is atomic: the document is written to a sibling temp file,
//     flushed to disk, then moved over the target in one filesystem rename. A crash at
//     any point mid-write leaves the previous document intact; a document on disk is
//     always a complete one. A failed write returns false with the prior bytes untouched.
//
//   * VERSIONING — every document carries its consumer schema version in a fixed,
//     endian-stable envelope, so a file written on one platform reads on any other.
//
//   * MIGRATION — the consumer declares its current version and registers one step per
//     version bump (each step transforms version v payload bytes into version v+1 payload
//     bytes). read() walks an older document through the steps in order and returns the
//     payload already current. The step logic — like the payload format itself — belongs
//     entirely to the consumer; the store orchestrates the walk, rejects files newer than
//     the declared current version, and rejects a chain with a missing step.
//
// Document names are flat identifiers ("slot1", "settings"), not paths: a name containing
// a path separator, a drive designator, or naming "." / ".." throws std::invalid_argument,
// so a document can never land outside the store's directory. Two stores at different base
// directories are fully independent; the store holds no global state.
//
// Persistence is a synchronous main-thread call, like the rest of the platform surface.
class SaveStore {
public:
    // One read result: the payload bytes and the schema version they are AT — the stored
    // version when no migrations apply, the declared current version after a migration walk.
    struct Document {
        std::uint32_t schemaVersion = 0;
        std::vector<std::byte> payload;
    };

    // One migration step: takes the payload at some version v, returns it at v+1. The
    // transform is plain consumer code; the store never looks inside either buffer.
    using MigrationStep = std::function<std::vector<std::byte>(std::vector<std::byte>)>;

    // Resolves the save directory from `defaultIdentity` and PortableInstall::active (both
    // seeded by setActive(), like the other per-type defaults). Throws
    // SaveStoreError when the identity is not set (an unconfigured identity has no correct
    // directory — a fallback would collide every unconfigured game into one), or when the
    // platform cannot supply a directory. A portable directory that cannot be created ends the
    // run — see userDataDir().
    SaveStore();

    // The application identity a default-constructed store resolves its directory from.
    // EngineConfig::setActive() assigns it from EngineConfig::identity — the one startup
    // call covers it; set it directly only when bypassing the config bundle entirely.
    static inline AppIdentity defaultIdentity{};

    // A store rooted at an explicit directory, bypassing the platform resolution. The
    // directory is created on first write; it does not need to exist yet.
    static SaveStore atPath(std::filesystem::path base);

    // Atomically writes (or replaces) the named document. Returns true when the document
    // is durably on disk; false when any step failed — in which case the previously stored
    // document, if any, is untouched. Never leaves a partial document behind.
    bool write(std::string_view name, std::uint32_t schemaVersion,
               std::span<const std::byte> payload);

    // Reads the named document. Returns std::nullopt when no such document exists — the
    // ordinary first-run case. Throws SaveStoreError when a document is present but cannot
    // be trusted (corrupt/truncated envelope, version newer than the declared current,
    // missing migration step). When a current version is declared and the stored version
    // is older, the payload is returned already walked through the registered migration
    // steps and tagged with the current version.
    std::optional<Document> read(std::string_view name) const;

    // Declares the schema version the consumer's code is written against. Once declared,
    // read() migrates older documents up to it and refuses newer ones. Without a declared
    // current version, read() returns every document verbatim at its stored version.
    void setCurrentVersion(std::uint32_t version);

    // Registers the step that lifts a version `fromVersion` payload to `fromVersion + 1`.
    // Register one step per version bump; read() applies them in sequence.
    void registerMigration(std::uint32_t fromVersion, MigrationStep step);

    // Whether the named document exists on disk. Existence only — no envelope validation.
    bool exists(std::string_view name) const;

    // Deletes the named document. Returns true when a document was removed, false when
    // there was none.
    bool remove(std::string_view name);

    // The resolved directory this store reads and writes documents in.
    const std::filesystem::path& basePath() const { return base_; }

private:
    explicit SaveStore(std::filesystem::path base) : base_(std::move(base)) {}

    std::filesystem::path documentPath(std::string_view name) const;
    Document migrate(Document doc) const;

    std::filesystem::path base_;
    std::optional<std::uint32_t> currentVersion_;
    std::map<std::uint32_t, MigrationStep> migrations_;

    // Invoked after the temp file is flushed and before the rename commit; a throw aborts
    // the write. Lets the crash-safety test sever a write at the exact pre-commit moment.
    std::function<void()> preCommitHook_;
    friend struct detail::SaveStoreTestAccess;
};

}  // namespace retropp

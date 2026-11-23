#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "retropp/app_identity.h"
#include "retropp/save_store.h"  // userDataDir, SaveStoreError — the directory this shares with saves

namespace retropp {

// Files a game keeps for one player, in the player's directory — the one `userDataDir()` answers, which a
// SaveStore writes into too (save_store.h). Extracted assets, screenshots, a cache, a log:
// anything that belongs to this player on this machine and is not a save document.
//
// SaveStore is the surface for documents the game must be able to READ BACK ACROSS VERSIONS — it wraps
// every payload in an envelope, tags it with a schema version, and walks old files forward through
// registered migrations. That machinery exists because a player's save has to survive the game changing
// underneath it. A decoded tile sheet does not: it is bytes the game wrote and will read back verbatim,
// and re-deriving it is cheaper than migrating it. So this store keeps the parts that are about the
// FILE — where it goes, and that a crash cannot leave half of one — and none of the parts about a
// document's life over time.
//
// What it keeps from SaveStore:
//
//   * WHERE — a default-constructed store roots at userDataDir(), resolved from the identity and the
//     placement EngineConfig::setActive() published. UserFiles::atPath roots at an explicit directory instead.
//     root() is the directory itself, which is what a game assigns to EngineConfig::assetRoot when its
//     assets live here.
//
//   * DURABILITY — write() is atomic: the bytes go to a sibling temp file, are flushed to the device,
//     then moved over the target in one filesystem rename. A crash mid-write leaves the previous file
//     intact, never a partial one.
//
// What it drops: the envelope, the schema version, and the migration chain. A file written here is
// exactly the bytes handed to write(), byte for byte, readable by any other program.
//
// Names ARE relative paths here, which is the other difference from SaveStore: "assets/tiles/00.png"
// names a file two directories down, and write() creates the directories on the way. A path that is
// absolute, names a drive, or contains a ".." component throws std::invalid_argument, so a file can
// never land outside the store's directory — the guarantee SaveStore gets by refusing separators
// outright, kept while allowing a tree.
//
// Two stores at different directories are fully independent; the store holds no global state. Like the
// rest of the platform surface, these are synchronous main-thread calls.
class UserFiles {
public:
    // Roots at the directory userDataDir() answers for the active identity and placement. Throws
    // SaveStoreError when the identity is unset, the platform cannot supply a directory, or the portable
    // directory cannot be created — see userDataDir().
    UserFiles();

    // Roots at `base` explicitly, bypassing platform resolution — the seam tests use to stay hermetic,
    // and the way to place files somewhere else deliberately. The directory is created on first write.
    [[nodiscard]] static UserFiles atPath(std::filesystem::path base);

    // The store's directory. Assign it (or a subdirectory of it) to EngineConfig::assetRoot to resolve
    // LoadFromPath assets out of the player's own files.
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return base_; }

    // Where `relativePath` lives, without touching the disk. Throws std::invalid_argument on a path that
    // could escape the store.
    [[nodiscard]] std::filesystem::path pathFor(std::string_view relativePath) const;

    // Write `bytes` to `relativePath`, atomically, creating any directories on the way. Returns false if
    // the write could not be completed — the previous file, if any, is untouched.
    bool write(std::string_view relativePath, std::span<const std::byte> bytes);

    // The file's bytes, or std::nullopt when it does not exist. An unreadable file reads as absent: this
    // store has no envelope to validate, so it has no way to tell a corrupt file from a short one.
    [[nodiscard]] std::optional<std::vector<std::byte>> read(std::string_view relativePath) const;

    [[nodiscard]] bool exists(std::string_view relativePath) const;

    // Deletes the file. Returns whether a file was removed — false when it was already absent.
    bool remove(std::string_view relativePath);

private:
    explicit UserFiles(std::filesystem::path base) : base_(std::move(base)) {}

    std::filesystem::path base_;
};

}  // namespace retropp

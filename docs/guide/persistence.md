# Persistence — the save store and the user's files

`save_store.h` · `user_files.h` · `app_identity.h` · `engine_config.h`

`SaveStore` is the platform's durable storage primitive: a store of named **byte documents**,
each tagged with a schema version, written atomically to the platform-correct per-user
directory. Game saves and settings both persist through it. The payload is opaque bytes end
to end — the store never interprets a document's contents; what a save *contains* is
entirely yours.

```cpp
#include "retropp/save_store.h"

retropp::SaveStore store;                       // resolves the platform save directory
store.write("slot1", 1, bytes);                 // atomic: never leaves a partial file
if (auto doc = store.read("slot1")) {           // nullopt = no such document
    load(doc->payload);                         // std::vector<std::byte>, exactly as written
}
```

## Contents

- [Where documents live](#where-documents-live)
- [Other files in the same directory](#other-files-in-the-same-directory)
  - [Which store to reach for](#which-store-to-reach-for)
- [Document names](#document-names)
- [Writes are atomic](#writes-are-atomic)
- [Absent is not corrupt](#absent-is-not-corrupt)
- [Schema versions and migration](#schema-versions-and-migration)
- [The envelope](#the-envelope)
- [The full surface](#the-full-surface)
- [Try it](#try-it)
- [Related pages & where to change things](#related-pages--where-to-change-things)

## Where documents live

A default-constructed `SaveStore` resolves its directory once, at construction, from the
application identity (`AppIdentity` — the program's identity to the host platform,
declared on `EngineConfig::identity`; `setActive()` fans it out to
`SaveStore::defaultIdentity` like the other per-type defaults). Set it in the startup
config before constructing a store:

```cpp
retropp::EngineConfig config;
config.identity = {.organization = "MyStudio", .application = "MyGame"};
retropp::EngineConfig::setActive(config);

retropp::SaveStore store;
store.basePath();   // the resolved directory
```

`AppIdentity` (`app_identity.h`) is a two-field aggregate, both required:

```cpp
struct AppIdentity { std::string organization, application; };   // no defaults — an empty field is refused
```

The identity maps to each platform's conventional per-user data location:

| Platform | Directory |
|---|---|
| Windows | `%APPDATA%\MyStudio\MyGame\` |
| macOS | `~/Library/Application Support/MyStudio/MyGame/` |
| Linux | `$XDG_DATA_HOME/MyStudio/MyGame/` (or `~/.local/share/...`) |

The identity is deliberately **required, with no defaults** — `setActive()` itself refuses a
config with either field empty (`std::invalid_argument`), and a default-constructed `SaveStore`
throws `SaveStoreError` (a `std::runtime_error`) when the identity it resolves against is unset,
or when the platform supplies no per-user directory. There is no fallback name —
a fallback would silently resolve every unconfigured game to the same directory and their
saves would collide, so an unset identity refuses loudly on first run instead. Set both
fields once, and never change them after players have saves: a changed identity is a
different directory, and the players' documents stay stranded under the old one.

To root a store somewhere else entirely — a test fixture, a portable-install layout, a
second profile — bypass the platform resolution with an explicit base:

```cpp
auto store = retropp::SaveStore::atPath("/some/explicit/directory");
```

Two stores at different bases are fully independent; the store holds no global state. The
directory does not need to exist yet — it is created on first write. This is also how tests
stay hermetic: every store test roots at a unique directory under the system temp path and
removes it afterward, so nothing touches real save data and no test sees another's files.

## Other files in the same directory

Not everything a game keeps for a player is a save. Extracted assets, screenshots, a cache, a log — they
belong in the same per-user directory, and `UserFiles` (`user_files.h`) is the surface for them. It is the
same directory a `SaveStore` resolves, obtained the same way, so a game's other files cannot end up beside
a different directory than its saves.

```cpp
#include "retropp/user_files.h"

retropp::UserFiles files;                              // the same directory the saves go in
files.write("screenshots/2026-08-19.png", png);        // your bytes, verbatim
if (auto bytes = files.read("screenshots/2026-08-19.png")) {
    show(*bytes);                                      // nullopt = no such file
}
```

A worked case: a game that decodes assets from a copy of the original the player supplies. Those assets
are derived from that player's own data and are never shipped, so they belong in the player's directory —
not beside the binary, not in the project tree.

```cpp
retropp::EngineConfig config;
config.identity = {.organization = "MyStudio", .application = "MyGame"};
retropp::EngineConfig::setActive(config);

retropp::UserFiles files;

if (!files.exists("assets/tiles/overworld.png")) {     // first launch only
    for (const Decoded& sheet : extractGraphics(source)) {
        files.write("assets/" + sheet.logicalPath, sheet.bytes);   // directories created on the way
    }
}

config.assetRoot = files.root() / "assets";            // LoadFromPath now resolves out of the player's files
retropp::EngineConfig::setActive(config);
```

`root()` is that directory, which is what makes the last two lines a one-line change rather than a second
path resolution. Once the asset root points there, every family's LoadFromPath path resolves out of the
player's own files — including a **data asset**, which is how a game reaches extracted content that is
not an image and not audio: `DataLibrary::registerData("corpus.bin")` returns a handle, `data()` returns
the bytes, and what they mean is the game's. See
[assets-and-embedding.md](assets-and-embedding.md#data--bytes-the-platform-never-interprets).

If you only want the location and not the store, `userDataDir()` names it directly — for the active
identity, or for any identity you pass:

```cpp
const std::filesystem::path dir   = retropp::userDataDir();
const std::filesystem::path tools = retropp::userDataDir({.organization = "MyStudio",
                                                          .application = "MyTools"});
```

Both throw `SaveStoreError` on an unset identity or a platform that supplies no directory — the same
refusal, for the same reason, as a default-constructed `SaveStore`. Resolving the directory creates it if
absent; nothing inside it is created until you write.

`UserFiles::atPath(base)` roots at an explicit directory, exactly as `SaveStore::atPath` does, and is how
the store's own tests stay hermetic.

### Which store to reach for

| | `SaveStore` | `UserFiles` |
|---|---|---|
| Names | flat identifiers (`"slot1"`) | relative paths (`"assets/tiles/00.png"`), directories created on write |
| On disk | a versioned envelope, then your payload | exactly your bytes — another program can open it |
| Reading an older file | migrated forward through registered steps | read back as written; there is nothing to migrate |
| A corrupt file | throws — a damaged save must never read as "no save" | reads as absent; there is no envelope to check |
| Atomic writes | yes | yes |

The split is about *time*, not importance. A save has to survive the game changing underneath it, which is
what the envelope, the schema version and the migration chain buy. A decoded tile sheet does not: it is
bytes the game wrote and will read back verbatim, and re-deriving it is cheaper than migrating it. Reach
for `SaveStore` when a file must still be readable by a future version of your game, and `UserFiles` when
it is data you can regenerate or replace.

## Document names

A document name is a flat identifier — `"slot1"`, `"settings"`, `"profile"` — not a path.
An empty name, names containing path separators or a drive designator, and the names `"."` /
`".."`, throw `std::invalid_argument`: a document can never land outside the store's directory.

## Writes are atomic

`write` puts the document on disk in a way a crash cannot corrupt:

1. The envelope and payload are written to a sibling temp file in the same directory.
2. The temp file is flushed to the device (the OS is asked to write it through, not
   merely accept it into a buffer).
3. The temp file is moved over the target in one filesystem rename — the single commit
   point.

A crash before the rename leaves the previous document exactly as it was; a crash after it
leaves the new document complete. There is no moment at which the named document is partial.
A failed `write` returns `false` with the prior document untouched and no temp debris left
behind.

This is why saving through the store needs no "backup the old file first" ritual — the
prior version *is* the fallback until the instant the new one is durably in place.

## Absent is not corrupt

The two failure signals are deliberately different, because they demand different responses:

- **Absent** — `read` returns `std::nullopt`. The ordinary first-run case; proceed with
  defaults.
- **Untrustworthy** — `read` throws `SaveStoreError`: the file is not a save document (bad
  magic tag), it is truncated or its payload length disagrees with the header, its container
  format is unknown, its schema version is newer than the running code declares, its migration
  chain has a gap, or the file exists but cannot be opened or read.

A corrupt document never reads as "no document." If it did, the natural caller response —
start fresh, write defaults — would silently overwrite the player's damaged-but-maybe-
recoverable data. Catch `SaveStoreError` where you read, and decide deliberately.

## Schema versions and migration

Every `write` tags the document with **your** schema version — a plain `uint32` whose
meaning belongs to you. When your format changes, bump the version and register one step
per bump; each step is plain code that transforms version-`v` payload bytes into
version-`v+1` payload bytes:

```cpp
store.setCurrentVersion(3);
store.registerMigration(1, upgradeV1ToV2);   // your function: v1 bytes in, v2 bytes out
store.registerMigration(2, upgradeV2ToV3);

auto doc = store.read("slot1");
// A v1 file on disk arrives here as doc->payload at v3, walked 1→2→3 in order.
```

The store orchestrates; it never interprets. On `read`:

- stored version **==** current — returned as-is, no steps run.
- stored version **<** current — walked through the registered steps in sequence and
  returned at the current version. Write it back at the current version when convenient;
  until you do, the old file just migrates again on each read.
- stored version **>** current — `SaveStoreError`. The file came from a newer build than
  the running code; guessing at it risks destroying it.
- A **gap** in the chain (current is 3, only `1→2` registered, a v1 file arrives) —
  `SaveStoreError`. A half-migrated document is worse than a refused one.

With no current version declared, `read` returns every document verbatim at its stored
version — the caller owns versioning entirely.

Because each step covers exactly one version bump, an ancient document upgrades through
every intermediate format with no combinatorial `1→N` transforms — one function per format
change you ever made, written once, when you made it.

## The envelope

On disk, a document is a small fixed header followed by the payload bytes: a magic tag
(`RPSV`), the store's own container-format version, your schema version, and the payload
length. All header fields are little-endian regardless of platform, so a document written
on one platform reads on any other. The header is validated on every read; the payload is
handed back untouched.

## The full surface

| Call | Does |
|---|---|
| `SaveStore()` | Store at the platform directory resolved from the application identity (`setActive()` seeds `SaveStore::defaultIdentity` from `EngineConfig::identity`); throws `SaveStoreError` if the identity is unset or the platform supplies no directory |
| `SaveStore::defaultIdentity` | The `AppIdentity` a default-constructed store resolves against — normally seeded by `setActive()`; assign directly only when bypassing the config bundle |
| `SaveStore::atPath(dir)` | Store rooted at an explicit directory |
| `write(name, version, bytes)` | Atomic write/replace; `false` on failure with the prior document intact |
| `read(name)` | `std::optional<Document>`; `nullopt` if absent; throws `SaveStoreError` if untrustworthy; migrates if older |
| `setCurrentVersion(v)` | Declare the schema version the running code is written against |
| `registerMigration(from, step)` | Register the `from → from+1` payload transform (`MigrationStep` = `std::function<std::vector<std::byte>(std::vector<std::byte>)>`) |
| `exists(name)` / `remove(name)` | Presence check / delete (`remove` returns `bool` — `true` if a document was removed) |
| `basePath()` | The resolved directory |

`Document` carries `schemaVersion` (the version the payload is *at* — post-migration when
steps ran) and `payload` (`std::vector<std::byte>`).

`UserFiles` (`user_files.h`), for the same directory without the document machinery:

| Call | Does |
|---|---|
| `userDataDir(identity)` / `userDataDir()` | The platform's per-user directory for an identity, or for the active one; throws `SaveStoreError` on an unset identity or a platform that supplies none. Resolving creates the directory |
| `UserFiles()` | Store at that directory for the active identity |
| `UserFiles::atPath(dir)` | Store rooted at an explicit directory |
| `root()` | The store's directory — assign it, or a subdirectory, to `EngineConfig::assetRoot` |
| `pathFor(relative)` | Where a file lives, without touching the disk |
| `write(relative, bytes)` | Atomic write/replace, creating directories on the way; `false` on failure with the prior file intact |
| `read(relative)` | `std::optional<std::vector<std::byte>>`; `nullopt` if absent |
| `exists(relative)` / `remove(relative)` | Presence check / delete (`remove` returns `bool` — `true` if a file was removed) |

A relative path that is absolute, names a drive, contains a `.` or `..` component, or names a directory
rather than a file throws `std::invalid_argument` — a file can never land outside the store.

## Try it

Three headless console programs, all against the real platform directory, all worth running twice — the
second run finding the first run's files is the point of the whole subsystem.

`examples/save_store_demo/` covers the document surface: it writes a v1 document, reads it back, then
declares v2 with a `1→2` migration and reads the same file already migrated.

`examples/user_files_demo/` covers the file surface: it writes a small tree of extracted assets, reads one
back, points `EngineConfig::assetRoot` at the result, prints both stores' resolved directories side by
side, and shows a path that tries to leave the store being refused. The files it writes are plain bytes —
open them in any editor and you get exactly what the demo wrote, which is the difference from a document.

`examples/data_assets/` picks up where that one stops: it extracts a corpus into the same directory,
points the asset root at it, registers the file as a data asset, and decodes the bytes the platform handed
back. It is the composition the two features exist for — one directory, one resolution, whatever content
a game derives from what the player supplied.

## Related pages & where to change things

- [platform-and-windowing.md](platform-and-windowing.md) — the `EngineConfig` startup
  bundle and `setActive()`, which carries the identity to the store's default.
- [assets-and-embedding.md](assets-and-embedding.md) — `EngineConfig::assetRoot`, which a game
  points at `UserFiles::root()` when its assets live in the player's own directory.
- The platform write path (temp + flush + atomic rename, per-OS) lives in `src/durable_file.cpp`,
  shared by both stores so there is one durability implementation rather than a copy each. The
  envelope layout and migration walk live in `src/save_store.cpp`, which also owns the directory
  resolution both stores root against. The public contracts are `include/retropp/save_store.h` and
  `include/retropp/user_files.h`; the identity type is `include/retropp/app_identity.h`.

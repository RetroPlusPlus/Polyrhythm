# Assets — embed vs. load-from-path

Everything the platform ingests from a file — an atlas image (`loadAtlas`), a map PNG (`loadMapPng`), a
palette image (`loadPaletteImage`), a chiptune or PCM track (`registerAudio`), a VM routine
(`registerRoutine`), arbitrary game data (`registerData`) — is delivered one of two ways, and **the
choice lives in your code, not in the build system**:

- **Embed** — the bytes are baked into the executable at build time and decoded from memory at runtime.
  The source file never ships. Use it for self-contained binaries, art you don't want altered, build-time
  design inputs (a tilemap's index PNG), and small clean-room code (a chiptune driver, a VM routine).
- **LoadFromPath** — the file ships beside the binary (or is extracted there) and is read from disk at
  runtime. Use it for moddable assets, large streamed media, and — importantly — for anything that must
  **never** be baked into a shipped binary (copyright-derived art or a routine a port populates at runtime
  from the user's own ROM).

You write the ingest call with a policy; the build does the rest automatically — it **bakes** the Embed
inputs into the binary and **copies** the LoadFromPath ones next to it. There is no build rule to write,
no copy step to add, and no path to construct.

## Contents

- [Two registration forms per family](#two-registration-forms-per-family)
- [Choosing the policy](#choosing-the-policy)
- [Data — bytes the platform never interprets](#data--bytes-the-platform-never-interprets)
- [Paths are logical; the platform resolves them](#paths-are-logical-the-platform-resolves-them)
  - [A LoadFromPath routine is assembled, never baked](#a-loadfrompath-routine-is-assembled-never-baked)
  - [Loading a file whose path you only know at runtime](#loading-a-file-whose-path-you-only-know-at-runtime)
- [What the build does, automatically](#what-the-build-does-automatically)
- [Worked example](#worked-example)
- [Status](#status)
- [Related](#related)

## Two registration forms per family

Each ingestible family has the same pair of forms. They differ only in what you hand over:

| Family | Path form (policy-governed) | Bytes form (you brought the bytes) |
|---|---|---|
| atlas image   | `Renderer::loadAtlas(path, …)`         | `Renderer::loadAtlasFromMemory(bytes, …)` |
| map PNG       | `loadMapPng(path, …)`                  | `loadMapPngFromMemory(bytes, …)` |
| palette image | `Renderer::loadPaletteImage(path, …)`  | *(none — compose `uploadPalette(slicePaletteImage(…))`)* |
| audio         | `AudioLibrary::registerAudio(path, …)` | `AudioLibrary::uploadAudio(bytes, …)` *(chiptune only)* |
| VM routine    | `Vm::registerRoutine(path, …)`         | `Vm::uploadRoutine(bytes, …)` |
| data          | `DataLibrary::registerData(path, …)`   | `DataLibrary::uploadData(bytes)` |

- The **path form** takes a compile-time literal logical path and an optional `AssetPolicy`. The build
  sees the literal, so it can bake or copy the file for you; this is the form you use for assets that are
  part of the project.
- The **bytes form** takes a ready byte span. It carries **no** policy — you already have the bytes, so
  nothing is baked or copied. It is the escape hatch for a resource whose path you only know at runtime
  (read/assemble/decode it yourself, then hand over the bytes).
- **Two families lack a symmetric bytes form.** A **palette image** has none — a palette is already
  buildable from colour data, so a runtime palette PNG composes the primitives
  (`uploadPalette(slicePaletteImage(loadPngFromMemory(bytes)))`) instead. And the **audio** bytes form
  (`uploadAudio`) is **chiptune only** — it always registers chiptune bytecode; a PCM track has no bytes
  form (register it by path, or decode + stream it yourself).

## Choosing the policy

On a path form the policy is an optional argument:

```cpp
// Embed — explicitly:
auto font = renderer.loadAtlas("game/assets/font.png", AssetDimensions::GameBoy8x8,
                               ContentKind::Tileset, ReadOrder::LeftRightThenDown,
                               /*count=*/0, TransparentIndices::None, /*framesPerAnimation=*/0,
                               AssetPolicy::Embed);

// LoadFromPath — explicitly:
auto mods = renderer.loadAtlas("game/assets/skin.png", AssetDimensions::GameBoy8x8,
                               ContentKind::Tileset, ReadOrder::LeftRightThenDown,
                               /*count=*/0, TransparentIndices::None, /*framesPerAnimation=*/0,
                               AssetPolicy::LoadFromPath);

// No policy argument — the per-type default applies:
auto map  = loadMapPng("game/assets/world.png");                 // loadMapPng     default → Embed
auto menu = renderer.loadAtlas("game/assets/menu.png",           // loadAtlas      default → LoadFromPath
                               AssetDimensions::GameBoy8x8, ContentKind::Tileset);
auto blip = AudioLibrary::instance().registerAudio("game/audio/blip.asm",   // chiptune .asm  → Embed
                                                   AudioType::Sfx, Isa::Sm83);
```

The effective policy is resolved by precedence (`resolveAssetPolicy`):

1. **The per-call argument**, if given.
2. Otherwise the **per-type default**, which follows what the input *is*:

   | Call | Input | Per-type default | Why |
   |---|---|---|---|
   | `loadAtlas`       | atlas image                            | `LoadFromPath` | atlases are the most likely copyright surface |
   | `loadMapPng`      | map PNG                                 | `Embed`        | bespoke build-time index data, not a shippable asset |
   | `loadPaletteImage`| palette image (colour PNG)              | `Embed`        | bespoke build-time colour data, like a map PNG |
   | `registerAudio`   | chiptune (`.asm`)                       | `Embed`        | a driver is a few hundred bytes — assembled to bytecode at build, only bytecode ships |
   | `registerAudio`   | PCM (`.wav` / `.ogg` / `.flac` / `.mp3`) | `LoadFromPath` | a multi-MB track streams from disk; bytes are never baked unless you ask |
   | `registerRoutine` | VM routine (`.asm`)                     | `Embed`        | assembled to bytecode at build, only bytecode ships |
   | `registerDriver`  | driver image (`DriverImagePath`)        | `Embed`        | per **image**, not per call — see below |
   | `registerData`    | arbitrary bytes (any extension)         | `LoadFromPath` | the family most likely to be derived from content a game cannot redistribute |

For audio, the kind (chiptune vs PCM) is inferred once from the file extension and frozen into the entry,
which is what selects the per-type default. The same precedence is evaluated identically at build time (to
decide what to bake vs. copy) and at runtime (to decide where to read from), so the two never disagree.
The only way to deviate from a per-type default is the explicit per-call argument — visible right at the
call site, never changed from a distance.

**Driver images carry the policy per image.** A driver registered with `registerDriver` declares its images
on a `HostedDriverBinding`, and each `DriverImagePath` names its own `.policy` — so one binding mixes them,
which is the point: a driver commonly pairs an `Embed` boot image with a `LoadFromPath` one holding content
a game may not ship inside its binary. An image that names no policy resolves to `Embed`. The build reads
each image from the binding's own initializer rather than from the `registerDriver` call, since the binding
is usually a separate variable. Extension decides the treatment, exactly as it does at `host()`: an `.asm`
image is assembled to bytecode, any other is baked as raw bytes.

An image of that binding may instead be given as inline bytes (a `DriverImage`), for content the game holds
at runtime and no build step should see. Such an image carries no path and no policy, so the scan reads
nothing from it and nothing about it reaches the binary; the images beside it resolve exactly as they would
on their own. See [audio.md](audio.md#mixing-byte-images-and-path-images-in-one-binding).

> **Write the policy as a literal `AssetPolicy::…` token at the call site — not through a variable.** The
> build scan that decides what to bake versus copy is **textual**: it reads the policy token directly out of
> the call. Pass the policy through a variable, a `constexpr` constant, a type alias, or any indirection and
> the scan can't see it, so it falls back to the **per-type default**. The runtime still honours the value
> you passed (an `Embed` whose bytes were never baked falls back to a disk read), so it doesn't crash: a
> `loadAtlas` you meant to `Embed` gets **copied** beside the binary instead of baked in. The fallback logs
> a warning naming the path, which is the only signal you get — the program runs, and the read fails only
> where the file is absent. Keep the token inline.
>
> ```cpp
> // Do — the literal token is visible to the scan, so the file is baked into the binary:
> renderer.loadAtlas("game/assets/font.png", AssetDimensions::GameBoy8x8, ContentKind::Tileset,
>                    ReadOrder::LeftRightThenDown, 0, TransparentIndices::None, 0, AssetPolicy::Embed);
>
> // Don't — the scan sees `kEmbed`, not `AssetPolicy::Embed`, so loadAtlas falls back to its LoadFromPath
> // default and the file is copied instead of embedded (the runtime still reads it, off disk):
> constexpr AssetPolicy kEmbed = AssetPolicy::Embed;
> renderer.loadAtlas("game/assets/font.png", /* … */, kEmbed);
> ```

## Data — bytes the platform never interprets

Every other family ends somewhere typed: an atlas ends up on the GPU because the platform owns pixel
interpretation, audio ends up in the audio library because the platform owns decode and streaming. A **data
asset** ends up as bytes. A text corpus, a character table, a stat block, a level script, a save-format
descriptor — the platform stores it, hands it back by id, and has no opinion about any of it.

```cpp
#include "retropp/data_library.h"

retropp::DataLibrary& library = retropp::DataLibrary::instance();

// A path — policy-governed, exactly like every other family:
const retropp::DataId table  = library.registerData("game/data/charmap.bin", AssetPolicy::Embed);
const retropp::DataId corpus = library.registerData("data/corpus.bin");   // default → LoadFromPath

// Ready bytes — no policy, nothing baked or copied:
const retropp::DataId index  = library.uploadData(builtAtRuntime);

// Resolve, then interpret them yourself:
const std::span<const std::uint8_t> bytes = library.data(corpus);
```

`DataLibrary::instance()` is the one library for the program, the shape `AudioLibrary` uses — a second one
cannot be declared, and a program that registers no data never links the catalog in at all.

**The bytes are resolved once and held for the life of the program.** `data()` reads the entry on first
call and caches it; every later call with the same `DataId` returns the same span, at the same address. A
consumer can therefore build a decoded view over the span — offsets, string tables, parsed records — and
keep it, without copying the bytes a second time. There is no eviction and no refresh.

**The per-type default is `LoadFromPath`, and it is a legal posture rather than a performance one.** Data
is the family a game is most likely to derive from content it cannot redistribute — a corpus extracted
from a player's own copy is the case this default exists for. A registration that names no policy ships
the file and reads it at runtime; the build bakes nothing for it. `Embed` on this family is only ever the
explicit `AssetPolicy::Embed` token at the call site, so nothing can end up inside a shipped binary
because a policy argument was left off.

**Reading a player's own files.** When the data was extracted onto the player's machine rather than
shipped, it lives in the per-user directory `UserFiles` manages. Point the asset root at that directory
and a logical path resolves into it:

```cpp
retropp::UserFiles files;
config.assetRoot = files.root();
retropp::EngineConfig::setActive(config);

const retropp::DataId corpus = library.registerData("corpus.bin");   // resolves under files.root()
```

See [persistence.md](persistence.md) for the extraction side.

**Errors are reported, never swallowed.** `data()` throws `std::runtime_error` naming the path when a
LoadFromPath file cannot be read, and `std::out_of_range` on a `DataId` the library never minted. An
empty span would be indistinguishable from a file that is legitimately empty, which would leave a game
decoding nothing with no way to say why.

`examples/data_assets/` registers the same corpus all three ways and decodes it — a headless console
program that prints where each one's bytes came from.

## Paths are logical; the platform resolves them

A path form's path is a **logical, project-root-relative** path (e.g. `"game/assets/world.png"`, or
`"game/audio/blip.asm"`) — and it is a *string literal* (a runtime-computed path can't be baked, so the
type rejects it at compile time). The **same string** addresses the input in both contexts:

- At build time the path is resolved against the project source root to read the file (to bake it, or to
  copy it).
- At runtime an Embed input is found by that logical key in the binary; a LoadFromPath one is resolved
  against the **asset root** and read from disk.

There is a single asset root for every family — images, audio, and routines all resolve LoadFromPath files
the same way; there is **no** separate audio or routine root. You never build a base-path string yourself.
The root is `EngineConfig::assetRoot` — by default the executable's own directory, which is exactly where
the build copies LoadFromPath files (preserving their logical path), so build and runtime agree with zero
configuration. Point it elsewhere for a game that ships its files in a subfolder or extracts them at
install time:

```cpp
EngineConfig config{ .identity  = {"MyStudio", "MyGame"},   // required first member (setActive throws if empty)
                     .assetRoot = "data" };                 // LoadFromPath files resolve under <exe>/data/...
```

`setActive` resolves `assetRoot` to an absolute path once (against the executable directory) and the
loaders use it internally — the one place the base directory is consulted.

### A LoadFromPath routine is assembled, never baked

A LoadFromPath `.asm` (audio driver or VM routine) ships beside the binary, is read at registration, and
is assembled in-process **once at startup** by the platform's own assembler (the Game Boy family → SM83, no
external toolchain). Its bytes are **never baked into the executable** — which is exactly what makes
LoadFromPath the right policy for a copyright-derived routine that must not ship inside the binary. An
Embed `.asm`, by contrast, is assembled to bytecode at *build* time, and only that bytecode ships.

### Loading a file whose path you only know at runtime

The path forms take a **literal** logical path on purpose — that is what the build can see to bake or copy.
A non-literal path (a `std::string`, a `constexpr` constant, a name from a table, a user-picked file) is a
**compile error**, not a silent miss. The literal-only restriction is the same one `assetPath()` carries,
so neither a path form nor `assetPath()` can resolve a runtime-chosen name. To ingest such a file, locate
it yourself — `assetRoot()` is the public runtime base — read its bytes, and use the family's **bytes
form** (`loadAtlasFromMemory` / `loadMapPngFromMemory` / `uploadAudio` / `uploadRoutine`). Those take bytes
rather than a path, are never baked or copied (you ship the file), and are the explicit "this input is mine
to manage" escape hatch:

```cpp
// `name` is a runtime value, so it goes through the bytes form, not a literal path form:
auto sheet = renderer.loadAtlasFromMemory(readFile(assetRoot() / name), AssetDimensions::GameBoy8x8,
                                          ContentKind::Tileset);
```

The atlas slicer's exhaustive demo (`atlas_load_demo`) reads a runtime table of filenames this way.

## What the build does, automatically

The build scans each linking target's sources for `loadAtlas` / `loadMapPng` / `loadPaletteImage` /
`registerAudio` / `registerRoutine` / `registerData` calls, resolves each input's policy by the precedence
above, and:

- **Embed** → bakes the bytes into the binary (an atlas/PNG's raw bytes, or a `.asm`'s assembled bytecode),
  decoded or run at runtime from memory.
- **LoadFromPath** → copies the file next to the binary at its logical path.

This is applied to every target that links `retropp::engine` — you do not call anything in CMake. An input
whose policy resolves to Embed but whose file isn't found at the project path falls back to a runtime disk
read rather than failing the build, and a LoadFromPath input is never baked — the safe direction for
copyright. The bytes forms are not scanned: you supplied the bytes, so there is nothing to bake or copy.

A target's baked inputs reach any binary that links it, including when the registering code sits in a
static library and the executable is only `main` — see
[build-and-consume.md](build-and-consume.md#registering-code-in-a-library). Whenever an Embed input does
fall back to a disk read, the platform logs a warning naming the path: Embed's promise is that nothing is
read at runtime, so a fallback means the build baked nothing for that path.

## Worked example

`examples/asset_embed_demo.cpp` renders one scene from three assets, one per outcome:

| Asset | Call | Policy | Delivery |
|---|---|---|---|
| map  | `loadMapPng("…map.png")`                       | Embed (default) | baked into the binary |
| font | `loadAtlas("…font.png", …, AssetPolicy::Embed)` | Embed (explicit) | baked into the binary |
| menu | `loadAtlas("…menu.png", …)`                    | LoadFromPath (default) | copied beside the binary |

After building, the demo's directory contains only the binary and the one menu PNG — the map and font live
inside the executable. Every call passes the same kind of bare logical path; the policy argument is the
only thing that differs. Audio and VM routines behave identically: a `registerAudio`/`registerRoutine`
call with the same policy argument bakes or ships the `.asm` the same way.

## Status

Every form listed here is realized: atlas images, map PNGs, palette images, chiptune and PCM audio, VM
routines, and data assets all resolve their policy and embed-or-load today. A PCM track decodes and
streams on an `AudioKind::Pcm` system (see [audio.md](audio.md)); a palette image is embedded/loaded and
sliced into a palette (see [tiles-and-colour.md](tiles-and-colour.md)); a data asset is handed back as
bytes and interpreted by the game.

## Related

- [images-and-transparency.md](images-and-transparency.md) — `loadAtlas` / `loadPng` and atlas slicing.
- [tiles-and-colour.md](tiles-and-colour.md) — `loadPaletteImage` / `uploadPalette` and the colour model.
- [tilemaps.md](tilemaps.md) — the map-PNG → `TileCatalog` → tile-layer pipeline.
- [vm-and-routines.md](vm/vm-and-routines.md) — the `registerRoutine` / `uploadRoutine` API and the VM.
- [audio.md](audio.md) — registering and cueing audio (`registerAudio`, `AudioLibrary`, `AudioSystem`).
- [persistence.md](persistence.md) — `UserFiles` and the per-user directory a data asset is extracted into.

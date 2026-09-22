# Build & consume

How to build the platform, what targets it exposes, and how a game attaches it.

## Contents

- [Requirements](#requirements)
- [Targets](#targets)
- [Build modes](#build-modes)
  - [Build options](#build-options)
- [Consuming the platform](#consuming-the-platform)
  - [No per-asset build rules](#no-per-asset-build-rules)
  - [Registering code in a library](#registering-code-in-a-library)
- [Versioning](#versioning)
- [Dependencies](#dependencies)
  - [Shader toolchain (build-time only)](#shader-toolchain-build-time-only)
- [License](#license)

## Requirements

Setting a machine up from nothing, on macOS, Windows or Linux? Follow
**[environment-setup.md](environment-setup.md)** — it installs each of these per-OS and ends with a
build you can verify. This section is the summary.

- CMake 3.28+
- A C++20 compiler: GCC 13+, Clang 16+, or MSVC 19.38+ (Visual Studio 2022 17.8+)
- Git — SDL3 and SameBoy are submodules, so clone with `--recurse-submodules`
- A shader toolchain (build-time): `glslang` on Linux; `glslang` + `spirv-cross` on macOS; the
  Windows SDK's `dxc` on Windows. See the Shader toolchain note below.

```sh
git clone --recurse-submodules <repo-url>
cd Polyrhythm
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Targets

| Target | Alias | Purpose |
|---|---|---|
| `retroppengine` | `retropp::engine` | The shipped library. Your game links this. |
| `retropp-testkit` | `retropp::testkit` | Test-tooling library. Link it only into test executables, never into a shipped game binary. |

The platform ships **as source** — there is no precompiled-binary distribution. The whole public
API is in the `retropp` namespace under `include/retropp/`.

## Build modes

The same source supports three configurations:

1. **Standalone** — configure the platform as the top-level CMake project (the commands
   above). This builds the platform library plus its own unit tests and the runnable examples
   (`hello_world`, `controller_scrolling`, `beach_demo`, `layer_transparency_demo`, …). This is the
   mode the platform is developed and CI-tested in.
2. **As a subproject** — a consuming game adds the platform with `add_subdirectory(engine)`
   and links `retropp::engine`. The platform's own tests are **off by default** in this mode, so a
   consumer's `ctest` shows only the consumer's tests. A consumer that wants the platform's test
   tooling links `retropp::testkit` into its own test target.
3. **Platform + game as one binary** — the subproject mode above, with the game's executable
   linking `retropp::engine` directly, produces a single self-contained binary per platform (an
   `.app` on macOS, etc.). There is no runtime dependency to install separately.

### Build options

| Option | Default | Effect |
|---|---|---|
| `RETROPP_BUILD_TESTS` | ON when the platform is top-level | Build the platform's own unit tests (fetches GoogleTest). |
| `RETROPP_BUILD_EXAMPLES` | ON when the platform is top-level | Build the runnable example hosts. |

Both default off when the platform is a subproject, so a consuming build pulls no test dependency and
compiles none of the platform's examples. Override either explicitly, e.g.
`cmake -S . -B build -DRETROPP_BUILD_TESTS=OFF`.

## Consuming the platform

The intended topology: each consuming game attaches this repository as a **git submodule** in its
own tree (e.g. `your-game/engine/`). The game's CMake references it with
`add_subdirectory(engine)` and links the platform target:

```cmake
add_subdirectory(engine)          # the Polyrhythm submodule
target_link_libraries(your-game PRIVATE retropp::engine)
```

Fork only if you need to carry your own changes: the submodule then points at your fork
instead of this repository, your changes stay first-class in your own history, and upstream merges
remain available. Nothing else about the topology differs.

### No per-asset build rules

A game writes no CMake to ship its assets, shaders, data files, or VM routines. The platform scans each
linking target's own sources and acts on what the code declares: an image, chiptune routine or
data file marked `Embed` is baked into the binary, one marked `LoadFromPath` is copied beside it, and
every custom shader registered by path is compiled to the platform's GPU bytecode — automatically, per
target. Adding a *new* asset, routine, or shader registration requires re-running CMake (the scan is
a configure-time read of the source). See [assets-and-embedding.md](assets-and-embedding.md) and
[rendering.md](rendering.md).

### Registering code in a library

Putting the registering code in a library and reducing the executable to `main` is a supported layout,
and the usual one when a test binary links the same code as the game:

```cmake
add_library(mygame-lib STATIC src/world.cpp src/audio.cpp)   # the register* calls live here
target_link_libraries(mygame-lib PRIVATE retropp::engine)

add_executable(mygame src/main.cpp)
target_link_libraries(mygame PRIVATE mygame-lib)
```

The scan runs on `mygame-lib`, bakes what its sources declare, and the results reach `mygame` with no
extra link settings — no whole-archive flag, no `--no-as-needed`, nothing on the consumer's side. Each
scanned target's baked symbols are named after that target, so a library and an executable that both
carry embedded assets keep their own.

Behind that guarantee: the scan emits one generated registry per kind, each a static initializer that
nothing references, and an archive member is only linked when some symbol needs it. The platform gives
each generated registry an anchor symbol and puts a matching link option on the library's interface, so
consumers pull in exactly those members. Adding a fourth registry to the build means anchoring it the
same way — `retropp_anchor_registry` in the platform's `CMakeLists.txt`.

## Versioning

`retropp::version()` (declared in `retropp/version.h`) returns the platform's semantic version as a
`std::string_view` — e.g. `"0.1.0-dev"`, where `-dev` marks an unreleased working tree; never empty.
It's an identity stamp a consumer can log or display, and carries no behavior.

## Dependencies

- **[SDL3](https://github.com/libsdl-org/SDL)** — the platform/window/GPU/audio boundary, vendored
  as a submodule at `third_party/sdl/`, built statically from source and linked into the platform.
  zlib license; pulled transitively with `--recurse-submodules`.
- **[SameBoy](https://github.com/LIJI32/SameBoy)** — vendored as a submodule at
  `third_party/sameboy/`, pinned to a tagged release. The reference Game Boy / Game Boy Color core:
  the runtime VM backend runs on it (see [vm-and-routines.md](vm-and-routines.md)).
  MIT-licensed; pulled transitively with
  `--recurse-submodules`. No `GB_*` symbol reaches a public header — consumers link it transitively
  but never see it.
- **[lodepng](https://github.com/lvandeve/lodepng)** — the PNG decoder for image ingestion (see
  [images-and-transparency.md](images-and-transparency.md)). Vendored as single-file source at
  `third_party/lodepng/` (pinned upstream commit), compiled into the platform as its own
  warning-isolated static target — no submodule, no separate build. zlib/MIT.
- **[dr_wav](https://github.com/mackron/dr_libs)** + **[stb_vorbis](https://github.com/nothings/stb)** —
  the audio-pack decoders (WAV / OGG Vorbis) for PCM audio packs (see [audio.md](audio.md)). Vendored as
  single-file source at `third_party/dr_libs/` + `third_party/stb/` (dr_wav v0.14.6, stb_vorbis v1.22),
  compiled into the platform as one warning-isolated static target (`retropp-audiodecode`) and linked
  PRIVATE — no submodule, no separate build. Public domain / MIT-0.
- **[GoogleTest](https://github.com/google/googletest)** — fetched at configure time **only**
  when the platform's own tests are built (`RETROPP_BUILD_TESTS`). Never part of a shipped game binary.

### Shader toolchain (build-time only)

Shaders are authored once in HLSL and **compiled to the running platform's native format at build
time** — `glslang` (+ `spirv-cross` and the Metal toolchain producing a precompiled `metallib` on
macOS) for SPIR-V/metallib, the Windows SDK's `dxc` for DXIL (see
[rendering.md](rendering.md) and `shaders/README.md`). These tools are dependencies of *building the
platform*, not of the shipped binary — the build embeds the compiled bytecode and the shipped game
needs nothing. A missing shader tool fails the CMake configure with an install hint
(`brew install glslang spirv-cross` + `xcodebuild -downloadComponent MetalToolchain` / `apt install glslang-tools` / the Windows SDK's `dxc`).

The shipped binary carries only the platform's own code plus the embedded shader bytecode and the
statically-linked SDL3 / lodepng / SameBoy / audio-decoder (dr_wav + stb_vorbis) objects. There is no
runtime third-party dependency to install.

## License

Polyrhythm is source-available commercial software: **PolyForm Noncommercial 1.0.0** for
noncommercial use, plus a separate **commercial license** for any commercial use. See
`LICENSING.md` and `LICENSE` at the repository root. Vendored dependencies retain their own
licenses.

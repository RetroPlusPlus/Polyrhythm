# Environment setup

Getting a machine ready to build Polyrhythm, from nothing, on macOS, Windows or Linux. Follow the
section for your OS, then the shared **Get the source** and **Build and verify** steps — they are the
same everywhere.

At the end you will have the platform built, its test suite passing, and a runnable example on screen.

Already have a toolchain and just want the requirements list? See
[build-and-consume.md](build-and-consume.md#requirements).

## Contents

- [What you need, on any OS](#what-you-need-on-any-os)
- [macOS](#macos)
- [Windows](#windows)
- [Linux](#linux)
- [Get the source](#get-the-source)
- [Build and verify](#build-and-verify)
- [Path A — work on the platform itself](#path-a--work-on-the-platform-itself)
- [Path B — build a game against the platform](#path-b--build-a-game-against-the-platform)
- [When something goes wrong](#when-something-goes-wrong)

## What you need, on any OS

| Thing | Why |
|---|---|
| **CMake 3.28+** | the build system |
| **A C++20 compiler** — GCC 13+, Clang 16+, or MSVC 19.38+ (Visual Studio 2022 17.8+) | the platform is C++20 |
| **Git** | SDL3 and SameBoy are submodules |
| **A shader toolchain** | shaders compile to the platform's native format at build time |
| **Ninja** (optional) | faster builds than the default generator |

The shader toolchain differs per platform and is the part people miss, so each section below installs
it explicitly. Everything else the build needs — SDL3, SameBoy, lodepng, the audio decoders,
GoogleTest — arrives with the submodules or is fetched by CMake. **There is nothing to install at
runtime**; a built game is self-contained.

---

## macOS

**Verified on Apple Silicon, macOS with Xcode command-line tools.**

Install [Homebrew](https://brew.sh) if you do not have it, then:

```sh
xcode-select --install                       # Apple Clang + the Metal compiler
brew install cmake ninja glslang spirv-cross
```

The Metal shader compiler ships inside Xcode. On recent Xcode versions it is a separate download; if
the build later complains it cannot find `metal`, run:

```sh
xcodebuild -downloadComponent MetalToolchain
```

Verify:

```sh
cmake --version        # 3.28 or newer
git --version
clang --version        # Apple clang
glslang --version
spirv-cross --version
xcrun -sdk macosx metal --version
```

---

## Windows

**Read this section before running anything — Windows blocks scripts by default and the failure is
confusing.**

### The PowerShell security settings that will stop you

Three separate mechanisms get in the way, and they produce different errors:

1. **Execution policy.** A stock Windows box ships `Restricted`, which refuses to run *any* `.ps1`
   file. The error mentions "running scripts is disabled on this system." Do **not** globally weaken
   it — pass the bypass per invocation, which applies to that one process only:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\setup.ps1
   ```

2. **Mark of the Web.** Windows tags files that came from the internet (including ones extracted from
   a downloaded zip). Under the `RemoteSigned` policy those tagged scripts are refused with a "not
   digitally signed" error even though local scripts run fine. Clear the tag:

   ```powershell
   Unblock-File -Path .\setup.ps1
   ```

   The `Bypass` invocation above sidesteps this too, so you only need `Unblock-File` if you are
   running under `RemoteSigned` rather than passing the flag. Check yours with `Get-ExecutionPolicy`.

3. **Elevation.** Installing the toolchain needs an **Administrator** PowerShell. Right-click
   Windows Terminal or PowerShell → *Run as administrator*. Building and running afterward do **not**
   need elevation, and should not be done elevated.

If you would rather not run a script at all, the commands below work pasted straight into an elevated
PowerShell one at a time.

### Install the toolchain

Everything comes from `winget`, which ships with Windows 10 21H2+ and Windows 11 as *App Installer*.
If `winget` is not found, install *App Installer* from the Microsoft Store, then reopen the terminal.

In an **elevated** PowerShell:

```powershell
winget install --id Git.Git --exact --silent --accept-source-agreements --accept-package-agreements
winget install --id Kitware.CMake --exact --silent --accept-source-agreements --accept-package-agreements
winget install --id Ninja-build.Ninja --exact --silent --accept-source-agreements --accept-package-agreements
```

Then Visual Studio 2022 Build Tools, with the C++ workload, the ClangCL toolset, and the Windows SDK
(the SDK is what provides `dxc.exe`, the DXIL shader compiler):

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --silent `
    --accept-source-agreements --accept-package-agreements `
    --override "--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --includeRecommended"
```

That single command takes a while and prints nothing while it works. This is normal.

**Open a new terminal afterward** so `PATH` picks up the new tools, then verify:

```powershell
cmake --version        # 3.28 or newer
git --version
where.exe dxc          # from the Windows SDK
```

If you already have the full Visual Studio 2022 (not just Build Tools), you have what you need as long
as the *Desktop development with C++* workload and a Windows SDK are installed — check via the Visual
Studio Installer rather than reinstalling.

---

## Linux

Two things catch people out here: **CMake is often too old in the distro repos**, and **SDL3 is built
from source**, so its development headers must be present even though SDL itself is not installed.

### Debian / Ubuntu

```sh
sudo apt update
sudo apt install -y build-essential git cmake ninja-build glslang-tools \
    libx11-dev libxext-dev libxrandr-dev libxi-dev libxcursor-dev libxfixes-dev \
    libxss-dev libxtst-dev \
    libwayland-dev wayland-protocols libxkbcommon-dev \
    libasound2-dev libpulse-dev libudev-dev \
    libgl1-mesa-dev libegl1-mesa-dev libgbm-dev libdrm-dev libvulkan-dev
```

**Check the CMake version before going further:**

```sh
cmake --version
```

Ubuntu 24.04 and newer ship 3.28+. **Ubuntu 22.04 ships 3.22, which is too old** — install a current
CMake from [Kitware's APT repository](https://apt.kitware.com) or with `sudo snap install cmake
--classic`, and make sure the new one is first on your `PATH`.

### Fedora

```sh
sudo dnf install -y gcc-c++ git cmake ninja-build glslang \
    libX11-devel libXext-devel libXrandr-devel libXi-devel libXcursor-devel libXfixes-devel \
    libXScrnSaver-devel libXtst-devel \
    wayland-devel wayland-protocols-devel libxkbcommon-devel \
    alsa-lib-devel pulseaudio-libs-devel systemd-devel \
    mesa-libGL-devel mesa-libEGL-devel mesa-libgbm-devel libdrm-devel vulkan-loader-devel
```

### Arch

```sh
sudo pacman -S --needed base-devel git cmake ninja glslang \
    libx11 libxext libxrandr libxi libxcursor libxfixes libxss libxtst \
    wayland wayland-protocols libxkbcommon alsa-lib libpulse \
    mesa libdrm vulkan-icd-loader
```

Verify:

```sh
cmake --version        # 3.28 or newer
git --version
c++ --version          # GCC 13+ or Clang 16+
glslangValidator --version
```

---

## Get the source

**The submodules are not optional** — SDL3 and SameBoy live in them, and a clone without them fails to
configure with a missing-directory error.

```sh
git clone --recurse-submodules https://github.com/RetroPlusPlus/Polyrhythm.git
cd Polyrhythm
```

Already cloned without them? Fix it in place:

```sh
git submodule update --init --recursive
```

## Build and verify

The same three commands on every OS:

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The first configure takes a few minutes — it fetches GoogleTest and configures SDL3. Later builds are
incremental.

**You are set up when `ctest` reports every test passing.** Then run something:

```sh
./build/examples/retropp-hello-world
```

On Windows that is `build\examples\retropp-hello-world.exe`, and if you used the Visual Studio
generator it sits in a per-configuration subdirectory (`build\examples\Release\`). Every built example
is in `build/examples/` — `ls` it to see the full list.

A window should open. That is the whole toolchain — compiler, shader compilation, SDL, the GPU
backend — working end to end.

---

## Path A — work on the platform itself

The build above *is* this path: the platform as the top-level CMake project, which builds the library,
its tests, and every runnable example. This is how the platform is developed and CI-tested.

Useful from here:

```sh
ctest --test-dir build --output-on-failure -R <NamePattern>   # run one suite
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug                  # a debug build
ls build/examples/                                            # every example that got built
```

The examples under `examples/` are the fastest way to see a surface in use — each one is a small,
complete program with heavy comments. [getting-started.md](getting-started.md) walks through one line
by line, and [README.md](README.md) indexes every subsystem page.

## Path B — build a game against the platform

A game consumes the platform as a **submodule** and links `retropp::engine`. Minimal shape:

```sh
mkdir my-game && cd my-game
git init
git submodule add https://github.com/RetroPlusPlus/Polyrhythm.git engine
git submodule update --init --recursive
```

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.28)
project(my-game LANGUAGES CXX)

add_subdirectory(engine)          # the Polyrhythm submodule

add_executable(my-game src/main.cpp)
target_link_libraries(my-game PRIVATE retropp::engine)
```

Then build it the same way:

```sh
cmake -S . -B build
cmake --build build --parallel
```

In this mode the platform's own tests are **off by default**, so your `ctest` shows only your tests. The
full consumer story — targets, build options, asset embedding, registering code in a library — is
[build-and-consume.md](build-and-consume.md). For what to actually write in `main.cpp`, start at
[getting-started.md](getting-started.md).

---

## When something goes wrong

**"Could not find … third_party/sdl" or a missing-directory error at configure time.** The submodules
are not checked out. Run `git submodule update --init --recursive`.

**CMake reports the version is too old.** You need 3.28+. On Ubuntu 22.04 the distro CMake is 3.22 —
see the Linux section. Check which one is being found with `which cmake` (`where.exe cmake` on
Windows); a newer install further down `PATH` will not be used.

**The configure fails naming a shader tool** (`glslang`, `spirv-cross`, `dxc`, `metal`). That tool is
missing from `PATH`. The error prints the install command for your platform — the shader toolchain is
required to *build*, even though a built game needs nothing. Reopen your terminal after installing, so
`PATH` refreshes.

**macOS: the build cannot find `metal`.** Run `xcodebuild -downloadComponent MetalToolchain`, then
`xcrun -sdk macosx metal --version` to confirm.

**Windows: "running scripts is disabled on this system."** Execution policy. Use
`powershell -ExecutionPolicy Bypass -File .\script.ps1` — see the Windows section.

**Windows: "not digitally signed" on a script you downloaded.** Mark of the Web. `Unblock-File -Path
.\script.ps1` first.

**Windows: `winget` is not recognised.** Install *App Installer* from the Microsoft Store, then open a
new terminal.

**Windows: `dxc` is not found after installing Build Tools.** The Windows SDK component did not
install. Re-run the Build Tools command, or add *Windows 11 SDK* through the Visual Studio Installer.

**Linux: SDL3 fails to configure with missing headers.** A development package is absent — install the
full list from your distro's section above, not just the ones whose names look relevant.

**The build succeeds but an example opens no window / crashes at startup.** That is a GPU or driver
issue rather than a build one. Make sure your graphics drivers are current; on a headless machine or
over SSH there is no display to open a window on.

**Tests fail on a fresh clone.** That is worth reporting rather than working around — a clean checkout
is expected to pass everywhere. Note your OS, compiler version, and the failing test names.

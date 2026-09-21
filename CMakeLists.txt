cmake_minimum_required(VERSION 3.28)
project(retropp-engine VERSION 0.1.0 LANGUAGES CXX C)

# ── Lean by default: optimized + stripped ───────────────────────────────────────
# A binary that ships or runs is lean by default. With no explicit build type a
# single-config generator (Ninja / Makefiles) produces an UNoptimized, UNstripped
# binary — routinely ~2x the size. Default to Release when the caller sets nothing so
# a bare `cmake -S . -B build` clone-and-build is lean without any -D flags; multi-config
# generators (Visual Studio / Xcode) choose the config per build, so leave those alone.
# A debug build stays an explicit opt-in: -DCMAKE_BUILD_TYPE=Debug.
get_property(_retropp_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _retropp_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (default Release)" FORCE)
endif()

# Strip the symbol table from release binaries — nothing at runtime reads it, and it is
# dead weight in a shipped artifact. Gated to Release so a Debug build keeps its symbols.
# -Wl,-x discards local symbols (the link-time equivalent of `strip -x`), portable across
# Apple ld64 and GNU/LLVM ld. MSVC keeps symbols in a separate PDB (the .exe carries none),
# so it needs no strip; its dead-code fold (/OPT:REF /OPT:ICF) is set below.
if(NOT MSVC)
    add_link_options($<$<CONFIG:Release>:-Wl,-x>)
endif()

# The engine's own source tree, captured for the shader helpers that reference engine-internal build
# assets (the generator script + the effect preamble) even when a CONSUMING project calls them from its
# own directory. CMake FUNCTIONS inherit the CALLER's CMAKE_CURRENT_SOURCE_DIR, so a consumer calling
# retropp_autocompile_shaders() from its own root would otherwise resolve those paths against the
# consumer's tree, not ours. A CACHE INTERNAL var survives the add_subdirectory scope boundary to the
# call site (a plain set() would not). Equals CMAKE_CURRENT_SOURCE_DIR whenever the engine is the current
# directory (every in-tree example/test caller), so those callers resolve to the identical paths.
set(RETROPP_ENGINE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "Retro++ engine source tree")

# ── Build modes ────────────────────────────────────────────────────────────────
# A single root build serves three modes; the active mode follows from whether
# the engine is the top-level project and one cache option:
#
#   1. Engine alone        — engine is top-level (PROJECT_IS_TOP_LEVEL). Builds the
#                            engine + testkit + the engine's own tests. This is what
#                            the engine's standalone CI exercises.
#   2. Consumer data-level — engine not referenced at all. Nothing here builds; a
#                            consumer's engine-free data/test build links zero engine
#                            code. This build must never break that posture.
#   3. Engine + consumer   — a consumer calls add_subdirectory(engine). The shipped
#                            retropp::engine target is exposed for linking; the consumer
#                            opts into retropp::testkit for fidelity-harness tests. The
#                            engine's own tests are OFF by default when not top-level.
#
# Consumers that want harness tests link retropp::testkit regardless of this option;
# the testkit target is always defined.
option(RETROPP_BUILD_TESTS "Build the engine's own tests" ${PROJECT_IS_TOP_LEVEL})

# Emit build/compile_commands.json for clangd and any other LSP consuming the
# database. The .clangd file at the repo root points clangd at it.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── ccache (optional build cache) ─────────────────────────────────────────────
# No-ops cleanly where ccache is absent; reuses the CI hosts' existing cache.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# ── MSVC: correct __cplusplus value ───────────────────────────────────────────
if(MSVC)
    add_compile_options(/Zc:__cplusplus)
endif()

# ── Compiler floors ────────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "13.0")
    message(FATAL_ERROR "GCC 13+ required (found ${CMAKE_CXX_COMPILER_VERSION})")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.0")
    message(FATAL_ERROR "Clang 16+ required (found ${CMAKE_CXX_COMPILER_VERSION})")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "19.38")
    message(FATAL_ERROR "MSVC 19.38+ (Visual Studio 2022 17.8+) required")
endif()

# ── Link-time dead-stripping (no unused code in any binary) ─────────────────────
# Every linked binary drops code it never references — third-party subsystems the
# program does not call (e.g. SDL's WAV/AIFF file loaders in a chiptune-only game),
# unused engine paths, and so on. Apple's linker strips at the atom level directly;
# the GNU/Clang and MSVC toolchains need per-function and per-data sections so the
# linker can drop each unused one individually. MSVC is checked first because it is
# also true for the clang-cl toolset used on Windows (which takes MSVC-style flags).
if(MSVC)
    add_compile_options(/Gy /Gw)              # function- and data-level COMDATs
    add_link_options(/OPT:REF /OPT:ICF)       # drop unreferenced; fold identical
elseif(APPLE)
    add_link_options(-Wl,-dead_strip)
    # And the same for dynamic libraries: a framework on a link line is recorded as a load command even
    # when nothing calls into it, which makes the dynamic loader map it at every start. This drops the
    # ones a binary never reaches — the OS TLS library being the one that matters, since a game that does
    # not network must not carry it. net_linkage_no_transport_symbols measures that it worked.
    add_link_options(-Wl,-dead_strip_dylibs)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections)
    # And the same for shared libraries: one named on a link line is recorded as a DT_NEEDED entry even
    # when nothing calls into it, which makes the dynamic loader open it at every start. This drops the
    # ones a binary never reaches — the OS TLS library being the one that matters, since a game that does
    # not network must not carry it. net_linkage_no_transport_symbols measures that it worked.
    add_link_options(-Wl,--as-needed)
endif()

# ── Warning flags (clean-warnings bar from day one) ────────────────────────────
function(retropp_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    # Out-of-order designated initializers are ill-formed C++20. GCC rejects them outright; Clang
    # (incl. clang-cl) accepts them as an extension and only warns — which lets code that breaks the
    # GCC CI jobs compile clean locally. Promote the warning to an error so every compiler agrees.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        if(MSVC)  # clang-cl takes native clang flags through /clang:
            target_compile_options(${target} PRIVATE /clang:-Werror=reorder-init-list)
        else()
            target_compile_options(${target} PRIVATE -Werror=reorder-init-list)
        endif()
    endif()
endfunction()

# Declared here rather than beside the example targets far below, because SDL's own configuration
# reads it: the Game Boy player example needs SDL's file dialog, and SDL is configured long before any
# example target exists. See the SDL_DIALOG line below.
option(RETROPP_BUILD_EXAMPLES "Build runnable example hosts" ${PROJECT_IS_TOP_LEVEL})

# ── SDL3 (vendored submodule) ──────────────────────────────────────────────────
# The project-locked platform boundary. Built statically from source so the engine
# links one self-contained binary; consumers inherit it transitively through
# retropp::engine. Subsystems are trimmed to what the engine uses — video + events +
# gamepad + audio (the audio sink's backend). Pinned in .gitmodules (release-3.4.10).
# Added SYSTEM so SDL's headers don't trip the engine's -Wpedantic bar.
set(SDL_SHARED       OFF CACHE BOOL "" FORCE)
set(SDL_STATIC       ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO        ON  CACHE BOOL "" FORCE)  # the audio sink's backend
set(SDL_CAMERA       OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC       OFF CACHE BOOL "" FORCE)
set(SDL_POWER        OFF CACHE BOOL "" FORCE)
# No FORCE — a consumer may opt in before add_subdirectory (native file picker, e.g. a port's
# first-start ROM selection); OFF stays the lean default for everyone who sets nothing. The examples
# are that opt-in in this tree: the Game Boy player asks for a ROM through the native picker, so
# building the examples brings the dialog in and a consumer building without them carries none of it.
if(RETROPP_BUILD_EXAMPLES)
    set(SDL_DIALOG   ON  CACHE BOOL "" FORCE)  # the player example needs it — not a preference
else()
    set(SDL_DIALOG   OFF CACHE BOOL "")
endif()
add_subdirectory(third_party/sdl EXCLUDE_FROM_ALL SYSTEM)

# ── lodepng (vendored single-file PNG decoder) ────────────────────────────────
# Image ingestion's decoder. Built as its own static library so the
# engine's strict -Wall/-Wextra/-Wpedantic bar never applies to third-party source,
# and its header is SYSTEM so including it from image.cpp doesn't trip -Wpedantic.
# Linked PRIVATE into the engine: lodepng symbols never reach a public header, and a
# consumer links it transitively through the static engine lib. Pinned in
# third_party/lodepng/ (upstream commit ed6fe58, zlib/MIT). No submodule, no own build.
add_library(retropp-lodepng STATIC third_party/lodepng/lodepng.cpp)
target_include_directories(retropp-lodepng SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/lodepng>)
target_compile_features(retropp-lodepng PUBLIC cxx_std_20)

# ── Audio-pack decoders (vendored single-header WAV + OGG Vorbis) ──────────────
# The audio-pack backend's file decoders, built as their own static library so the engine's strict
# warning bar never applies to third-party source (the lodepng precedent). dr_wav and stb_vorbis are
# decode-from-memory; src/audio/pcm_decode.cpp is the only engine TU that includes their headers. Linked
# PRIVATE into the engine — no decoder symbol reaches a public header. STB_VORBIS_NO_STDIO drops its
# file-IO path (decode is from memory only); NO_PUSHDATA_API drops the streaming API the memory decoder
# does not use. Pinned in third_party/ (dr_wav v0.14.6, stb_vorbis v1.22 — public domain / MIT-0).
add_library(retropp-audiodecode STATIC
    third_party/dr_libs/dr_wav.c
    third_party/stb/stb_vorbis.c)
target_compile_definitions(retropp-audiodecode PUBLIC
    STB_VORBIS_NO_STDIO STB_VORBIS_NO_PUSHDATA_API)
target_include_directories(retropp-audiodecode SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/dr_libs>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb>)

# ── SameBoy core (vendored submodule — the VM backend's emulation core) ───────
# SameBoy's Game Boy / Game Boy Color core. Its Core/ is GNU C, so it
# builds as its own static library — never under the engine's C++ warning bar —
# with its headers SYSTEM so they never trip -Wpedantic, and linked PRIVATE into
# the engine so no GB_* symbol reaches a public header (the retropp-lodepng
# precedent above). The internal src/vm/ backend and the VM-host public API
# sit on it; consumers never see SameBoy. Pinned at v1.0.3 (208ba4a) in
# third_party/sameboy/.
#
# MSVC cl.exe cannot compile this GNU C (statement-expressions, __attribute__);
# the two Windows CI jobs build the whole engine with the clang-cl toolset
# (-T ClangCL) so the core compiles and links MSVC-ABI-compatibly.
set(RETROPP_SB "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sameboy")

# The 15 Core files left after the GB_DISABLE_* set removes the 6 filtered ones
# (rewind, debugger, sm83_disassembler, symbol_hash, cheats, cheat_search). The
# list is explicit and auditable; the Windows compat shims below are globbed
# exactly as SameBoy's own Makefile does (never hardcode a subset — the link
# needs vasprintf from stdio.c).
set(RETROPP_SB_CORE
    apu camera display gb joypad mbc memory printer
    random rumble save_state sgb sm83_cpu timing workboy)
set(_retropp_sb_sources "")
foreach(_f IN LISTS RETROPP_SB_CORE)
    list(APPEND _retropp_sb_sources "${RETROPP_SB}/Core/${_f}.c")
endforeach()

add_library(retropp-sameboy STATIC ${_retropp_sb_sources})
set_target_properties(retropp-sameboy PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
# Core/ is PUBLIC so the internal src/vm/ backend (linking retropp-sameboy through
# the engine) resolves gb.h; SYSTEM so it never faces the engine warning bar. The
# PRIVATE link into retroppengine below keeps this usage requirement from reaching
# the engine's consumers.
target_include_directories(retropp-sameboy SYSTEM PUBLIC
    $<BUILD_INTERFACE:${RETROPP_SB}/Core>)
target_include_directories(retropp-sameboy SYSTEM PRIVATE "${RETROPP_SB}")
target_compile_definitions(retropp-sameboy PRIVATE
    GB_INTERNAL
    GB_DISABLE_TIMEKEEPING GB_DISABLE_REWIND GB_DISABLE_DEBUGGER
    GB_DISABLE_CHEATS GB_DISABLE_CHEAT_SEARCH
    GB_VERSION="1.0.3" GB_COPYRIGHT_YEAR="2025"
    _USE_MATH_DEFINES)

if(WIN32)
    # SameBoy's Windows compat shims — globbed as its Makefile does
    # (CORE_SOURCES += $(shell ls Windows/*.c)): the libc gaps clang-on-Windows
    # needs — vasprintf (stdio.c), dirent, crt, math, utf8 — plus random()->rand.
    file(GLOB _retropp_sb_win_shims "${RETROPP_SB}/Windows/*.c")
    target_sources(retropp-sameboy PRIVATE ${_retropp_sb_win_shims})
    target_include_directories(retropp-sameboy SYSTEM PRIVATE "${RETROPP_SB}/Windows")
    target_compile_definitions(retropp-sameboy PRIVATE random=rand)
else()
    target_compile_definitions(retropp-sameboy PRIVATE _GNU_SOURCE)
endif()

# Third-party C: never under the engine's warning bar.
if(MSVC)
    target_compile_options(retropp-sameboy PRIVATE /w)
else()
    target_compile_options(retropp-sameboy PRIVATE -w)
endif()

# ── Snaggletooth (vendored submodule — the SNES VM backend's core + assemblers) ──
# Snaggletooth's SNES machine and its two assemblers. MIT, C++20 stdlib only, no
# external dependencies. Added EXCLUDE_FROM_ALL so only the targets something links
# are built — a binary that names no SNES core builds none of it — and SYSTEM so its
# headers are not held to the engine's warning bar (the SameBoy and SDL precedent).
# The tool libraries are on so the 65816 and SPC700 assemblers exist as targets for
# test fixtures and examples to author cartridges from source; the test suite and the
# player stay off, the parent build running its own. The build type and the link-time
# dead-strip the engine sets reach it through the ordinary subdirectory inheritance.
set(SNAGGLETOOTH_BUILD_TOOLS  ON  CACHE BOOL "" FORCE)
set(SNAGGLETOOTH_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(SNAGGLETOOTH_BUILD_PLAYER OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/snaggletooth EXCLUDE_FROM_ALL SYSTEM)

# ── Build-time shader generation (per-platform native toolchain) ──────────────
# Every build compiles shaders/src/*.hlsl to the ONE format this platform's SDL_GPU
# backend runs, using the platform's native tools — nothing is cross-built and no
# bytecode is committed. gen_shader.py emits headers into the build tree under the
# exact include path the renderer already uses (shaders/generated/<stem>.h), with
# the same symbol surface as before; formats not built here are nullptr constants
# selectShader treats as absent.
#
#   macOS   → metallib (spirv-cross SPIR-V→MSL, xcrun metal/metallib → lib)  brew install glslang spirv-cross
#   Linux   → SPIR-V   (glslang HLSL→SPIR-V)                                  apt install glslang-tools
#   Windows → DXIL     (dxc, ships with the Windows SDK)
if(APPLE)
    set(RETROPP_SHADER_FORMAT metallib)
elseif(WIN32)
    set(RETROPP_SHADER_FORMAT dxil)
else()
    set(RETROPP_SHADER_FORMAT spirv)
endif()

set(RETROPP_SHADER_TOOL_ARGS "")
if(RETROPP_SHADER_FORMAT STREQUAL "dxil")
    # dxc lives in the Windows SDK bin dir, which is not on PATH outside a VS
    # developer prompt — search the installed SDK versions, host arch first,
    # newest SDK first. SDKs install tool dirs for FOREIGN architectures too, so
    # mere existence is not enough: every candidate is validated by running it
    # (an arm64 dxc.exe on an x64 host exists but cannot execute — observed on
    # the x64 CI runner, which selected it and died with "unknown error").
    if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        set(_retropp_dxc_arch arm64)
    else()
        set(_retropp_dxc_arch x64)
    endif()
    file(GLOB _retropp_sdk_bins
        "C:/Program Files (x86)/Windows Kits/10/bin/10.*/${_retropp_dxc_arch}")
    list(SORT _retropp_sdk_bins ORDER DESCENDING)
    function(retropp_dxc_runs out_var candidate)
        execute_process(COMMAND "${candidate}" --version
                        RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
        if(NOT _rc EQUAL 0)
            set(${out_var} FALSE PARENT_SCOPE)
        endif()
    endfunction()
    find_program(RETROPP_DXC NAMES dxc
                 HINTS ${_retropp_sdk_bins}
                 VALIDATOR retropp_dxc_runs)
    if(NOT RETROPP_DXC)
        message(FATAL_ERROR "no runnable dxc found — install the Windows SDK's "
                            "${_retropp_dxc_arch} build tools (ships dxc) or put a "
                            "native dxc on PATH")
    endif()
    list(APPEND RETROPP_SHADER_TOOL_ARGS "-DDXC=${RETROPP_DXC}")
else()
    # HINTS cover Homebrew's install prefixes for runner services whose PATH
    # doesn't include them (launchd services commonly ship a minimal PATH).
    find_program(RETROPP_GLSLANG NAMES glslang glslangValidator
                 HINTS /opt/homebrew/bin /usr/local/bin)
    if(NOT RETROPP_GLSLANG)
        message(FATAL_ERROR "glslang not found — brew install glslang / apt install glslang-tools")
    endif()
    list(APPEND RETROPP_SHADER_TOOL_ARGS "-DGLSLANG=${RETROPP_GLSLANG}")
    if(RETROPP_SHADER_FORMAT STREQUAL "metallib")
        find_program(RETROPP_SPIRV_CROSS NAMES spirv-cross
                     HINTS /opt/homebrew/bin /usr/local/bin)
        if(NOT RETROPP_SPIRV_CROSS)
            message(FATAL_ERROR "spirv-cross not found — brew install spirv-cross")
        endif()
        list(APPEND RETROPP_SHADER_TOOL_ARGS "-DSPIRV_CROSS=${RETROPP_SPIRV_CROSS}")
        # Metal toolchain (metal + metallib) — shaders compile to a precompiled metallib
        # at BUILD time, never MSL source on the player's machine. Required: a configure
        # without it is a hard error, never a silent fallback to shipping MSL source.
        # Run `metal --version`, not `xcrun -f metal`: the shim resolves even when the
        # downloadable Metal Toolchain component is absent, so only executing it proves
        # the compiler is really present.
        execute_process(COMMAND xcrun -sdk macosx metal --version
                        RESULT_VARIABLE _retropp_metal_rc OUTPUT_QUIET ERROR_QUIET)
        if(NOT _retropp_metal_rc EQUAL 0)
            message(FATAL_ERROR
                "Metal toolchain cannot run (xcrun -sdk macosx metal --version failed). Install "
                "it with: xcodebuild -downloadComponent MetalToolchain. Shaders compile at build "
                "time; there is no MSL-source fallback.")
        endif()
    endif()
endif()

# Promote the resolved shader format + tool paths to CACHE INTERNAL. Like RETROPP_ENGINE_SOURCE_DIR, these
# are consumed inside retropp_generate_shader / retropp_autocompile_shaders, which run in the CALLER's
# directory scope. The set()s above live in the engine's own scope, so a CONSUMING project calling the
# helper from its root would see them empty (→ `-DFORMAT=` → "unknown FORMAT"); the cache entries survive
# the add_subdirectory boundary to the consumer's call site. The engine's own in-tree calls run in this
# scope and use the directory-scope values directly, so they are unaffected.
set(RETROPP_SHADER_FORMAT "${RETROPP_SHADER_FORMAT}" CACHE INTERNAL "Retro++ platform shader bytecode format")
set(RETROPP_SHADER_TOOL_ARGS "${RETROPP_SHADER_TOOL_ARGS}" CACHE INTERNAL "Retro++ shader compiler tool paths")

# ── Engine-internal shared shader headers (shaders/common/*.hlsli) ────────────
# The engine's own shaders under shaders/src reach these through a real #include. Both HLSL frontends take
# an include search path — glslang -I<dir>, dxc -I <dir> — and gen_shader.cmake forwards INCLUDE to
# whichever one this platform runs. Nothing downstream needs it: spirv-cross, `metal` and `metallib`
# consume SPIR-V / MSL, by which point every #include is already resolved.
#
# Deliberately NOT shaders/include. Those three .hlsli files are GAME preambles that gen_shader.cmake
# textually PREPENDS to a custom shader; putting them on the search path would let a game #include
# declarations it has already been handed, and get a redefinition. Separate directories make that
# collision inexpressible rather than merely documented.
set(RETROPP_SHADER_INCLUDE_DIR "${RETROPP_ENGINE_SOURCE_DIR}/shaders/common"
    CACHE INTERNAL "Retro++ engine-internal shared shader header directory")
# Every shared header is a DEPENDS edge on every generated shader, so editing one retriggers its
# dependents. Deliberately coarse — a header edit rebuilds all shaders, not just the ones that include it.
# The alternative is per-shader depfiles from each frontend, which buys nothing at 29 shaders that compile
# in well under a second each, and costs a second dependency mechanism to keep correct across three
# toolchains. CONFIGURE_DEPENDS so adding a header is picked up without a manual cmake re-run.
file(GLOB RETROPP_SHADER_INCLUDES CONFIGURE_DEPENDS "${RETROPP_SHADER_INCLUDE_DIR}/*.hlsli")
set(RETROPP_SHADER_INCLUDES "${RETROPP_SHADER_INCLUDES}"
    CACHE INTERNAL "Retro++ shared shader headers — DEPENDS edges for every generated shader")

# Generate ONE HLSL shader to this platform's bytecode header (shaders/generated/<ns>.h in the build
# tree). The engine calls this for its own stems below; it is also the EXPOSED build-time hook a
# CONSUMING GAME uses for a custom shader stage — point it at the game's own
# .hlsl and it compiles through the same per-platform toolchain (no runtime compiler, no committed
# bytecode). STEM is the dotted name ("displace.frag", "ripple.frag") → namespace retropp::shaders::<ns>
# and the <ns>.h header the renderer/consumer #includes; SRC is the .hlsl path; OUT is the header path;
# HEADERS_VAR (optional) names a list variable in the caller's scope to append OUT to.
function(retropp_generate_shader)
    cmake_parse_arguments(_gs "BATCHED;GATHER;SPRITE;SPRITE_BELOW;EMISSION;EMISSION_EXTRACT;EMISSION_RECT;EMISSION_CONSUMER"
                          "STEM;SRC;OUT;HEADERS_VAR;PREAMBLE" "" ${ARGN})
    string(REGEX MATCH "[^.]+$" _stage "${_gs_STEM}")
    # Optional PREAMBLE (a custom shader's injected standard declarations): pass it through to the
    # generator and depend on it so editing it regenerates.
    set(_gs_preamble_arg "")
    set(_gs_preamble_dep "")
    if(_gs_PREAMBLE)
        set(_gs_preamble_arg "-DPREAMBLE=${_gs_PREAMBLE}")
        set(_gs_preamble_dep "${_gs_PREAMBLE}")
    endif()
    # Optional BATCHED / GATHER / SPRITE / SPRITE_BELOW, mutually exclusive: emit a second variant of a custom
    # shader with a different generated entry point (a distinct symbol <ns>_batched / <ns>_gather / <ns>_sprite
    # / <ns>_sprite_below) instead of the normal fullscreen variant. See gen_shader.cmake's BATCHED / GATHER /
    # SPRITE / SPRITE_BELOW blocks. The SPRITE variant inlines the body into the sprite fragment (sampleSource
    # reads the art); SPRITE_BELOW inlines it into the below sprite fragment (sampleSource reads the scene) —
    # each depends on its base fragment + preamble (passed through as SPRITE_BASE/SPRITE_PREAMBLE or
    # SPRITE_BELOW_BASE/SPRITE_BELOW_PREAMBLE).
    set(_gs_variant_arg "")
    set(_gs_variant_dep "")
    set(_gs_comment "Generating ${RETROPP_SHADER_FORMAT} shader header for ${_gs_STEM}")
    if(_gs_BATCHED)
        set(_gs_variant_arg "-DBATCHED=1")
        set(_gs_comment "Generating ${RETROPP_SHADER_FORMAT} BATCHED shader header for ${_gs_STEM}")
    elseif(_gs_GATHER)
        set(_gs_variant_arg "-DGATHER=1")
        set(_gs_comment "Generating ${RETROPP_SHADER_FORMAT} GATHER shader header for ${_gs_STEM}")
    elseif(_gs_SPRITE)
        set(_gs_sprite_base "${RETROPP_ENGINE_SOURCE_DIR}/shaders/src/sprite.frag.hlsl")
        set(_gs_sprite_preamble "${RETROPP_ENGINE_SOURCE_DIR}/shaders/include/retropp_sprite_effect.hlsli")
        set(_gs_variant_arg "-DSPRITE=1" "-DSPRITE_BASE=${_gs_sprite_base}"
                            "-DSPRITE_PREAMBLE=${_gs_sprite_preamble}")
        # An emission-consumer stage's Layer-scope sprite variant gets a sampleEmission() stub so its body
        # compiles (the Layer field read is wired at the Layer-scope surface).
        if(_gs_EMISSION_CONSUMER)
            list(APPEND _gs_variant_arg "-DEMISSION_CONSUMER=1")
        endif()
        set(_gs_variant_dep "${_gs_sprite_base}" "${_gs_sprite_preamble}")
        set(_gs_comment "Generating ${RETROPP_SHADER_FORMAT} SPRITE shader header for ${_gs_STEM}")
    elseif(_gs_SPRITE_BELOW)
        set(_gs_sb_base "${RETROPP_ENGINE_SOURCE_DIR}/shaders/src/sprite_below.frag.hlsl")
        set(_gs_sb_preamble "${RETROPP_ENGINE_SOURCE_DIR}/shaders/include/retropp_sprite_below_effect.hlsli")
        set(_gs_variant_arg "-DSPRITE_BELOW=1" "-DSPRITE_BELOW_BASE=${_gs_sb_base}"
                            "-DSPRITE_BELOW_PREAMBLE=${_gs_sb_preamble}")
        # An emission-consumer stage's below variant also gets sampleEmission() (reading its blurred field off
        # the emission atlas via the gate-lane index) — the injected block below its param loader.
        if(_gs_EMISSION_CONSUMER)
            list(APPEND _gs_variant_arg "-DEMISSION_CONSUMER=1")
        endif()
        set(_gs_variant_dep "${_gs_sb_base}" "${_gs_sb_preamble}")
        set(_gs_comment "Generating ${RETROPP_SHADER_FORMAT} SPRITE_BELOW shader header for ${_gs_STEM}")
    elseif(_gs_EMISSION_RECT)
        # The below WRITE variant (<ns>_emission_rect): the shader's emission() body injected into the
        # rect-instanced below extract fragment, authoring a Below-scope Custom lens's field over the field's
        # rect (sampleSource reads the SCENE). The engine cbuffer is stubbed by the rect preamble; the body's
        # own params load from the fx record lanes (the SPRITE loader).
        set(_gs_er_base "${RETROPP_ENGINE_SOURCE_DIR}/shaders/src/emission_extract_rect.frag.hlsl")
        set(_gs_er_preamble "${RETROPP_ENGINE_SOURCE_DIR}/shaders/include/retropp_emission_rect_effect.hlsli")
        set(_gs_variant_arg "-DEMISSION_RECT=1" "-DEMISSION_RECT_BASE=${_gs_er_base}"
                            "-DEMISSION_RECT_PREAMBLE=${_gs_er_preamble}")
        set(_gs_variant_dep "${_gs_er_base}" "${_gs_er_preamble}")
        set(_gs_comment "Generating ${RETROPP_SHADER_FORMAT} EMISSION_RECT shader header for ${_gs_STEM}")
    elseif(_gs_EMISSION)
        # The MAIN fullscreen variant of an emission-declared stage: same <ns> symbol, compiled with the
        # emission binding (EmissionTexture t1/s1 + sampleEmission, RowDataTexture → t2) so main() can read
        # the field. PREAMBLE is passed independently below; this only adds the compile-time define.
        set(_gs_variant_arg "-DEMISSION=1")
        set(_gs_comment "Generating ${RETROPP_SHADER_FORMAT} EMISSION shader header for ${_gs_STEM}")
    elseif(_gs_EMISSION_EXTRACT)
        # The emission EXTRACT variant (<ns>_emission): the shader's emission() body as the entry, at the
        # stock single-sampler layout (an extract cannot read emission).
        set(_gs_variant_arg "-DEMISSION_EXTRACT=1")
        set(_gs_comment "Generating ${RETROPP_SHADER_FORMAT} EMISSION_EXTRACT shader header for ${_gs_STEM}")
    endif()
    add_custom_command(
        OUTPUT "${_gs_OUT}"
        COMMAND "${CMAKE_COMMAND}"
                -DFORMAT=${RETROPP_SHADER_FORMAT} -DSTAGE=${_stage} -DSTEM=${_gs_STEM}
                "-DSRC=${_gs_SRC}"
                "-DOUT=${_gs_OUT}"
                "-DTMP=${CMAKE_CURRENT_BINARY_DIR}/generated-shaders/tmp"
                "-DINCLUDE=${RETROPP_SHADER_INCLUDE_DIR}"
                ${_gs_preamble_arg}
                ${_gs_variant_arg}
                ${RETROPP_SHADER_TOOL_ARGS}
                -P "${RETROPP_ENGINE_SOURCE_DIR}/shaders/gen_shader.cmake"
        DEPENDS "${_gs_SRC}"
                ${_gs_preamble_dep}
                ${_gs_variant_dep}
                ${RETROPP_SHADER_INCLUDES}
                "${RETROPP_ENGINE_SOURCE_DIR}/shaders/gen_shader.cmake"
        COMMENT "${_gs_comment}"
        VERBATIM)
    if(_gs_HEADERS_VAR)
        list(APPEND ${_gs_HEADERS_VAR} "${_gs_OUT}")
        set(${_gs_HEADERS_VAR} "${${_gs_HEADERS_VAR}}" PARENT_SCOPE)
    endif()
endfunction()

# Anchor a generated registry TU so a static library keeps it through the link. Every scan below
# (shaders, assets, routines) calls this on the TU it emits; a new registry added here calls it too.
#
# A registry TU's entire content is an anonymous-namespace static initializer, so it defines no symbol any
# other translation unit references. An executable links its own objects unconditionally and runs the
# initializer. A static library does not: an archive member is pulled into a link only to satisfy an
# undefined symbol, so an unanchored registry is discarded along with every registration it holds, and the
# baked bytes stay in the archive rather than reaching the program. The lookups then miss and fall back to
# a runtime disk read — which succeeds wherever the source tree is present and fails in a shipped artifact.
#
# The anchor is one extern "C" symbol defined in the TU plus an INTERFACE link option naming it undefined,
# so each consumer's link pulls in exactly that member. Whole-archive linkage would also work and is not
# used: it drags in every unrelated object of the consumer's library, against the lean-binary rule the
# registries are built around (routine_registry.h). Executables and shared libraries need no anchor and
# get none.
function(retropp_anchor_registry _target _registry _kind)
    get_target_property(_type ${_target} TYPE)
    if(NOT _type STREQUAL "STATIC_LIBRARY")
        return()
    endif()
    # A target name may carry characters a C identifier cannot (kirpich-lib); the symbol is per target AND
    # per registry kind, since one target can carry all three.
    string(MAKE_C_IDENTIFIER "${_target}" _ident)
    set(_sym "retropp_registry_anchor_${_kind}_${_ident}")
    file(APPEND "${_registry}"
        "// Linker anchor (retropp_anchor_registry). The initializer above defines no symbol anything\n"
        "// references, which is exactly what a static archive prunes. This gives the linker a reason to\n"
        "// keep this member; the matching -u / -INCLUDE link option is on the library's INTERFACE.\n"
        "extern \"C\" void ${_sym}() {}\n")
    # Mach-O decorates C symbols with a leading underscore; ELF and 64-bit COFF do not. SHELL: keeps the
    # two-token form intact through CMake's option handling.
    if(MSVC)
        target_link_options(${_target} INTERFACE "/INCLUDE:${_sym}")
    elseif(APPLE)
        target_link_options(${_target} INTERFACE "SHELL:-u _${_sym}")
    else()
        target_link_options(${_target} INTERFACE "SHELL:-u ${_sym}")
    endif()
endfunction()

# Auto-compile every custom shader a target REGISTERS BY PATH. A game writes only the
# .hlsl path in a registration call — `renderer.registerPostProcessStage("game/shaders/foo.frag.hlsl")`
# — and NO build rule: this scans the target's sources for registerPostProcessStage(...) CALLS, takes
# each call's .hlsl path, compiles it to this platform's GPU bytecode (retropp_generate_shader), and
# emits a generated registry TU whose static initializers register each path → its compiled
# ShaderVariants (retropp::detail::registerShaderVariants). The bytecode is embedded in the executable;
# the path the code passed resolves against it at runtime. Apply once per consumer target — there is no
# per-shader step. Re-run CMake after adding a NEW registration (the scan is a configure-time read).
#
# Call-keyed, exactly like the asset/routine scans (loadAtlas( / registerAudio …): the path is taken
# from a registration CALL, NOT from any bare "*.hlsl" literal in the source. A diagnostic string, a
# doc-example string, or any other stray path is therefore never compiled (and never fed to the cbuffer
# reflection, which FATAL_ERRORs on a path that doesn't resolve). Comments are stripped first (a path in
# a comment is never a registration); this is the call-keying layered on top of that.
#
# ${ARGN} = extra .hlsl paths to compile for this target that have NO registration call to discover —
# e.g. a device-free test that drives a shader's generated packer directly, with no Renderer to register
# through. Trusted, build-author-declared; not scanned.
function(retropp_autocompile_shaders _target)
    get_target_property(_srcs ${_target} SOURCES)
    set(_paths "")
    foreach(_extra ${ARGN})
        list(APPEND _paths "${_extra}")
    endforeach()
    foreach(_src ${_srcs})
        if(NOT IS_ABSOLUTE "${_src}")
            set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
        endif()
        if(EXISTS "${_src}")
            file(READ "${_src}" _text)
            # Strip comments BEFORE matching. A path written in a comment — an example, a doc note, a
            # disabled call — is NOT a real registration and must never be compiled. Block comments
            # first, then line comments.
            string(REGEX REPLACE "/\\*[^*]*\\*+([^/*][^*]*\\*+)*/" "" _text "${_text}")
            string(REGEX REPLACE "//[^\n]*" "" _text "${_text}")
            # Match each registerPostProcessStage(...) CALL up to its statement `;`, then take the .hlsl
            # path literal from inside it. `[^;(]*` between the name and `(` admits both the plain
            # `registerPostProcessStage(` and the templated `registerPostProcessStage<T>(` forms (the
            # `<T>` sits between the name and the paren). NOT keyed on bare "*.hlsl" literals.
            string(REGEX MATCHALL "registerPostProcessStage[^;(]*\\([^;]*;" _calls "${_text}")
            foreach(_call ${_calls})
                string(REGEX MATCH "\"[^\"]*\\.hlsl\"" _hit "${_call}")   # path = the call's .hlsl literal
                if(_hit)
                    string(REGEX REPLACE "\"" "" _p "${_hit}")
                    list(APPEND _paths "${_p}")
                endif()
            endforeach()
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _paths)
    if(NOT _paths)
        return()
    endif()

    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/autoshaders/${_target}")
    set(_headers "")
    set(_reg_includes "")
    set(_reg_body "")
    foreach(_p ${_paths})
        get_filename_component(_fname "${_p}" NAME)            # foo.frag.hlsl
        string(REGEX REPLACE "\\.hlsl$" "" _stem "${_fname}")   # foo.frag
        string(REPLACE "." "_" _ns "${_stem}")                 # foo_frag
        if(IS_ABSOLUTE "${_p}")
            set(_abs "${_p}")
        else()
            set(_abs "${CMAKE_CURRENT_SOURCE_DIR}/${_p}")
        endif()
        # Feed the global custom-shader set that gen_effect_fields.cmake reflects into ScreenSpaceEffect's
        # param union. One union, shared by the engine lib + every consumer → ODR-safe. The batched variant
        # (below) reflects the SAME cbuffer, so it is NOT added here (its packer is the normal one).
        set_property(GLOBAL APPEND PROPERTY RETROPP_CUSTOM_SHADERS "${_abs}")
        # Read the RAW source once, up front: the `// @retropp:` declarations are COMMENTS, and the call
        # scan above stripped comments, so they must be read from the raw text. Emission is detected here
        # rather than beside the other opt-ins because the MAIN <ns> variant is itself the emission-bound
        # one (compiled with the emission binding), so its flag must be known before that variant is emitted.
        file(READ "${_abs}" _raw)
        set(_is_emission FALSE)
        set(_emission_kw "")
        set(_emission_consumer_lit "false")
        if(_raw MATCHES "//[ \t]*@retropp:[ \t]*emission")
            set(_is_emission TRUE)
            set(_emission_kw EMISSION)
            set(_emission_consumer_lit "true")
        endif()
        set(_is_additive FALSE)
        if(_raw MATCHES "//[ \t]*@retropp:[ \t]*additive")
            set(_is_additive TRUE)
        endif()
        # @retropp:emission + @retropp:additive is a contradiction: an emission consumer wants its own
        # extracted + blurred scratch retained for its pass; an additive stage batches a source-independent
        # delta into ONE pass with no retained scratch. Different contracts — one shader cannot be both.
        if(_is_emission AND _is_additive)
            message(FATAL_ERROR
                "gen_shader: ${_p} declares BOTH // @retropp:emission and // @retropp:additive — these are "
                "mutually exclusive contracts (an emission consumer retains a blurred scratch to sample; an "
                "additive stage batches a source-independent delta into one pass). Remove one declaration.")
        endif()
        # The MAIN fullscreen variant. For an emission-declared stage `${_emission_kw}` is EMISSION, so it
        # compiles with the emission binding (sampleEmission + EmissionTexture t1/s1, RowDataTexture → t2);
        # for every other stage it is empty and the variant is byte-identical to before.
        retropp_generate_shader(
            STEM ${_stem}
            SRC  "${_abs}"
            OUT  "${_gen_dir}/shaders/generated/${_ns}.h"
            PREAMBLE "${RETROPP_ENGINE_SOURCE_DIR}/shaders/include/retropp_effect.hlsli"
            ${_emission_kw}
            HEADERS_VAR _headers)
        string(APPEND _reg_includes "#include \"shaders/generated/${_ns}.h\"\n")
        # Additive opt-in: a `// @retropp:additive` declaration earns a SECOND compiled variant, the
        # instanced-additive region batch (<ns>_batched, the batched entry point). The renderer routes
        # eligible same-shader regions through it; undeclared shaders are unchanged (batched = null).
        set(_batched_ptr "nullptr")
        if(_is_additive)
            retropp_generate_shader(
                STEM ${_stem}
                SRC  "${_abs}"
                OUT  "${_gen_dir}/shaders/generated/${_ns}_batched.h"
                PREAMBLE "${RETROPP_ENGINE_SOURCE_DIR}/shaders/include/retropp_effect.hlsli"
                BATCHED
                HEADERS_VAR _headers)
            string(APPEND _reg_includes "#include \"shaders/generated/${_ns}_batched.h\"\n")
            set(_batched_ptr "&retropp::shaders::${_ns}_batched")
        endif()
        # Gather opt-OUT: gather routing is AUTOMATIC (a custom stage can only see sampleSource + its own
        # params, so gathering N replace regions into one union-shape pass is always safe). So EVERY custom
        # shader earns a gather variant (<ns>_gather, the gather entry point) EXCEPT (a) additive-declared
        # shaders — those belong to the additive path, and gather-once would silently turn their overlapping-glow SUMS
        # into a single application — and (b) `// @retropp:no-gather` shaders that genuinely want sequential
        # lap compounding. The registry absence IS the exclusion (no variant ⇒ no pipeline ⇒ unroutable);
        # the declarations are COMMENTS, read from the RAW source (the call scan above strips comments).
        # (c) `// @retropp:emission` shaders — an emission consumer has no gather variant either: gathered
        # regions with differing `.radius`/`.threshold` cannot share one blurred scratch (the same reason
        # additive is excluded); a region-confined emission custom runs on the per-region path instead.
        set(_gather_ptr "nullptr")
        if(NOT _is_additive AND NOT _is_emission AND NOT _raw MATCHES "//[ \t]*@retropp:[ \t]*no-gather")
            retropp_generate_shader(
                STEM ${_stem}
                SRC  "${_abs}"
                OUT  "${_gen_dir}/shaders/generated/${_ns}_gather.h"
                PREAMBLE "${RETROPP_ENGINE_SOURCE_DIR}/shaders/include/retropp_effect.hlsli"
                GATHER
                HEADERS_VAR _headers)
            string(APPEND _reg_includes "#include \"shaders/generated/${_ns}_gather.h\"\n")
            set(_gather_ptr "&retropp::shaders::${_ns}_gather")
        endif()
        # Sprite opt-OUT: a custom shader also runs INLINE on a sprite (Layer-scope, one pass) via a
        # <ns>_sprite variant — the sprite fragment with this body injected + sampleSource() reading the
        # sprite's own art — EXCEPT (a) // @retropp:no-sprite shaders and (b) shaders whose cbuffer carries an
        # int / uint param (the sprite-effect store is a float texture; only float params round-trip bit-exact).
        # The registry absence IS the exclusion (no variant ⇒ no pipeline ⇒ the sprite can't carry that custom
        # effect; the shader's Layer / Below use is unaffected).
        set(_sprite_ptr "nullptr")
        set(_sprite_ok TRUE)
        if(_raw MATCHES "//[ \t]*@retropp:[ \t]*no-sprite")
            set(_sprite_ok FALSE)
        endif()
        if(_sprite_ok)
            string(REGEX REPLACE "//[^\n]*" "" _nocomment "${_raw}")
            if(_nocomment MATCHES "cbuffer[^{]*{([^}]*)}")
                set(_cbcheck "${CMAKE_MATCH_1}")
                if(_cbcheck MATCHES "(^|[ \t\r\n;])(int|uint)[ \t\r\n]")
                    set(_sprite_ok FALSE)
                endif()
            endif()
        endif()
        set(_sprite_below_ptr "nullptr")
        if(_sprite_ok)
            # An emission-consumer stage's sprite variants get sampleEmission() (EMISSION_CONSUMER): the Layer
            # variant carries a stub (its field read is wired at the Layer-scope surface), the below variant
            # reads its blurred field for real. A non-consumer's variants are byte-identical to before.
            set(_sb_emission_kw "")
            if(_is_emission)
                set(_sb_emission_kw EMISSION_CONSUMER)
            endif()
            retropp_generate_shader(
                STEM ${_stem}
                SRC  "${_abs}"
                OUT  "${_gen_dir}/shaders/generated/${_ns}_sprite.h"
                SPRITE
                ${_sb_emission_kw}
                HEADERS_VAR _headers)
            string(APPEND _reg_includes "#include \"shaders/generated/${_ns}_sprite.h\"\n")
            set(_sprite_ptr "&retropp::shaders::${_ns}_sprite")
            # Below-custom variant: the SAME body inlined into the BELOW sprite fragment (sampleSource reads the
            # SCENE, not the art) so the shader can distort / grade the scene through a sprite silhouette (a
            # Below-scope Custom effect). Same eligibility as the Layer-scope sprite variant (a shader that
            # opts out of the sprite path opts out of both scopes); the registry absence IS the "no below-custom
            # variant" flag (the below sprite skips a Custom effect it can't run). `_sb_emission_kw` (set above)
            # gives an emission-consumer below variant sampleEmission() reading its blurred field for real.
            retropp_generate_shader(
                STEM ${_stem}
                SRC  "${_abs}"
                OUT  "${_gen_dir}/shaders/generated/${_ns}_sprite_below.h"
                SPRITE_BELOW
                ${_sb_emission_kw}
                HEADERS_VAR _headers)
            string(APPEND _reg_includes "#include \"shaders/generated/${_ns}_sprite_below.h\"\n")
            set(_sprite_below_ptr "&retropp::shaders::${_ns}_sprite_below")
        endif()
        # Emission WRITE half: an emission-declared shader that ALSO defines a `float4 emission(float2 uv)`
        # body (detected on the comment-stripped source — a comment may contain the word "emission") earns
        # the <ns>_emission EXTRACT variant, its emission() body run in the extract pass to AUTHOR the field
        # content. Absent the body the demand defaults to the stock brightpass at the effect's threshold, so
        # a cheap consumer stays one declaration line (emission variant = null).
        set(_emission_extract_ptr "nullptr")
        set(_emission_rect_ptr "nullptr")
        if(_is_emission)
            string(REGEX REPLACE "//[^\n]*" "" _emcheck "${_raw}")
            if(_emcheck MATCHES "float4[ \t\r\n]+emission[ \t\r\n]*\\(")
                retropp_generate_shader(
                    STEM ${_stem}
                    SRC  "${_abs}"
                    OUT  "${_gen_dir}/shaders/generated/${_ns}_emission.h"
                    PREAMBLE "${RETROPP_ENGINE_SOURCE_DIR}/shaders/include/retropp_effect.hlsli"
                    EMISSION_EXTRACT
                    HEADERS_VAR _headers)
                string(APPEND _reg_includes "#include \"shaders/generated/${_ns}_emission.h\"\n")
                set(_emission_extract_ptr "&retropp::shaders::${_ns}_emission")
                # The below WRITE variant: the SAME emission() body over the rect-instanced below extract
                # fragment, so a Below-scope Custom lens's field is scene-authored just like the frame-class one.
                retropp_generate_shader(
                    STEM ${_stem}
                    SRC  "${_abs}"
                    OUT  "${_gen_dir}/shaders/generated/${_ns}_emission_rect.h"
                    EMISSION_RECT
                    HEADERS_VAR _headers)
                string(APPEND _reg_includes "#include \"shaders/generated/${_ns}_emission_rect.h\"\n")
                set(_emission_rect_ptr "&retropp::shaders::${_ns}_emission_rect")
            endif()
        endif()
        # Register path → (compiled variants, generated cbuffer packer, batched, gather, sprite, sprite_below,
        # emission-extract | nullptr each; then the emissionConsumer flag). The packer is pack_<ns> from the
        # shared custom_effect_packers.h (one inline fn per custom shader); every extra variant reuses it (each
        # rewrites the SAME cbuffer, so the packed bytes match).
        string(APPEND _reg_body
            "    retropp::detail::registerShaderVariants(\"${_p}\", &retropp::shaders::${_ns}, &retropp::shaders::pack_${_ns}, ${_batched_ptr}, ${_gather_ptr}, ${_sprite_ptr}, ${_sprite_below_ptr}, ${_emission_extract_ptr}, ${_emission_rect_ptr}, ${_emission_consumer_lit});\n")
    endforeach()

    # The registry TU: its anonymous-namespace static initializer registers every scanned path → its
    # compiled ShaderVariants + packer before main(). Generated at configure time; recompiled when the
    # headers (build artifacts) change.
    set(_registry "${_gen_dir}/_retropp_shader_registry.cpp")
    file(WRITE "${_registry}"
        "// AUTO-GENERATED by retropp_autocompile_shaders for target '${_target}'. DO NOT EDIT.\n"
        "#include \"retropp/shader_registry.h\"\n"
        "#include \"retropp/generated/custom_effect_packers.h\"\n"
        "${_reg_includes}"
        "namespace {\n"
        "const bool _retropp_shaders_registered = [] {\n"
        "${_reg_body}"
        "    return true;\n"
        "}();\n"
        "}  // namespace\n")
    retropp_anchor_registry(${_target} "${_registry}" shaders)

    target_sources(${_target} PRIVATE "${_registry}")
    target_include_directories(${_target} PRIVATE "${_gen_dir}")
    add_custom_target(${_target}-autoshaders DEPENDS ${_headers})
    add_dependencies(${_target} ${_target}-autoshaders)
endfunction()

# retropp_add_example(<name>): one line per in-tree example. Each new example lives in its OWN directory
# examples/<name>/ (ALL its .cpp + its own examples/<name>/assets/ for the committed art + generator) —
# self-contained, never loose under examples/. The example may be MULTI-FILE: every .cpp in the dir is
# compiled into retropp-<name>-demo (a CONFIGURE_DEPENDS glob, so adding/removing a TU re-globs on the
# next build). The asset / routine / shader scans iterate the target's full SOURCES, so loaders and
# register* calls are found in whichever TU they live in. Declares it as retropp-<name>-demo, links the
# engine, sets C++20 + warnings, applies the custom-shader autoscan, and gives it its OWN output dir
# (build/<name>_demo) so the shared retropp-demo-assets pile is NEVER copied beside it — only this
# example's own policy-driven assets ride along (placed there by the auto-finalizer's per-target copy,
# retropp_finalize_assets). The convention for NEW examples; existing flat demos keep the shared pile
# (retiring the shared pile is a separate cleanup).
function(retropp_add_example _name)
    file(GLOB _retropp_ex_srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/${_name}/*.cpp")
    add_executable(retropp-${_name}-demo ${_retropp_ex_srcs})
    target_compile_features(retropp-${_name}-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-${_name}-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-${_name}-demo)
    retropp_autocompile_shaders(retropp-${_name}-demo)
    set_target_properties(retropp-${_name}-demo PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/${_name}_demo")
endfunction()

# Asset embed/copy — driven ENTIRELY by the code. The asset POLICY in a loadAtlas() /
# loadMapPng() / loadPaletteImage() / registerData() call is the single source of truth, and the build
# acts on it automatically, per asset:
#   * Embed        → the file's bytes are baked into the binary (a constexpr array + a pre-main()
#                    registerEmbeddedAsset) and decoded from memory at runtime.
#   * LoadFromPath → the file is COPIED to the build output beside the binary (at its logical path), so
#                    the runtime disk read (assetPath()) finds it.
# The developer writes NO build rule — not to embed, not to copy. This runs automatically on every
# engine-linking target (retropp_finalize_assets, below); a target whose code has no asset calls is a
# no-op. The code says embed-or-load; the build copies-or-bakes. That is the whole feature.
#
# Policy resolution mirrors retropp::resolveAssetPolicy exactly, at build time — two tiers:
#   per-call AssetPolicy:: token in the call  >  per-type default
#   (loadMapPng = Embed, loadPaletteImage = Embed, loadAtlas = LoadFromPath,
#    registerData = LoadFromPath).
#
# Conservative by construction — the copyright hazard is baking a LoadFromPath asset, so this NEVER does:
#   * an asset is baked ONLY when its policy clearly resolves to Embed;
#   * a logical path whose file is absent at ${CMAKE_SOURCE_DIR}/<logical> is left alone (an Embed one
#     falls back to the runtime disk read; a LoadFromPath one is the consumer's to ship) — never
#     silently mis-resolved;
#   * comments are stripped before matching (a path in a comment is never touched), reusing the M.a scan
#     discipline; the matcher keys on `<loader>(` so loadAtlasFromMemory / loadMapPngFromMemory and the
#     "loadAtlas:"-style strings never match.
# Collect the driver images declared in `_text` (one source file, comments already stripped), appending
# each image's logical path to the Embed list or the Copy list by its OWN policy.
#
# A driver image is not shaped like the calls the loader scans read. `AudioLibrary::registerDriver` takes a
# DriverPathBinding, and that binding is normally its own variable declaration — so the registerDriver
# statement holds a variable name and no path at all. The paths live in the DriverImagePath initializers
# inside the binding, which is what this keys on. A binding also carries SEVERAL images, each with its own
# `.policy`, so a per-statement policy would get at least one of them wrong: a driver commonly pairs an
# Embed boot stub with a LoadFromPath image that must never be baked (copyright-derived content).
#
# An image whose `.policy` is unset resolves to Embed — `DriverImagePath::policy` is a
# std::optional<AssetPolicy> and AudioSystem resolves it against Embed at host() time, so the build and the
# runtime agree. As with every other scan the policy must be a literal `AssetPolicy::…` token to be seen.
function(retropp_scan_driver_images _text _out_embed _out_copy)
    set(_embed "${${_out_embed}}")
    set(_copy "${${_out_copy}}")
    string(REGEX MATCHALL "DriverImagePath[ \t\r\n]*\\{[^{}]*\\}" _images "${_text}")
    foreach(_image IN LISTS _images)
        string(REGEX MATCH "\\.path[ \t\r\n]*=[ \t\r\n]*\"([^\"]*)\"" _matched "${_image}")
        if(NOT _matched)
            continue()   # a path built at runtime is not scannable, exactly as elsewhere
        endif()
        set(_logical "${CMAKE_MATCH_1}")
        if(_image MATCHES "AssetPolicy::LoadFromPath")
            list(APPEND _copy "${_logical}")
        else()
            list(APPEND _embed "${_logical}")
        endif()
    endforeach()
    set(${_out_embed} "${_embed}" PARENT_SCOPE)
    set(${_out_copy} "${_copy}" PARENT_SCOPE)
endfunction()

function(retropp_autoembed_assets _target)
    get_target_property(_srcs ${_target} SOURCES)
    if(NOT _srcs)
        return()
    endif()

    # Scan every loader call → its logical path + resolved policy → the Embed set / the Copy set.
    set(_embed_paths "")
    set(_copy_paths "")
    foreach(_src ${_srcs})
        if(NOT IS_ABSOLUTE "${_src}")
            set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
        endif()
        if(NOT EXISTS "${_src}")
            continue()
        endif()
        file(READ "${_src}" _t)
        string(REGEX REPLACE "/\\*[^*]*\\*+([^/*][^*]*\\*+)*/" "" _t "${_t}")
        string(REGEX REPLACE "//[^\n]*" "" _t "${_t}")
        foreach(_loader loadMapPng loadAtlas loadPaletteImage registerAudio registerData)
            if(_loader STREQUAL "loadMapPng" OR _loader STREQUAL "loadPaletteImage")
                set(_type_default "Embed")  # bespoke build-time data (index grid / palette colours)
            else()
                # loadAtlas, registerAudio PCM and registerData all ship beside the binary: each is a
                # family whose content a game may have no right to redistribute, so omitting the policy
                # must never bake.
                set(_type_default "LoadFromPath")
            endif()
            # Each call: the loader name immediately followed by `(`, up to the statement's first `;`.
            string(REGEX MATCHALL "${_loader}\\([^;]*;" _calls "${_t}")
            foreach(_call ${_calls})
                string(REGEX MATCH "\"[^\"]*\"" _pathlit "${_call}")   # path = first string literal
                if(NOT _pathlit)
                    continue()
                endif()
                string(REGEX REPLACE "\"" "" _logical "${_pathlit}")
                # registerAudio carries BOTH chiptune (.asm — the routine scan assembles those) and PCM
                # audio-pack files (.wav/.ogg/...) — a file-bytes asset baked/copied like a PNG, here. Skip
                # its .asm calls so only the PCM containers are handled on the asset side.
                if(_loader STREQUAL "registerAudio" AND
                   NOT _logical MATCHES "\\.(wav|ogg|flac|mp3)$")
                    continue()
                endif()
                if(_call MATCHES "AssetPolicy::Embed")
                    set(_eff "Embed")
                elseif(_call MATCHES "AssetPolicy::LoadFromPath")
                    set(_eff "LoadFromPath")
                else()
                    set(_eff "${_type_default}")
                endif()
                if(_eff STREQUAL "Embed")
                    list(APPEND _embed_paths "${_logical}")
                else()
                    list(APPEND _copy_paths "${_logical}")
                endif()
            endforeach()
        endforeach()
        # Driver images (registerDriver): a non-`.asm` image is raw bytes, so it bakes and copies on this
        # side. The `.asm` ones belong to the routine scan, which assembles them first.
        set(_drv_embed "")
        set(_drv_copy "")
        retropp_scan_driver_images("${_t}" _drv_embed _drv_copy)
        foreach(_logical IN LISTS _drv_embed)
            if(NOT _logical MATCHES "\\.asm$")
                list(APPEND _embed_paths "${_logical}")
            endif()
        endforeach()
        foreach(_logical IN LISTS _drv_copy)
            if(NOT _logical MATCHES "\\.asm$")
                list(APPEND _copy_paths "${_logical}")
            endif()
        endforeach()
    endforeach()
    if(_embed_paths)
        list(REMOVE_DUPLICATES _embed_paths)
    endif()
    if(_copy_paths)
        list(REMOVE_DUPLICATES _copy_paths)
    endif()

    # ── LoadFromPath → copy the file beside the binary, preserving its logical path, so the runtime disk
    # read (assetPath(logical), rooted at the executable dir by default) finds it. Only files that exist
    # at the project-root logical path are copied; anything else is the consumer's own to ship.
    foreach(_logical ${_copy_paths})
        set(_abs "${CMAKE_SOURCE_DIR}/${_logical}")
        if(NOT EXISTS "${_abs}")
            continue()
        endif()
        get_filename_component(_dir "${_logical}" DIRECTORY)
        add_custom_command(TARGET ${_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_target}>/${_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_abs}" "$<TARGET_FILE_DIR:${_target}>/${_logical}"
            COMMENT "Copying LoadFromPath asset '${_logical}' beside ${_target}")
    endforeach()

    # ── Embed → bake the file's bytes into the binary.
    if(NOT _embed_paths)
        return()
    endif()
    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/autoassets/${_target}")
    file(MAKE_DIRECTORY "${_gen_dir}/generated")
    set(_reg_includes "")
    set(_reg_body "")
    set(_idx 0)
    # The baked arrays are namespace-scope inline constexpr variables, so their names are external and must
    # be unique across every target that reaches one link. Two targets in the same program — the documented
    # library-plus-executable layout is exactly that — otherwise both emit retropp_asset_0 and the linker
    # merges them, leaving one target's registry pointing at the other's bytes.
    string(MAKE_C_IDENTIFIER "${_target}" _ident)
    foreach(_logical ${_embed_paths})
        set(_abs "${CMAKE_SOURCE_DIR}/${_logical}")
        if(NOT EXISTS "${_abs}")
            message(STATUS "retropp_autoembed_assets(${_target}): Embed asset '${_logical}' not found at "
                           "'${_abs}' — skipped (falls back to the runtime disk read).")
            continue()
        endif()
        # Re-run configure if the asset's bytes change, so the baked copy stays current.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_abs}")
        file(READ "${_abs}" _hex HEX)
        string(LENGTH "${_hex}" _hexlen)
        math(EXPR _nbytes "${_hexlen} / 2")
        string(REGEX REPLACE "(..)" "0x\\1," _bytes "${_hex}")
        set(_sym "retropp_asset_${_ident}_${_idx}")
        file(WRITE "${_gen_dir}/generated/asset_${_idx}.h"
            "// AUTO-GENERATED by retropp_autoembed_assets for '${_logical}'. DO NOT EDIT.\n"
            "#pragma once\n#include <cstddef>\n#include <cstdint>\n"
            "namespace retropp::generated {\n"
            "inline constexpr std::uint8_t ${_sym}[] = { ${_bytes} };\n"
            "inline constexpr std::size_t  ${_sym}_size = ${_nbytes};\n"
            "}  // namespace retropp::generated\n")
        string(APPEND _reg_includes "#include \"generated/asset_${_idx}.h\"\n")
        string(APPEND _reg_body
            "    retropp::detail::registerEmbeddedAsset(\"${_logical}\", "
            "retropp::generated::${_sym}, retropp::generated::${_sym}_size);\n")
        math(EXPR _idx "${_idx} + 1")
    endforeach()
    if(_idx EQUAL 0)
        return()
    endif()

    # The registry TU: its anonymous-namespace static initializer registers every baked path → its
    # bytes before main(), exactly like the shader registry TU.
    set(_registry "${_gen_dir}/_retropp_asset_registry.cpp")
    file(WRITE "${_registry}"
        "// AUTO-GENERATED by retropp_autoembed_assets for target '${_target}'. DO NOT EDIT.\n"
        "#include \"retropp/asset_registry.h\"\n"
        "${_reg_includes}"
        "namespace {\n"
        "const bool _retropp_assets_registered = [] {\n"
        "${_reg_body}"
        "    return true;\n"
        "}();\n"
        "}  // namespace\n")
    retropp_anchor_registry(${_target} "${_registry}" assets)
    target_sources(${_target} PRIVATE "${_registry}")
    target_include_directories(${_target} PRIVATE "${_gen_dir}")
endfunction()

# The register*-call analog of retropp_autoembed_assets, for VM ROUTINES / chiptune
# audio (the `.asm` ones). Scans registerRoutine(...) / registerAudio(...) calls (the path is the call's
# first string literal), resolves the embed/load policy exactly as the asset scan does
# (per-call AssetPolicy:: token > per-type default Embed), and acts on it per routine:
#   * LoadFromPath → COPY the .asm beside the binary (preserving its logical path), so the runtime
#                    assembles it from assetRoot() — the same copy the asset scan does for a file.
#   * Embed        → assemble the .asm to BYTECODE AT COMPILE TIME (the constexpr SM83 assembler) and
#                    register the bytes in the routine registry; ONLY the bytecode ships, the .asm never
#                    does. This is the producer for retropp::detail::findEmbeddedRoutine.
# Only `.asm` paths are routines; a PCM container path (.wav/.ogg) is left to the asset side / its future
# arm. Only the SM83 ISA exists in v1 (a routine is GameBoy-family) — a future ISA adds its assembler +
# symbols to the generated registry TU here. Conservative like the asset scan: a missing file is skipped
# (Embed → runtime disk read; LoadFromPath → the consumer's to ship), comments are stripped before
# matching, and a LoadFromPath routine is NEVER baked.
function(retropp_autoembed_routines _target)
    get_target_property(_srcs ${_target} SOURCES)
    if(NOT _srcs)
        return()
    endif()

    # Scan every register* call → its logical .asm path + resolved policy → the Embed / Copy set.
    set(_embed_paths "")
    set(_copy_paths "")
    foreach(_src ${_srcs})
        if(NOT IS_ABSOLUTE "${_src}")
            set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
        endif()
        if(NOT EXISTS "${_src}")
            continue()
        endif()
        # Re-run configure (so the scan re-reads the call sites) if a scanned source changes — otherwise
        # an edited register* policy/path would not re-bake until the next manual reconfigure.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src}")
        file(READ "${_src}" _t)
        string(REGEX REPLACE "/\\*[^*]*\\*+([^/*][^*]*\\*+)*/" "" _t "${_t}")
        string(REGEX REPLACE "//[^\n]*" "" _t "${_t}")
        # registerRoutine is a template (registerRoutine<Sig>(...)), so match the call's name up to the
        # statement's `;` rather than an immediate `(`; the path is the first string literal within.
        foreach(_fn registerRoutine registerAudio)
            string(REGEX MATCHALL "${_fn}[^;]*;" _calls "${_t}")
            foreach(_call ${_calls})
                string(REGEX MATCH "\"[^\"]*\"" _pathlit "${_call}")
                if(NOT _pathlit)
                    continue()
                endif()
                string(REGEX REPLACE "\"" "" _logical "${_pathlit}")
                # Only .asm paths are routines; a PCM container is not baked/copied here.
                if(NOT _logical MATCHES "\\.asm$")
                    continue()
                endif()
                if(_call MATCHES "AssetPolicy::Embed")
                    set(_eff "Embed")
                elseif(_call MATCHES "AssetPolicy::LoadFromPath")
                    set(_eff "LoadFromPath")
                else()
                    set(_eff "Embed")   # per-type default for a routine / chiptune driver
                endif()
                if(_eff STREQUAL "Embed")
                    list(APPEND _embed_paths "${_logical}")
                else()
                    list(APPEND _copy_paths "${_logical}")
                endif()
            endforeach()
        endforeach()
        # Driver images (registerDriver): an `.asm` image assembles to bytecode, so it belongs here beside
        # the other routines. Any other extension is raw bytes and is the asset scan's.
        set(_drv_embed "")
        set(_drv_copy "")
        retropp_scan_driver_images("${_t}" _drv_embed _drv_copy)
        foreach(_logical IN LISTS _drv_embed)
            if(_logical MATCHES "\\.asm$")
                list(APPEND _embed_paths "${_logical}")
            endif()
        endforeach()
        foreach(_logical IN LISTS _drv_copy)
            if(_logical MATCHES "\\.asm$")
                list(APPEND _copy_paths "${_logical}")
            endif()
        endforeach()
    endforeach()
    if(_embed_paths)
        list(REMOVE_DUPLICATES _embed_paths)
    endif()
    if(_copy_paths)
        list(REMOVE_DUPLICATES _copy_paths)
    endif()

    # ── LoadFromPath → copy the .asm beside the binary, preserving its logical path (assetRoot()-rooted).
    foreach(_logical ${_copy_paths})
        set(_abs "${CMAKE_SOURCE_DIR}/${_logical}")
        if(NOT EXISTS "${_abs}")
            continue()
        endif()
        get_filename_component(_dir "${_logical}" DIRECTORY)
        add_custom_command(TARGET ${_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_target}>/${_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_abs}" "$<TARGET_FILE_DIR:${_target}>/${_logical}"
            COMMENT "Copying LoadFromPath routine '${_logical}' beside ${_target}")
    endforeach()

    # ── Embed → assemble the .asm at compile time (constexpr SM83 assembler) and register the bytecode.
    if(NOT _embed_paths)
        return()
    endif()
    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/autoroutines/${_target}")
    file(MAKE_DIRECTORY "${_gen_dir}/generated")
    set(_reg_includes "")
    set(_reg_body "")
    set(_idx 0)
    # Per-target symbol names, for the reason retropp_autoembed_assets states: these arrays are external,
    # and a library and an executable that both carry baked routines land in one program.
    string(MAKE_C_IDENTIFIER "${_target}" _ident)
    foreach(_logical ${_embed_paths})
        set(_abs "${CMAKE_SOURCE_DIR}/${_logical}")
        if(NOT EXISTS "${_abs}")
            message(STATUS "retropp_autoembed_routines(${_target}): Embed routine '${_logical}' not found "
                           "at '${_abs}' — skipped (falls back to the runtime disk read).")
            continue()
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_abs}")
        # The .asm SOURCE as a constexpr char[] (each byte cast, so a high-bit UTF-8 byte in a comment
        # does not narrow), exactly as the engine's own preset-source codegen does.
        file(READ "${_abs}" _hex HEX)
        string(REGEX REPLACE "(..)" "char(0x\\1)," _bytes "${_hex}")
        set(_sym "retropp_routine_${_ident}_${_idx}")
        file(WRITE "${_gen_dir}/generated/routine_${_idx}.h"
            "// AUTO-GENERATED by retropp_autoembed_routines for '${_logical}'. DO NOT EDIT.\n"
            "#pragma once\n#include <array>\n#include <cstddef>\n#include <cstdint>\n#include <string_view>\n"
            "#include \"src/vm/assembler.h\"\n#include \"src/vm/gameboy/gb_symbols.h\"\n"
            "#include \"src/vm/gameboy/sm83_assembler.h\"\n"
            "namespace retropp::generated {\n"
            "inline constexpr char ${_sym}_chars[] = { ${_bytes} char(0x00) };\n"
            "inline constexpr std::string_view ${_sym}_src{${_sym}_chars, sizeof(${_sym}_chars) - 1};\n"
            "// Assemble at compile time in the SM83 ISA; a constexpr std::vector cannot persist to\n"
            "// runtime, so its size seeds the fixed array its bytes are copied into.\n"
            "inline constexpr std::size_t ${_sym}_size =\n"
            "    retropp::vm::assembleSm83(${_sym}_src, retropp::vm::gbHardwareSymbols()).bytes.size();\n"
            "inline constexpr std::array<std::uint8_t, ${_sym}_size> ${_sym} = [] {\n"
            "    std::array<std::uint8_t, ${_sym}_size> out{};\n"
            "    const retropp::vm::AssembledRoutine r =\n"
            "        retropp::vm::assembleSm83(${_sym}_src, retropp::vm::gbHardwareSymbols());\n"
            "    for (std::size_t i = 0; i < ${_sym}_size; ++i) out[i] = r.bytes[i];\n"
            "    return out;\n"
            "}();\n"
            "}  // namespace retropp::generated\n")
        string(APPEND _reg_includes "#include \"generated/routine_${_idx}.h\"\n")
        string(APPEND _reg_body
            "    retropp::detail::registerEmbeddedRoutine(\"${_logical}\", "
            "retropp::generated::${_sym}.data(), retropp::generated::${_sym}.size());\n")
        math(EXPR _idx "${_idx} + 1")
    endforeach()
    if(_idx EQUAL 0)
        return()
    endif()

    set(_registry "${_gen_dir}/_retropp_routine_registry.cpp")
    file(WRITE "${_registry}"
        "// AUTO-GENERATED by retropp_autoembed_routines for target '${_target}'. DO NOT EDIT.\n"
        "#include \"retropp/routine_registry.h\"\n"
        "${_reg_includes}"
        "namespace {\n"
        "const bool _retropp_routines_registered = [] {\n"
        "${_reg_body}"
        "    return true;\n"
        "}();\n"
        "}  // namespace\n")
    retropp_anchor_registry(${_target} "${_registry}" routines)
    target_sources(${_target} PRIVATE "${_registry}")
    # The generated TU compiles the engine's INTERNAL constexpr assembler (src/vm/...) over the source —
    # the engine source root resolves the `src/...` includes; _gen_dir resolves the per-routine generated
    # headers. RETROPP_ENGINE_SOURCE_DIR points at the engine tree even when a consumer is the top-level
    # project (where CMAKE_SOURCE_DIR would be the consumer's root, and the src/... includes would not
    # resolve); for the engine's own examples the two are identical, so this is a no-op for them.
    target_include_directories(${_target} PRIVATE "${_gen_dir}" "${RETROPP_ENGINE_SOURCE_DIR}")
endfunction()

# Auto-apply the asset embed/copy scan to EVERY target that links the engine — so a consumer never writes
# a build rule to embed or copy an asset; the code's policy alone drives the build. Deferred to the end of
# the top-level directory so it sees every target defined after the engine (the demos, the tests, and a
# downstream game's own targets). A target with no asset calls scans to a no-op.
function(retropp_finalize_assets)
    get_property(_targets DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t ${_targets})
        if(_t STREQUAL "retroppengine")
            continue()  # the engine library defines the loaders themselves; never scan its own sources
        endif()
        get_target_property(_type ${_t} TYPE)
        if(_type STREQUAL "UTILITY" OR _type STREQUAL "INTERFACE_LIBRARY")
            continue()
        endif()
        get_target_property(_libs ${_t} LINK_LIBRARIES)
        if(_libs AND _libs MATCHES "retropp")
            retropp_autoembed_assets(${_t})
            retropp_autoembed_routines(${_t})
        endif()
    endforeach()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL retropp_finalize_assets)

set(RETROPP_SHADER_STEMS blit.vert blit.frag tile.vert tile.frag sprite.vert sprite.frag sprite_below.frag
                       sprite_emission.frag guest_frame.frag
                       postprocess.vert region_batch.vert displace.frag ripple.frag colorfill.frag
                       colorfill_gather.frag gleam.frag saturation.frag swirl.frag
                       emission_extract.frag emission_extract_rect.vert emission_extract_rect.frag
                       emission_blur.frag emission_composite.frag
                       region_select.frag
                       region_select_curve.frag region_select_curve_mask.frag region_stencil.frag
                       region_stencil_curve.frag region_stencil_curve_mask.frag blend.frag)
set(_retropp_shader_headers "")
foreach(_stem IN LISTS RETROPP_SHADER_STEMS)
    string(REPLACE "." "_" _ns "${_stem}")
    retropp_generate_shader(
        STEM ${_stem}
        SRC  "${CMAKE_CURRENT_SOURCE_DIR}/shaders/src/${_stem}.hlsl"
        OUT  "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders/shaders/generated/${_ns}.h"
        HEADERS_VAR _retropp_shader_headers)
endforeach()
add_custom_target(retropp-shaders DEPENDS ${_retropp_shader_headers})

# ── Shipped engine target ───────────────────────────────────────────────────────
# Static library consumers link. The retropp::engine alias is the surface consumers
# link against — a missing target then fails at configure time with a clear name.
# The platform layer (SdlPlatform) is compiled on every platform so its live SDL_GPU
# usage cannot silently rot; the live device is exercised by the window demo, never
# in CI (CI runs the headless suites against MockPlatform).
# ── The transport ────────────────────────────────────────────────────────────────
# Its own static library, linked PRIVATE into the engine below. An archive member reaches a link only to
# satisfy an undefined symbol, so a game that names nothing under retropp::net carries no socket — and
# that holds with no build flag for a consumer to set and no option for one to forget.
#
# Deliberately NOT anchored: retropp_anchor_registry exists to defeat archive pruning for the
# registries, and here pruning is the property. net_linkage_no_transport_symbols measures it.
#
# Exactly one socket implementation compiles. Both files also guard their own bodies, so the archive is
# well-formed whichever way a consumer's build reaches it.
if(WIN32)
    set(_retropp_net_sources src/net/socket_win32.cpp src/net/in_process.cpp)
else()
    set(_retropp_net_sources src/net/socket_posix.cpp src/net/in_process.cpp)
endif()
# TLS layers over the socket seam rather than replacing it, so its per-platform file joins the same
# archive. Every platform has a stack, and exactly one of these compiles.
if(APPLE)
    list(APPEND _retropp_net_sources src/net/tls_securetransport.cpp)
    set(RETROPP_TLS_STACK "SecureTransport")
elseif(WIN32)
    list(APPEND _retropp_net_sources src/net/tls_schannel.cpp)
    set(RETROPP_TLS_STACK "SChannel")
else()
    # The system's own OpenSSL, which every desktop Linux already carries — a declared dependency for a
    # game that opts into the transport rather than a free one. Required rather than optional on purpose:
    # a platform whose TLS quietly did not build would take its cases with it, and a measurement that can
    # be absent measures nothing.
    find_package(OpenSSL REQUIRED)
    list(APPEND _retropp_net_sources src/net/tls_openssl.cpp)
    set(RETROPP_TLS_STACK "OpenSSL ${OPENSSL_VERSION}")
endif()
add_library(retropp-net STATIC ${_retropp_net_sources})
target_compile_features(retropp-net PUBLIC cxx_std_20)
target_include_directories(retropp-net PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
retropp_set_warnings(retropp-net)
if(WIN32)
    # An import library: it contributes to a binary's import table only when a symbol is used, so a
    # consumer that links the archive and calls nothing in it pays nothing for this either.
    target_link_libraries(retropp-net PRIVATE ws2_32)
endif()
if(APPLE)
    # The OS's own TLS, dynamically linked. A framework on this archive's link line reaches every
    # consumer, so -dead_strip_dylibs up in the link-options block is what keeps an uninterested binary
    # from recording a dependency on it; net_linkage_no_transport_symbols measures that it works.
    target_link_libraries(retropp-net PRIVATE "-framework Security" "-framework CoreFoundation")
elseif(WIN32)
    # The OS's own TLS, its certificate handling, and the two key stores a server identity's key is
    # removed from — all through import libraries, so the same property the socket library has holds here
    # without a link flag to arrange it, and a binary that calls none of them imports none of them.
    target_link_libraries(retropp-net PRIVATE secur32 crypt32 ncrypt advapi32)
else()
    # The same arrangement in this platform's idiom: the libraries reach every consumer's link line, and
    # --as-needed up in the link-options block is what keeps an uninterested binary from recording
    # either one.
    target_link_libraries(retropp-net PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

add_library(retroppengine STATIC
    src/version.cpp
    src/clock.cpp
    src/input.cpp
    src/run_loop.cpp
    src/input_actions.cpp
    src/analog_response.cpp
    src/windowed_host.cpp
    src/window.cpp
    src/sdl_platform.cpp
    src/draw_state.cpp
    src/frame_timing.cpp
    src/interpolation.cpp
    src/engine_config.cpp
    src/renderer.cpp
    src/shader_registry.cpp
    src/asset_registry.cpp
    src/routine_registry.cpp
    src/image.cpp
    src/tilemap.cpp
    src/animation.cpp
    src/vibration.cpp
    src/tween.cpp
    src/curve.cpp
    src/path_walker.cpp
    src/sprite_path.cpp
    src/sprite_mask.cpp
    src/durable_file.cpp
    src/save_store.cpp
    src/user_files.cpp
    src/data_library.cpp
    src/audio/audio_system.cpp
    src/audio/audio_mixer.cpp
    src/audio/audio_library.cpp
    src/audio/audio_library_pcm.cpp
    src/audio/pcm_decode.cpp
    src/audio/gb_audio.cpp
    src/vm/gameboy/sameboy_machine.cpp
    src/vm/gameboy/sameboy_backend.cpp
    src/vm/gameboy/gb_routines.cpp
    src/vm/vm.cpp
    src/vm/vm_runner.cpp
    src/vm/save_writer.cpp)
add_library(retropp::engine ALIAS retroppengine)
target_include_directories(retroppengine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Private: the renderer #includes the generated shader headers under shaders/generated/.
# They are generated into the build tree (BEFORE keeps that copy authoritative) by the
# retropp-shaders target. Not part of the public interface — consumers never see them.
target_include_directories(retroppengine BEFORE PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated-shaders>)
target_include_directories(retroppengine PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
add_dependencies(retroppengine retropp-shaders)

# ── Custom-shader parameter reflection ────────────────────────────────────────────
# gen_effect_fields.cmake reflects every custom shader's own cbuffer into ScreenSpaceEffect's param union
# (custom_effect_fields.inc, #include'd INSIDE the struct) + one packer each (custom_effect_packers.h).
# ONE shared generated set on a PUBLIC include dir → the engine library AND every consumer compile the
# SAME ScreenSpaceEffect layout (ODR-safe; the engine builds from source in the consumer's tree). The
# files are produced at the END of this CMakeLists (after every target's custom shaders are known).
# CACHE INTERNAL so retropp_finalize_effect_fields (deferred into the top-level directory, where it runs in
# the consumer's scope) still sees these paths. Same treatment RETROPP_ENGINE_SOURCE_DIR / the shader
# format+tool-args vars get, for the same cross-scope reason.
set(RETROPP_EFFECT_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-effect" CACHE INTERNAL "Retro++ effect-reflection output dir")
set(RETROPP_EFFECT_FIELDS  "${RETROPP_EFFECT_GEN_DIR}/retropp/generated/custom_effect_fields.inc" CACHE INTERNAL "Retro++ effect fields include")
set(RETROPP_EFFECT_PACKERS "${RETROPP_EFFECT_GEN_DIR}/retropp/generated/custom_effect_packers.h" CACHE INTERNAL "Retro++ effect packers header")
file(MAKE_DIRECTORY "${RETROPP_EFFECT_GEN_DIR}/retropp/generated")
target_include_directories(retroppengine PUBLIC
    $<BUILD_INTERFACE:${RETROPP_EFFECT_GEN_DIR}>)
target_compile_features(retroppengine PUBLIC cxx_std_20)
# PUBLIC: SdlPlatform's headers expose SDL types, and consumers inherit SDL when they
# link the engine.
target_link_libraries(retroppengine PUBLIC SDL3::SDL3)
# PUBLIC: the engine owns the SDL entry point. SDL_MAIN_HANDLED stops SDL's headers from redefining
# `main`, and SdlPlatform calls SDL_SetMainReady() before SDL_Init — so a game writes a plain main()
# with no SDL_main.h include and no SDL_SetMainReady() call. Propagates to every consumer TU on the
# command line, before any SDL header is included.
target_compile_definitions(retroppengine PUBLIC SDL_MAIN_HANDLED)
# PRIVATE: lodepng backs image.cpp's decode; no public header exposes a lodepng symbol, so it
# stays an implementation detail consumers link transitively but never see.
target_link_libraries(retroppengine PRIVATE retropp-lodepng)
# PRIVATE: the audio-pack backend's WAV/OGG decoders back src/audio/pcm_decode.cpp; no public header
# exposes a decoder symbol, so consumers link them transitively but never see them.
target_link_libraries(retroppengine PRIVATE retropp-audiodecode)
# PRIVATE: the transport is an implementation detail the engine's own networking layers sit on; no
# public header exposes a retropp::net symbol. The archive rides every consumer's link line and
# contributes nothing to a binary that names none of it.
target_link_libraries(retroppengine PRIVATE retropp-net)
# PRIVATE: SameBoy is the VM backend's emulation core; the internal src/vm/ wrapper is
# the only engine code that touches it. No public header exposes a GB_* symbol, so it stays an
# implementation detail consumers link transitively but never see.
target_link_libraries(retroppengine PRIVATE retropp-sameboy)
# Bake the engine's own VM routine .asm files into constexpr char[] SOURCE headers, so the
# constexpr SM83 assembler (sm83_assembler.h) turns them into embedded bytecode AT COMPILE TIME
# (gb_routines.cpp registers from the baked bytes) with no runtime .asm read. The .asm file stays the
# source of truth; this only exposes its text to the compiler. Re-runs configure if a baked .asm
# changes. The generated header is retropp/generated/gb_routine_sources.h.
set(RETROPP_ROUTINE_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-routines")
file(MAKE_DIRECTORY "${RETROPP_ROUTINE_GEN_DIR}/retropp/generated")
set(_routine_hdr "${RETROPP_ROUTINE_GEN_DIR}/retropp/generated/gb_routine_sources.h")
# One single string (not a CMake list) so the literal ';' terminators survive the write; the body is
# written QUOTED for the same reason. Each source byte is emitted as char(0xNN) — an explicit cast, so
# a high-bit byte (e.g. a UTF-8 em-dash in an .asm comment) does not narrow into the signed char array.
set(_routine_body "// AUTO-GENERATED by CMake. DO NOT EDIT. The .asm files under src/vm/gameboy/\n// routines/ are the source of truth; this only exposes their text to the constexpr SM83 assembler.\n#pragma once\n#include <cstddef>\n#include <string_view>\n\nnamespace retropp::sameboy::routinesrc {\n")
foreach(_r div_rng=divRng tone=tone)
    string(REPLACE "=" ";" _kv "${_r}")
    list(GET _kv 0 _file)
    list(GET _kv 1 _sym)
    set(_path "${CMAKE_CURRENT_SOURCE_DIR}/src/vm/gameboy/routines/${_file}.asm")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_path}")
    file(READ "${_path}" _hex HEX)
    string(REGEX REPLACE "(..)" "char(0x\\1)," _bytes "${_hex}")
    string(APPEND _routine_body "inline constexpr char ${_sym}Chars[] = { ${_bytes} char(0x00) };\n")
    string(APPEND _routine_body "inline constexpr std::string_view ${_sym}Source{${_sym}Chars, sizeof(${_sym}Chars) - 1};\n")
endforeach()
string(APPEND _routine_body "}  // namespace retropp::sameboy::routinesrc\n")
file(WRITE "${_routine_hdr}" "${_routine_body}")
target_include_directories(retroppengine PRIVATE "${RETROPP_ROUTINE_GEN_DIR}")

# The Game Boy diagnostic-tone AUDIO preset (src/audio/gb_audio.cpp) reads its .asm from this
# directory at registration. The VM routine presets (gb_routines.cpp) embed at compile time (above)
# and don't use it. PRIVATE.
target_compile_definitions(retroppengine PRIVATE
    RETROPP_VM_GAMEBOY_ROUTINES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/vm/gameboy/routines")
retropp_set_warnings(retroppengine)

# ── Test-tooling target ──────────────────────────────────────────────────────────
# The fidelity-harness wrapper's home. Always defined (so the link surface exists
# in every mode), but NEVER linked into a shipped game binary — only into test
# executables. Currently a stub TU.
add_library(retropp-testkit STATIC testkit/src/testkit_stub.cpp)
add_library(retropp::testkit ALIAS retropp-testkit)
target_include_directories(retropp-testkit PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/testkit/include>)
target_compile_features(retropp-testkit PUBLIC cxx_std_20)
retropp_set_warnings(retropp-testkit)

# ── Engine tests (mode 1 only by default) ────────────────────────────────────────
# GoogleTest is fetched only inside this branch, so a mode-3 consumer building
# without engine tests pulls no test dependency.
if(RETROPP_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
    )
    set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    # The static-library consumer fixture for the registry link surface. A consumer that puts its
    # registering code in a library — so an executable and a test binary can link the same code — is the
    # documented shape, and it is the one shape where a generated registry TU can be pruned out of the
    # link. Building it here keeps that path exercised on every platform: registry_linkage_test.cpp links
    # this archive and asserts all three registries arrive. It links the engine so the deferred asset and
    # routine scans pick it up; the shader scan is per-target and called directly.
    add_library(retropp-registry-linkage-fixture STATIC
        tests/registry_linkage/registry_linkage_fixture.cpp)
    target_link_libraries(retropp-registry-linkage-fixture PRIVATE retropp::engine)
    # A static library cannot link its own dependencies, so even a PRIVATE one is handed to consumers to
    # link — and the test executable already names the engine itself, which puts the archive on its link
    # line twice. Clearing the propagated libraries drops the duplicate; the anchors are INTERFACE link
    # OPTIONS, a separate property, and are unaffected.
    set_target_properties(retropp-registry-linkage-fixture PROPERTIES INTERFACE_LINK_LIBRARIES "")
    target_compile_features(retropp-registry-linkage-fixture PUBLIC cxx_std_20)
    retropp_set_warnings(retropp-registry-linkage-fixture)
    retropp_autocompile_shaders(retropp-registry-linkage-fixture)

    add_executable(retroppengine-tests
        tests/smoke_test.cpp
        tests/run_loop_test.cpp
        tests/timing_test.cpp
        tests/frame_timing_test.cpp
        tests/interpolation_test.cpp
        tests/declared_cadence_test.cpp
        tests/input_test.cpp
        tests/analog_input_test.cpp
        tests/analog_response_test.cpp
        tests/cursor_visibility_test.cpp
        tests/window_chrome_test.cpp
        tests/window_position_test.cpp
        tests/action_map_test.cpp
        tests/engine_config_test.cpp
        tests/windowed_host_test.cpp
        tests/pacing_test.cpp
        tests/geometry_test.cpp
        tests/output_test.cpp
        tests/viewport_test.cpp
        tests/shader_format_test.cpp
        tests/draw_state_test.cpp
        tests/tilemap_test.cpp
        tests/tilemap_catalog_test.cpp
        tests/palette_test.cpp
        tests/tile_attributes_test.cpp
        tests/sprite_test.cpp
        tests/sprite_transform_test.cpp
        tests/sprite_anchor_test.cpp
        tests/sprite_mask_test.cpp
        tests/shape_points_contains_test.cpp
        tests/registry_linkage_test.cpp
        tests/driver_image_scan_test.cpp
        tests/user_files_test.cpp
        tests/data_library_test.cpp
        tests/frame_finish_test.cpp
        tests/postprocess_test.cpp
        tests/ripple_effect_test.cpp
        tests/colorfill_effect_test.cpp
        tests/colorfill_gather_test.cpp
        tests/gleam_effect_test.cpp
        tests/saturation_effect_test.cpp
        tests/bloom_effect_test.cpp
        tests/glow_effect_test.cpp
        tests/emission_chain_test.cpp
        tests/emission_atlas_test.cpp
        tests/sprite_region_emission_test.cpp
        tests/sprite_region_field_demand_test.cpp
        tests/sprite_emission_atlas_placement_test.cpp
        tests/sprite_below_emission_test.cpp
        tests/swirl_effect_test.cpp
        tests/blend_mode_test.cpp
        tests/blend_alpha_composite_test.cpp
        tests/sprite_concave_silhouette_test.cpp
        tests/custom_stage_test.cpp
        tests/custom_emission_test.cpp
        tests/custom_below_emission_test.cpp
        tests/custom_layer_emission_test.cpp
        tests/image_test.cpp
        tests/image_rgba_decode_test.cpp
        tests/palette_image_slice_test.cpp
        tests/transparent_indices_test.cpp
        tests/atlas_transparency_test.cpp
        tests/atlas_slice_test.cpp
        tests/animation_test.cpp
        tests/vibration_test.cpp
        tests/tween_test.cpp
        tests/curve_test.cpp
        tests/path_walker_test.cpp
        tests/sprite_path_test.cpp
        tests/vm_machine_test.cpp
        tests/vm_host_test.cpp
        tests/return_landing_test.cpp
        tests/host_rom_test.cpp
        tests/rom_run_test.cpp
        tests/tick_advance_test.cpp
        tests/guest_escape_test.cpp
        tests/sameboy_escape_test.cpp
        tests/escape_table_threads_test.cpp
        tests/guest_watch_test.cpp
        tests/sameboy_watch_test.cpp
        tests/guest_frame_content_test.cpp
        tests/guest_input_test.cpp
        tests/vm_save_data_test.cpp
        tests/snaggletooth_link_test.cpp
        tests/net_socket_test.cpp
        tests/net_tls_test.cpp
        tests/vm_frame_sink_test.cpp
        tests/guest_nesting_test.cpp
        tests/sameboy_nesting_test.cpp
        tests/region_map_test.cpp
        tests/region_access_test.cpp
        tests/gb_regions_test.cpp
        tests/cycle_budget_test.cpp
        tests/vm_runner_test.cpp
        tests/free_run_park_test.cpp
        tests/driver_hosting_vm_test.cpp
        tests/driver_registration_test.cpp
        tests/driver_mixed_images_test.cpp
        tests/driver_hosting_system_test.cpp
        tests/sm83_assembler_test.cpp
        tests/asm_pipeline_test.cpp
        tests/ring_buffer_test.cpp
        tests/audio_system_test.cpp
        tests/audio_mixer_test.cpp
        tests/audio_system_auto_close_test.cpp
        tests/audio_multivoice_test.cpp
        tests/lane_underflow_test.cpp
        tests/hosted_driver_threads_test.cpp
        tests/hosted_driver_underflow_test.cpp
        tests/audio_stats_test.cpp
        tests/audio_production_thread_test.cpp
        tests/audio_library_test.cpp
        tests/audio_waveform_diag_test.cpp
        tests/audio_pcm_pack_test.cpp
        tests/routine_embed_scan_test.cpp
        tests/transform_test.cpp
        tests/region_effect_test.cpp
        tests/region_scissor_test.cpp
        tests/region_batching_test.cpp
        tests/curve_region_test.cpp
        tests/curve_mask_test.cpp
        tests/stencil_test.cpp
        tests/golden_readback_test.cpp
        tests/render_stats_test.cpp
        tests/renderer_cache_rekey_test.cpp
        tests/tile_upload_skip_test.cpp
        tests/sprite_upload_skip_test.cpp
        tests/frame_fingerprint_test.cpp
        tests/compose_skip_test.cpp
        tests/store_upload_growth_test.cpp
        tests/sprite_instancing_test.cpp
        tests/colour_parity_layer_test.cpp
        tests/sprite_blend_test.cpp
        tests/sprite_effects_test.cpp
        tests/sprite_effects_custom_test.cpp
        tests/sprite_effects_below_test.cpp
        tests/sprite_effects_below_closeout_test.cpp
        tests/literal_path_test.cpp
        tests/asset_policy_test.cpp
        tests/asset_registry_test.cpp
        tests/row_data_table_test.cpp
        tests/save_store_test.cpp
        tests/exit_guard_test.cpp
        tests/exit_multitick_test.cpp)
    target_compile_features(retroppengine-tests PRIVATE cxx_std_20)
    # tests/ for test-only helpers (manual_clock.h); the repo root so vm_machine_test
    # can include the internal "src/vm/sameboy_machine.h" backend header (the engine's
    # first internal-only header — not part of include/retropp/).
    target_include_directories(retroppengine-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR})
    # image_test.cpp reads the committed PNG fixtures + the shipped demo asset from the source
    # tree (headless, pure CPU).
    target_compile_definitions(retroppengine-tests PRIVATE
        RETROPP_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
        RETROPP_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")
    # Which stack the TLS cases exercise here, named in the configure log so a job says what it ran
    # against rather than only that it passed.
    message(STATUS "TLS stack: ${RETROPP_TLS_STACK}")
    # Link the NAMESPACED aliases exactly as a mode-3 consumer would. This test
    # executable IS the in-tree consumer fixture for the link surface: it proves
    # both retropp::engine and retropp::testkit resolve as aliases and link cleanly.
    # The two Snaggletooth assemblers, for snaggletooth_link_test: it authors a
    # cartridge from 65816 source, runs it on a snaggletooth::Snes, and assembles a
    # line of SPC700. Both reach the core and the shared assembler through their own
    # PUBLIC links, so naming them here is the whole of what the suite needs.
    target_link_libraries(retroppengine-tests PRIVATE
        retropp::engine
        retropp::testkit
        retropp-registry-linkage-fixture
        snaggletooth_cpu65816
        snaggletooth_spc700
        GTest::gtest_main)
    # custom_stage_test drives the probe shader's generated packer DIRECTLY (device-free) — it has no
    # registerPostProcessStage(...) call for the scan to discover — so its shader is declared explicitly
    # here (the EXTRA-path arg), compiled + reflected so pack_effect_probe_frag + the .offset/.strength
    # fields exist for the packer test, independent of whether the example demos are built.
    # custom_emission_test's device-free registry round-trip reads the table for the two emission probes by
    # PATH with no registration call of its own; declared here (the EXTRA-path arg) so they are compiled +
    # registered regardless of whether the device tests run. emission_probe carries an emission() body (its
    # extract variant); emission_probe_nobody carries the declaration only (the stock-brightpass default).
    # emission_layer_probe is the Layer-scope consumer (its sprite variant reads the art-brightpass field back
    # through sampleEmission); emission_layer_probe_plain is its non-emission control (art, no halo).
    retropp_autocompile_shaders(retroppengine-tests
        tests/shaders/effect_probe.frag.hlsl
        tests/shaders/emission_probe.frag.hlsl
        tests/shaders/emission_probe_nobody.frag.hlsl
        tests/shaders/emission_below_probe.frag.hlsl
        tests/shaders/emission_below_probe_nobody.frag.hlsl
        tests/shaders/emission_layer_probe.frag.hlsl
        tests/shaders/emission_layer_probe_plain.frag.hlsl)
    # The region-batching equivalence twins (tests/shaders/additive_glow{,_plain}.frag.hlsl) need no entry
    # here — they carry registerPostProcessStage("…") calls in golden_readback_test.cpp, which the call scan
    # discovers and compiles (the DECLARED twin additionally emits its batched variant from its pragma).

    include(GoogleTest)
    gtest_discover_tests(retroppengine-tests)

    # Shader lint (device-free, every platform): no dynamically-indexed StructuredBuffer in engine HLSL.
    # SDL's D3D12 backend leaves StructureByteStride 0, which AMD uses to index a StructuredBuffer — a
    # dynamic index collapses to element 0 on AMD (only the first record reads). ByteAddressBuffer + Load4
    # is stride-independent. This gate catches the class at CI time on any machine — no AMD GPU needed.
    add_test(NAME shader_lint_no_structured_buffer
             COMMAND ${CMAKE_COMMAND}
                     -DSHADER_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/shaders
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/shader_lint.cmake)

    # ── The transport's linkage measurement (device-free, every platform) ────────────
    # The transport is its own static library, so a game that never names it links no socket. That is a
    # property of the link, and this measures it: net_linkage_control names only the platform's version
    # string, retroppengine-tests names the seam, and the transport's symbols must be in the second and
    # absent from the first. A control in the tests block rather than an example, because the examples
    # are behind an option and a control that can silently not exist measures nothing.
    add_executable(net_linkage_control tests/net_linkage_control.cpp)
    target_compile_features(net_linkage_control PRIVATE cxx_std_20)
    target_link_libraries(net_linkage_control PRIVATE retropp::engine)
    retropp_set_warnings(net_linkage_control)

    # What a binary records about the transport is read out of the image's own strings, on every
    # platform. The cost of an OS library a binary never calls is a recorded dependency — a load command,
    # a DT_NEEDED entry, an import table entry — and none of those is a symbol, so the library's name is
    # the claim and a symbol reader would pass while the cost stayed. Which names to look for differs,
    # and the probe that proves the read happened with them. Resolved at configure time — a measurement
    # this unit exists to make is never answered with a skip.
    if(MSVC)
        # The seam's own symbols are not among the needles here: a Release PE keeps no symbol table, so
        # the imported libraries are the whole of what the image says. kernel32 is in every PE, which is
        # what proves the read happened.
        set(_retropp_net_symbols "ws2_32.dll,secur32.dll")
        set(_retropp_net_probe kernel32.dll)
    elseif(APPLE)
        set(_retropp_net_symbols "connectstream,security.framework")
        set(_retropp_net_probe retropp)
    else()
        set(_retropp_net_symbols "connectstream,libssl")
        set(_retropp_net_probe retropp)
    endif()

    add_test(NAME net_linkage_no_transport_symbols
             COMMAND ${CMAKE_COMMAND}
                     -DREFERENCING=$<TARGET_FILE:retroppengine-tests>
                     -DCONTROL=$<TARGET_FILE:net_linkage_control>
                     -DSYMBOL=${_retropp_net_symbols}
                     -DPROBE=${_retropp_net_probe}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/net_linkage.cmake)

    # ── The cores' linkage measurement (device-free, every platform) ─────────────────
    # A Vm constructor resolves its platform's core where the game writes it, so a binary carries the
    # cores its own code names and no other. Three controls, built and read, never run: a pre-bound Game
    # Boy Color machine, the same machine from its platform value, and a machine for a platform with no
    # core. The needle is a literal only the Game Boy core holds; the probe is the transport guard's.
    add_executable(vm_linkage_prebound_gameboy tests/vm_linkage_prebound_gameboy.cpp)
    target_compile_features(vm_linkage_prebound_gameboy PRIVATE cxx_std_20)
    target_link_libraries(vm_linkage_prebound_gameboy PRIVATE retropp::engine)
    retropp_set_warnings(vm_linkage_prebound_gameboy)

    add_executable(vm_linkage_raw_gameboy tests/vm_linkage_raw_gameboy.cpp)
    target_compile_features(vm_linkage_raw_gameboy PRIVATE cxx_std_20)
    target_link_libraries(vm_linkage_raw_gameboy PRIVATE retropp::engine)
    retropp_set_warnings(vm_linkage_raw_gameboy)

    add_executable(vm_linkage_unbuilt_platform tests/vm_linkage_unbuilt_platform.cpp)
    target_compile_features(vm_linkage_unbuilt_platform PRIVATE cxx_std_20)
    target_link_libraries(vm_linkage_unbuilt_platform PRIVATE retropp::engine)
    retropp_set_warnings(vm_linkage_unbuilt_platform)

    set(_retropp_vm_gameboy_needle "game boy routine arena exhausted")

    add_test(NAME vm_linkage_prebound_carries_its_core
             COMMAND ${CMAKE_COMMAND}
                     -DREFERENCING=$<TARGET_FILE:vm_linkage_prebound_gameboy>
                     -DCONTROL=$<TARGET_FILE:net_linkage_control>
                     "-DNEEDLE=${_retropp_vm_gameboy_needle}"
                     -DPROBE=${_retropp_net_probe}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm_linkage.cmake)

    add_test(NAME vm_linkage_raw_form_carries_its_core
             COMMAND ${CMAKE_COMMAND}
                     -DREFERENCING=$<TARGET_FILE:vm_linkage_raw_gameboy>
                     -DCONTROL=$<TARGET_FILE:net_linkage_control>
                     "-DNEEDLE=${_retropp_vm_gameboy_needle}"
                     -DPROBE=${_retropp_net_probe}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm_linkage.cmake)

    add_test(NAME vm_linkage_unbuilt_platform_carries_no_core
             COMMAND ${CMAKE_COMMAND}
                     -DREFERENCING=$<TARGET_FILE:vm_linkage_raw_gameboy>
                     -DCONTROL=$<TARGET_FILE:vm_linkage_unbuilt_platform>
                     "-DNEEDLE=${_retropp_vm_gameboy_needle}"
                     -DPROBE=${_retropp_net_probe}
                     -DCONFIG=$<CONFIG>
                     -DFOLDED_CONTROL=ON
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm_linkage.cmake)
    set_tests_properties(vm_linkage_unbuilt_platform_carries_no_core
                         PROPERTIES SKIP_REGULAR_EXPRESSION "vm linkage SKIPPED")
endif()

# ── Runnable example hosts (mode 1 only by default) ──────────────────────────────
# The example hosts are the manual runtime checks for the live platform path (open a
# window, composite real draw state, route real input). They are built — so their use of
# the live SdlPlatform/Renderer compiles and links on every CI platform — but never run
# in CI, which has no display. Off for consumers (built only when the engine is top-level). The option
# itself is declared up beside the SDL configuration, which reads it.
if(RETROPP_BUILD_EXAMPLES)
    # Every example binary lands under build/examples/ — the flat demos directly, the
    # retropp_add_example demos in their own build/examples/<name>_demo/ subdir (the helper sets that
    # explicit per-target dir, which overrides this directory default). Scoped to this block.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
    # The beach scene: the per-layer screen-space-effect showcase — a wavy ocean that
    # waves while the sky/sand stay still, beating over a rock split across the ocean's z. Hand-built
    # tiles, no asset files.
    add_executable(retropp-beach-demo examples/beach_demo.cpp)
    target_compile_features(retropp-beach-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-beach-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-beach-demo)

    # All demos that load runtime assets share ONE copy of examples/assets next to the built binaries.
    # Independent per-demo POST_BUILD copies to the same destination race under parallel builds
    # (cmake -E copy_directory is not atomic). One
    # custom target copies exactly once; every asset-using demo depends on it (below). The destination
    # rides a built demo's output dir via $<TARGET_FILE_DIR:...>, so it stays correct on multi-config
    # generators (VS: build/<Config>/) and single-config (Ninja: build/) alike.
    add_custom_target(retropp-demo-assets
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/assets"
                "$<TARGET_FILE_DIR:retropp-beach-demo>/assets"
        COMMENT "Copying demo assets next to the demo binaries (shared, once)")

    # The transform showcase: a Mode-7-style checkerboard floor that rotates + recedes
    # under one per-pixel projective transform, with the footprint edge policy (Blank/Stretch) on the
    # rotated corners, a scale pulse, a translucent wavy haze, and a day/night frame modifier — every
    # tile-path transform feature in one scene. Hand-built tiles, no asset files.
    add_executable(retropp-transform-demo examples/transform_demo.cpp)
    target_compile_features(retropp-transform-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-transform-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-transform-demo)

    # The sprite transform capstone: a 16×16 arrow spinning about its own centre under a
    # per-sprite projective transform (scale pulse / perspective / flip toggles), plus three arrows
    # orbiting together on a rotating ride layer — proving DrawLayer::transform reaches the sprite path.
    # Hand-built sprite + background, no asset files.
    add_executable(retropp-sprite-transform-demo examples/sprite_transform_demo.cpp)
    target_compile_features(retropp-sprite-transform-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-sprite-transform-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-sprite-transform-demo)

    # The layer transparency showcase: a real indexed PNG uploaded twice (opaque + with
    # an index-hole) over two scrolling layers. Loads examples/assets/demo_tiles.png at runtime, so the
    # assets dir is copied next to the binary after each build (found via SDL_GetBasePath()).
    add_executable(retropp-layer-transparency-demo examples/layer_transparency_demo.cpp)
    target_compile_features(retropp-layer-transparency-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-layer-transparency-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-layer-transparency-demo)
    add_dependencies(retropp-layer-transparency-demo retropp-demo-assets)

    # The tilemap-import showcase: ONE map drawn from TWO image atlases mixed together — a
    # font sheet + a menu-border sheet — selected per-cell. Loads tilemap_demo_{font,menu}.png +
    # the 16-bit tilemap_demo_map.png at runtime (copied next to the binary), builds a TileCatalog whose
    # menu border is the flip-irreducible minimum (corner+h-edge+v-edge+fill), and renders "HELLO/WORLD"
    # framed by the menu — the multi-atlas headline. Static image.
    add_executable(retropp-tilemap-demo examples/tilemap_demo.cpp)
    target_compile_features(retropp-tilemap-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-tilemap-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-tilemap-demo)
    add_dependencies(retropp-tilemap-demo retropp-demo-assets)

    # The asset-embed-policy showcase: the same HELLO/WORLD-in-a-menu scene, but the map +
    # font are baked into the binary and only the menu ships beside it — and this target declares NONE of
    # that. The policy lives entirely in asset_embed_demo.cpp's loadAtlas/loadMapPng calls; the engine's
    # auto-applied scan (retropp_finalize_assets) bakes the Embed ones and copies the LoadFromPath menu.
    # No retropp_autoembed_assets() call, no copy rule — the point of the feature.
    add_executable(retropp-asset-embed-demo examples/asset_embed_demo.cpp)
    target_compile_features(retropp-asset-embed-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-asset-embed-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-asset-embed-demo)
    # Its own output dir so the shared retropp-demo-assets pile (build/assets/, for the other demos) is
    # NOT beside this binary. The point of this demo is that ONLY its LoadFromPath asset rides along; the
    # auto-copy places that one menu here, the two embedded assets live in the binary, nothing else.
    set_target_properties(retropp-asset-embed-demo PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/asset_embed_demo")

    # The atlas-load showcase: loadAtlases committed numbered indexed PNGs and walks the
    # whole ingestion matrix — every arrangement × both content kinds × all 8 read orders — drawing
    # the source grid above and the carved slots (in slot order) below. Loads the atlas_*.png assets at
    # runtime, so the assets dir is copied next to the binary (found via SDL_GetBasePath()).
    add_executable(retropp-atlas-load-demo examples/atlas_load_demo.cpp)
    target_compile_features(retropp-atlas-load-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-atlas-load-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-atlas-load-demo)
    add_dependencies(retropp-atlas-load-demo retropp-demo-assets)

    # The atlas BASICS demo: the minimal, linear teaching companion to the exhaustive
    # atlas-load demo — loadAtlas one sheet, then place each carved manifest slot as a sprite, no
    # lookup tables. Loads the same atlas_*.png assets (copied next to the binary).
    add_executable(retropp-atlas-basics-demo examples/atlas_basics_demo.cpp)
    target_compile_features(retropp-atlas-basics-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-atlas-basics-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-atlas-basics-demo)
    add_dependencies(retropp-atlas-basics-demo retropp-demo-assets)

    # The arbitrary-palette demo: live-GPU proof of the uncapped colour pipeline — a
    # 1024-colour palette + an R32_UINT atlas with indices past 255, rendered as one smooth drifting
    # rainbow. Built entirely in code (no asset). The flat-offset/index math is proven headlessly in
    # tests; this is the on-device proof CI can't run.
    add_executable(retropp-arbitrary-palette-demo examples/arbitrary_palette_demo.cpp)
    target_compile_features(retropp-arbitrary-palette-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-arbitrary-palette-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-arbitrary-palette-demo)

    # The animation showcase: four DISTINCT animations — one per playback type (looping /
    # one-off / N-loop / palette-cycle-for-a-duration) — each driven by its own button (A / B / Up /
    # Down), each press play/pause/restarting the one it owns. Loads the atlas_grid_3x2.png sheet,
    # builds an Animation per type, holds a game-owned AnimationPlayer each, advances them on the sim
    # tick, and threads current() into draw state. Assets copied next to the binary.
    add_executable(retropp-animation-demo examples/animation_demo.cpp)
    target_compile_features(retropp-animation-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-animation-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-animation-demo)
    add_dependencies(retropp-animation-demo retropp-demo-assets)

    # The value-animation showcase: two TweenPlayers drive EXISTING draw-state values over a
    # real committed PNG — a TweenPlayer<float> yoyos a layer's alpha (slow fade-to-reveal-and-back) and a
    # TweenPlayer<Vec3> ramps the frame's ColorModifier toward dusk and back. Proves the "animate any
    # draw-state value" thesis with no new effect. Loads demo_tiles.png (copied next to the binary).
    add_executable(retropp-tween-demo examples/tween_demo.cpp)
    target_compile_features(retropp-tween-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-tween-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-tween-demo)
    add_dependencies(retropp-tween-demo retropp-demo-assets)

    # Pong: a small complete game on the engine — player paddle (Up/Down) vs a tracking AI, a bouncing
    # ball with paddle english, a tile-font scoreboard + dashed net, and a gentle point-scored
    # frame dim (Tween<float> → ColorModifier). All art hand-built in code; no asset files.
    add_executable(retropp-pong-demo examples/pong_demo.cpp)
    target_compile_features(retropp-pong-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-pong-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-pong-demo)

    # Missile Command: a meatier arcade game in the GENESIS viewport (320×224) at a 60 Hz timing profile
    # (TimingProfile{TickPeriodNs::Hz60}). Enemy missiles with trails rain on 6 cities; a d-pad crosshair
    # aims a battery, A fires counter-missiles that detonate into transform-scaled circular blasts that
    # destroy (and chain) missiles. Tile backdrop + dynamic-count sprite layers; all art hand-built.
    add_executable(retropp-missile-command-demo examples/missile_command_demo.cpp)
    target_compile_features(retropp-missile-command-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-missile-command-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-missile-command-demo)

    # Breakout: a brick-breaker in the NES viewport (256×240) at a 60 Hz timing profile. Paddle + a
    # bouncing ball with paddle "english" + a grid of per-row-coloured bricks (a dynamic-count sprite
    # layer of the standing bricks); tile backdrop + HUD. All art hand-built; no asset files.
    add_executable(retropp-breakout-demo examples/breakout_demo.cpp)
    target_compile_features(retropp-breakout-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-breakout-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-breakout-demo)

    # Tempest: a vector-tube shooter in the SNES viewport (256×224) at a 60 Hz timing profile. The whole
    # tube, claw, flippers, and bolts are drawn as LINES — 1×1 solid sprites stretched + rotated into thin
    # quads by Sprite::transform (the engine has no line primitive). Tile HUD; all art hand-built.
    add_executable(retropp-tempest-demo examples/tempest_demo.cpp)
    target_compile_features(retropp-tempest-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-tempest-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-tempest-demo)

    # Centipede: a SPRITE + PALETTE showcase in the NES viewport (256×240) at 60 Hz. Hand-pixelled indexed
    # multi-colour sprites (centipede/mushrooms/blaster/spider/scorpion) coloured entirely via palette
    # SELECTION + animated CYCLING (rainbow march) + SWAP (per-wave recolour, poison). All art in code.
    add_executable(retropp-centipede-demo examples/centipede_demo.cpp)
    target_compile_features(retropp-centipede-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-centipede-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-centipede-demo)

    # Space Invaders: a SPRITE + PALETTE demo via the ASSET-LOAD route (vs Centipede's embedded bytes).
    # Loads the committed indexed PNG examples/assets/space_invaders.png with Renderer::loadAtlas, slices
    # it into 8x8 sprite slots, and colours via palette selection (per-row alien colours). SNES, 60 Hz.
    # Loads the PNG at runtime, so it depends on the shared asset-copy target.
    add_executable(retropp-space-invaders-demo examples/space_invaders_demo.cpp)
    target_compile_features(retropp-space-invaders-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-space-invaders-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-space-invaders-demo)
    add_dependencies(retropp-space-invaders-demo retropp-demo-assets)

    # Controller scrolling (docs/guide/getting-started.md): a window + a scrolling hand-built tile
    # background steered by the d-pad; also the canonical "retained" example (frame built once, only
    # scroll mutates). No asset files. Built on every CI platform so the getting-started source stays
    # guaranteed-compiling; never run in CI (no display).
    add_executable(retropp-controller-scrolling examples/controller_scrolling.cpp)
    target_compile_features(retropp-controller-scrolling PRIVATE cxx_std_20)
    target_link_libraries(retropp-controller-scrolling PRIVATE retropp::engine)
    retropp_set_warnings(retropp-controller-scrolling)

    # Hello, world! — the bare-minimum program: open a window and show "Hello, world!" (a committed
    # indexed PNG) as one centred sprite, loaded via loadAtlas (ContentKind::Single). Loads
    # examples/assets/hello_world.png at runtime, so the assets dir is copied next to the binary
    # (found via SDL_GetBasePath()).
    add_executable(retropp-hello-world examples/hello_world.cpp)
    target_compile_features(retropp-hello-world PRIVATE cxx_std_20)
    target_link_libraries(retropp-hello-world PRIVATE retropp::engine)
    retropp_set_warnings(retropp-hello-world)
    add_dependencies(retropp-hello-world retropp-demo-assets)

    # ── VM routines demo — hosting your own assembly on the VM, beside one the engine ships. A window
    # whose centre shows a decimal byte rolled ~every 2 s by a real SM83 routine, with X switching
    # between the engine's divRng preset and the program's OWN xorshift
    # (assets/xorshift_rng.asm, registered through Vm::registerRoutine with a RoutineBinding and
    # seeded by writing its high-RAM cell). The build finds the literal path and bakes the assembled
    # bytes in, so nothing is read from disk at run time. The digit atlas is hand-built in code.
    # Built on every CI platform so the VM host + tile path keep compiling against the live engine;
    # never run in CI (no display).
    retropp_add_example(vm_routines)

    # The audio tone demo: constructs an AudioSystem (which owns a hidden VM), registers the
    # built-in gentle diagnostic tone driver, and plays it through a real SDL audio sink for a few
    # seconds — the de-risk that SameBoy's APU, the hardware-speed throttle, and the SDL sink align on
    # sample rate without artifacts. Dev-run only (audio playback), never run in CI. Uses only public
    # headers (retropp/audio_system.h + retropp/sdl_platform.h) — no VM type in sight.
    add_executable(retropp-audio-tone-demo examples/audio_tone_demo.cpp)
    target_compile_features(retropp-audio-tone-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-audio-tone-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-audio-tone-demo)

    # The audio keyboard demo: a window of on-screen buttons (d-pad + A/B) that each play a
    # gentle triangle note through its OWN AudioSystem — hold several for a chord, showcasing many
    # independent audio systems mixed at once. Doubles as the EXHAUSTIVE audio embed/load proof:
    # tone_c/tone_d register Embed (baked into the binary at compile time by
    # retropp_autoembed_routines — their .asm never ships), tone_e/g/a/c2 register LoadFromPath (the
    # auto-copy places ONLY those four .asm beside the binary). Its OWN output dir keeps it off the shared
    # retropp-demo-assets pile, so the folder shows exactly the LoadFromPath set — the asset_embed_demo
    # pattern. Dev-run only; never run in CI. Public headers only — no VM type in sight.
    add_executable(retropp-audio-keyboard-demo examples/audio_keyboard_demo.cpp)
    target_compile_features(retropp-audio-keyboard-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-audio-keyboard-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-audio-keyboard-demo)
    set_target_properties(retropp-audio-keyboard-demo PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/audio_keyboard_demo")

    # The ripple demo, driving the BUILT-IN ripple: names the built-in kind (designated-init,
    # .kind = ScreenSpaceEffectKind::Ripple) rather than registering a fragment — no shader-gen, no
    # per-demo include dir. Loads the committed demo PNG.
    add_executable(retropp-ripple-demo examples/ripple_demo.cpp)
    target_compile_features(retropp-ripple-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-ripple-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-ripple-demo)
    add_dependencies(retropp-ripple-demo retropp-demo-assets)

    # ── Custom shader-stage demo: three deliberately-useless consumer shaders ──
    # The custom-shader hook is for effects too weird to ever be engine built-ins. This demo registers
    # three of them BY PATH — registerPostProcessStage<T>("examples/shaders/*.frag.hlsl") — and writes no
    # build rule: retropp_autocompile_shaders scans the source for those .hlsl paths, compiles each to this
    # platform's bytecode, and registers it. (Also the CI signal that a consumer fragment generates on
    # SPIR-V + DXIL + MSL.)
    add_executable(retropp-custom-shader-demo examples/custom_shader_demo.cpp)
    target_compile_features(retropp-custom-shader-demo PRIVATE cxx_std_20)
    target_link_libraries(retropp-custom-shader-demo PRIVATE retropp::engine)
    retropp_set_warnings(retropp-custom-shader-demo)
    retropp_autocompile_shaders(retropp-custom-shader-demo)
    add_dependencies(retropp-custom-shader-demo retropp-demo-assets)

    # ── Region-confined effects: a focused single-idea example suite + a capstone ──
    # Each focused demo teaches exactly one concept; region_showcase combines them. All use BUILT-IN
    # effects today; retropp_autocompile_shaders is applied to each anyway, so any future region demo can
    # reference a custom shader by path with zero extra build wiring (a no-op when none is referenced).
    foreach(_demo region_shapes region_transform region_motion region_vertical_wave effect_stacking
                  region_ripple region_showcase)
        add_executable(retropp-${_demo}-demo examples/${_demo}_demo.cpp)
        target_compile_features(retropp-${_demo}-demo PRIVATE cxx_std_20)
        target_link_libraries(retropp-${_demo}-demo PRIVATE retropp::engine)
        retropp_set_warnings(retropp-${_demo}-demo)
        retropp_autocompile_shaders(retropp-${_demo}-demo)
    endforeach()

    # ── Bongusoid — an example using the one-line retropp_add_example helper + its own self-contained
    # directory (examples/bongusoid/, art under examples/bongusoid/assets/).
    # An Arkanoid-style brick-breaker on a 640×480 viewport, drawing from PNG-sliced sprites
    # (examples/bongusoid/assets/bongusoid_sprites.png, LoadFromPath → rides along) with the HUD font baked
    # in (examples/bongusoid/assets/bongusoid_font.png, Embed). No add_dependencies(retropp-demo-assets):
    # the helper's own-folder convention + the auto-finalizer place ONLY the LoadFromPath sprite sheet
    # beside the binary. Dev-run only; the live SdlPlatform/Renderer/loadAtlas/analog path is the CI signal.
    retropp_add_example(bongusoid)

    # ── Input probe — a live hand-verification host for the action-based input system: one block per
    # digital action (multi-source: keys, positional + printed-letter pad buttons, a trigger past its
    # threshold, stick clicks, a mouse button), a dot on the Move vector (stick + WASD/arrows/d-pad),
    # a throttle bar on the left trigger's axis, the active-device swatch, and console edge prints.
    # In-code art only (no assets). Dev-run only; the device-free suites are the CI signal.
    retropp_add_example(input_probe)

    # ── Render-stats demo — a 1080p load bench behind a hi-fi deck. Nine controls each load a different
    # part of the renderer (sprite count, extra tile layers, Below lenses, blend-run splitting, region
    # Blooms, per-sprite rotation, a screen-space ripple, interpolation, the evaluation grid) while a
    # live readout reports presented-vs-callback frame rate, the last frame's cost split by phase, and
    # every issued/skipped counter — so a number can be tied to a cause. The deck's knobs turn by sprite
    # transform and its lamps light by palette; mouse, keyboard and gamepad all reach every control.
    # Six embedded indexed sheets + eleven palette images, all generated (assets/gen_render_stats_assets.py).
    # Dev-run only; the device-free suites are the CI signal.
    retropp_add_example(render_stats)

    # ── Game Boy player — a hosted machine's picture on screen and nothing else. Asks for a ROM through
    # the native file picker (the SDL_DIALOG opt-in at the top of this file), hosts it, advances it one
    # frame per engine tick, and submits its completed frames as a layer's content at 6×. It ships no
    # ROM: the image is the developer's own, read at runtime. Dev-run only — it needs a display and a
    # file, so CI compiles it and never runs it.
    retropp_add_example(gb_player)

    # ── Curve demo — plots a Catmull-Rom curve through waypoints, a constant-speed atDistance walker, and
    # a Hermite curve, using a tiny in-memory marker atlas (no committed asset). Dev-run only; the live
    # SdlPlatform/Renderer path is the CI compile signal for the curve primitive's example host.
    retropp_add_example(curve_demo)

    # ── Path-walker demo — three movers walk ONE visible curve at once: a constant-speed walker, an eased
    # walker on the same path, and a Tween-driven walker whose multi-segment distance profile advances,
    # backs up, then continues. Each mover is an oriented sprite rotated from its walker's facing, so
    # position AND heading come from the walker. Dev-run only; the live SdlPlatform/Renderer path is the CI
    # compile signal for the path-walker example host.
    retropp_add_example(path_walker_demo)

    # ── Sprite-path demo — several slow movers on looping paths, each exercising one part of the sprite-path
    # orchestrator: a walk-cycle sprite that mirrors along a patrol line, an arrow that noses along a
    # multi-point loop, a shuttle on a Hermite arc that eases out and reverses, a tumbler that rotates and
    # breathes at once, a corner-pivot swinger, a mover on a pre-authored curve, and a shadow twin that reads
    # the raw sample. Dev-run only; the live SdlPlatform/Renderer path is the CI compile signal.
    retropp_add_example(sprite_path_demo)

    # ── Sprite-patrol demo — the SEQUENCE + INTERRUPT layer: chained-leg patrol routes (with a wait node and
    # a sentinel post), the four sequence PlaybackModes, and the interrupt stack (detour / dodge depth-2 /
    # chase loop + explicit pop) with exact resume. Dev-run only; the live SdlPlatform/Renderer path is the
    # CI compile signal for the sprite-path sequencing gate.
    retropp_add_example(sprite_patrol_demo)

    # ── Articulation demo — sprite anchors + pivot placement + per-sprite z, all on one layer: a
    # multi-sprite crab-bot chained by anchor re-feeds (child x/y = parent anchor(joint, Space::Layer),
    # child pivot = its own anchor(socket, Space::Quad)), z keys stacking the parts against a deliberately
    # front-first submission, anchors
    # mirroring under a facing flip, and a quadratic Curve trail pinned to the tail anchor. Hand-built
    # art, no asset files. Dev-run only; the live SdlPlatform/Renderer path is the CI compile signal.
    retropp_add_example(articulation_demo)

    # ── Curve-region demo — confines a gentle built-in effect to a true QUADRATIC-curved boundary and
    # shows it beside a sampled-polygon approximation of the same arc, so the no-facets win reads. Dev-run
    # only; the live SdlPlatform/Renderer path is the CI compile signal for the curve region gate.
    retropp_add_example(curve_region_demo)

    # ── Curve-mask region demo — confines a gentle built-in effect to a CUBIC (Catmull-Rom) curved boundary
    # evaluated exactly via a baked signed-distance mask, shown side by side with the faceted sampled-polygon
    # fallback of the same boundary so the no-facets win reads at a glance. Dev-run only; the live
    # SdlPlatform/Renderer path is the CI compile signal for the curve-mask gate + its two shaders.
    retropp_add_example(curve_region_mask_demo)

    # ── Stencil demo — the built-in region-erase effect: an EraseInside hole revealing a layer below (the
    # x-ray window), an EraseOutside porthole, a curved-boundary stencil, and a feathered edge beside a
    # hard one. Dev-run only; the live SdlPlatform/Renderer path is the CI compile signal for the stencil
    # pass + its two shaders (the SPIR-V + DXIL + MSL gen).
    retropp_add_example(stencil_demo)

    # ── ColorFill demo — the built-in region-confinable colour fill: a solid filled shape, a stroked
    # colored ring, a curved colored path (fromCurve + stroke = a drawn line), and a shaped mul/add tint
    # over some art. A static scene (the effect is static by nature). Dev-run only; the live
    # SdlPlatform/Renderer path is the CI compile signal for the colorfill pass + its shader (SPIR-V + DXIL
    # + MSL gen).
    retropp_add_example(color_fill_demo)

    # ── Gleam demo — the built-in luminance-keyed diagonal sheen sweep (the marquee "shine"): a diagonal
    # band sweeps across six brightness bands so the luminance keying is visible (bright glints, dark stays
    # dark), with dev keys to explore gain/slant/width. Programmatic art, no PNG. Dev-run only; the live
    # SdlPlatform/Renderer path is the CI compile signal for the gleam pass + its shader (SPIR-V + DXIL + MSL).
    retropp_add_example(gleam)

    # ── Declared-cadence demo — two blocks moving identically, one declaring `.advancesEvery` and one not,
    # so the difference is only the declaration. The undeclared one develops the smooth/stepped wave a
    # multi-tick move produces when it is eased across a single tick; the declared one never does, and
    # trails by the ticks it declares. Programmatic art, no PNG. Dev-run only — the wave is an eyes-only
    # property, and the arithmetic under it is pinned device-free in tests/declared_cadence_test.cpp.
    retropp_add_example(declared_cadence)

    # ── Colour-saturation demo — the built-in cross-channel desaturation: three copies of the same sprite
    # side by side (one at full saturation, one fully greyscale, one that a key toggles between the two via a
    # slow saturation tween), so the identity, the greyscale floor, and an animated grade all read at once.
    # Programmatic art, no PNG. Dev-run only; the live SdlPlatform/Renderer path is the CI compile signal for
    # the saturation pass + its shader (SPIR-V + DXIL + MSL).
    retropp_add_example(saturation)
    # ── Emission-field demo — region Bloom at a count no per-layer store could hold: every orb on screen
    # carries one, so every orb needs a field of its own, and the fields pack into a shared atlas sized to
    # their footprints. Keys walk the emitter count past the old eight-field ceiling (the notch on the count
    # bar) and sweep the reach, which shares one blur pass across every field carrying it. Programmatic art,
    # no PNG. Dev-run only; the live SdlPlatform/Renderer path is the CI compile signal for the atlas raster
    # + its per-page blur.
    retropp_add_example(emission_field)
    # ── Bloom demo — the built-in threshold-blur-add glow: three copies of the same horseshoe magnet (bare /
    # steady aura / breathing aura) + a whole-frame toggle over a starfield. The Bloom sprite chain path is
    # the CI compile signal.
    retropp_add_example(bloom)
    # ── Glow demo — the built-in authored-colour aura: three copies of the same dark gem, each radiating a
    # DIFFERENT chosen tint (gold / ember / violet) the art nowhere contains, with a threshold toggle between
    # whole-silhouette and brightness-keyed emission. The Glow sprite chain path + its tint fields are the CI
    # compile signal.
    retropp_add_example(glow)
    retropp_add_example(swirl)
    # ── Custom-emission demo — a game-registered custom shader joins the emission grammar with a
    # `// @retropp:emission` line: two stages (examples/custom_emission/*.frag.hlsl), one with an emission()
    # body that authors a blue-mask glow, one declaration-only that takes the stock brightpass, shown at a
    # frame postEffect and on a Below-scope sprite lens. The shader scan + the emission-consumer variants on
    # SPIR-V + DXIL + MSL are the CI compile signal.
    retropp_add_example(custom_emission)

    # ── Driver-hosting demo — a media-player faceplate hosting two synthetic sound drivers (one of each
    # family: RAM-flag mailbox + argument register) on one AudioSystem, with every handle verb under a
    # control: pads cue play(id) / play(id, Sfx), faders drive slots(...) and the vmDriver mixer bus, STOP /
    # EJECT perform the stop / close verbs, and the readout shows slots() read back live. Dev-run only.
    retropp_add_example(driver_hosting)

    # ── Mixed-driver-image demo — ONE hosted driver whose two images arrive by different routes: the tick
    # routine read out of a cartridge at runtime and handed over as bytes (no path, no policy, never in the
    # binary), beside the project's own setup image declared as an Embed path the build bakes. The cartridge
    # is destroyed before the driver is hosted, which is what copy-at-registration buys. Console + audio
    # device, no window.
    retropp_add_example(driver_mixed_images)

    # ── Window-drag demo — a borderless window with a game-drawn title bar: the strip is a WindowLoop
    # draggable region (the OS drags the window natively on a mouse press) AND the grab action drives the
    # automatic window movement (stick / d-pad / pointer move the window), plus programmatic centre/nudge
    # via moveTo. Dev-run only; the live SDL_SetWindowHitTest + SDL_Set/GetWindowPosition path is the CI
    # compile signal.
    retropp_add_example(window_drag)

    # ── Random-tilemap demo — a programmatically generated map (no PNG): an RNG clamped to a TileCatalog's
    # sparse 16-bit ids fills an IndexGrid, assembleTilemap → asTileContent renders it. Dev-run only; the
    # live SdlPlatform/Renderer + tilemap-import path is the CI compile signal.
    retropp_add_example(random_tilemap_demo)

    # ── Tempest — the vector tube shooter drawn with real stroked colour-fill regions. Dev-run only; the
    # live SdlPlatform/Renderer path is the CI compile signal for the example host.
    retropp_add_example(tempest_vector_demo)

    # ── Per-row effect data-table demo — a custom post-process effect reads a per-line horizontal-scale
    # ramp from an effect's paramTable (a curve a single sine can't express), domed slowly across the
    # frame, plus a region-confined pass. Dev-run only; the live SdlPlatform/Renderer + custom-shader +
    # row-data-store path is the CI compile signal.
    retropp_add_example(row_data_table_demo)

    # ── Blend-mode demo — the container blend modes: a translucent Half layer (DrawLayer::blend), an
    # additive glow region and a multiply shadow region (Region::blend), and a frame-level Screen overlay
    # (FrameDrawState::blend). Slow drift only. Dev-run only; the live SdlPlatform/Renderer path is the
    # CI compile signal for the blend composite + region-gate blend shaders.
    retropp_add_example(blend_mode_demo)

    # ── Colour-effects demo — day/night, flash, fade, tints, and an exposure lift, all as ColorFill +
    # a blend mode + alpha/fillIntensity at whole-frame and per-region scope (no bespoke colour member).
    # Slow drift + gentle capped key-triggered flash only. Dev-run only; the live SdlPlatform/Renderer +
    # ColorFill + region-gate blend path is the CI compile signal.
    retropp_add_example(colour_effects_demo)

    # ── Colour-parity demo — the effect SCOPE distinction: one holed lattice over a scene, the same
    # ColorFill grade confined to the left half at Layer scope (holes stay clear) and the right half at
    # Below scope (the tint reaches through the holes), cycling blend modes. The layer-path precedent for a
    # per-sprite effect. Static; A cycles the mode. Dev-run only; the live SdlPlatform/Renderer + per-layer
    # ColorFill scope path is the CI compile signal (the per-layer scope matrix is colour_parity_layer_test).
    retropp_add_example(colour_parity_demo)

    # ── Sprite-blend demo — per-sprite Sprite::blend: a hero disc drifts across a scene carrying a Multiply
    # shadow decal and an Add flare, and a probe disc cycles every blend mode over the same background. The
    # live SdlPlatform/Renderer + the sprite run-split composite path is the CI compile signal (the
    # per-sprite blend matrix is proven headlessly in sprite_blend_test).
    retropp_add_example(sprite_blend_demo)
    retropp_add_example(sprite_effects_demo)

    # ── Sprite-mask demo — a sprite's image as a mask, decoupled from its texture. A drifting sprite's
    # maskShape() geometry feeds a pinned textureless Region, an overlay outline, an inflated aura, and a
    # coarse-budget hitbox — across flips / rotation / transform, re-queried each frame; the console
    # legend names which mechanism draws what (the exact forms are tested, never drawn; the trail is the
    # Sprite-copy art route for contrast). The live SdlPlatform/Renderer + maskShape() → Region path is
    # the CI compile signal (the trace + containment + the exact contains()/freezeMask() forms are proven
    # headlessly in sprite_mask_test).
    retropp_add_example(sprite_mask)

    # ── Palette-image demo — loads colour PNGs one-pixel-per-entry via loadPaletteImage: a 4x4 hue grid
    # Embed (baked) walked through all 8 read orders, and an 8-step ramp LoadFromPath (copied beside the
    # binary, read from disk) — the sibling of the atlas-load demo, adding the asset-policy axis. Static,
    # manual stepping. Dev-run only; the live SdlPlatform/Renderer + loadPaletteImage + both asset
    # policies are the CI compile signal (the slicer math is proven headlessly in palette_image_slice_test).
    retropp_add_example(palette_image_demo)

    # ── Cracked-wall demo — a brick wall over a drifting background, coloured through three 16-bit RGBA
    # palette images (loadPaletteImage). The holes are the palette entries' own alpha: an alpha-0 entry
    # discards (the moving background shows through), a partial-alpha entry blends. A Tween eases a
    # fracture sprite layer in. Dev-run only; the live loadPaletteImage / loadAtlas / Tween path is the CI
    # compile signal.
    retropp_add_example(wall_cracks_demo)

    # ── Numberator — a working calculator styled after classic Mac OS, a non-game UI on the engine. The
    # chrome (window, title bar, sunken display) is a tilemap assembled from a map PNG + a catalog; the
    # keys are sprites that flip X+Y to look pressed; digits/labels are transparent glyph sprites. A
    # multi-file example (every .cpp in the dir compiles in); every asset is an embedded PNG.
    retropp_add_example(Numberator)

    # ── Tile-rotation demo — a box frame assembled from ONE corner tile + ONE edge tile in their four 90°
    # rotations (TileCatalogEntry::rotation), plus a square glyph sprite and a non-square arrow sprite that
    # rotate on a key (the arrow transposes its read at 90°/270°). Dev-run only; the live SdlPlatform/
    # Renderer + tile/sprite rotation path is the CI compile signal (the permutation math is proven
    # headlessly in tile_attributes_test's SourceCellTexel cases).
    retropp_add_example(tile_rotation_demo)

    # ── Audio-pack demo — the audio-pack backend: register a .wav and a .ogg on the AudioLibrary and play
    # them through a bare AudioSystem, exactly as a chiptune plays. The Pcm AudioIds dispatch to the file
    # decoder (dr_wav / stb_vorbis); the VM/APU stays dormant. Dev-run only (audio, never auto-launched);
    # the live decode + AudioSystem path is the CI compile signal (the decode math is proven headlessly in
    # audio_pcm_pack_test). Its .wav/.ogg ride along beside the binary (LoadFromPath default).
    retropp_add_example(audio_pack_demo)

    # ── Audio mixer — four on-screen volume sliders (Master + Music/SFX/Vocals) the developer adjusts
    # live, over a sustained Music tone, a Vocals tone, and an SFX blip, each its own AudioSystem all
    # reading the one shared AudioMixer. Makes the perceptual taper audible (half-slider sounds about
    # half) and proves per-bus routing (pull one bus down, only it quiets). All four tones register
    # Embed, so the auto-scan bakes them in — nothing ships beside the binary. Dev-run only; the live
    # AudioSystem + AudioMixer + tile/input path is the CI compile signal.
    retropp_add_example(audio_mixer_demo)

    # ── Polterball — a hybrid arcade game (Breakout paddle/ball × Pac-Man pellet maze + pursuit +
    # power-pellet reversal × carvable soft walls): the ball is the pellet-eater and the ghosts hunt
    # the BALL, steered only through paddle english. Multi-file (sim / ghost AI / assets / feel /
    # audio / render), three embedded indexed PNGs + 7 embedded chiptune SFX — the binary is fully
    # self-contained. Dev-run only; the live SdlPlatform/Renderer/loadAtlas/audio/analog path plus
    # the Curve (eyes homing), AnimationPlayer (palette pulse + skirt wave), Tween (feel), and
    # Add-blend ColorFill region (ignite glow) consumers are the CI compile signal.
    retropp_add_example(polterball)

    # ── Vantium — a Uridium-style bidirectional dreadnought strafer, and a broad renderer
    # showcase: parallax star layer under a Blank-wrapped sectional deck with index-0 hull holes,
    # tile orientation via flips + Rot90 (one hazard/corner/pipe art, every edge), frame animation
    # (Manta banks, pooled explosion clips) AND palette animation (pod glow, star twinkle, wave
    # liveries), spinning-mine per-sprite Transforms, Curve/ArcLengthTable squadron paths, Add/
    # Multiply regions + a deck-confined Ripple, a rich 16×16 gradient font, and a live START
    # toggle of the EvaluationGrid. Multi-file; three embedded PNGs + 7 embedded SFX — the binary
    # is self-contained. Dev-run only; the live full-surface path is the CI compile signal.
    retropp_add_example(vantium)
    retropp_add_example(vm_threads)

    # ── Ferryman — a hybrid arcade game (open-sea rescue-and-carry × a toned-down bullet hell):
    # collect stranded souls from the islets and sail them home — every soul aboard slows the
    # ferry, pays escalating on delivery, AND occasionally fires back (there is no fire button;
    # the crew fights, the ferryman sails). Three enemy craft classes fire straight, readable
    # bullets; an abductor steals waiting souls and losses come back as mutant hunters. Multi-file
    # (sim / rival AI / assets / feel / audio / render), four embedded indexed PNGs + 30 embedded
    # PALETTE IMAGES (every colour via loadPaletteImage) + 7 embedded chiptune SFX — the binary is
    # fully self-contained. Dev-run only; the live SdlPlatform/Renderer/loadAtlas/loadPaletteImage/
    # audio path plus the two-plane parallax scroll, the 32×32-macro-tile stamping, the Add-blend
    # beam/glow regions, the Curve/ArcLengthTable abductor swoop, and the palette-cycle clips
    # (sea shimmer, beacon glow, running lights) are the CI compile signal.
    retropp_add_example(ferryman)

    # ── Save-store demo — persistence without a window: a console program that resolves the
    # platform save directory from the application identity (EngineConfig::identity), writes
    # a versioned document,
    # reads it back, then bumps the schema version and reads again through a registered
    # migration step — printing every stage. Run it twice: the second run finds the first
    # run's document on disk, the point of the whole subsystem. Headless (no window, no GPU,
    # no audio device); dev-run only; the live SDL_GetPrefPath + atomic-write path is the CI
    # compile signal (the store's behaviour is proven headlessly in save_store_test).
    retropp_add_example(save_store_demo)

    # ── User-files demo — the other half of the per-user directory: a console program that resolves
    # the same location the save store does, writes a small tree of extracted assets into it,
    # reads one back, and points EngineConfig::assetRoot at the result. It prints both stores'
    # resolved directories side by side, which is the one-resolution property made visible, and
    # shows a path that tries to leave the store being refused. Run it twice: the second run finds
    # the first run's files. Headless (no window, no GPU, no audio device); dev-run only; the live
    # SDL_GetPrefPath + atomic-write path is the CI compile signal (the behaviour is proven
    # headlessly in user_files_test).
    retropp_add_example(user_files_demo)

    # ── Data-assets demo — the third ingestible family: bytes the engine stores and never interprets.
    # A console program registering the same corpus three ways — an Embed character table baked into the
    # binary, a corpus extracted into the player's own directory and registered by path (the per-type
    # LoadFromPath default, so nothing is baked), and a derived index handed over as ready bytes — then
    # decoding the corpus through the table and printing the strings. It prints where each one's bytes
    # came from, which is the whole distinction the policy expresses. Headless (no window, no GPU, no
    # audio device); dev-run only; the behaviour is proven headlessly in data_library_test.
    retropp_add_example(data_assets)

    # ── Cartridge-assets demo — pulling content out of a cartridge the game hosts. Registers the
    # image as a data asset (no policy argument, so the LoadFromPath default keeps it out of the
    # binary), hosts it, declares the places its tile art and name table live at, and reads them:
    # tiles arrive as plain bytes in the hardware's 2bpp layout and are decoded and printed here, a
    # tile is patched in the hosted image to show writing works, and gb::WorkRam is read through the
    # same verb to show a shipped constant is just another place. The cartridge is authored by
    # assets/gen_cartridge_assets.py, so nothing here comes from anyone's ROM. Headless (no window,
    # no GPU, no audio device); dev-run only; the behaviour is proven headlessly in
    # region_access_test / gb_regions_test / host_rom_test.
    retropp_add_example(cartridge_assets)

    # ── Rom-run demo — a hosted cartridge living its own life, watched and steered while it runs.
    # Authors a commercial-shaped little game in-code (assembled by the engine's own SM83 assembler),
    # hosts it, declares its frame counter and echo cells, and run()s it: the machine holds hardware
    # cadence on its own thread while this program measures it at {1,1} / {2,1} / {1,2} / {0,1},
    # round-trips a write through the guest's own loop, and parks/resumes it mid-count. Headless
    # (no window, no GPU, no audio device). CI runs it with --verify on every platform: the demo
    # asserts each measured rate within ±5% of its factor's target (paused exactly still, the echo
    # byte exact) and exits nonzero on violation — so the governor holding real wall-clock cadence
    # is CI-checked with the printed measurements captured in the test log. Deterministic coverage
    # of the same machinery is rom_run_test's.
    retropp_add_example(rom_run)
    add_test(NAME RomRunExample.HoldsHardwareCadenceLive
             COMMAND retropp-rom_run-demo --verify)

    # ── Guest-escape demo — a running cartridge hands control to native code and carries on.
    # Authors a cartridge whose loop calls its own routine every frame, hosts it and run()s it, then
    # tells the story in two acts: an escape at that routine counts the calls from C++ while the
    # machine runs on its own thread, and then the routine is REPLACED with no engine verb for it —
    # its first byte patched to a return through the ordinary write surface, an escape declared there,
    # and the answer computed natively and left where the routine promised its caller it would be.
    # Headless (no window, no GPU, no audio device). CI runs it with --verify on every platform: the
    # demo asserts one fire per frame the guest lived in each act (measured against the guest's OWN
    # frame counter — how many frames fit a wall-clock window varies, the relation does not), the
    # cartridge's own rule answering before the replacement and this program's answer after it, and
    # exits nonzero otherwise. Deterministic coverage of the same machinery is guest_escape_test's
    # and sameboy_escape_test's.
    retropp_add_example(guest_escape)
    add_test(NAME GuestEscapeExample.EscapesAndAnswersARunningCartridge
             COMMAND retropp-guest_escape-demo --verify)

    # ── Guest-nesting demo — native code calling INTO the cartridge, and giving the guest back.
    # Authors a cartridge with three routines of its own — a damage rule its loop asks every frame, a
    # generator that advances a seed it keeps, and a decompressor the loop never calls — hosts it and
    # run()s it, then tells the story in two acts: a replacement answers the damage rule natively and
    # builds its answer by calling the cartridge's OWN generator nested inside the escape, on the
    # machine's own thread; then, with the machine parked, this program calls the cartridge's own
    # decompressor on a packed table the game never reached. Headless (no window, no GPU, no audio
    # device). CI runs it with --verify on every platform: the demo asserts one nested generator call
    # per native answer, a seed that advanced exactly as the cartridge's own arithmetic would, the
    # published damage being this program's rule over the guest's number, and the unpacked bytes —
    # and exits nonzero otherwise. Deterministic coverage of the same machinery is
    # guest_nesting_test's and sameboy_nesting_test's.
    retropp_add_example(guest_nesting)
    add_test(NAME GuestNestingExample.CallsTheCartridgesOwnRoutines
             COMMAND retropp-guest_nesting-demo --verify)

    # ── Co-execution demo — the whole seam in one program, on one panel, while it runs.
    # Authors one cartridge — a loop that walks a marker by a step it reads from its own ROM, asks
    # its own damage rule for a number, and calls its own end-of-frame routine — hosts it, names its
    # places (the console's own gb::Hram, a table in bank 2 through gb::banked, a byte inside the
    # image itself), binds two of its routines, declares an observing escape and a replacement, and
    # runs it. The panel reads the guest live from the per-step publish and shows what each verb did:
    # the speed factor, the guest's own counters, which rule answered the damage, what the
    # replacement's nested calls did to the cartridge's generator and its seed, and — with the
    # machine parked — what its own decompressor produced and what its pointer table points at.
    # The guest's own picture is not drawn: a hosted machine's framebuffer is a separate surface.
    # Dev-run only; the live SdlPlatform/Renderer + Vm path is the CI compile signal (the seam is
    # proven headlessly in rom_run, guest_escape, guest_nesting and their test suites).
    retropp_add_example(coexecution)

    # ── Exit-snapshot demo — the application-exit guard as a visible round-trip over the persistence
    # core. Two quit routes each run a close-out guard that saves a snapshot before teardown: C runs a
    # CALLBACK guard (write + Proceed at once), G runs a MULTI-TICK guard (a short fade returning NotYet,
    # then write + Proceed); Esc / the window-close button is a plain quit that saves nothing — and even
    # the OS close routes through the same guard. This session's random numbers show as white bars; on
    # relaunch the loaded snapshots (green = callback, cyan = gates) match the PRIOR session's numbers,
    # proving they were restored. Dev-run only; the live SdlPlatform/Renderer + SaveStore path is the CI
    # compile signal (the exit surface is proven headlessly in exit_guard / exit_multitick).
    retropp_add_example(exit_snapshot)
endif()

# ── Reflect the accumulated custom shaders into ScreenSpaceEffect's param union ──
# Produces the shared custom_effect_fields.inc (#include'd inside the struct) + custom_effect_packers.h
# every TU compiles against. Generated at configure time for immediate correctness (the very first build +
# clangd), and re-generated at build time when a shader's cbuffer changes. retroppengine — hence every
# consumer linking it — depends on it.
#
# Deferred to the END of the top-level directory (mirroring retropp_finalize_assets) so the reflected set
# covers a CONSUMER's shaders too: a consumer's retropp_autocompile_shaders(<consumer>) call runs at the
# top level, AFTER add_subdirectory(engine) has returned. DEFER still fires during the configure phase
# (at directory-end, before generate), so first-build + clangd correctness is preserved. When the engine
# is the top-level project, its own examples' autocompile calls already ran before the deferred call, so
# the reflected set — and the generated files — are byte-identical to the non-deferred order.
#
# gen_effect_fields.cmake is addressed via RETROPP_ENGINE_SOURCE_DIR, not CMAKE_CURRENT_SOURCE_DIR: once
# deferred, this runs in the top-level (consumer) scope where CMAKE_CURRENT_SOURCE_DIR would be the
# consumer's root — the same relocatability reason gen_shader.cmake uses it.
function(retropp_finalize_effect_fields)
    get_property(_retropp_custom_shaders GLOBAL PROPERTY RETROPP_CUSTOM_SHADERS)
    if(_retropp_custom_shaders)
        list(REMOVE_DUPLICATES _retropp_custom_shaders)
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND}
            "-DSHADERS=${_retropp_custom_shaders}"
            "-DOUT_FIELDS=${RETROPP_EFFECT_FIELDS}"
            "-DOUT_PACKERS=${RETROPP_EFFECT_PACKERS}"
            -P "${RETROPP_ENGINE_SOURCE_DIR}/shaders/gen_effect_fields.cmake"
        RESULT_VARIABLE _retropp_eff_rc)
    if(NOT _retropp_eff_rc EQUAL 0)
        message(FATAL_ERROR "gen_effect_fields.cmake failed (${_retropp_eff_rc})")
    endif()
    add_custom_command(
        OUTPUT "${RETROPP_EFFECT_FIELDS}" "${RETROPP_EFFECT_PACKERS}"
        COMMAND ${CMAKE_COMMAND}
                "-DSHADERS=${_retropp_custom_shaders}"
                "-DOUT_FIELDS=${RETROPP_EFFECT_FIELDS}"
                "-DOUT_PACKERS=${RETROPP_EFFECT_PACKERS}"
                -P "${RETROPP_ENGINE_SOURCE_DIR}/shaders/gen_effect_fields.cmake"
        DEPENDS ${_retropp_custom_shaders}
                "${RETROPP_ENGINE_SOURCE_DIR}/shaders/gen_effect_fields.cmake"
        COMMENT "Reflecting custom-shader params into ScreenSpaceEffect (gen_effect_fields)"
        VERBATIM)
    add_custom_target(retropp-effect-fields DEPENDS "${RETROPP_EFFECT_FIELDS}" "${RETROPP_EFFECT_PACKERS}")
    add_dependencies(retroppengine retropp-effect-fields)
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL retropp_finalize_effect_fields)

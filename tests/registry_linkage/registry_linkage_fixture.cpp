// The in-tree static-library consumer for the registry link surface.
//
// The three build scans (shaders, assets, routines) read a target's sources for registration calls and
// emit one generated registry translation unit per kind. Those TUs are pure static initializers, so a
// static library needs a linker anchor to keep them (retropp_anchor_registry in CMakeLists.txt). This
// library is the shape that proves it: the paths below are baked into ITS archive, and
// registry_linkage_test.cpp — linked into the test executable alongside it — asserts each one arrives.
//
// Nothing here runs. The scans read call sites textually, so the registrations happen at build time; a
// Renderer and a Vm are named only to make the calls compile. Each path is used by no other target, so a
// passing assertion cannot be satisfied by some other target's bake.

#include <array>
#include <cstdint>
#include <span>

#include "retropp/audio_library.h"  // DriverImagePath, DriverImageSource, HostedDriverBinding
#include "retropp/gb.h"            // gb::A, gb::Mbc3
#include "retropp/image.h"         // loadMapPng
#include "retropp/renderer.h"      // Renderer::registerPostProcessStage
#include "retropp/vm.h"            // Vm::registerRoutine

namespace retropp::linkage_fixture {

void declareProbes(Renderer& renderer, Vm& vm) {
    vm.registerRoutine<std::uint8_t()>("tests/fixtures/linkage/probe.asm",
                                       RoutineBinding{.output = gb::A});
    [[maybe_unused]] const IndexGrid grid = loadMapPng("tests/fixtures/linkage/probe.png");
    renderer.registerPostProcessStage("tests/shaders/linkage_probe.frag.hlsl");
}

// A driver's images are declared on the binding, not on the registerDriver call, and each carries its own
// policy — so the three below cover the whole rule in one binding: an `.asm` image with no policy at all
// (which resolves to Embed and assembles into the routine registry), a raw image asking for Embed (which
// bakes into the asset registry), and one asking for LoadFromPath (which must NOT be baked — that is the
// posture for driver content a game may not ship inside its binary).
HostedDriverBinding driverProbeBinding() {
    return HostedDriverBinding{
        .images    = {DriverImagePath{.base = 0x6000, .path = "tests/fixtures/linkage/driver_boot.asm"},
                      DriverImagePath{.base  = 0x6100,
                                      .path   = "tests/fixtures/linkage/driver_embedded.bin",
                                      .policy = AssetPolicy::Embed},
                      DriverImagePath{.base   = 0x6200,
                                      .path   = "tests/fixtures/linkage/driver_shipped.bin",
                                      .policy = AssetPolicy::LoadFromPath}},
        .mapper    = gb::Mbc3,
        .tickEntry = 0x6000,
        .isa       = Isa::Sm83,
    };
}

// The byte half of the mixed binding below. These are the exact contents of
// tests/fixtures/linkage/driver_mixed_unbaked.bin — the same image, on disk, under a name the binding
// never spells. That file exists so the scan test's absence assertion is answerable: declaring this image
// as a path instead would bake those bytes under that name, and the assertion would see them.
constexpr std::array<std::uint8_t, 4> kMixedDriverBytes{0xC1, 0xC2, 0xC3, 0xC4};

// One binding, both sources. A byte image sits BETWEEN two path images so the scan's per-image keying is
// exercised where it would break: an image carrying no path at all must not disturb how its neighbors
// resolve, and the Embed / LoadFromPath pair on either side of it must still land on opposite lists.
HostedDriverBinding mixedDriverProbeBinding() {
    return HostedDriverBinding{
        .images    = {DriverImagePath{.base   = 0x6300,
                                      .path   = "tests/fixtures/linkage/driver_mixed_embed.bin",
                                      .policy = AssetPolicy::Embed},
                      DriverImage{.bytes = std::span<const std::uint8_t>(kMixedDriverBytes),
                                  .base  = 0x6400},
                      DriverImagePath{.base   = 0x6500,
                                      .path   = "tests/fixtures/linkage/driver_mixed_shipped.bin",
                                      .policy = AssetPolicy::LoadFromPath}},
        .mapper    = gb::Mbc3,
        .tickEntry = 0x6300,
        .isa       = Isa::Sm83,
    };
}

}  // namespace retropp::linkage_fixture

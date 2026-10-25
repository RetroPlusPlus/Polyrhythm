// Cartridge-assets demo — pulling content out of a cartridge the game hosts.
//
// A developer extending an existing cartridge already knows where its content is. This is the path
// from that knowledge to bytes an ingestion surface can take: host the image, declare the places
// that matter, read them.
//
//   * The CARTRIDGE is registered with registerData and no policy argument, so it takes the family's
//     per-type default (LoadFromPath) and the build bakes nothing. That default is the legal posture
//     for exactly this case — a cartridge is the content a game is least likely to be allowed to
//     ship inside its own binary, so leaving the policy off must never embed it.
//   * The PLACES are declared as one batch, so every entry is checked once. Declare a bad one and
//     the failure names it, before any of them is read.
//   * The TILES come back as plain bytes in the hardware's own 2bpp layout. The engine does not know
//     that is what they are — decoding them is this program's job, and it prints them as characters
//     so the demo needs no window.
//   * A PATCH shows writing works too: the hosted image is a buffer this process owns, so a tile can
//     be rewritten and read straight back.
//
// The cartridge is authored by assets/gen_cartridge_assets.py, so nothing here comes from anyone's
// ROM. Headless: no window, no GPU, no audio device. Safe to run anywhere, including over SSH.

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "retropp/data_library.h"   // DataLibrary + DataId — how the cartridge's bytes are delivered
#include "retropp/engine_config.h"  // EngineConfig — where LoadFromPath resolves
#include "retropp/gb.h"             // gb::WorkRam and friends — the machine's own memories
#include "retropp/memory_region.h"  // MemoryRegion — where a place is
#include "retropp/vm.h"             // Vm — hosting the cartridge and reading it

namespace {

// Where this cartridge keeps its content. A real one gets these from a disassembly's symbol file;
// the demo's are wherever the generator put them.
constexpr std::uint32_t kTileBase  = 0x1000;
constexpr std::uint32_t kTextBase  = 0x2000;
constexpr std::uint32_t kTileBytes = 16;  // one 8x8 tile in the hardware's two-bitplane layout
constexpr std::uint32_t kTileCount = 4;
constexpr std::uint32_t kTextBytes = 8;
constexpr std::uint32_t kTextCount = 4;

// The places this game cares about. Never instantiated — it exists so the places have names, and so
// naming one wrong is a compile error rather than a bad address.
struct Places {
    retropp::MemoryRegion tiles;
    retropp::MemoryRegion names;
    retropp::MemoryRegion scratch;
};

// One 8x8 tile, decoded from the two-bitplane layout the hardware stores into one shade per pixel.
// The engine handed over bytes and stopped there; this is the part that knows what they mean.
std::vector<std::uint8_t> decodeTile(std::span<const std::uint8_t> tile) {
    std::vector<std::uint8_t> pixels(64);
    for (int y = 0; y < 8; ++y) {
        const std::uint8_t low  = tile[static_cast<std::size_t>(y) * 2];
        const std::uint8_t high = tile[static_cast<std::size_t>(y) * 2 + 1];
        for (int x = 0; x < 8; ++x) {
            const int bit = 7 - x;
            pixels[static_cast<std::size_t>(y) * 8 + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>((((high >> bit) & 1) << 1) | ((low >> bit) & 1));
        }
    }
    return pixels;
}

void printTile(std::span<const std::uint8_t> pixels) {
    static constexpr char kShades[] = {' ', '.', '+', '#'};
    for (int y = 0; y < 8; ++y) {
        std::printf("    ");
        for (int x = 0; x < 8; ++x) {
            const std::uint8_t shade = pixels[static_cast<std::size_t>(y) * 8 +
                                              static_cast<std::size_t>(x)];
            std::printf("%c%c", kShades[shade], kShades[shade]);  // doubled: square-ish in a terminal
        }
        std::printf("\n");
    }
}

std::string textOf(std::span<const std::uint8_t> entry) {
    std::string out(reinterpret_cast<const char*>(entry.data()), entry.size());
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

}  // namespace

int main() {
    retropp::EngineConfig config;
    config.identity = {.organization = "Retro++", .application = "CartridgeAssetsDemo"};
    retropp::EngineConfig::setActive(config);

    // ── Delivering the cartridge ────────────────────────────────────────────────────────────────
    // No policy argument, so the per-type default applies and the build copies the file beside the
    // binary instead of baking it in. hostRom takes BYTES, so where they came from is open: this
    // registration, a file the game read itself, anything.
    retropp::DataLibrary& library = retropp::DataLibrary::instance();
    const retropp::DataId cartridgeId =
        library.registerData("examples/cartridge_assets/assets/demo_cartridge.gb");
    const std::span<const std::uint8_t> image = library.data(cartridgeId);
    std::printf("cartridge: %zu bytes\n", image.size());

    retropp::Vm::GBC vm;
    vm.hostRom(image);
    std::printf("hosted — every byte of it is addressable now\n\n");

    // ── Declaring the places ────────────────────────────────────────────────────────────────────
    // One batch, checked once. A place is an array: how big one entry is, and how many follow.
    const auto places = vm.registerRegions(regions(
        region(&Places::tiles,
               retropp::MemoryRegion{.at = kTileBase, .size = kTileBytes, .count = kTileCount},
               "tile art"),
        region(&Places::names,
               retropp::MemoryRegion{.at = kTextBase, .size = kTextBytes, .count = kTextCount},
               "tile names"),
        region(&Places::scratch, retropp::gb::WorkRam, "work ram")));
    std::printf("%zu places declared and checked\n\n", places.size());

    // ── Reading them ────────────────────────────────────────────────────────────────────────────
    // Entry N of a place, resolved in the machine's own decoded space — so an array longer than a
    // bank reads correctly across the boundaries instead of running off the end of the first one.
    for (std::uint32_t i = 0; i < kTileCount; ++i) {
        const std::vector<std::uint8_t> raw  = vm.read(places, &Places::tiles, i);
        const std::vector<std::uint8_t> name = vm.read(places, &Places::names, i);
        std::printf("  tile %u — \"%s\"\n", i, textOf(name).c_str());
        printTile(decodeTile(raw));
    }

    // The bytes are the caller's. Hand them to uploadData if they should be cataloged, or convert
    // them and hand the result to uploadAtlas — this demo prints them instead, so it needs no GPU.

    // ── Writing one back ────────────────────────────────────────────────────────────────────────
    // The hosted image is a buffer this process owns, not read-only silicon, so patching it is
    // allowed. The write lands in memory only: the file on disk is untouched, and re-hosting the
    // cartridge would bring the original tile straight back.
    std::vector<std::uint8_t> solid(kTileBytes, 0xFF);  // both bitplanes set: the darkest shade
    vm.write(places, &Places::tiles, solid, 0);
    std::printf("  tile 0, after patching the hosted image\n");
    printTile(decodeTile(vm.read(places, &Places::tiles, 0)));

    // ── The machine's own memories are places too ───────────────────────────────────────────────
    // gb::WorkRam is a MemoryRegion the platform header ships, exactly as gb::A is a Location it
    // ships. Nothing about reading it differs from reading a place the game declared.
    std::printf("\nwork ram: %zu bytes, read through the same verb\n",
                vm.read(places, &Places::scratch).size());
    return 0;
}

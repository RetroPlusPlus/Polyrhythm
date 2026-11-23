// Portable install demo — the player decides where the game's files go, without a window.
//
// A console program whose config allows a portable install (EngineConfig::portable, `allowFile`), followed by the
// ordinary persistence code: a default SaveStore writes a save, and a default UserFiles writes a file. Nothing in
// that code knows about the switch.
//
// Run it as it is and both land in the per-user directory (~/Library/Application Support/Retro++/PortableInstallDemo
// on macOS, %APPDATA% on Windows, $XDG_DATA_HOME on Linux). Put an empty file named portable.txt beside the
// executable and run it again, and both land in a PortableInstallDemo directory beside the executable instead.
// Delete portable.txt and they go back to the per-user directory.
//
// Headless: no window, no GPU, no audio device. Safe to run anywhere, including over SSH.

#include <cstdio>
#include <string>
#include <vector>

#include "retropp/engine_config.h"  // EngineConfig — the identity and the portable switch
#include "retropp/save_store.h"     // SaveStore — save documents
#include "retropp/user_files.h"     // UserFiles — plain files in the same directory

namespace {

std::vector<std::byte> toBytes(const std::string& text) {
    const auto* p = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(p, p + text.size());
}

}  // namespace

int main() {
    // Allow a portable.txt beside the application to put the files there. Without the file, nothing changes.
    retropp::EngineConfig config;
    config.identity = {.organization = "Retro++", .application = "PortableInstallDemo"};
    config.portable = {.allowFile = true};
    retropp::EngineConfig::setActive(config);

    // ── The ordinary persistence code ────────────────────────────────────────────────────────────
    retropp::SaveStore saves;
    retropp::UserFiles files;

    std::printf("the saves are in: %s\n", saves.basePath().string().c_str());
    std::printf("the files are in: %s\n", files.root().string().c_str());

    if (!saves.write("slot1", 1, toBytes("a save")) || !files.write("note.txt", toBytes("a note"))) {
        std::printf("could not write the save and the file\n");
        return 1;
    }
    std::printf("wrote %s\n", (saves.basePath() / "slot1").string().c_str());
    std::printf("wrote %s\n", files.pathFor("note.txt").string().c_str());

    return 0;
}

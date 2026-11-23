#pragma once

namespace retropp {

// Where the player's files go: the platform's per-user directory for the application's identity, or a directory
// named for the application beside the application. Declared once, on EngineConfig::portable, and read by
// userDataDir() — the one resolution every store rooted by default goes through (save_store.h), so a SaveStore,
// a UserFiles and a hosted machine's battery saves all follow it with nothing changed at their call sites.
//
// `enabled` puts the files beside the application on every machine. `allowFile` lets a file named portable.txt
// beside the application do the same, so a player decides by placing the file and a release ships without it.
// The file's contents are never read. Beside the application is `<the application's directory>/<application>/`:
// the executable's directory on Windows and Linux, the directory that holds the bundle on macOS, with one
// directory named for the application inside it. The file sits in the application's directory itself.
//
// The decision is made at each resolution: a store reads it once, when it is constructed. A portable resolution
// never asks the platform for the per-user directory, so a run from removable media leaves nothing behind on the
// machine it runs on. The engine never writes portable.txt and never moves files between the two locations.
struct PortableInstall {
    bool enabled   = false;  // the files go beside the application
    bool allowFile = false;  // portable.txt beside the application is allowed to turn the switch on

    // The placement a default-rooted store resolves with. EngineConfig::setActive() assigns it from
    // EngineConfig::portable — the one startup call covers it; set it directly only when bypassing the config
    // bundle entirely. Self-typed static: declared here, defined `inline` just below the struct.
    static PortableInstall active;
};

inline PortableInstall PortableInstall::active{};

}  // namespace retropp

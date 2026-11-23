#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "retropp/app_identity.h"
#include "retropp/portable_install.h"

namespace retropp::detail {

// The application's directory, from the base path the platform answers: the base path itself, with no trailing
// separator, or — when it lies inside an .app bundle, which is where the last component ending in ".app" is the
// final one or is followed by "Contents" — the directory that holds the bundle. The rule is applied on Apple
// platforms; it is a pure function of its argument so a case runs it anywhere.
[[nodiscard]] std::filesystem::path applicationDirFrom(const std::filesystem::path& basePath);

// userDataDir with the application's directory supplied instead of read from the platform — what the public
// forms call after reading it, and the seam a case uses to stand a temporary directory in for the application's,
// so portable.txt and the portable directory never land beside a real binary. An empty `applicationDir` is a
// platform that supplied none: a portable resolution then throws SaveStoreError, and a per-user one never reads it.
// Where the public forms end the process on a portable directory that cannot be created, this one throws
// SaveStoreError, so a case drives the refusal in-process.
[[nodiscard]] std::filesystem::path userDataDirAt(const std::filesystem::path& applicationDir,
                                                 const AppIdentity& identity, const PortableInstall& placement);

// What the player is shown when the portable directory cannot be created: the application's name, the directory,
// and the reason the operating system gave. The public userDataDir forms show it in an error box and end the
// process; this one only answers it, so a case pins the text without a dialog.
[[nodiscard]] std::string portableRefusalMessage(std::string_view application, const std::filesystem::path& directory,
                                                 std::string_view reason);

}  // namespace retropp::detail

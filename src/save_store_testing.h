#pragma once

#include <filesystem>

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
[[nodiscard]] std::filesystem::path userDataDirAt(const std::filesystem::path& applicationDir,
                                                 const AppIdentity& identity, const PortableInstall& placement);

}  // namespace retropp::detail

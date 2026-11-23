// Where the player's files go under a portable install, device-free. Each case stands a temporary directory in for
// the application's directory through detail::userDataDirAt, so portable.txt and a portable directory land under
// the system temp path, never beside a real binary; the cases that go through the public forms with a portable
// placement resolve beside the test binary and remove what they made. Four layers:
//   * The decision — with no portable.txt the default placement is the per-user directory; `enabled` puts the
//     files beside the application; portable.txt does the same only when `allowFile` is set, which it is by
//     default, and it is looked for at every resolution.
//   * The portable directory — exactly `<application's directory>/<application>`, created by the resolution,
//     refused with SaveStoreError when it cannot be made or there is no application directory, and reached without
//     asking the platform for the per-user directory.
//   * The bundle rule — on macOS the application's directory is the one that holds the .app bundle.
//   * The fan-out — setActive() seeds PortableInstall::active, the one- and no-argument forms read it, and a
//     default SaveStore and UserFiles follow while a store rooted at an explicit path stays where it is put.
#include "retropp/portable_install.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/app_identity.h"
#include "retropp/engine_config.h"
#include "retropp/save_store.h"
#include "retropp/user_files.h"
#include "src/save_store_testing.h"

namespace retropp {
namespace {

namespace fs = std::filesystem;

const AppIdentity kId{.organization = "RetroppTest", .application = "PortableApp"};

// True when `path` lies outside `root`: the relative path from `root` climbs out of it, or there is none.
bool outside(const fs::path& path, const fs::path& root) {
    const fs::path rel = path.lexically_relative(root);
    return rel.empty() || *rel.begin() == "..";
}

// A directory path with any trailing separator dropped, so its parent is the directory above it.
fs::path withoutTrailingSeparator(const fs::path& path) {
    return path.filename().empty() ? path.parent_path() : path;
}

void removeQuietly(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

class PortableInstallTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        app_ = fs::temp_directory_path() / (std::string("retropp-portable-install-test-") + info->name());
        fs::remove_all(app_);
        fs::create_directories(app_);
        savedPlacement_ = PortableInstall::active;
        savedIdentity_  = SaveStore::defaultIdentity;
    }
    void TearDown() override {
        PortableInstall::active    = savedPlacement_;
        SaveStore::defaultIdentity = savedIdentity_;
        removeQuietly(app_);
    }

    fs::path marker() const { return app_ / "portable.txt"; }
    void writeMarker() const { std::ofstream{marker()} << "portable"; }

    fs::path        app_;
    PortableInstall savedPlacement_{};
    AppIdentity     savedIdentity_{};
};

// ── The decision ────────────────────────────────────────────────────────────────────────────────

TEST_F(PortableInstallTest, TheDefaultPlacementIsThePerUserDirectory) {
    // No portable.txt beside the application.
    PortableInstall::active = {};
    const fs::path perUser = detail::userDataDirAt(app_, kId, {});
    EXPECT_TRUE(outside(perUser, app_)) << perUser;
    EXPECT_EQ(perUser, userDataDir(kId, {}));
    EXPECT_EQ(perUser, userDataDir(kId));
    EXPECT_TRUE(fs::is_directory(perUser)) << perUser;
}

TEST_F(PortableInstallTest, EnabledPutsTheFilesBesideTheApplication) {
    const fs::path dir = detail::userDataDirAt(app_, kId, {.enabled = true});
    EXPECT_EQ(dir, app_ / "PortableApp");
    EXPECT_TRUE(fs::is_directory(dir)) << dir;
    // One directory named for the application, with no organization level above it.
    EXPECT_FALSE(fs::exists(app_ / "RetroppTest"));
}

TEST_F(PortableInstallTest, TheFileTurnsTheSwitchOnOnlyWhenAllowed) {
    const PortableInstall allow{.allowFile = true};
    const PortableInstall ignore{.allowFile = false};
    EXPECT_EQ(detail::userDataDirAt(app_, kId, allow), detail::userDataDirAt(app_, kId, ignore));

    writeMarker();
    EXPECT_EQ(detail::userDataDirAt(app_, kId, allow), app_ / "PortableApp");
    // The file is present, but a placement that does not allow it resolves per-user all the same.
    EXPECT_TRUE(outside(detail::userDataDirAt(app_, kId, ignore), app_));
}

TEST_F(PortableInstallTest, TheFileIsAllowedByDefault) {
    EXPECT_TRUE(PortableInstall{}.allowFile);
    EXPECT_FALSE(PortableInstall{}.enabled);
    writeMarker();
    EXPECT_EQ(detail::userDataDirAt(app_, kId, {}), app_ / "PortableApp");
}

TEST_F(PortableInstallTest, EnabledWinsWithoutTheFile) {
    ASSERT_FALSE(fs::exists(marker()));
    EXPECT_EQ(detail::userDataDirAt(app_, kId, {.enabled = true, .allowFile = true}), app_ / "PortableApp");
}

TEST_F(PortableInstallTest, TheFileIsReadAtEveryResolution) {
    const PortableInstall allow{.allowFile = true};
    EXPECT_TRUE(outside(detail::userDataDirAt(app_, kId, allow), app_));
    writeMarker();
    EXPECT_EQ(detail::userDataDirAt(app_, kId, allow), app_ / "PortableApp");
    fs::remove(marker());
    EXPECT_TRUE(outside(detail::userDataDirAt(app_, kId, allow), app_));
}

// ── The portable directory ──────────────────────────────────────────────────────────────────────

TEST_F(PortableInstallTest, APortableResolutionLeavesNoPerUserDirectoryBehind) {
    // The per-user layout is `<base>/<organization>/<application>` on every platform, so a sibling of a probe's
    // directory is where the per-user directory for another application of the same organization would be made.
    const fs::path probe = withoutTrailingSeparator(
        detail::userDataDirAt(app_, {.organization = "RetroppTest", .application = "PortableProbe"},
                              {.allowFile = false}));
    const fs::path wouldBe = probe.parent_path() / "PortableNever";
    removeQuietly(wouldBe);

    (void)detail::userDataDirAt(app_, {.organization = "RetroppTest", .application = "PortableNever"},
                                {.enabled = true});
    EXPECT_FALSE(fs::exists(wouldBe)) << wouldBe;

    removeQuietly(wouldBe);
    removeQuietly(probe);
}

TEST_F(PortableInstallTest, AnUnmakeablePortableDirectoryIsRefused) {
    // A file standing where the directory would go.
    std::ofstream{app_ / "PortableApp"} << "not a directory";
    EXPECT_THROW((void)detail::userDataDirAt(app_, kId, {.enabled = true}), SaveStoreError);

    // A platform that supplied no application directory has nowhere beside it.
    EXPECT_THROW((void)detail::userDataDirAt({}, kId, {.enabled = true}), SaveStoreError);

    // The per-user resolution never reads the application directory, so its absence costs it nothing.
    EXPECT_NO_THROW((void)detail::userDataDirAt({}, kId, {}));
}

// ── The bundle rule ─────────────────────────────────────────────────────────────────────────────

TEST_F(PortableInstallTest, TheBundleRuleAnswersTheDirectoryThatHoldsTheBundle) {
    EXPECT_EQ(detail::applicationDirFrom("/Applications/X.app/Contents/Resources/"), fs::path("/Applications"));
    EXPECT_EQ(detail::applicationDirFrom("/Applications/X.app/Contents/MacOS/"), fs::path("/Applications"));
    EXPECT_EQ(detail::applicationDirFrom("/Applications/X.app/"), fs::path("/Applications"));
    EXPECT_EQ(detail::applicationDirFrom("/Users/p/Downloads/K/K.app/Contents/Resources/"),
              fs::path("/Users/p/Downloads/K"));
    EXPECT_EQ(detail::applicationDirFrom("/opt/x/"), fs::path("/opt/x"));
    EXPECT_EQ(detail::applicationDirFrom("/opt/x"), fs::path("/opt/x"));
    // The extension here is ".app-tools", so the path is no bundle.
    EXPECT_EQ(detail::applicationDirFrom("/opt/x.app-tools/bin/"), fs::path("/opt/x.app-tools/bin"));
}

// ── The fan-out ─────────────────────────────────────────────────────────────────────────────────

TEST_F(PortableInstallTest, DefaultStoresFollowTheActivePlacement) {
    // Through the real application directory: the files land beside the test binary, and are removed after.
    SaveStore::defaultIdentity = {.organization = "RetroppTest", .application = "PortableStores"};
    PortableInstall::active    = {.enabled = true};

    const fs::path dir = userDataDir();
    EXPECT_EQ(dir.filename(), "PortableStores");
    EXPECT_EQ(SaveStore{}.basePath(), dir);
    EXPECT_EQ(UserFiles{}.root(), dir);

    SaveStore saves;
    const std::vector<std::byte> payload{std::byte{7}};
    ASSERT_TRUE(saves.write("slot1", 1, payload));
    EXPECT_TRUE(fs::is_regular_file(dir / "slot1"));

    const fs::path perUser = withoutTrailingSeparator(userDataDir(SaveStore::defaultIdentity, {.allowFile = false}));
    EXPECT_TRUE(outside(dir, perUser.parent_path())) << dir;

    removeQuietly(dir);
    removeQuietly(perUser);
}

TEST_F(PortableInstallTest, AnExplicitRootIsNotMoved) {
    PortableInstall::active = {.enabled = true};
    EXPECT_EQ(SaveStore::atPath(app_ / "x").basePath(), app_ / "x");
    EXPECT_EQ(UserFiles::atPath(app_ / "y").root(), app_ / "y");
}

TEST_F(PortableInstallTest, SetActiveSeedsThePlacement) {
    EngineConfig config;
    config.identity = {.organization = "Retro++", .application = "PortableInstallTest"};
    config.portable = {.enabled = true, .allowFile = true};
    EngineConfig::setActive(config);
    EXPECT_TRUE(PortableInstall::active.enabled);
    EXPECT_TRUE(PortableInstall::active.allowFile);

    EngineConfig defaults;
    defaults.identity = config.identity;
    config.portable = {.enabled = false, .allowFile = false};
    EngineConfig::setActive(config);
    EXPECT_FALSE(PortableInstall::active.enabled);
    EXPECT_FALSE(PortableInstall::active.allowFile);

    EngineConfig::setActive(defaults);
    EXPECT_FALSE(PortableInstall::active.enabled);
    EXPECT_TRUE(PortableInstall::active.allowFile);
}

TEST_F(PortableInstallTest, TheOneArgumentFormReadsTheActivePlacement) {
    PortableInstall::active = {.enabled = true};
    const fs::path portable = userDataDir(kId);
    EXPECT_EQ(portable.filename(), "PortableApp");
    EXPECT_EQ(portable, userDataDir(kId, {.enabled = true}));

    PortableInstall::active = {};
    EXPECT_EQ(userDataDir(kId), userDataDir(kId, {}));

    removeQuietly(portable);
}

}  // namespace
}  // namespace retropp

// UserFiles end-to-end, hermetic: every test roots its store at a unique directory under the system temp
// path (UserFiles::atPath — no SDL pref-path, no device, no display) and removes it afterward. Four
// layers:
//   * Byte transparency — a file on disk is EXACTLY the bytes written, with no envelope in front of it,
//     which is the whole difference from SaveStore and the reason another program can read it.
//   * Trees — a relative path descends, and write() creates the directories on the way.
//   * Containment — a path that could escape the store is refused before anything touches disk.
//   * The shared directory — a default-constructed store and a default-constructed SaveStore resolve the
//     same root, which is the property the whole surface exists to guarantee.
#include "retropp/user_files.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/save_store.h"

namespace retropp {
namespace {

std::vector<std::byte> bytesOf(const std::string& text) {
    const auto* p = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(p, p + text.size());
}

class UserFilesTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        base_ = std::filesystem::temp_directory_path() /
                (std::string("retropp-user-files-test-") + info->name());
        std::filesystem::remove_all(base_);
    }
    void TearDown() override { std::filesystem::remove_all(base_); }

    UserFiles files() const { return UserFiles::atPath(base_); }

    std::filesystem::path base_;
};

// ── Byte transparency ───────────────────────────────────────────────────────────────────────────

TEST_F(UserFilesTest, AFileOnDiskIsExactlyTheBytesWritten) {
    UserFiles f = files();
    const std::vector<std::byte> payload = bytesOf("\x89PNG\r\n\x1a\n decoded tile sheet");
    ASSERT_TRUE(f.write("assets/tiles.png", payload));

    // Read through the store...
    const auto back = f.read("assets/tiles.png");
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, payload);

    // ...and straight off the disk, which is the claim that matters: no envelope, no header, nothing a
    // second program would have to know about to open this file.
    std::ifstream in{base_ / "assets" / "tiles.png", std::ios::binary};
    const std::string raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    ASSERT_EQ(raw.size(), payload.size());
    EXPECT_EQ(0, std::memcmp(raw.data(), payload.data(), payload.size()));
}

TEST_F(UserFilesTest, AnEmptyFileRoundTrips) {
    UserFiles f = files();
    ASSERT_TRUE(f.write("empty.bin", {}));
    const auto back = f.read("empty.bin");
    ASSERT_TRUE(back.has_value());
    EXPECT_TRUE(back->empty());
    EXPECT_TRUE(f.exists("empty.bin"));  // present, just zero-length — not the same as absent
}

TEST_F(UserFilesTest, TheFullByteRangeSurvives) {
    UserFiles f = files();
    std::vector<std::byte> all(256);
    for (int i = 0; i < 256; ++i) all[static_cast<std::size_t>(i)] = static_cast<std::byte>(i);
    ASSERT_TRUE(f.write("cache/range.bin", all));
    EXPECT_EQ(*f.read("cache/range.bin"), all);
}

TEST_F(UserFilesTest, AWriteReplacesThePreviousContentsWhole) {
    UserFiles f = files();
    ASSERT_TRUE(f.write("log.txt", bytesOf("a much longer first version")));
    ASSERT_TRUE(f.write("log.txt", bytesOf("short")));
    // Not a prefix of the old contents: the replace is whole-file, so no tail survives.
    EXPECT_EQ(*f.read("log.txt"), bytesOf("short"));
}

// ── Trees ───────────────────────────────────────────────────────────────────────────────────────

TEST_F(UserFilesTest, WriteCreatesTheDirectoriesOnTheWay) {
    UserFiles f = files();
    ASSERT_FALSE(std::filesystem::exists(base_ / "assets" / "tiles"));
    ASSERT_TRUE(f.write("assets/tiles/overworld/00.png", bytesOf("x")));
    EXPECT_TRUE(std::filesystem::is_regular_file(base_ / "assets" / "tiles" / "overworld" / "00.png"));
}

TEST_F(UserFilesTest, PathForNamesTheFileWithoutTouchingTheDisk) {
    UserFiles f = files();
    EXPECT_EQ(f.pathFor("assets/tiles.png"), base_ / "assets" / "tiles.png");
    EXPECT_FALSE(std::filesystem::exists(base_ / "assets"));  // naming it created nothing
}

TEST_F(UserFilesTest, RootIsTheDirectoryAGameAssignsToAssetRoot) {
    EXPECT_EQ(files().root(), base_);
}

// ── Absence, removal ────────────────────────────────────────────────────────────────────────────

TEST_F(UserFilesTest, AnAbsentFileReadsAsNulloptRatherThanThrowing) {
    UserFiles f = files();
    EXPECT_FALSE(f.read("never/written.bin").has_value());
    EXPECT_FALSE(f.exists("never/written.bin"));
}

TEST_F(UserFilesTest, RemoveReportsWhetherThereWasAFile) {
    UserFiles f = files();
    ASSERT_TRUE(f.write("tmp.bin", bytesOf("x")));
    EXPECT_TRUE(f.remove("tmp.bin"));
    EXPECT_FALSE(f.remove("tmp.bin"));  // already gone
    EXPECT_FALSE(f.exists("tmp.bin"));
}

TEST_F(UserFilesTest, ADirectoryIsNotAFile) {
    UserFiles f = files();
    ASSERT_TRUE(f.write("assets/tiles.png", bytesOf("x")));
    // "assets" exists on disk but naming it must not read as a file the game can open.
    EXPECT_FALSE(f.exists("assets"));
    EXPECT_FALSE(f.read("assets").has_value());
}

// ── Containment ─────────────────────────────────────────────────────────────────────────────────

TEST_F(UserFilesTest, APathThatCouldEscapeTheStoreIsRefused) {
    UserFiles f = files();
    // Each of these would resolve outside base_. The throw happens in pathFor, so it fires before any of
    // the mutating calls touch the disk.
    for (const char* bad : {"../outside.bin", "assets/../../outside.bin", "a/./b.bin", "", "assets/"}) {
        EXPECT_THROW((void)f.pathFor(bad), std::invalid_argument) << bad;
        EXPECT_THROW((void)f.write(bad, {}), std::invalid_argument) << bad;
        EXPECT_THROW((void)f.read(bad), std::invalid_argument) << bad;
        EXPECT_THROW((void)f.remove(bad), std::invalid_argument) << bad;
    }
    // Nothing reached the disk: a refused path never even created the store's directory.
    EXPECT_TRUE(!std::filesystem::exists(base_) || std::filesystem::is_empty(base_));
}

TEST_F(UserFilesTest, AnAbsolutePathIsRefused) {
    UserFiles f = files();
    // Runs on every platform on purpose, and it is Windows that makes it interesting: there this is not
    // an ABSOLUTE path (that needs a drive), but appending it to the store's directory would drop the
    // store's root and land at the root of the drive. Refusing a root-directory covers both readings.
    EXPECT_THROW((void)f.pathFor("/etc/passwd"), std::invalid_argument);
#ifdef _WIN32
    // Drive-qualified and drive-relative: both name a location the store does not own.
    EXPECT_THROW((void)f.pathFor("C:\\Windows\\system.ini"), std::invalid_argument);
    EXPECT_THROW((void)f.pathFor("C:system.ini"), std::invalid_argument);
#endif
}

// ── The shared directory ────────────────────────────────────────────────────────────────────────

TEST(UserDataDir, RefusesAnIdentityTheDeveloperDidNotChoose) {
    // The same refusal SaveStore makes, for the same reason: every unconfigured program would otherwise
    // share one directory and overwrite each other's files.
    EXPECT_THROW((void)userDataDir(AppIdentity{}), SaveStoreError);
    EXPECT_THROW((void)userDataDir({.organization = "Org", .application = ""}), SaveStoreError);
    EXPECT_THROW((void)userDataDir({.organization = "", .application = "App"}), SaveStoreError);
}

TEST(UserDataDir, AStoreAndTheSavesResolveTheSameDirectory) {
    // The property the request was written for. Two resolutions that could drift would put a game's
    // extracted assets beside a DIFFERENT directory than its saves; there is only one resolution, so
    // this cannot come apart.
    const AppIdentity saved = SaveStore::defaultIdentity;
    SaveStore::defaultIdentity = {.organization = "RetroppTest", .application = "UserFilesShared"};

    const std::filesystem::path dir = userDataDir();
    EXPECT_EQ(UserFiles{}.root(), dir);
    EXPECT_EQ(userDataDir(SaveStore::defaultIdentity), dir);

    // The store's own answer for where it put itself, and a written document landing there — the
    // accessor and the disk agreeing is what rules out a resolution that drifted after construction.
    SaveStore saves;
    EXPECT_EQ(saves.basePath(), dir);
    const std::vector<std::byte> payload{std::byte{7}};
    ASSERT_TRUE(saves.write("slot1", 1, payload));
    EXPECT_TRUE(std::filesystem::is_regular_file(dir / "slot1"));

    SaveStore::defaultIdentity = saved;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);  // the resolution creates it; leave no test data behind
}

}  // namespace
}  // namespace retropp

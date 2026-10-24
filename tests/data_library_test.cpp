// The data family: bytes the engine stores, hands back by id, and never interprets.
//
// Two halves are under test here, and the second is the one that decides whether the feature works in a
// shipped binary. The SURFACE half is ordinary: bytes go in, the same bytes come out, ids are dense, a
// bad id and a missing file are reported rather than swallowed. The BUILD half is what the registration
// calls in this file are for — the calls themselves are the input to the build's asset scan, and what is
// asserted is what the build did with them. A registration form no scan reads bakes nothing, so an Embed
// path falls back to a disk read that works on a machine with the source tree and fails everywhere else.
//
// The most important case in the file is the one that asserts a registration with NO policy argument was
// never baked. The family's per-type default is LoadFromPath because a data asset is the one most likely
// to be derived from content a game cannot redistribute, and that promise is only worth anything if the
// build honors it without being asked.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "retropp/asset_policy.h"
#include "retropp/asset_registry.h"   // assetRoot / setAssetRoot, findEmbeddedAsset
#include "retropp/data_library.h"
#include "retropp/routine_registry.h"  // findEmbeddedRoutine — data must never land here

namespace retropp {
namespace {

// The fixture the build must BAKE: registered below with an explicit AssetPolicy::Embed token.
constexpr std::string_view kEmbedPath = "tests/fixtures/data/embedded.bin";
// The fixture the build must NOT bake: registered below with no policy argument at all. It exists in the
// source tree, so the scan does see it — being left unbaked is the default's doing, not a missing file's.
constexpr std::string_view kUnsetPolicyPath = "tests/fixtures/data/corpus.bin";

// tests/fixtures/data/embedded.bin, byte for byte.
constexpr std::uint8_t kEmbedBytes[] = {0xD1, 0xA7, 0x00, 0x5B, 0xFF, 0x10, 0x42, 0xE9};

// ── The surface ───────────────────────────────────────────────────────────────────────────────────

TEST(DataLibrary, UploadedBytesAreCopiedIntoTheLibrary) {
    // The registration promise: the caller's buffer need not outlive the call. Hand over a buffer, destroy
    // it, and the library still answers with the original bytes.
    DataId id{};
    {
        const std::vector<std::uint8_t> transient{0x11, 0x22, 0x33, 0x44};
        id = DataLibrary::instance().uploadData(transient);
    }
    const std::span<const std::uint8_t> bytes = DataLibrary::instance().data(id);
    ASSERT_EQ(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x11u);
    EXPECT_EQ(bytes[1], 0x22u);
    EXPECT_EQ(bytes[2], 0x33u);
    EXPECT_EQ(bytes[3], 0x44u);
}

TEST(DataLibrary, IdsAreDenseAndAscendingFromZero) {
    // The handle contract every family shares: size() is also the next id to be minted, so two
    // registrations in a row take consecutive values.
    DataLibrary&      library = DataLibrary::instance();
    const std::size_t before  = library.size();
    const std::uint8_t byte   = 0x01;
    const DataId       first  = library.uploadData(std::span(&byte, 1));
    const DataId       second = library.uploadData(std::span(&byte, 1));
    EXPECT_EQ(static_cast<std::size_t>(first), before);
    EXPECT_EQ(static_cast<std::size_t>(second), before + 1);
    EXPECT_EQ(library.size(), before + 2);
}

TEST(DataLibrary, DataThrowsOnAnIdTheLibraryNeverMinted) {
    // An id past the end is a programming error, not an empty result — an empty span would decode to
    // nothing and look like a legitimately empty asset.
    const auto beyond = static_cast<DataId>(DataLibrary::instance().size() + 1000);
    EXPECT_THROW((void)DataLibrary::instance().data(beyond), std::out_of_range);
}

TEST(DataLibrary, TheSameIdReturnsTheSameSpan) {
    // Resolution is idempotent. A consumer keeps a decoded view over these bytes for the life of the
    // program, so a second call moving the span would leave that view pointing at nothing.
    const std::vector<std::uint8_t> payload{0xAB, 0xCD};
    const DataId                    id = DataLibrary::instance().uploadData(payload);
    const std::span<const std::uint8_t> first  = DataLibrary::instance().data(id);
    const std::span<const std::uint8_t> second = DataLibrary::instance().data(id);
    EXPECT_EQ(first.data(), second.data());
    EXPECT_EQ(first.size(), second.size());
}

// ── What the build did with the registrations in this file ────────────────────────────────────────

TEST(DataAssetScan, AnEmbedDataFileIsBakedIntoTheBinary) {
    // The registration call this asserts on is at the bottom of the file. If the build's asset scan does
    // not know the registration form, nothing is baked for the path and this is the case that says so —
    // rather than the program discovering it wherever the source tree is absent.
    const std::span<const std::uint8_t> baked = detail::findEmbeddedAsset(kEmbedPath);
    ASSERT_FALSE(baked.empty()) << "an explicit AssetPolicy::Embed data path must be baked by the build";
    ASSERT_EQ(baked.size(), std::size(kEmbedBytes));
    for (std::size_t i = 0; i < std::size(kEmbedBytes); ++i) {
        EXPECT_EQ(baked[i], kEmbedBytes[i]) << "baked byte " << i;
    }
}

TEST(DataAssetScan, AnUnsetPolicyIsNeverBaked) {
    // The case that matters most. Data is the family a game is most likely to derive from content it
    // cannot redistribute, so a registration that names no policy must ship the file rather than bake it.
    // The file exists in the source tree, so the scan resolved a policy for it and chose to copy — this
    // is the default's doing, not a skipped file's.
    EXPECT_TRUE(detail::findEmbeddedAsset(kUnsetPolicyPath).empty())
        << "registerData with no policy argument must default to LoadFromPath and never bake";
}

TEST(DataAssetScan, AnEmbedDataFileLandsInTheAssetRegistryOnly) {
    // Data is raw bytes and reads back from the asset registry. The routine registry holds assembled
    // bytecode; bytes baked into it would be invisible to data() even though they are in the binary.
    EXPECT_TRUE(detail::findEmbeddedRoutine(kEmbedPath).empty());
    EXPECT_TRUE(detail::findEmbeddedRoutine(kUnsetPolicyPath).empty());
}

// ── Reading a LoadFromPath file ───────────────────────────────────────────────────────────────────

// The asset root is process-global, so it is saved and restored around every case here. These logical
// paths name no file under the project root, so the build scan skips them entirely (its documented
// behavior for an absent file) and this fixture owns the only copy on disk.
class DataLibraryDisk : public ::testing::Test {
protected:
    void SetUp() override {
        previousRoot_ = assetRoot();
        root_         = std::filesystem::temp_directory_path() /
                ("retropp-data-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                 "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_ / "data");
        setAssetRoot(root_);
    }

    void TearDown() override {
        setAssetRoot(previousRoot_);
        std::filesystem::remove_all(root_);
    }

    void writeFile(const std::filesystem::path& relative, std::span<const std::uint8_t> bytes) const {
        std::ofstream out{root_ / relative, std::ios::binary};
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    std::filesystem::path root_;
    std::filesystem::path previousRoot_;
};

TEST_F(DataLibraryDisk, ALoadFromPathFileIsReadFromTheAssetRoot) {
    constexpr std::uint8_t kPayload[] = {0x7A, 0x00, 0x91, 0x03};
    writeFile("data/on_disk.bin", kPayload);

    const DataId id = DataLibrary::instance().registerData("data/on_disk.bin", AssetPolicy::LoadFromPath);
    const std::span<const std::uint8_t> bytes = DataLibrary::instance().data(id);
    ASSERT_EQ(bytes.size(), std::size(kPayload));
    for (std::size_t i = 0; i < std::size(kPayload); ++i) {
        EXPECT_EQ(bytes[i], kPayload[i]) << "byte " << i;
    }
}

TEST_F(DataLibraryDisk, AMissingLoadFromPathFileThrowsNamingIt) {
    // A game whose corpus did not ship needs the path in the message; an empty span would decode to
    // nothing and give it no way to tell which file went missing.
    const DataId id = DataLibrary::instance().registerData("data/never_written.bin");
    try {
        (void)DataLibrary::instance().data(id);
        FAIL() << "a missing LoadFromPath data file must throw";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("never_written.bin"), std::string::npos)
            << "the message must name the file: " << e.what();
    }
}

TEST_F(DataLibraryDisk, AResolvedEntryIsCachedNotReRead) {
    // The lifetime promise: resolved once, held for the program. Delete the file after the first call and
    // the second still answers — which is what lets a consumer keep a decoded view over the span.
    constexpr std::uint8_t kPayload[] = {0x5E, 0x5E};
    writeFile("data/vanishes.bin", kPayload);

    const DataId id = DataLibrary::instance().registerData("data/vanishes.bin");
    ASSERT_EQ(DataLibrary::instance().data(id).size(), std::size(kPayload));

    std::filesystem::remove(root_ / "data/vanishes.bin");
    const std::span<const std::uint8_t> again = DataLibrary::instance().data(id);
    ASSERT_EQ(again.size(), std::size(kPayload));
    EXPECT_EQ(again[0], 0x5Eu);
}

// ── The registrations the build scan reads ────────────────────────────────────────────────────────
//
// These two calls exist so the scan has something to find in this file; the assertions about them are the
// DataAssetScan cases above. They run through a test that registers and immediately reads, so the runtime
// half of the Embed path is exercised too rather than only the build half.

TEST(DataAssetScan, AnEmbedRegistrationResolvesToItsBakedBytes) {
    const DataId id = DataLibrary::instance().registerData("tests/fixtures/data/embedded.bin",
                                                           AssetPolicy::Embed);
    const std::span<const std::uint8_t> bytes = DataLibrary::instance().data(id);
    ASSERT_EQ(bytes.size(), std::size(kEmbedBytes));
    EXPECT_EQ(bytes[0], kEmbedBytes[0]);
    EXPECT_EQ(bytes[7], kEmbedBytes[7]);
    // Resolved from inside the binary, not from disk: the span points into the baked array itself.
    EXPECT_EQ(bytes.data(), detail::findEmbeddedAsset(kEmbedPath).data());
}

TEST(DataAssetScan, AnUnsetPolicyRegistrationIsScannedAndCopiedNotBaked) {
    // The registration the AnUnsetPolicyIsNeverBaked case asserts on. No policy argument, deliberately.
    const DataId id = DataLibrary::instance().registerData("tests/fixtures/data/corpus.bin");
    EXPECT_LT(static_cast<std::size_t>(id), DataLibrary::instance().size());
    EXPECT_TRUE(detail::findEmbeddedAsset(kUnsetPolicyPath).empty());
}

}  // namespace
}  // namespace retropp

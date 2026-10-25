// The SaveStore end-to-end, hermetic: every test roots its store at a unique directory under
// the system temp path (SaveStore::atPath — no SDL pref-path, no device, no display) and
// removes it afterward. Four layers:
//   * Byte transparency — write/read round-trips return the exact payload bytes and schema
//     version, for empty, small, and multi-KB payloads over the full byte range.
//   * Durability — the atomic-commit guarantee: a write severed before its rename commit
//     leaves the prior document intact and no temp debris; the crash-safety gate.
//   * Trust — absent reads as std::nullopt; corrupt/truncated/too-new reads THROW. The two
//     conditions never share a signal (a corrupt save must not read as "no save").
//   * Versioning — the envelope's exact on-disk layout (endian-stable), and the migration
//     walk: steps applied in order, gaps and newer-than-current rejected.
#include "retropp/save_store.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/engine_config.h"

namespace retropp {

namespace detail {
// The crash seam: sever a write between its flushed temp file and its rename commit — the
// exact moment a crash would have to not corrupt the store. Befriended by SaveStore.
struct SaveStoreTestAccess {
    static void setPreCommitHook(SaveStore& store, std::function<void()> hook) {
        store.preCommitHook_ = std::move(hook);
    }
};
}  // namespace detail

namespace {

std::vector<std::byte> bytesOf(const std::string& text) {
    const auto* p = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(p, p + text.size());
}

std::string textOf(const std::vector<std::byte>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// Each test gets its own store directory under the system temp path, created fresh and
// removed on teardown, so tests are isolated from each other and from any real save data.
class SaveStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        base_ = std::filesystem::temp_directory_path() /
                (std::string("retropp-save-store-test-") + info->name());
        std::filesystem::remove_all(base_);
    }
    void TearDown() override { std::filesystem::remove_all(base_); }

    SaveStore store() const { return SaveStore::atPath(base_); }

    // The raw on-disk bytes of a named document — for asserting the envelope layout.
    std::vector<std::byte> rawFile(const std::string& name) const {
        std::ifstream in(base_ / name, std::ios::binary);
        std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        const auto* p = reinterpret_cast<const std::byte*>(chars.data());
        return std::vector<std::byte>(p, p + chars.size());
    }

    void writeRawFile(const std::string& name, const std::vector<std::byte>& bytes) const {
        std::filesystem::create_directories(base_);
        std::ofstream out(base_ / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    bool anyTempFiles() const {
        if (!std::filesystem::exists(base_)) return false;
        for (const auto& entry : std::filesystem::directory_iterator(base_)) {
            if (entry.path().extension() == ".tmp") return true;
        }
        return false;
    }

    std::filesystem::path base_;
};

// ── Byte transparency ────────────────────────────────────────────────────────────────────

TEST_F(SaveStoreTest, RoundTripReturnsExactPayloadAndVersion) {
    SaveStore s = store();
    ASSERT_TRUE(s.write("slot", 7, bytesOf("hello bytes")));
    const auto doc = s.read("slot");
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->schemaVersion, 7u);
    EXPECT_EQ(textOf(doc->payload), "hello bytes");
}

TEST_F(SaveStoreTest, RoundTripEmptyPayload) {
    SaveStore s = store();
    ASSERT_TRUE(s.write("empty", 1, {}));
    const auto doc = s.read("empty");
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->schemaVersion, 1u);
    EXPECT_TRUE(doc->payload.empty());
}

TEST_F(SaveStoreTest, RoundTripMultiKbFullByteRange) {
    // Every byte value appears, including 0x00 and 0xFF, across a multi-KB buffer — the
    // store must be transparent to arbitrary binary content.
    std::vector<std::byte> payload(8192);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = std::byte(i * 31 + 7);
    SaveStore s = store();
    ASSERT_TRUE(s.write("big", 3, payload));
    const auto doc = s.read("big");
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->payload, payload);
}

TEST_F(SaveStoreTest, OverwriteReplacesTheDocument) {
    SaveStore s = store();
    ASSERT_TRUE(s.write("slot", 1, bytesOf("first")));
    ASSERT_TRUE(s.write("slot", 2, bytesOf("second")));
    const auto doc = s.read("slot");
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->schemaVersion, 2u);
    EXPECT_EQ(textOf(doc->payload), "second");
}

// ── Durability: the atomic commit ────────────────────────────────────────────────────────

TEST_F(SaveStoreTest, SeveredWriteLeavesPriorDocumentIntact) {
    SaveStore s = store();
    ASSERT_TRUE(s.write("slot", 1, bytesOf("the good save")));

    // Sever the second write at the pre-commit moment: the temp file is written and
    // flushed, the rename never happens — the crash window the design closes.
    detail::SaveStoreTestAccess::setPreCommitHook(s, [] { throw std::runtime_error("crash"); });
    EXPECT_FALSE(s.write("slot", 2, bytesOf("the doomed save")));

    detail::SaveStoreTestAccess::setPreCommitHook(s, nullptr);
    const auto doc = s.read("slot");
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->schemaVersion, 1u);
    EXPECT_EQ(textOf(doc->payload), "the good save");
    EXPECT_FALSE(anyTempFiles());  // no severed-write debris, nothing masquerading as a save
}

TEST_F(SaveStoreTest, SeveredFirstWriteLeavesNoDocument) {
    SaveStore s = store();
    detail::SaveStoreTestAccess::setPreCommitHook(s, [] { throw std::runtime_error("crash"); });
    EXPECT_FALSE(s.write("slot", 1, bytesOf("never lands")));

    detail::SaveStoreTestAccess::setPreCommitHook(s, nullptr);
    EXPECT_FALSE(s.exists("slot"));
    EXPECT_FALSE(s.read("slot").has_value());
    EXPECT_FALSE(anyTempFiles());
}

// ── Trust: absent vs corrupt never share a signal ────────────────────────────────────────

TEST_F(SaveStoreTest, AbsentDocumentReadsAsNullopt) {
    EXPECT_FALSE(store().read("never-written").has_value());
}

TEST_F(SaveStoreTest, CorruptMagicIsAHardError) {
    SaveStore s = store();
    ASSERT_TRUE(s.write("slot", 1, bytesOf("data")));
    auto bytes = rawFile("slot");
    bytes[0] = std::byte{'X'};  // not a save document any more
    writeRawFile("slot", bytes);
    EXPECT_THROW((void)s.read("slot"), SaveStoreError);
}

TEST_F(SaveStoreTest, TruncatedHeaderIsAHardError) {
    writeRawFile("slot", std::vector<std::byte>(10, std::byte{0x41}));
    EXPECT_THROW((void)store().read("slot"), SaveStoreError);
}

TEST_F(SaveStoreTest, TruncatedPayloadIsAHardError) {
    SaveStore s = store();
    ASSERT_TRUE(s.write("slot", 1, bytesOf("a payload that will lose its tail")));
    auto bytes = rawFile("slot");
    bytes.resize(bytes.size() - 5);  // the envelope now promises more bytes than exist
    writeRawFile("slot", bytes);
    EXPECT_THROW((void)s.read("slot"), SaveStoreError);
}

TEST_F(SaveStoreTest, UnknownContainerVersionIsAHardError) {
    SaveStore s = store();
    ASSERT_TRUE(s.write("slot", 1, bytesOf("data")));
    auto bytes = rawFile("slot");
    bytes[4] = std::byte{99};  // a container layout this store does not read
    writeRawFile("slot", bytes);
    EXPECT_THROW((void)s.read("slot"), SaveStoreError);
}

TEST_F(SaveStoreTest, InvalidDocumentNamesAreRejected) {
    SaveStore s = store();
    const std::vector<std::byte> payload = bytesOf("x");
    for (const char* name : {"", ".", "..", "a/b", "a\\b", "../escape", "c:evil"}) {
        EXPECT_THROW((void)s.write(name, 1, payload), std::invalid_argument) << name;
        EXPECT_THROW((void)s.read(name), std::invalid_argument) << name;
    }
}

// ── Versioning: the envelope layout ──────────────────────────────────────────────────────

TEST_F(SaveStoreTest, EnvelopeLayoutIsExactlyAsSpecified) {
    // The header is fixed little-endian regardless of host: magic "RPSV", container
    // version 1, the consumer schemaVersion, the payload length as a u64, then the payload.
    SaveStore s = store();
    ASSERT_TRUE(s.write("slot", 0x11223344u, bytesOf("AB")));
    const std::vector<std::byte> expected = {
        std::byte{'R'},  std::byte{'P'},  std::byte{'S'},  std::byte{'V'},   // magic
        std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},  // container v1
        std::byte{0x44}, std::byte{0x33}, std::byte{0x22}, std::byte{0x11},  // schema (LE)
        std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},  // length (LE u64)
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{'A'},  std::byte{'B'},                                     // payload
    };
    EXPECT_EQ(rawFile("slot"), expected);
}

TEST_F(SaveStoreTest, HandCraftedEnvelopeReadsBack) {
    // The inverse direction: bytes laid out by hand to the specified format read as a
    // document — the layout is a contract, not an implementation detail.
    const std::vector<std::byte> file = {
        std::byte{'R'},  std::byte{'P'},  std::byte{'S'},  std::byte{'V'},
        std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x05}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{'x'},  std::byte{'y'},  std::byte{'z'},
    };
    writeRawFile("slot", file);
    const auto doc = store().read("slot");
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->schemaVersion, 5u);
    EXPECT_EQ(textOf(doc->payload), "xyz");
}

// ── Versioning: the migration walk ───────────────────────────────────────────────────────

TEST_F(SaveStoreTest, MigrationChainWalksStepsInOrder) {
    SaveStore s = store();
    ASSERT_TRUE(s.write("slot", 1, bytesOf("one")));
    s.setCurrentVersion(3);
    s.registerMigration(1, [](std::vector<std::byte> p) { return bytesOf(textOf(p) + "-two"); });
    s.registerMigration(2, [](std::vector<std::byte> p) { return bytesOf(textOf(p) + "-three"); });
    const auto doc = s.read("slot");
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->schemaVersion, 3u);
    EXPECT_EQ(textOf(doc->payload), "one-two-three");
}

TEST_F(SaveStoreTest, MigrationGapIsAHardError) {
    SaveStore s = store();
    ASSERT_TRUE(s.write("slot", 1, bytesOf("one")));
    s.setCurrentVersion(3);
    s.registerMigration(1, [](std::vector<std::byte> p) { return p; });  // 2→3 missing
    EXPECT_THROW((void)s.read("slot"), SaveStoreError);
}

TEST_F(SaveStoreTest, NewerThanCurrentIsAHardError) {
    // A document from a newer build than the running code: refuse, never guess.
    SaveStore s = store();
    ASSERT_TRUE(s.write("slot", 5, bytesOf("from the future")));
    s.setCurrentVersion(3);
    EXPECT_THROW((void)s.read("slot"), SaveStoreError);
}

TEST_F(SaveStoreTest, NoDeclaredCurrentVersionReadsVerbatim) {
    SaveStore s = store();
    ASSERT_TRUE(s.write("slot", 9, bytesOf("as stored")));
    const auto doc = s.read("slot");  // no setCurrentVersion — every version passes through
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->schemaVersion, 9u);
    EXPECT_EQ(textOf(doc->payload), "as stored");
}

TEST_F(SaveStoreTest, DocumentAlreadyCurrentSkipsMigrations) {
    SaveStore s = store();
    ASSERT_TRUE(s.write("slot", 2, bytesOf("current")));
    s.setCurrentVersion(2);
    s.registerMigration(1, [](std::vector<std::byte>) {
        return bytesOf("a step that must not run");
    });
    const auto doc = s.read("slot");
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(textOf(doc->payload), "current");
}

// ── Identity is required ─────────────────────────────────────────────────────────────────

TEST_F(SaveStoreTest, DefaultConstructionRefusesAnUnsetIdentity) {
    // Every platform requires an explicit application identity; the store enforces the same
    // before anything touches disk. No fallback name exists — a fallback would resolve every
    // unconfigured game to one shared directory. Assigned directly (not via setActive) so the
    // check stays local to this test; restored afterward.
    const AppIdentity saved = SaveStore::defaultIdentity;
    SaveStore::defaultIdentity = {};
    EXPECT_THROW(SaveStore{}, SaveStoreError);
    SaveStore::defaultIdentity = {.organization = "Org", .application = ""};
    EXPECT_THROW(SaveStore{}, SaveStoreError);
    SaveStore::defaultIdentity = {.organization = "", .application = "App"};
    EXPECT_THROW(SaveStore{}, SaveStoreError);
    SaveStore::defaultIdentity = saved;
}

TEST_F(SaveStoreTest, SetActiveSeedsTheDefaultIdentity) {
    // The identity rides the one startup call, like the other per-type defaults. Restored by
    // direct assignment (not a second setActive) so the restore is exact even when the saved
    // config predates any setActive call.
    const EngineConfig savedConfig = EngineConfig::active;
    const AppIdentity savedIdentity = SaveStore::defaultIdentity;
    EngineConfig config;
    config.identity = {.organization = "TestOrg", .application = "TestApp"};
    EngineConfig::setActive(config);
    EXPECT_EQ(SaveStore::defaultIdentity.organization, "TestOrg");
    EXPECT_EQ(SaveStore::defaultIdentity.application, "TestApp");
    EngineConfig::active = savedConfig;
    SaveStore::defaultIdentity = savedIdentity;
}

// ── Store independence ───────────────────────────────────────────────────────────────────

TEST_F(SaveStoreTest, StoresAtDifferentBasesAreIsolated) {
    SaveStore a = SaveStore::atPath(base_ / "a");
    SaveStore b = SaveStore::atPath(base_ / "b");
    ASSERT_TRUE(a.write("slot", 1, bytesOf("a's document")));
    EXPECT_FALSE(b.exists("slot"));
    EXPECT_FALSE(b.read("slot").has_value());
    ASSERT_TRUE(b.write("slot", 1, bytesOf("b's document")));
    EXPECT_EQ(textOf(a.read("slot")->payload), "a's document");
    EXPECT_EQ(textOf(b.read("slot")->payload), "b's document");
}

TEST_F(SaveStoreTest, ExistsAndRemove) {
    SaveStore s = store();
    EXPECT_FALSE(s.exists("slot"));
    EXPECT_FALSE(s.remove("slot"));  // nothing to remove
    ASSERT_TRUE(s.write("slot", 1, bytesOf("here")));
    EXPECT_TRUE(s.exists("slot"));
    EXPECT_TRUE(s.remove("slot"));
    EXPECT_FALSE(s.exists("slot"));
    EXPECT_FALSE(s.read("slot").has_value());
}

}  // namespace
}  // namespace retropp

// The sample cache's arithmetic, with no machine: where whole samples sit in the SNES's audio RAM, which
// directory entries play them, which give way when room or an entry is needed, and how the echo ring's
// base moves the ceiling. Pure bookkeeping — every address here is an audio-RAM offset the sampler would
// write to.

#include <cstdint>

#include <gtest/gtest.h>

#include "retropp/audio_library.h"  // AudioId
#include "src/audio/snes/default_driver.h"
#include "src/audio/snes/sample_cache.h"

namespace retropp {
namespace {

namespace sd = audio::snesdriver;

constexpr AudioId id(unsigned n) { return static_cast<AudioId>(n); }

constexpr std::uint16_t kStart = sd::kSamplesStart;

TEST(SnesSampleCache, PlacesLowestFirstAndReusesAGap) {
    sd::SampleCache cache;
    cache.lowerLimit(static_cast<std::uint16_t>(kStart + 300));  // room for three 100-byte samples

    const auto a = cache.place(id(0), 100, /*cueNow=*/0);
    const auto b = cache.place(id(1), 100, 1);
    const auto c = cache.place(id(2), 100, 2);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(c.has_value());
    cache.hold(id(0));  // A and C held; B is the only sample the cache may drop
    cache.hold(id(2));

    EXPECT_EQ(a->start, kStart);
    EXPECT_EQ(b->start, kStart + 100);
    EXPECT_EQ(c->start, kStart + 200);
    EXPECT_EQ(a->entry, sd::kVoices);      // the first sample entry past the eight windows'
    EXPECT_EQ(b->entry, sd::kVoices + 1);
    EXPECT_EQ(c->entry, sd::kVoices + 2);

    // A fourth sample needs B's room; B gives way and the fourth lands in its gap, recycling its entry.
    const auto d = cache.place(id(3), 100, 3);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->start, kStart + 100);
    EXPECT_EQ(d->entry, sd::kVoices + 1);
    EXPECT_FALSE(cache.placementOf(id(1)).has_value());  // B dropped
    EXPECT_TRUE(cache.placementOf(id(0)).has_value());   // A held, survives
    EXPECT_TRUE(cache.placementOf(id(2)).has_value());   // C held, survives
}

TEST(SnesSampleCache, EvictsTheStalestHeldFreeSpan) {
    sd::SampleCache cache;
    cache.lowerLimit(static_cast<std::uint16_t>(kStart + 500));

    ASSERT_TRUE(cache.place(id(0), 100, /*cueNow=*/10).has_value());  // left span, most recent cue 11
    ASSERT_TRUE(cache.place(id(1), 100, 11).has_value());
    ASSERT_TRUE(cache.place(id(2), 100, 5).has_value());              // held, splits the region
    cache.hold(id(2));
    ASSERT_TRUE(cache.place(id(3), 100, 1).has_value());              // right span, staler than the left
    ASSERT_TRUE(cache.place(id(4), 100, 2).has_value());

    const auto n = cache.place(id(5), 150, /*cueNow=*/20);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->start, kStart + 300);                   // the right (staler) span's base
    EXPECT_TRUE(cache.placementOf(id(0)).has_value());   // the left span survives
    EXPECT_TRUE(cache.placementOf(id(1)).has_value());
    EXPECT_TRUE(cache.placementOf(id(2)).has_value());   // the held sample survives
    EXPECT_FALSE(cache.placementOf(id(3)).has_value());  // the right span gave way
    EXPECT_FALSE(cache.placementOf(id(4)).has_value());
}

TEST(SnesSampleCache, AHeldPlacementIsNeverEvicted) {
    sd::SampleCache cache;
    cache.lowerLimit(static_cast<std::uint16_t>(kStart + 300));

    ASSERT_TRUE(cache.place(id(0), 100, 0).has_value());
    ASSERT_TRUE(cache.place(id(1), 100, 1).has_value());
    ASSERT_TRUE(cache.place(id(2), 100, 2).has_value());
    cache.hold(id(0));
    cache.hold(id(1));
    cache.hold(id(2));

    const auto d = cache.place(id(3), 100, 3);  // nothing the cache may drop — the file streams
    EXPECT_FALSE(d.has_value());
    EXPECT_TRUE(cache.placementOf(id(0)).has_value());
    EXPECT_TRUE(cache.placementOf(id(1)).has_value());
    EXPECT_TRUE(cache.placementOf(id(2)).has_value());
}

TEST(SnesSampleCache, TheDirectoryRecyclesAndAnEmptyFreeListEvicts) {
    sd::SampleCache cache;
    const unsigned  sampleEntries = sd::kDirectorySlots - sd::kVoices;  // the entries the cache hands out
    for (unsigned i = 0; i < sampleEntries; ++i) {
        ASSERT_TRUE(cache.place(id(i), 9, /*cueNow=*/i).has_value()) << "placing sample " << i;
    }
    cache.hold(id(1));  // one sample held — its entry must never be handed out

    // Every sample entry is taken; a new sample frees one by dropping the stalest unheld span (sample 0).
    const auto fresh = cache.place(id(sampleEntries), 9, /*cueNow=*/sampleEntries);
    ASSERT_TRUE(fresh.has_value());
    EXPECT_EQ(fresh->entry, sd::kVoices);                          // sample 0's recycled entry
    EXPECT_FALSE(cache.placementOf(id(0)).has_value());           // the stalest, dropped
    EXPECT_TRUE(cache.placementOf(id(1)).has_value());            // held — entry untouched
    EXPECT_TRUE(cache.placementOf(id(2)).has_value());            // not in the dropped span
}

TEST(SnesSampleCache, TheLimitMovesDownAndWhatIsAboveItGoes) {
    sd::SampleCache cache;
    ASSERT_TRUE(cache.place(id(0), 100, 0).has_value());  // at the base, below any lowering
    ASSERT_TRUE(cache.place(id(1), 100, 1).has_value());  // held, straddles the new ceiling
    cache.hold(id(1));
    ASSERT_TRUE(cache.place(id(2), 100, 2).has_value());  // unheld, above the new ceiling

    const std::uint16_t base = static_cast<std::uint16_t>(kStart + 150);
    const bool          blocked = cache.lowerLimit(base);
    EXPECT_TRUE(blocked);                                  // a held sample reaches above the new base
    EXPECT_EQ(cache.limit(), base);                        // the ceiling moves; the ring's chip move waits
    EXPECT_FALSE(cache.placementOf(id(2)).has_value());    // the unheld one above gave way
    EXPECT_TRUE(cache.placementOf(id(1)).has_value());     // the held one above survives
    EXPECT_TRUE(cache.placementOf(id(0)).has_value());

    cache.release(id(1));
    const bool blockedNow = cache.lowerLimit(base);        // the retry when the voice has released
    EXPECT_FALSE(blockedNow);
    EXPECT_FALSE(cache.placementOf(id(1)).has_value());    // now unheld, it gives way
    EXPECT_EQ(cache.limit(), base);
}

TEST(SnesSampleCache, AnEndingEntryReturnsWithItsSample) {
    sd::SampleCache cache;
    cache.lowerLimit(static_cast<std::uint16_t>(kStart + 100));  // room for exactly one 100-byte sample

    const auto a = cache.place(id(0), 100, 0);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->entry, sd::kVoices);
    const std::uint8_t ending = cache.endingEntry(id(0));
    EXPECT_EQ(ending, sd::kVoices + 1);  // a second entry for the pass-ending directory slot

    // A new sample needs the full region; sample 0 gives way, returning BOTH of its entries.
    const auto b = cache.place(id(1), 50, 1);
    ASSERT_TRUE(b.has_value());
    EXPECT_FALSE(cache.placementOf(id(0)).has_value());
    EXPECT_EQ(b->entry, sd::kVoices);                        // sample 0's first entry, recycled
    EXPECT_EQ(cache.endingEntry(id(1)), sd::kVoices + 1);    // and its ending entry, recycled
}

}  // namespace
}  // namespace retropp

#ifndef RETROPP_SRC_AUDIO_SNES_SAMPLE_CACHE_H
#define RETROPP_SRC_AUDIO_SNES_SAMPLE_CACHE_H

// The sample cache: the arithmetic of where whole samples sit in the SNES's audio RAM and which directory
// entries play them, free of any machine. The system's sampler owns one; it decides placement and eviction
// here, then writes the bytes to the machine itself.
//
// The cache owns the region [kSamplesStart, limit): `limit` is the echo ring's base, which moves down as
// the ring grows and up as it shrinks, and the no-path ceiling (kSamplesEnd) before a path is set. It owns
// the directory's sample entries, kVoices..kDirectorySlots-1 — entries 0..kVoices-1 are the stream windows',
// managed by the sampler. A whole sample is placed lowest-address-first in a free gap; when no gap fits,
// the stalest span that holds no voice's sample gives way, the samples in it dropped so their room and
// entries return. A sample a voice is playing is held: it is never evicted and the ring never grows over
// it. A placement's bytes stay in audio RAM after it is dropped, until the next placement overwrites them —
// the caller keeps the encoded source and reloads it on the next cue.
//
// INTERNAL — under src/audio/, never include/retropp/.

#include <cstdint>
#include <optional>
#include <vector>

#include "retropp/audio_library.h"  // AudioId
#include "src/audio/snes/default_driver.h"

namespace retropp::audio::snesdriver {

class SampleCache {
public:
    // The ceiling at the no-path top (kSamplesEnd); every sample entry free.
    SampleCache();

    // Where a whole sample's bytes and its directory entry go.
    struct Placement {
        std::uint16_t start = 0;
        std::uint8_t  entry = 0;
    };

    // Place `id`'s whole sample of `size` bytes and answer where, or answer nothing when it cannot be
    // placed (the caller delivers the file through the voice's window instead). `cueNow` is the system's
    // cue counter, kept as the placement's recency. An id already placed answers its placement unchanged,
    // its recency refreshed. Unheld placements are dropped — the stalest span first — to open a gap, or to
    // free a directory entry when every one is taken.
    [[nodiscard]] std::optional<Placement> place(AudioId id, std::uint16_t size, std::uint64_t cueNow);

    // The directory entry that ends `id`'s pass on the silent sample, taken the first time a pass plays it
    // through and the same entry every time after. `id` must be placed. Drops the stalest unheld span to
    // free an entry when every one is taken — which always yields, since at most two entries per voice are
    // ever held.
    [[nodiscard]] std::uint8_t endingEntry(AudioId id);

    // Where `id`'s whole sample sits, or nothing when it is not held whole (never placed, streamed, or
    // dropped).
    [[nodiscard]] std::optional<std::uint16_t> placementOf(AudioId id) const;

    // Mark / unmark `id` as held by a voice. A held sample is never dropped and the ring never grows over
    // it. Held is a count — the same sample cued on two voices is held twice. hold() of an id that is not
    // placed, and release() of one not held, are no-ops.
    void hold(AudioId id);
    void release(AudioId id);

    // Lower the ceiling to `base` (the echo ring grown downward): drop every unheld placement reaching
    // above `base`, and answer whether a held one still does — the caller's signal that the ring's move on
    // the chip waits on that voice. `limit()` becomes `base` regardless, so nothing new is placed above it.
    bool lowerLimit(std::uint16_t base);

    // Raise the ceiling to `base` (the echo ring shrunk): the freed room below `base` is available again.
    void raiseLimit(std::uint16_t base);

    // The cache's current ceiling — the ring's base, or the no-path top before a path is set.
    [[nodiscard]] std::uint16_t limit() const noexcept { return limit_; }

    // A bit per voice is NOT the cache's to know; held is counted per sample. These two answer the tests
    // and the ring protocol: whether any placement is held above `base`, and how many samples are placed.
    [[nodiscard]] bool anyHeldAbove(std::uint16_t base) const;
    [[nodiscard]] std::size_t placedCount() const noexcept { return slots_.size(); }

private:
    // One placed whole sample.
    struct Slot {
        AudioId                     id{};
        std::uint16_t               start   = 0;
        std::uint16_t               size    = 0;
        std::uint8_t                entry   = 0;
        std::optional<std::uint8_t> ending;         // the pass-ending entry, once a pass has played it through
        std::uint64_t               lastCue = 0;     // the cue counter at its most recent placement/cue
        unsigned                    held    = 0;     // voices holding it right now
    };

    [[nodiscard]] Slot*       find(AudioId id);
    [[nodiscard]] const Slot* find(AudioId id) const;

    // The lowest free gap of at least `size` bytes in [kSamplesStart, limit_), or nothing.
    [[nodiscard]] std::optional<std::uint16_t> lowestGap(std::uint16_t size) const;

    // Drop the stalest unheld span that totals at least `size` free-or-droppable bytes and open it; answer
    // its base, or nothing when no such span exists. `size` 0 means "free any droppable span" (for an entry).
    [[nodiscard]] std::optional<std::uint16_t> evictStalestSpan(std::uint16_t size);

    // The lowest free directory entry, dropping the stalest unheld span for one when every entry is taken.
    [[nodiscard]] std::uint8_t takeEntry();
    void                       freeSlotEntries(const Slot& slot);
    void                       drop(const Slot& slot);  // free its entries, forget it

    std::vector<Slot> slots_;
    std::uint16_t     limit_ = kSamplesEnd;
    // Entries 0..kVoices-1 are the windows' (reserved here), kVoices..kDirectorySlots-1 the cache's.
    std::vector<bool> entryUsed_;  // size kDirectorySlots; true = taken
};

}  // namespace retropp::audio::snesdriver

#endif  // RETROPP_SRC_AUDIO_SNES_SAMPLE_CACHE_H

#include "src/audio/snes/sample_cache.h"

#include <algorithm>

namespace retropp::audio::snesdriver {

namespace {
// A placement reaches above `base` when its last byte sits at or past it.
[[nodiscard]] bool reachesAbove(std::uint16_t start, std::uint16_t size, std::uint16_t base) {
    return static_cast<int>(start) + static_cast<int>(size) > static_cast<int>(base);
}
}  // namespace

SampleCache::SampleCache() : entryUsed_(kDirectorySlots, false) {
    // Entries 0..kVoices-1 are the stream windows' — reserved so the cache never hands one out.
    for (std::uint16_t e = 0; e < kVoices; ++e) {
        entryUsed_[e] = true;
    }
}

SampleCache::Slot* SampleCache::find(AudioId id) {
    for (Slot& s : slots_) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

const SampleCache::Slot* SampleCache::find(AudioId id) const {
    for (const Slot& s : slots_) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

std::optional<std::uint16_t> SampleCache::placementOf(AudioId id) const {
    const Slot* s = find(id);
    return s != nullptr ? std::optional<std::uint16_t>(s->start) : std::nullopt;
}

void SampleCache::hold(AudioId id) {
    if (Slot* s = find(id)) {
        ++s->held;
    }
}

void SampleCache::release(AudioId id) {
    if (Slot* s = find(id); s != nullptr && s->held > 0) {
        --s->held;
    }
}

bool SampleCache::anyHeldAbove(std::uint16_t base) const {
    return std::any_of(slots_.begin(), slots_.end(), [&](const Slot& s) {
        return s.held > 0 && reachesAbove(s.start, s.size, base);
    });
}

std::optional<std::uint16_t> SampleCache::lowestGap(std::uint16_t size) const {
    // The occupied intervals within [kSamplesStart, limit_), by start.
    std::vector<const Slot*> inRegion;
    for (const Slot& s : slots_) {
        if (s.start < limit_ && reachesAbove(s.start, s.size, kSamplesStart)) {
            inRegion.push_back(&s);
        }
    }
    std::sort(inRegion.begin(), inRegion.end(),
              [](const Slot* a, const Slot* b) { return a->start < b->start; });
    int cursor = kSamplesStart;
    for (const Slot* s : inRegion) {
        const int start = std::max<int>(s->start, kSamplesStart);
        if (start - cursor >= static_cast<int>(size)) {
            return static_cast<std::uint16_t>(cursor);
        }
        cursor = std::max(cursor, std::min<int>(s->start + s->size, limit_));
    }
    if (limit_ - cursor >= static_cast<int>(size)) {
        return static_cast<std::uint16_t>(cursor);
    }
    return std::nullopt;
}

std::optional<std::uint16_t> SampleCache::evictStalestSpan(std::uint16_t size) {
    // Held placements partition [kSamplesStart, limit_) into spans. A candidate span totals at least `size`
    // bytes (free space plus the room its unheld placements would give back) and holds at least one unheld
    // placement to drop. Among candidates, the one whose most recently cued placement is the stalest wins,
    // then the lowest address.
    std::vector<const Slot*> held;
    for (const Slot& s : slots_) {
        if (s.held > 0 && s.start < limit_ && reachesAbove(s.start, s.size, kSamplesStart)) {
            held.push_back(&s);
        }
    }
    std::sort(held.begin(), held.end(),
              [](const Slot* a, const Slot* b) { return a->start < b->start; });

    int                          bestBase   = -1;
    std::uint64_t                bestStale  = 0;
    int                          spanStart  = kSamplesStart;
    std::size_t                  heldIndex  = 0;
    auto considerSpan = [&](int from, int to) {
        if (to <= from) {
            return;
        }
        std::uint64_t newest    = 0;
        bool          droppable = false;
        for (const Slot& s : slots_) {
            if (s.held == 0 && s.start >= from && s.start < to) {
                droppable = true;
                newest    = std::max(newest, s.lastCue);
            }
        }
        if (!droppable) {
            return;  // nothing to give back
        }
        if (size != 0 && to - from < static_cast<int>(size)) {
            return;  // not wide enough for the sample
        }
        if (bestBase < 0 || newest < bestStale) {
            bestBase  = from;
            bestStale = newest;
        }
    };
    for (; heldIndex < held.size(); ++heldIndex) {
        const Slot* h = held[heldIndex];
        considerSpan(spanStart, h->start);
        spanStart = std::max(spanStart, std::min<int>(h->start + h->size, limit_));
    }
    considerSpan(spanStart, limit_);

    if (bestBase < 0) {
        return std::nullopt;
    }
    // Drop every unheld placement in the chosen span.
    const int spanTo = [&] {
        for (const Slot* h : held) {
            if (h->start >= bestBase) {
                return static_cast<int>(h->start);
            }
        }
        return static_cast<int>(limit_);
    }();
    std::vector<AudioId> toDrop;
    for (const Slot& s : slots_) {
        if (s.held == 0 && s.start >= bestBase && s.start < spanTo) {
            toDrop.push_back(s.id);
        }
    }
    for (AudioId id : toDrop) {
        if (Slot* s = find(id)) {
            drop(*s);
        }
    }
    return static_cast<std::uint16_t>(bestBase);
}

void SampleCache::freeSlotEntries(const Slot& slot) {
    entryUsed_[slot.entry] = false;
    if (slot.ending.has_value()) {
        entryUsed_[*slot.ending] = false;
    }
}

void SampleCache::drop(const Slot& slot) {
    const AudioId id = slot.id;
    freeSlotEntries(slot);
    slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
                                [&](const Slot& s) { return s.id == id; }),
                 slots_.end());
}

std::uint8_t SampleCache::takeEntry() {
    auto lowestFree = [&]() -> int {
        for (std::uint16_t e = kVoices; e < kDirectorySlots; ++e) {
            if (!entryUsed_[e]) {
                return static_cast<int>(e);
            }
        }
        return -1;
    };
    int e = lowestFree();
    while (e < 0) {
        if (!evictStalestSpan(0).has_value()) {
            break;  // the guarantee (at most two held entries per voice) means this is unreachable
        }
        e = lowestFree();
    }
    if (e < 0) {
        e = kVoices;  // defensive — never reached while the hold invariant holds
    }
    entryUsed_[static_cast<std::size_t>(e)] = true;
    return static_cast<std::uint8_t>(e);
}

std::optional<SampleCache::Placement> SampleCache::place(AudioId id, std::uint16_t size,
                                                         std::uint64_t cueNow) {
    if (Slot* existing = find(id)) {
        existing->lastCue = cueNow;
        return Placement{.start = existing->start, .entry = existing->entry};
    }
    std::optional<std::uint16_t> base = lowestGap(size);
    if (!base.has_value()) {
        base = evictStalestSpan(size);
    }
    if (!base.has_value()) {
        return std::nullopt;  // the caller streams it
    }
    const std::uint8_t entry = takeEntry();
    slots_.push_back(Slot{.id      = id,
                          .start   = *base,
                          .size    = size,
                          .entry   = entry,
                          .ending  = std::nullopt,
                          .lastCue = cueNow,
                          .held    = 0});
    return Placement{.start = *base, .entry = entry};
}

std::uint8_t SampleCache::endingEntry(AudioId id) {
    Slot* s = find(id);
    if (s == nullptr) {
        return 0;  // not placed — the caller only asks for a placed sample
    }
    if (s->ending.has_value()) {
        return *s->ending;
    }
    const std::uint8_t entry = takeEntry();
    // takeEntry may have dropped this very slot to free an entry only if it were unheld and stale; a pass
    // is played through from a cue that has just placed it, so it is the freshest — never the evictee. Find
    // it again in case the vector moved.
    s = find(id);
    if (s == nullptr) {
        return entry;
    }
    s->ending = entry;
    return entry;
}

bool SampleCache::lowerLimit(std::uint16_t base) {
    std::vector<AudioId> toDrop;
    bool                 heldAbove = false;
    for (const Slot& s : slots_) {
        if (!reachesAbove(s.start, s.size, base)) {
            continue;
        }
        if (s.held > 0) {
            heldAbove = true;
        } else {
            toDrop.push_back(s.id);
        }
    }
    for (AudioId id : toDrop) {
        if (Slot* s = find(id)) {
            drop(*s);
        }
    }
    limit_ = base;
    return heldAbove;
}

void SampleCache::raiseLimit(std::uint16_t base) { limit_ = base; }

}  // namespace retropp::audio::snesdriver

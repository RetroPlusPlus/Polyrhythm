// Waveform diagnostics — replay a press sequence and MEASURE the produced PCM per press, catching
// audio that degrades across repeated plays (a note falling out of key or turning tinny). Headless:
// any such degradation lives in the deterministic APU/driver state (re-trigger), so it reproduces
// without a real device. Prints per-press frequency / amplitude / harmonic content, and asserts every
// press produces the SAME tone.
#include "retropp/audio_system.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/asset_policy.h"    // AssetPolicy::LoadFromPath
#include "retropp/asset_registry.h"  // setAssetRoot — the single LoadFromPath base for the literal names
#include "retropp/audio.h"
#include "retropp/audio_library.h"   // AudioLibrary — registration lives here
#include "retropp/gb_audio.h"
#include "src/audio/audio_system_testing.h"  // detail::AudioSystemTestAccess — synchronous production seam
#include "mock_platform.h"

namespace retropp {
namespace {

using Access = detail::AudioSystemTestAccess;

// Collect `want` frames of whatever is currently playing by producing + draining.
std::vector<AudioFrame> collect(AudioSystem& audio, test::CaptureAudioSink& sink, std::size_t want) {
    std::vector<AudioFrame> out;
    int guard = 0;
    while (out.size() < want && guard++ < 10'000) {
        Access::step(audio);
        const std::vector<AudioFrame> chunk = sink.drain(want);
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    return out;
}

// Dominant frequency via zero-crossing rate over a steady window (skip the onset). For a clean periodic
// tone the crossing count is ~2 per cycle; a noisy/corrupt waveform crosses far more often.
struct Measure {
    double freqHz;
    int    peak;
    double rms;
    std::size_t crossings;
};

Measure measure(const std::vector<AudioFrame>& pcm, unsigned rate) {
    Measure m{0, 0, 0, 0};
    if (pcm.size() < 2000) {
        return m;
    }
    const std::size_t start = 1500;  // skip the attack / onset
    std::size_t crossings = 0;
    double sumSq = 0;
    for (std::size_t i = start; i < pcm.size(); ++i) {
        const int s = pcm[i].left;
        m.peak = std::max(m.peak, std::abs(s));
        sumSq += static_cast<double>(s) * s;
        if (i > start && ((pcm[i - 1].left < 0 && s >= 0) || (pcm[i - 1].left >= 0 && s < 0))) {
            ++crossings;
        }
    }
    const std::size_t span = pcm.size() - start;
    m.crossings = crossings;
    m.rms = std::sqrt(sumSq / static_cast<double>(span));
    m.freqHz = (static_cast<double>(crossings) / 2.0) * static_cast<double>(rate) / static_cast<double>(span);
    return m;
}

TEST(AudioDiagnostic, RepeatedPressesStayInTune) {
    test::CaptureAudioSink sink;
    auto audioOwner = Access::makeManual(AudioKind::Chiptune, sink);
    AudioSystem& audio = *audioOwner;
    const AudioId tone = sameboy::diagnosticTone();

    constexpr int kPresses = 6;
    constexpr std::size_t kSamplesPerPress = 12'000;  // ~250 ms

    std::vector<Measure> results;
    std::printf("\n=== repeated-press waveform diagnostic (tone = ~262 Hz triangle) ===\n");
    std::printf("press |  freqHz | peak |   rms  | crossings\n");
    for (int p = 0; p < kPresses; ++p) {
        audio.play(tone);                                  // press
        const std::vector<AudioFrame> note = collect(audio, sink, kSamplesPerPress);
        const Measure m = measure(note, kAudioSampleRate);
        results.push_back(m);
        std::printf("  %2d  | %7.1f | %4d | %6.0f | %zu\n", p + 1, m.freqHz, m.peak, m.rms, m.crossings);

        audio.stop();                                      // release
        for (int i = 0; i < 4; ++i) {                      // let the buffer drain to silence
            Access::step(audio);
            sink.drain(1u << 20);
        }
    }
    std::printf("===================================================================\n\n");

    // Every press should produce essentially the same tone. Compare each to the FIRST press.
    ASSERT_GE(results.size(), 1u);
    const Measure& first = results.front();
    ASSERT_GT(first.freqHz, 1.0);
    for (std::size_t i = 1; i < results.size(); ++i) {
        const double freqRatio = results[i].freqHz / first.freqHz;
        EXPECT_GT(freqRatio, 0.9) << "press " << (i + 1) << " pitch dropped";
        EXPECT_LT(freqRatio, 1.1) << "press " << (i + 1) << " pitch rose";
        // Crossing count must not balloon (that is the "tinny" — extra high-frequency content).
        EXPECT_LT(results[i].crossings, first.crossings * 2 + 50)
            << "press " << (i + 1) << " gained harmonic noise (tinny)";
    }
}

// Onset discontinuity: a "pop" at a note's start is a large sample-to-sample jump (a step the highpass
// rings on). Measure the biggest jump in the first few ms after play() vs. the biggest jump in the
// steady middle of the note — an onset jump much larger than the steady one IS the click.
std::size_t maxAbsDelta(const std::vector<AudioFrame>& pcm, std::size_t from, std::size_t to) {
    std::size_t worst = 0;
    for (std::size_t i = from + 1; i < to && i < pcm.size(); ++i) {
        const std::size_t d = static_cast<std::size_t>(std::abs(pcm[i].left - pcm[i - 1].left));
        worst = std::max(worst, d);
    }
    return worst;
}

TEST(AudioDiagnostic, OnsetDiscontinuity) {
    test::CaptureAudioSink sink;
    auto audioOwner = Access::makeManual(AudioKind::Chiptune, sink);
    AudioSystem& audio = *audioOwner;
    const AudioId tone = sameboy::diagnosticTone();

    audio.play(tone);
    const std::vector<AudioFrame> note = collect(audio, sink, 6'000);
    ASSERT_GT(note.size(), 3'000u);

    std::printf("\n=== onset discontinuity (first 24 left samples after play) ===\n");
    for (std::size_t i = 0; i < 24 && i < note.size(); ++i) {
        std::printf("%d ", note[i].left);
    }
    const std::size_t onsetJump  = maxAbsDelta(note, 0, 400);
    const std::size_t steadyJump = maxAbsDelta(note, 2'000, 3'000);
    std::printf("\nmax |delta|: onset(first 400)=%zu  steady(2000-3000)=%zu  ratio=%.1f\n",
                onsetJump, steadyJump,
                steadyJump ? static_cast<double>(onsetJump) / static_cast<double>(steadyJump) : 0.0);
    std::printf("==============================================================\n\n");
}

// The self-contained note driver (channel init + settle + trigger in one routine, run on a fresh voice
// VM): the NOTE must start cleanly. The driver sets up the APU/wave RAM/DAC/volumes while the channel is
// untriggered (silent), holds one frame of settle so the output centers, then triggers — so the trigger
// region carries no pop. (The DAC-enable step itself lands at the very start of the voice, on silence,
// a frame before the note.) The trigger lands one settle-frame in: ~816 output frames at 48 kHz.
TEST(AudioDiagnostic, InitTriggerDriverHasCleanOnset) {
    setAssetRoot(std::filesystem::path(RETROPP_ASSETS_DIR) / "tones");  // single root for the literal names
    test::CaptureAudioSink sink;
    auto audioOwner = Access::makeManual(AudioKind::Chiptune, sink);
    AudioSystem& audio = *audioOwner;
    const AudioId note = AudioLibrary::instance().registerAudio("tone_c.asm", AudioType::Music, Isa::Sm83,
                                                                AssetPolicy::LoadFromPath);

    audio.play(note);  // one self-contained driver: init (silent) → settle → trigger
    const std::vector<AudioFrame> onset = collect(audio, sink, 6'000);
    ASSERT_GT(onset.size(), 4'500u);

    const std::size_t triggerJump = maxAbsDelta(onset, 500, 1'500);   // window containing the trigger
    const std::size_t steadyJump  = maxAbsDelta(onset, 3'000, 4'500);  // mid-note reference
    std::printf("\n=== init/settle/trigger onset: trigger=%zu steady=%zu ratio=%.1f ===\n\n",
                triggerJump, steadyJump,
                steadyJump ? static_cast<double>(triggerJump) / static_cast<double>(steadyJump) : 0.0);
    // The note's start is no rougher than its steady body — the pop-free onset the settle frame buys.
    EXPECT_LT(triggerJump, steadyJump * 3 + 200);
}

}  // namespace
}  // namespace retropp

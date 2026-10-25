// The hosted-driver system surface, device-free. A game hosts its own resident sound driver on an
// AudioSystem (AudioLibrary::uploadDriver → AudioSystem::host), then drives it through the durable typed
// handle — driver.play(id[, lane]) / stop() / slots(...) — exactly the player's own verbs. Each case
// registers a SYNTHETIC mini-driver (hand-written SM83 assembled in-process — NO ROM), hosts it against a
// headless CaptureAudioSink, drives production synchronously through the internal test seam, and observes
// the result two ways: the produced PCM (a real APU waveform) and the published slot snapshot the handle
// reads back. Every VM core is deterministic, so a driver's output and RAM effect are reproducible without
// a device.
//
// Two driver families are exercised: the RAM-FLAG family (play lands a sound id in a memory mailbox the
// driver polls each tick — the mailbox lineage) and the ARGUMENT family (play rides a sound id in a
// CPU register into an entry the engine calls — the tracker-driver lineage).
#include "retropp/audio_system.h"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/audio.h"
#include "retropp/audio_library.h"
#include "retropp/audio_mixer.h"
#include "retropp/driver_binding.h"
#include "retropp/gb.h"
#include "retropp/vm.h"
#include "src/audio/audio_system_testing.h"  // detail::AudioSystemTestAccess — synchronous production seam
#include "mock_platform.h"                    // test::CaptureAudioSink

namespace retropp {
namespace {

using Access = detail::AudioSystemTestAccess;

// Assemble SM83 source into image bytes through a throwaway VM (the engine's own assembler).
std::vector<std::uint8_t> asm83(const std::string& source) {
    Vm probe{VMPlatform::GameBoyColor};
    return probe.assemble(source);
}

// Count frames whose left or right sample is non-zero — a silent stream is all zeros.
std::size_t nonSilentCount(const std::vector<AudioFrame>& frames) {
    std::size_t n = 0;
    for (const AudioFrame& f : frames) {
        if (f.left != 0 || f.right != 0) {
            ++n;
        }
    }
    return n;
}

// Whether two equal-length PCM captures differ in any sample (AudioFrame has no operator==).
bool pcmDiffers(const std::vector<AudioFrame>& a, const std::vector<AudioFrame>& b) {
    if (a.size() != b.size()) {
        return true;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].left != b[i].left || a[i].right != b[i].right) {
            return true;
        }
    }
    return false;
}

// The static APU + wave-channel setup both synthetic drivers run in their .init: enable the APU, load a
// 32-step triangle into wave RAM, route both channels, and enable CH3's DAC at 100% output level. It does
// NOT trigger a tone — the per-sound trigger is the tick's (RAM-flag) or the play entry's (argument) job,
// so a hosted-but-un-played driver is silent until the first play.
const char* kInitSource = R"(
  ld hl, $C010          ; zero the driver's state RAM ($C010..$C030 — mailboxes + slots), as a real
  ld c, $21             ; driver's init does: post-reset WRAM is not zero, and a garbage mailbox would
  xor a                 ; otherwise read as a spurious play / stop on the first tick
.clr:
  ld [hl+], a
  dec c
  jr nz, .clr
  ld a, $80
  ldh [$FF26], a        ; NR52 — APU master enable
  ld a, $00
  ldh [$FF1A], a        ; NR30 — CH3 DAC off before writing wave RAM
  ld a, $01
  ldh [$FF30], a
  ld a, $23
  ldh [$FF31], a
  ld a, $45
  ldh [$FF32], a
  ld a, $67
  ldh [$FF33], a
  ld a, $89
  ldh [$FF34], a
  ld a, $AB
  ldh [$FF35], a
  ld a, $CD
  ldh [$FF36], a
  ld a, $EF
  ldh [$FF37], a
  ld a, $FE
  ldh [$FF38], a
  ld a, $DC
  ldh [$FF39], a
  ld a, $BA
  ldh [$FF3A], a
  ld a, $98
  ldh [$FF3B], a
  ld a, $76
  ldh [$FF3C], a
  ld a, $54
  ldh [$FF3D], a
  ld a, $32
  ldh [$FF3E], a
  ld a, $10
  ldh [$FF3F], a
  ld a, $FF
  ldh [$FF25], a        ; NR51 — route all channels L+R
  ld a, $77
  ldh [$FF24], a        ; NR50 — master volume L+R
  ld a, $80
  ldh [$FF1A], a        ; NR30 — CH3 DAC on
  ld a, $20
  ldh [$FF1C], a        ; NR32 — CH3 output level 100%
  ret
)";

// ── The RAM-flag mini-driver ────────────────────────────────────────────────────────────────────────
//
// Mailboxes (written by the play / stop verbs): music $C010, sfx $C011, stop $C012. Each tick the driver
// consumes a non-zero mailbox: it records the value to a "last seen" byte, acts on it (music sets CH3's
// frequency from the value and triggers → the pitch tracks the played id; sfx just records; stop turns the
// DAC off → silence), then CLEARS the mailbox so the sound does not re-trigger on later idle ticks. A
// writable "volume" slot ($C022) overrides NR32 when set; a "trigger" slot ($C023) is consumed once (copied
// to $C024 and cleared) so a slots() write is proven to apply exactly once.
const char* kRamTickSource = R"(
  ld a, [$C010]         ; music mailbox
  or a
  jr z, .sfx
  ld [$C020], a         ; music last-seen
  ldh [$FF1D], a        ; NR33 — CH3 frequency low = the played id (pitch tracks it)
  ld a, $86
  ldh [$FF1E], a        ; NR34 — trigger + frequency high bits
  xor a
  ld [$C010], a         ; clear the music mailbox (no re-trigger)
.sfx:
  ld a, [$C011]         ; sfx mailbox
  or a
  jr z, .stopf
  ld [$C021], a         ; sfx last-seen
  xor a
  ld [$C011], a         ; clear the sfx mailbox
.stopf:
  ld a, [$C012]         ; stop mailbox
  or a
  jr z, .vol
  xor a
  ldh [$FF1A], a        ; NR30 — CH3 DAC off → silence
  ld [$C012], a         ; clear the stop mailbox
.vol:
  ld a, [$C022]         ; volume slot
  or a
  jr z, .trig
  ldh [$FF1C], a        ; NR32 — CH3 output level from the slot
.trig:
  ld a, [$C023]         ; trigger slot
  or a
  jr z, .done
  ld [$C024], a         ; trigger seen
  xor a
  ld [$C023], a         ; consume the trigger (apply-once)
.done:
  ret
)";

// The RAM-flag driver's game-facing slot struct. Field TYPE carries slot width; declaration order = index.
struct RamSlots {
    std::optional<std::uint8_t> mailbox;        // 0: $C010 — the music mailbox (cleared each tick)
    std::optional<std::uint8_t> musicLastSeen;  // 1: $C020
    std::optional<std::uint8_t> sfxLastSeen;    // 2: $C021
    std::optional<std::uint8_t> volume;         // 3: $C022 (ReadWrite)
    std::optional<std::uint8_t> trigger;        // 4: $C023 (ReadWrite — consumed once)
    std::optional<std::uint8_t> triggerSeen;    // 5: $C024
};

// Register the RAM-flag driver on the library and return its typed id. init at $6000, tick at $6100.
DriverId<RamSlots> registerRamDriver() {
    static const std::vector<std::uint8_t> initBytes = asm83(kInitSource);
    static const std::vector<std::uint8_t> tickBytes = asm83(kRamTickSource);
    DriverBinding b;
    b.images    = {DriverImage{.bytes = initBytes, .base = 0x6000},
                   DriverImage{.bytes = tickBytes, .base = 0x6100}};
    b.tickEntry = 0x6100;
    b.init      = Instruction::call(0x6000, gb::A, /*fixedValue=*/0);  // run the static setup once at host()
    const DriverVerbs verbs{
        .play = {.music = Instruction::write(Location::memory(0xC010), 1),
                 .sfx   = Instruction::write(Location::memory(0xC011), 1)},
        .stop = Instruction::write(Location::memory(0xC012), 1, /*fixedValue=*/1),
    };
    return AudioLibrary::instance().uploadDriver(
        b, verbs,
        slots(slot(&RamSlots::mailbox, 0xC010, SlotDirection::Read),
              slot(&RamSlots::musicLastSeen, 0xC020, SlotDirection::Read),
              slot(&RamSlots::sfxLastSeen, 0xC021, SlotDirection::Read),
              slot(&RamSlots::volume, 0xC022, SlotDirection::ReadWrite),
              slot(&RamSlots::trigger, 0xC023, SlotDirection::ReadWrite),
              slot(&RamSlots::triggerSeen, 0xC024, SlotDirection::Read)));
}


// The RAM-flag driver registered WITHOUT a declared .init, so restart() has no gesture to perform.
// Its state RAM is therefore never zeroed by the engine — irrelevant to the one case that uses it.
DriverId<RamSlots> registerInitlessDriver() {
    static const std::vector<std::uint8_t> initBytes = asm83(kInitSource);
    static const std::vector<std::uint8_t> tickBytes = asm83(kRamTickSource);
    DriverBinding b;
    b.images    = {DriverImage{.bytes = initBytes, .base = 0x6000},
                   DriverImage{.bytes = tickBytes, .base = 0x6100}};
    b.tickEntry = 0x6100;
    // b.init deliberately left unset.
    const DriverVerbs verbs{
        .play = {.music = Instruction::write(Location::memory(0xC010), 1)},
    };
    return AudioLibrary::instance().uploadDriver(
        b, verbs, slots(slot(&RamSlots::volume, 0xC022, SlotDirection::ReadWrite)));
}

// ── The argument mini-driver ──────────────────────────────────────────────────────────────────────
//
// play rides the sound id in register A into a play ENTRY the engine calls: the entry records the id to a
// "last song" byte, sets CH3's frequency from A, and triggers — so the pitch tracks the register argument.
// The tick is a bare return (the APU sustains during the idle remainder).
const char* kArgPlayEntrySource = R"(
  ld [$C030], a         ; last song = the register argument
  ldh [$FF1D], a        ; NR33 — CH3 frequency low = the argument (pitch tracks it)
  ld a, $86
  ldh [$FF1E], a        ; NR34 — trigger + frequency high bits
  ret
)";
const char* kArgTickSource = "  ret\n";  // sustains via the idle remainder

struct ArgSlots {
    std::optional<std::uint8_t> lastSong;  // 0: $C030 (Read)
};

// Register the argument driver. init at $6000, play entry at $6100, tick at $6200.
DriverId<ArgSlots> registerArgDriver() {
    static const std::vector<std::uint8_t> initBytes  = asm83(kInitSource);
    static const std::vector<std::uint8_t> entryBytes = asm83(kArgPlayEntrySource);
    static const std::vector<std::uint8_t> tickBytes  = asm83(kArgTickSource);
    DriverBinding b;
    b.images    = {DriverImage{.bytes = initBytes, .base = 0x6000},
                   DriverImage{.bytes = entryBytes, .base = 0x6100},
                   DriverImage{.bytes = tickBytes, .base = 0x6200}};
    b.tickEntry = 0x6200;
    b.init      = Instruction::call(0x6000, gb::A, /*fixedValue=*/0);
    const DriverVerbs verbs{.play = {.music = Instruction::call(0x6100, gb::A)}};
    return AudioLibrary::instance().uploadDriver(
        b, verbs, slots(slot(&ArgSlots::lastSong, 0xC030, SlotDirection::Read)));
}

// Drive one production pass and return everything buffered as PCM frames.
std::vector<AudioFrame> stepAndDrain(AudioSystem& sys, test::CaptureAudioSink& sink) {
    Access::step(sys);
    return sink.drain(sys.audioStats().framesBuffered);
}

// ── RAM-flag family ─────────────────────────────────────────────────────────────────────────────────

// The headline: a hosted RAM-flag driver, played through its handle, produces a real APU waveform, and its
// published slots reflect the tick — the played id landed in the "last seen" byte and the mailbox was
// consumed (cleared).
TEST(DriverHostingSystem, RamFlagDriverPlaysAndPublishesSlots) {
    const DriverId<RamSlots> id = registerRamDriver();
    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Chiptune, sink);

    HostedDriver<RamSlots> driver = sys->host(id);
    driver.play(0x40);
    const std::vector<AudioFrame> pcm = stepAndDrain(*sys, sink);

    EXPECT_GT(nonSilentCount(pcm), std::size_t{100});  // a real waveform, not a flat line

    const RamSlots read = driver.slots();
    ASSERT_TRUE(read.musicLastSeen.has_value());
    EXPECT_EQ(*read.musicLastSeen, 0x40u);  // the played id reached the driver's state
    ASSERT_TRUE(read.mailbox.has_value());
    EXPECT_EQ(*read.mailbox, 0u);           // the mailbox was consumed and cleared by the tick
}

// The played id changes the tone: two systems hosting the same driver, played with different ids, produce
// DIFFERENT waveforms (the id sets CH3's frequency) — both non-silent.
TEST(DriverHostingSystem, PlayValueChangesTheTone) {
    const DriverId<RamSlots> id = registerRamDriver();

    test::CaptureAudioSink sinkA;
    auto sysA = Access::makeManual(AudioKind::Chiptune, sinkA);
    HostedDriver<RamSlots> a = sysA->host(id);
    a.play(0x40);
    const std::vector<AudioFrame> low = stepAndDrain(*sysA, sinkA);

    test::CaptureAudioSink sinkB;
    auto sysB = Access::makeManual(AudioKind::Chiptune, sinkB);
    HostedDriver<RamSlots> b = sysB->host(id);
    b.play(0xF0);
    const std::vector<AudioFrame> high = stepAndDrain(*sysB, sinkB);

    EXPECT_GT(nonSilentCount(low), std::size_t{100});
    EXPECT_GT(nonSilentCount(high), std::size_t{100});
    ASSERT_EQ(low.size(), high.size());
    EXPECT_TRUE(pcmDiffers(low, high))
        << "the two played ids produced identical PCM — the id is not selecting the pitch";
}

// A mailbox is consumed exactly once: after a play + tick the mailbox reads cleared, and a further tick
// with no new play leaves the last-seen value unchanged — the sound does not re-trigger on idle.
TEST(DriverHostingSystem, MailboxIsConsumedOnceNoRetrigger) {
    const DriverId<RamSlots> id = registerRamDriver();
    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Chiptune, sink);
    HostedDriver<RamSlots> driver = sys->host(id);

    driver.play(0x55);
    stepAndDrain(*sys, sink);
    const RamSlots afterPlay = driver.slots();
    ASSERT_TRUE(afterPlay.musicLastSeen.has_value());
    EXPECT_EQ(*afterPlay.musicLastSeen, 0x55u);
    EXPECT_EQ(afterPlay.mailbox.value_or(0xFF), 0u);  // consumed + cleared

    stepAndDrain(*sys, sink);  // another pass, no new play
    const RamSlots afterIdle = driver.slots();
    EXPECT_EQ(afterIdle.musicLastSeen.value_or(0), 0x55u);  // unchanged — not re-consumed
    EXPECT_EQ(afterIdle.mailbox.value_or(0xFF), 0u);        // still empty
}

// A second play lane routes to its own realization: play(id, AudioType::Sfx) reaches the SFX mailbox, not
// the music one — the sfx last-seen byte gets the value and the music last-seen stays untouched.
TEST(DriverHostingSystem, SecondLaneRoutesToItsOwnMailbox) {
    const DriverId<RamSlots> id = registerRamDriver();
    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Chiptune, sink);
    HostedDriver<RamSlots> driver = sys->host(id);

    driver.play(0x77, AudioType::Sfx);
    stepAndDrain(*sys, sink);

    const RamSlots read = driver.slots();
    EXPECT_EQ(read.sfxLastSeen.value_or(0), 0x77u);    // the SFX lane landed here
    EXPECT_EQ(read.musicLastSeen.value_or(0xFF), 0u);  // the music lane was untouched
}

// A slots() write batch applies in submission order — the last write to a field wins within one tick.
TEST(DriverHostingSystem, SlotsWriteBatchLastWins) {
    const DriverId<RamSlots> id = registerRamDriver();
    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Chiptune, sink);
    HostedDriver<RamSlots> driver = sys->host(id);

    driver.slots(RamSlots{.volume = 0x10});
    driver.slots(RamSlots{.volume = 0x60});  // later submission wins
    stepAndDrain(*sys, sink);

    const RamSlots read = driver.slots();
    EXPECT_EQ(read.volume.value_or(0), 0x60u);
}

// A slots() write applies exactly ONCE (mailbox semantics — never retained or re-asserted): the driver
// consumes the trigger and clears it; a later tick with no new write leaves it consumed, not re-applied.
TEST(DriverHostingSystem, SlotsWriteAppliesExactlyOnce) {
    const DriverId<RamSlots> id = registerRamDriver();
    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Chiptune, sink);
    HostedDriver<RamSlots> driver = sys->host(id);

    driver.slots(RamSlots{.trigger = 0x01});
    stepAndDrain(*sys, sink);
    const RamSlots afterWrite = driver.slots();
    EXPECT_EQ(afterWrite.triggerSeen.value_or(0), 0x01u);  // the driver consumed it
    EXPECT_EQ(afterWrite.trigger.value_or(0xFF), 0u);      // and cleared it

    stepAndDrain(*sys, sink);  // another pass, no new slots() write
    const RamSlots afterIdle = driver.slots();
    EXPECT_EQ(afterIdle.trigger.value_or(0xFF), 0u);       // still cleared — the write was not re-asserted
}

// Writing a Read-only slot through slots() is a loud error (the direction gate).
TEST(DriverHostingSystem, WritingAReadOnlySlotThrows) {
    const DriverId<RamSlots> id = registerRamDriver();
    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Chiptune, sink);
    HostedDriver<RamSlots> driver = sys->host(id);

    EXPECT_THROW(driver.slots(RamSlots{.musicLastSeen = 0x01}), std::logic_error);
}


// ── Restart ─────────────────────────────────────────────────────────────────────────────────────
//
// A console reset is "clear the driver's RAM, then initialize" — and a driver's own init entry
// usually does not clear that RAM, the game's startup does. restart() performs the driver's declared
// .init again, which is the gesture that DOES both, so a game reproducing a reset says this rather
// than reproducing the routine's effects a byte at a time.

// The headline: state written into the driver is gone after a restart, because the declared .init
// zeroes the driver's state RAM exactly as a real one does.
TEST(DriverHostingSystem, RestartReRunsTheDeclaredInit) {
    const DriverId<RamSlots> id = registerRamDriver();
    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Chiptune, sink);
    HostedDriver<RamSlots> driver = sys->host(id);

    driver.slots(RamSlots{.volume = 0x60});
    stepAndDrain(*sys, sink);
    ASSERT_EQ(driver.slots().volume.value_or(0), 0x60u);  // the driver is holding it

    driver.restart();
    stepAndDrain(*sys, sink);

    EXPECT_EQ(driver.slots().volume.value_or(0xFF), 0u);  // the init wiped the state RAM
}

// restart() is ordered with the other verbs on the same channel — it lands BETWEEN them. The play
// submitted before it is wiped by it (its mailbox never reaches the tick, so nothing is "last seen"),
// and the slot write submitted after it survives. Both halves are needed: asserting only the later
// write would pass even if the restart did nothing at all.
TEST(DriverHostingSystem, RestartIsOrderedWithTheOtherVerbs) {
    const DriverId<RamSlots> id = registerRamDriver();
    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Chiptune, sink);
    HostedDriver<RamSlots> driver = sys->host(id);

    driver.play(1);                          // before the restart — wiped before the tick reads it
    driver.restart();
    driver.slots(RamSlots{.volume = 0x20});  // after the restart — survives
    stepAndDrain(*sys, sink);

    const RamSlots read = driver.slots();
    EXPECT_EQ(read.musicLastSeen.value_or(0xFF), 0u);  // the play never reached the tick
    EXPECT_EQ(read.volume.value_or(0), 0x20u);         // the later write did
}

// A restart is not a close: the voice keeps running and the driver keeps producing after it.
TEST(DriverHostingSystem, RestartLeavesTheDriverRunning) {
    const DriverId<RamSlots> id = registerRamDriver();
    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Chiptune, sink);
    HostedDriver<RamSlots> driver = sys->host(id);

    driver.restart();
    stepAndDrain(*sys, sink);

    driver.play(1);
    stepAndDrain(*sys, sink);
    EXPECT_EQ(driver.slots().musicLastSeen.value_or(0), 1u);  // still ticking, still accepting verbs
}

// A driver that declares no .init has no gesture to perform again, and says so rather than silently
// doing nothing.
TEST(DriverHostingSystem, RestartThrowsWhenNoInitIsDeclared) {
    const DriverId<RamSlots> id = registerInitlessDriver();
    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Chiptune, sink);
    HostedDriver<RamSlots> driver = sys->host(id);

    EXPECT_THROW(driver.restart(), std::logic_error);
}

// The VMDriver mixer bus scales the whole driver voice: muting the bus silences the sustained tone, and
// restoring it brings the same tone back — the driver keeps running throughout (the tone was triggered
// once and sustains).
TEST(DriverHostingSystem, VmDriverBusScalesTheOutput) {
    const DriverId<RamSlots> id = registerRamDriver();
    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Chiptune, sink);
    HostedDriver<RamSlots> driver = sys->host(id);
    driver.play(0x40);

    AudioMixer::instance().levels(AudioLevels{.vmDriver = 0});  // mute the driver bus
    const std::vector<AudioFrame> muted = stepAndDrain(*sys, sink);
    EXPECT_EQ(nonSilentCount(muted), std::size_t{0}) << "the muted VMDriver bus still produced sound";

    AudioMixer::instance().levels(AudioLevels{.vmDriver = 255});  // restore unity
    const std::vector<AudioFrame> restored = stepAndDrain(*sys, sink);
    EXPECT_GT(nonSilentCount(restored), std::size_t{100}) << "the tone did not sustain past the bus change";
}

// ── Argument family ─────────────────────────────────────────────────────────────────────────────────

// A hosted argument-family driver, played through its handle, cues via a register-carried id: the entry
// records the id and triggers a real waveform.
TEST(DriverHostingSystem, ArgumentDriverPlaysViaRegisterCall) {
    const DriverId<ArgSlots> id = registerArgDriver();
    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Chiptune, sink);
    HostedDriver<ArgSlots> driver = sys->host(id);

    driver.play(0x40);
    const std::vector<AudioFrame> pcm = stepAndDrain(*sys, sink);

    EXPECT_GT(nonSilentCount(pcm), std::size_t{100});
    EXPECT_EQ(driver.slots().lastSong.value_or(0), 0x40u);  // the register argument reached the entry
}

// The register argument selects the tone: two argument-driver systems played with different ids produce
// different waveforms.
TEST(DriverHostingSystem, ArgumentValueSelectsTheTone) {
    const DriverId<ArgSlots> id = registerArgDriver();

    test::CaptureAudioSink sinkA;
    auto sysA = Access::makeManual(AudioKind::Chiptune, sinkA);
    HostedDriver<ArgSlots> a = sysA->host(id);
    a.play(0x40);
    const std::vector<AudioFrame> low = stepAndDrain(*sysA, sinkA);

    test::CaptureAudioSink sinkB;
    auto sysB = Access::makeManual(AudioKind::Chiptune, sinkB);
    HostedDriver<ArgSlots> b = sysB->host(id);
    b.play(0xF0);
    const std::vector<AudioFrame> high = stepAndDrain(*sysB, sinkB);

    ASSERT_EQ(low.size(), high.size());
    EXPECT_TRUE(pcmDiffers(low, high));
}

// ── Host preconditions + lifecycle ───────────────────────────────────────────────────────────────────

// One resident machine per registration per system: a second host() of the same id on the same system
// throws.
TEST(DriverHostingSystem, DoubleHostOfSameIdThrows) {
    const DriverId<RamSlots> id = registerRamDriver();
    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Chiptune, sink);
    HostedDriver<RamSlots> first = sys->host(id);
    EXPECT_THROW((void)sys->host(id), std::runtime_error);
}

// Only a Chiptune system hosts a driver — a Pcm system rejects host().
TEST(DriverHostingSystem, PcmSystemRejectsHost) {
    const DriverId<RamSlots> id = registerRamDriver();
    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Pcm, sink);
    EXPECT_THROW((void)sys->host(id), std::runtime_error);
}

// The system's stop() does NOT close a hosted driver: it is always-running and keeps producing after a
// stop() (which release-fades cued voices, of which there are none here).
TEST(DriverHostingSystem, SystemStopDoesNotCloseTheDriver) {
    const DriverId<RamSlots> id = registerRamDriver();
    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Chiptune, sink);
    HostedDriver<RamSlots> driver = sys->host(id);
    driver.play(0x40);
    stepAndDrain(*sys, sink);
    EXPECT_TRUE(sys->isPlaying());

    sys->stop();
    const std::vector<AudioFrame> afterStop = stepAndDrain(*sys, sink);
    EXPECT_TRUE(sys->isPlaying()) << "stop() closed the resident driver";
    EXPECT_GT(nonSilentCount(afterStop), std::size_t{100}) << "the driver stopped producing after stop()";
}

// close() releases the registration: the same driver can be hosted again afterward. The one-machine-per-
// registration rule is about machines that are live, not about ids that have ever been used. The second
// machine also takes the gestures while the first one finishes fading — a verb never lands on a voice on
// its way out.
TEST(DriverHostingSystem, AClosedDriverCanBeHostedAgain) {
    const DriverId<RamSlots> id = registerRamDriver();
    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Chiptune, sink);

    HostedDriver<RamSlots> first = sys->host(id);
    first.play(0x40);
    (void)stepAndDrain(*sys, sink);
    first.close();

    HostedDriver<RamSlots> second = sys->host(id);
    second.play(0x55);
    (void)stepAndDrain(*sys, sink);

    const RamSlots read = second.slots();
    ASSERT_TRUE(read.musicLastSeen.has_value());
    EXPECT_EQ(*read.musicLastSeen, 0x55u);  // the play reached the new machine, not the closing one
}

// The nested platform-bound instantiation types construct with their console pre-bound — the all-caps
// hardware spelling, symmetric across the Vm and AudioSystem halves.
TEST(DriverHostingSystem, NestedPlatformTypesConstruct) {
    Vm::GBC vm;
    EXPECT_EQ(vm.platform(), VMPlatform::GameBoyColor);
    Vm::GB gb;
    EXPECT_EQ(gb.platform(), VMPlatform::GameBoy);

    test::CaptureAudioSink sink;
    AudioSystem::GBC music{AudioKind::Chiptune, sink};  // a Game Boy Color chiptune system, console pre-bound
    EXPECT_TRUE(sink.started());
}

}  // namespace
}  // namespace retropp

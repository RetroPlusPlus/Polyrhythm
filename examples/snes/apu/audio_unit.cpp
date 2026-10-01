#include "audio_unit.h"

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "retropp/audio_library.h"
#include "retropp/memory_region.h"
#include "retropp/snes.h"

namespace demo {
namespace {

using namespace retropp;

// The S-DSP's registers, by the chip's own names. A voice's eight live at voice x $10 + these.
constexpr std::uint8_t kVolumeLeft  = 0x00;
constexpr std::uint8_t kVolumeRight = 0x01;
constexpr std::uint8_t kPitchLow    = 0x02;
constexpr std::uint8_t kPitchHigh   = 0x03;
constexpr std::uint8_t kSource      = 0x04;
constexpr std::uint8_t kGain        = 0x07;
constexpr std::uint8_t kVoiceStride = 0x10;
// The chip's own, in one place each.
constexpr std::uint8_t kMainVolumeLeft  = 0x0C;
constexpr std::uint8_t kMainVolumeRight = 0x1C;
constexpr std::uint8_t kKeyOn           = 0x4C;
constexpr std::uint8_t kKeyOff          = 0x5C;
constexpr std::uint8_t kDirectoryPage   = 0x5D;
constexpr std::uint8_t kFlags           = 0x6C;

// Where the samples live in audio RAM: the directory is a page of four-byte entries, and the blocks
// follow it, nine bytes each.
constexpr std::uint16_t kDirectory = 0x0400;
constexpr std::uint16_t kBlocks    = 0x0500;

// How long to give the sound CPU to post its ready bytes, and to answer a kick. It runs on its own clock
// and reaches the ports in its own time, so each is read back rather than assumed.
constexpr std::chrono::milliseconds kPortWait{500};

// Read `port` until it holds `expected`, or the wait runs out.
bool portBecomes(retropp::AudioSystem& system, int port, std::uint8_t expected);

// A place: `size` bytes at an address a snes:: helper names.
MemoryRegion at(std::uint32_t address, std::uint32_t size = 1) { return MemoryRegion{.at = address, .size = size}; }

std::uint8_t voiceRegister(int voice, std::uint8_t reg) {
    return static_cast<std::uint8_t>(voice * kVoiceStride + reg);
}

bool portBecomes(AudioSystem& system, int port, std::uint8_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + kPortWait;
    while (system.read(at(snes::audioPort(port))).at(0) != expected) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}  // namespace

AudioUnit::AudioUnit() : system_{AudioKind::HostDriven} { setUpChip(); }

AudioUnit::AudioUnit(AudioSink& sink) : system_{AudioKind::HostDriven, sink} { setUpChip(); }

void AudioUnit::setUpChip() {
    writeDsp(kDirectoryPage, kDirectory >> 8);
    writeDsp(kMainVolumeLeft, 0x60);
    writeDsp(kMainVolumeRight, 0x60);
    writeDsp(kFlags, 0x20);  // unmuted, echo writes off
    for (int voice = 0; voice < kVoices; ++voice) {
        writeDsp(voiceRegister(voice, kGain), 0x7F);  // full gain, applied directly: no envelope shaping
    }
}

bool AudioUnit::bootProgramReady() {
    return portBecomes(system_, 0, 0xAA) && portBecomes(system_, 1, 0xBB);
}

bool AudioUnit::bootProgramAcknowledgesAKick() {
    system_.write(at(snes::audioPort(2), 2), std::vector<std::uint8_t>{0x00, 0x02});  // a destination, $0200
    system_.write(at(snes::audioPort(1)), std::vector<std::uint8_t>{0x01});           // a transfer, not a start
    system_.write(at(snes::audioPort(0)), std::vector<std::uint8_t>{0xCC});           // the kick
    return portBecomes(system_, 0, 0xCC);
}

void AudioUnit::loadSample(int slot, const BrrBlock& block) {
    const auto start = static_cast<std::uint16_t>(kBlocks + slot * block.size());
    system_.write(at(snes::audioRam(start), static_cast<std::uint32_t>(block.size())),
                  std::vector<std::uint8_t>(block.begin(), block.end()));
    const auto low = static_cast<std::uint8_t>(start & 0xFF), high = static_cast<std::uint8_t>(start >> 8);
    system_.write(at(snes::audioRam(static_cast<std::uint16_t>(kDirectory + slot * 4)), 4),
                  std::vector<std::uint8_t>{low, high, low, high});  // the start, and the loop point
}

void AudioUnit::source(int voice, int slot) { writeDsp(voiceRegister(voice, kSource), static_cast<std::uint8_t>(slot)); }

void AudioUnit::pitch(int voice, std::uint16_t pitch) {
    writeDsp(voiceRegister(voice, kPitchLow), static_cast<std::uint8_t>(pitch & 0xFF));
    writeDsp(voiceRegister(voice, kPitchHigh), static_cast<std::uint8_t>(pitch >> 8));
}

void AudioUnit::volume(int voice, std::uint8_t left, std::uint8_t right) {
    writeDsp(voiceRegister(voice, kVolumeLeft), left);
    writeDsp(voiceRegister(voice, kVolumeRight), right);
}

void AudioUnit::keyOn(int voice) {
    const auto bit = static_cast<std::uint8_t>(1u << voice);
    writeDsp(kKeyOff, static_cast<std::uint8_t>(dspRegister(kKeyOff) & ~bit));  // the level, cleared
    writeDsp(kKeyOn, bit);                                                       // the event
}

void AudioUnit::keyOff(int voice) {
    const auto bit = static_cast<std::uint8_t>(1u << voice);
    writeDsp(kKeyOff, static_cast<std::uint8_t>(dspRegister(kKeyOff) | bit));
}

std::uint8_t AudioUnit::dspRegister(std::uint8_t reg) { return system_.read(at(snes::dspRegister(reg))).at(0); }

std::uint8_t AudioUnit::audioRam(std::uint16_t address) { return system_.read(at(snes::audioRam(address))).at(0); }

// The sound CPU's own write to the chip, made from here.
void AudioUnit::writeDsp(std::uint8_t reg, std::uint8_t value) {
    system_.write(at(snes::dspRegister(reg)), std::vector<std::uint8_t>{value});
}

}  // namespace demo

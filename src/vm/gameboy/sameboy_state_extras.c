// What a whole-machine state needs from the Game Boy core beside the core's own save state, read and
// written where the core's fields have names.
//
// The core's save state carries the sound chip and leaves out the stage that turns the chip's levels
// into samples at the host's rate: where the machine stands between two samples, the band-limiting
// history of each channel, the DAC levels as they discharge, the running value of the DC-blocking
// filter. A machine loaded from a save state alone plays the right chip on the wrong sample grid.
// With these values put back after the load, the samples that follow are the ones the machine would
// have produced.
//
// It also answers whether a frame is part-drawn, from the display registers as they stand, and sets
// what the state alone has to decide for a machine that hands states over.
//
// C, and built in the core's own target, because the fields are in the half of the core's header
// that is C alone and their offsets follow the definitions the core is built with.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gb.h"

typedef struct {
    uint32_t           sample_rate;
    uint32_t           sample_cycles;
    uint32_t           cycles_since_render;
    uint32_t           sample_fraction;
    GB_band_limited_t  band_limited[GB_N_CHANNELS];
    double             dac_discharge[GB_N_CHANNELS];
    bool               edge_triggered[GB_N_CHANNELS];
    GB_double_sample_t highpass_diff;
    double             interference_highpass;
    bool               square_sweep_disable_stepping;
} retropp_sameboy_output_stage_t;

// How many bytes the output stage is.
size_t retropp_sameboy_output_stage_size(void)
{
    return sizeof(retropp_sameboy_output_stage_t);
}

// Copy the output stage's running values into `out`, which holds retropp_sameboy_output_stage_size()
// bytes. The rate rides along so a write can tell a stage taken at another rate.
void retropp_sameboy_read_output_stage(GB_gameboy_t *gb, uint8_t *out)
{
    retropp_sameboy_output_stage_t stage;
    memset(&stage, 0, sizeof(stage));
    stage.sample_rate = gb->apu_output.sample_rate;
    stage.sample_cycles = gb->apu_output.sample_cycles;
    stage.cycles_since_render = gb->apu_output.cycles_since_render;
    stage.sample_fraction = gb->apu_output.sample_fraction;
    memcpy(stage.band_limited, gb->apu_output.band_limited, sizeof(stage.band_limited));
    memcpy(stage.dac_discharge, gb->apu_output.dac_discharge, sizeof(stage.dac_discharge));
    memcpy(stage.edge_triggered, gb->apu_output.edge_triggered, sizeof(stage.edge_triggered));
    stage.highpass_diff = gb->apu_output.highpass_diff;
    stage.interference_highpass = gb->apu_output.interference_highpass;
    stage.square_sweep_disable_stepping = gb->apu_output.square_sweep_disable_stepping;
    memcpy(out, &stage, sizeof(stage));
}

// Put an output stage back. Answers false and writes nothing when it was taken at another rate than
// the machine produces at: its values count in that rate's samples.
bool retropp_sameboy_write_output_stage(GB_gameboy_t *gb, const uint8_t *in)
{
    retropp_sameboy_output_stage_t stage;
    memcpy(&stage, in, sizeof(stage));
    if (stage.sample_rate != gb->apu_output.sample_rate) {
        return false;
    }
    gb->apu_output.sample_cycles = stage.sample_cycles;
    gb->apu_output.cycles_since_render = stage.cycles_since_render;
    gb->apu_output.sample_fraction = stage.sample_fraction;
    memcpy(gb->apu_output.band_limited, stage.band_limited, sizeof(stage.band_limited));
    memcpy(gb->apu_output.dac_discharge, stage.dac_discharge, sizeof(stage.dac_discharge));
    memcpy(gb->apu_output.edge_triggered, stage.edge_triggered, sizeof(stage.edge_triggered));
    gb->apu_output.highpass_diff = stage.highpass_diff;
    gb->apu_output.interference_highpass = stage.interference_highpass;
    gb->apu_output.square_sweep_disable_stepping = stage.square_sweep_disable_stepping;
    return true;
}

// Whether a frame is part-drawn: the display is on, the beam is short of the vertical blank, and it
// is past the first line's sprite search, where the frame's first pixel is still to come.
bool retropp_sameboy_picture_in_progress(GB_gameboy_t *gb)
{
    if (!(gb->io_registers[GB_IO_LCDC] & GB_LCDC_ENABLE)) {
        return false;
    }
    const uint8_t mode = gb->io_registers[GB_IO_STAT] & 3;
    if (mode == 1) {
        return false;
    }
    if (mode == 2 && gb->io_registers[GB_IO_LY] == 0) {
        return false;
    }
    return true;
}

// A camera cartridge's picture, from where each pixel is.
static uint8_t fixed_camera_pixel(GB_gameboy_t *gb, uint8_t x, uint8_t y)
{
    (void)gb;
    return (uint8_t)(x * 151u + y * 149u);
}

// From this call on, the machine's state alone decides what it does next. Two things in the core take
// an answer from outside the state: a cartridge's real-time clock, which reads the host's clock, and a
// camera cartridge's picture, which holds noise from a generator every machine shares. Here the clock
// counts the machine's own cycles, whose count the state carries, and the camera sees a fixed picture.
void retropp_sameboy_decide_from_state_alone(GB_gameboy_t *gb)
{
    gb->rtc_mode = GB_RTC_MODE_ACCURATE;
    gb->camera_get_pixel_callback = fixed_camera_pixel;
}

// After the core's own load: the state may hold a key still settling, and the core's note that nothing
// is settling stays from the run before it, so the joypad looks again.
void retropp_sameboy_after_load(GB_gameboy_t *gb)
{
    gb->joypad_is_stable = false;
}

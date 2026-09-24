#pragma once

// A hosted machine's completed picture, as a layer's content.
//
// A machine draws whole frames, so whole frames are what it hands over: the platform holds the last one
// the machine FINISHED and shows that. A tick's cycle budget can land part-way through a raster, and a
// half-drawn picture cannot be told from a finished one by looking at it — so the machine says when a
// frame is done, and the platform never asks mid-raster.
//
// `generation` is how many frames the machine has finished. It answers the only question anything
// downstream asks — is the picture I am holding still the current one — without asking WHEN it changed,
// so a machine advancing on the game's tick and a machine advancing on a clock of its own are read the
// same way. A machine that finished nothing since the last submission reports the same generation, and
// the layer keeps its picture on screen, which is what a console does on a display that refreshes faster
// than the console draws.
//
// The picture is a raster of the machine's own dimensions in the machine's own pixel layout, and neither
// is the platform's to choose: a machine that draws 256×224 says 256×224, and a machine whose native
// layout is not one of the enumerators below adds one rather than paying for a conversion. Nothing here
// says how OFTEN a machine draws, because nothing needs it — the completion is the whole signal.

#include <cstddef>
#include <cstdint>
#include <span>

namespace retropp {

// The pixel layout a machine's picture arrives in. Each enumerator names the byte order in memory,
// lowest address first.
enum class GuestPixelFormat : std::uint8_t {
    Rgba8888,  // four bytes per pixel: red, green, blue, alpha
};

// How many bytes one pixel of `format` occupies.
[[nodiscard]] constexpr std::size_t bytesPerPixel(GuestPixelFormat format) noexcept {
    switch (format) {
        case GuestPixelFormat::Rgba8888: return 4;
    }
    return 4;  // unreachable; quiets -Wreturn-type
}

// A layer's content: a raster of pixels — the last complete picture a hosted machine drew, or one a
// program painted itself. `pixels` is owned by whoever drew it and valid for the duration of the
// renderFrame() call it is submitted in — the same lifetime every other content alternative's spans
// carry.
struct GuestFrameContent {
    std::span<const std::uint8_t> pixels;  // row-major, width * height * bytesPerPixel(format) bytes
    int              width  = 0;
    int              height = 0;
    GuestPixelFormat format = GuestPixelFormat::Rgba8888;
    // How many frames this raster's source has finished — 0 before the first. For a machine's picture
    // the platform counts the completions the machine reported; a program drawing its own raster counts
    // its own. The pixels are never examined to decide anything. Two submissions carrying the same
    // non-zero generation carry the same picture, so what is already on the GPU stays there and nothing
    // is sent again; generation 0 is sent every time.
    std::uint64_t    generation = 0;
};

}  // namespace retropp

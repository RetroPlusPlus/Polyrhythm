#pragma once

#include <algorithm>

namespace retropp {

// A size in physical pixels. Used for the window's drawable surface and the internal
// viewport. Identity is the named fields — never a positional pair.
struct PixelSize {
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr bool operator==(const PixelSize&) const noexcept = default;
};

// An axis-aligned integer rectangle in pixels (origin top-left). The blit's destination
// region within the swapchain is one of these.
struct IntRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr bool operator==(const IntRect&) const noexcept = default;
};

// An integer 2-D POSITION in pixels (origin top-left) — the int companion to PixelSize's
// dimensions. A pointer's cursor lives in these (viewport pixels); windowToViewport below
// takes a window-space one. Identity is the named fields — never a positional pair.
struct Vec2i {
    int x = 0;
    int y = 0;

    [[nodiscard]] constexpr bool operator==(const Vec2i&) const noexcept = default;
};

// Float vectors — the C++ image of HLSL float2/float3/float4. A custom post-process shader declares its
// own cbuffer in those HLSL types; the build reflects the cbuffer and surfaces each field on
// ScreenSpaceEffect with the matching type below (float→float, float2→Vec2, …), so the game sets the
// shader's own vector params inline. Plain layout-compatible PODs (tightly packed floats, no padding) so
// the generated packer can memcpy them straight into the cbuffer.
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    [[nodiscard]] constexpr bool operator==(const Vec2&) const noexcept = default;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    [[nodiscard]] constexpr bool operator==(const Vec3&) const noexcept = default;
};

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    [[nodiscard]] constexpr bool operator==(const Vec4&) const noexcept = default;
};

// The pixel dimensions of ONE asset — the unit a sprite draws (Sprite::size) AND the unit the
// atlas slicer carves a loaded image into (sliceLayout / loadAtlas). It lives here in
// geometry.h (foundational, beside PixelSize) so both the draw-state layer and the asset-ingestion
// layer depend on it without image.h ever including draw_state.h. Identity is the named fields.
//
// Console asset sizes are named presets — static members of the type (AssetDimensions::GameBoy8x8,
// …), the self-type-constant idiom (declared in-class, defined inline constexpr just below) — the
// same pattern as ViewportResolution / TimingProfile. An asset size IS a {width, height} tuple,
// so a preset and a raw value are interchangeable. The preset names carry their dimensions
// (GameBoy8x16, not "GameBoyTall") so the value is legible at the call site. Not an exhaustive
// registry; the platform generalizes beyond the Game Boy, so an arbitrary AssetDimensions{w,h} covers
// anything not named.
struct AssetDimensions {
    int width  = 8;
    int height = 8;
    [[nodiscard]] constexpr bool operator==(const AssetDimensions&) const noexcept = default;

    static const AssetDimensions GameBoy8x8;        // default when nothing is specified
    static const AssetDimensions GameBoy8x16;
    static const AssetDimensions GameBoyColor8x8;
    static const AssetDimensions GameBoyColor8x16;
    static const AssetDimensions GameBoyAdvance8x8; // GBA base; OBJ range 8×8…64×64
    static const AssetDimensions Nes8x8;
    static const AssetDimensions Nes8x16;
    static const AssetDimensions MasterSystem8x8;
    static const AssetDimensions MasterSystem8x16;
    static const AssetDimensions Snes8x8;
    static const AssetDimensions Snes16x16;
    static const AssetDimensions Snes32x32;
    static const AssetDimensions Snes64x64;
    static const AssetDimensions Genesis32x32;      // max single sprite; MD composes 8px cells
};

inline constexpr AssetDimensions AssetDimensions::GameBoy8x8{8, 8};
inline constexpr AssetDimensions AssetDimensions::GameBoy8x16{8, 16};
inline constexpr AssetDimensions AssetDimensions::GameBoyColor8x8{8, 8};
inline constexpr AssetDimensions AssetDimensions::GameBoyColor8x16{8, 16};
inline constexpr AssetDimensions AssetDimensions::GameBoyAdvance8x8{8, 8};
inline constexpr AssetDimensions AssetDimensions::Nes8x8{8, 8};
inline constexpr AssetDimensions AssetDimensions::Nes8x16{8, 16};
inline constexpr AssetDimensions AssetDimensions::MasterSystem8x8{8, 8};
inline constexpr AssetDimensions AssetDimensions::MasterSystem8x16{8, 16};
inline constexpr AssetDimensions AssetDimensions::Snes8x8{8, 8};
inline constexpr AssetDimensions AssetDimensions::Snes16x16{16, 16};
inline constexpr AssetDimensions AssetDimensions::Snes32x32{32, 32};
inline constexpr AssetDimensions AssetDimensions::Snes64x64{64, 64};
inline constexpr AssetDimensions AssetDimensions::Genesis32x32{32, 32};

// Largest integer multiple of `viewport` that fits within `drawable`, centered, with the
// leftover split into letterbox/pillarbox margins (the negative-margin case below). This
// is the faithful default output scaling — integer-scale-to-fit + letterbox; the richer
// scaling-mode vocabulary (fit / fullscreen / N×) lives in the later enhancement chain.
//
// Clamps the scale to a minimum of 1×: if the window is smaller than the viewport the
// content is shown at 1× and the centered rect's origin goes negative (it overflows the
// window) rather than collapsing to nothing. Degenerate (non-positive) sizes yield an
// empty rect.
[[nodiscard]] constexpr IntRect integerScaleToFitRect(PixelSize drawable,
                                                      PixelSize viewport) noexcept {
    if (drawable.width <= 0 || drawable.height <= 0 ||
        viewport.width <= 0 || viewport.height <= 0) {
        return IntRect{};
    }
    const int scale = std::max(1, std::min(drawable.width / viewport.width,
                                           drawable.height / viewport.height));
    const int width = viewport.width * scale;
    const int height = viewport.height * scale;
    return IntRect{(drawable.width - width) / 2, (drawable.height - height) / 2,
                   width, height};
}

// The compose scale for a window drawable: the integer-scale-to-fit factor (composing at exactly the
// drawn-region size, so the blit is a 1:1 centering copy — fill parity with the faithful path), clamped
// to [1, maxScale]. The offscreen targets are sized at viewport × this. Above maxScale the blit
// integer-upscales the remainder as usual. Pure / constexpr so the clamp is unit-testable without a
// window; degenerate sizes yield 1. Mirrors integerScaleToFitRect's scale exactly (fit.width / viewport.width).
[[nodiscard]] constexpr int composeScaleToFit(PixelSize drawable, PixelSize viewport,
                                              int maxScale) noexcept {
    if (viewport.width <= 0) return 1;
    const IntRect fit = integerScaleToFitRect(drawable, viewport);
    return std::clamp(fit.width / viewport.width, 1, std::max(1, maxScale));
}

// The integer window scale to actually use, given a desired `target` multiple and the `usable`
// display area the window must fit inside (both in the SAME units — logical points, since window
// sizing and SDL_GetDisplayUsableBounds are both logical). The window is opened/resized to
// `viewport * scale`; this clamps that scale DOWN so the window never exceeds the usable display
// in either dimension:
//
//   • `target` if viewport*target fits the usable area in both width and height;
//   • otherwise the largest k in [1, target] whose viewport*k fits — the "nearest ratio";
//   • a floor of 1× even when the viewport is larger than the display (the window opens at the OS
//     max / the content letterboxes — never zero).
//
// Pure / constexpr so the clamp is unit-testable without a window. The default target (4×) and the
// huge-viewport guard both flow through here. Degenerate (non-positive) sizes yield max(1, target).
[[nodiscard]] constexpr int fitWindowScale(PixelSize viewport, PixelSize usable,
                                           int target) noexcept {
    const int want = std::max(1, target);
    if (viewport.width <= 0 || viewport.height <= 0 ||
        usable.width <= 0 || usable.height <= 0) {
        return want;  // nothing meaningful to clamp against
    }
    const int maxFitW = usable.width / viewport.width;
    const int maxFitH = usable.height / viewport.height;
    const int maxFit  = std::max(1, std::min(maxFitW, maxFitH));
    return std::min(want, maxFit);
}

// The result of mapping a window-space pixel into the internal viewport: the viewport pixel
// plus whether the source pixel landed inside the drawn content (false when it fell in the
// letterbox/pillarbox bars or off the window). `pos` is clamped into [0, viewport) on both
// axes even when `inside` is false, so a consumer can always read a usable coordinate.
struct ViewportHit {
    Vec2i pos{};
    bool  inside = false;

    [[nodiscard]] constexpr bool operator==(const ViewportHit&) const noexcept = default;
};

// Invert the blit transform: map a pixel in WINDOW/drawable space (e.g. the OS mouse position,
// already in the same physical-pixel space as `blitRect`) back into VIEWPORT space. `blitRect` is
// the destination region the renderer integer-scales the viewport into (origin = letterbox offset,
// size = viewport × the integer scale — exactly what integerScaleToFitRect produced). The map
// subtracts the letterbox origin and divides by that integer scale; it flags off-content when the
// pixel is outside `blitRect`. Pure / constexpr so the pointer→viewport mapping is unit-testable
// with no window. Degenerate inputs (non-positive viewport / empty blit rect) yield a false hit at
// the origin.
[[nodiscard]] constexpr ViewportHit windowToViewport(Vec2i windowPx, IntRect blitRect,
                                                     PixelSize viewport) noexcept {
    if (viewport.width <= 0 || viewport.height <= 0 ||
        blitRect.width <= 0 || blitRect.height <= 0) {
        return ViewportHit{};
    }
    // The blit is a uniform integer scale (same factor both axes — integerScaleToFitRect). Recover
    // it from the width; height yields the same value by construction.
    const int scale = std::max(1, blitRect.width / viewport.width);
    const int localX = windowPx.x - blitRect.x;
    const int localY = windowPx.y - blitRect.y;
    const bool inside = localX >= 0 && localY >= 0 &&
                        localX < blitRect.width && localY < blitRect.height;
    // Integer division truncates toward zero; for inside pixels localX/Y are non-negative so this is
    // a floor. The clamp keeps the reported coordinate valid even for an off-content pixel.
    const int vx = std::clamp(localX / scale, 0, viewport.width - 1);
    const int vy = std::clamp(localY / scale, 0, viewport.height - 1);
    return ViewportHit{Vec2i{vx, vy}, inside};
}

}  // namespace retropp

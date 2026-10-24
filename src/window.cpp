#include "retropp/window.h"

#include <cmath>

#include "retropp/platform.h"
#include "retropp/postprocess.h"  // curveRegionContains — the region gate's CPU containment mirror

namespace retropp {

const WindowMovement WindowMovement::None{};

namespace {

// Stick / d-pad drag rate: logical points per second at full deflection. Per-second, so drag speed
// is the same on any display refresh rate. The pointer needs no rate — its delta IS motion.
constexpr float kAutoMoveSpeed = 240.0f;

}  // namespace

Window::Window(Platform& platform) : platform_(platform) {
    platform_.dragTest(&Window::dragTestThunk, this);
}

Window::~Window() {
    platform_.dragTest(nullptr, nullptr);  // never leave the seam pointing at a dead object
}

Vec2i Window::position() const { return platform_.windowPosition(); }

void Window::position(Vec2i pos) {
    if (lastPosition_ == pos) return;  // unchanged value: no OS call, the actual window stays as-is
    lastPosition_ = pos;
    platform_.windowPosition(pos);
}

PixelSize Window::size() const { return platform_.windowSize(); }

void Window::size(PixelSize size) {
    if (lastSize_ == size) return;
    lastSize_ = size;
    platform_.windowSize(size);
}

bool Window::fullscreen() const { return platform_.fullscreen(); }

void Window::fullscreen(bool enabled) {
    if (lastFullscreen_ == enabled) return;
    lastFullscreen_ = enabled;
    platform_.fullscreen(enabled);
}

void Window::dragHandles(std::span<const Region> regions) {
    dragRegions_.assign(regions.begin(), regions.end());
}

bool Window::hitsDragRegion(Vec2i viewportPos) const {
    // Test the pixel's center, exactly where the drawn region resolves that pixel — so the draggable
    // area and the painted title bar agree to the pixel. curveRegionContains handles every boundary a
    // ShapePoints can carry (polygon, radius, stroke, transform, invert, curves).
    const Point centre{static_cast<float>(viewportPos.x) + 0.5f,
                       static_cast<float>(viewportPos.y) + 0.5f};
    for (const Region& region : dragRegions_) {
        if (curveRegionContains(centre, region.shape)) return true;
    }
    return false;
}

bool Window::dragTestThunk(void* user, Vec2i viewportPos) {
    return static_cast<const Window*>(user)->hitsDragRegion(viewportPos);
}

void Window::update(const InputSample& sample, std::chrono::nanoseconds frameTime) {
    if (!movement_.has_value()) return;
    const PlayerSample& p = sample.players[0];
    if (!p.held.test(movement_->trigger.id)) {
        moveRemainder_ = Vec2{};  // a fresh grab starts clean — no stale fraction from the last one
        return;
    }

    // Sum the declared sources: pointer delta 1:1, stick/d-pad scaled to points-per-second × this
    // frame's duration (display-rate-independent).
    const float dt    = std::chrono::duration<float>(frameTime).count();
    const float scale = kAutoMoveSpeed * dt;
    Vec2 motion = moveRemainder_;
    for (const MotionSource source : movement_->motion) {
        switch (source) {
            case MotionSource::Pointer:
                motion.x += p.analog.rawDeltaX;
                motion.y += p.analog.rawDeltaY;
                break;
            case MotionSource::LeftStick:
                motion.x += p.analog.leftX * scale;
                motion.y += p.analog.leftY * scale;
                break;
            case MotionSource::RightStick:
                motion.x += p.analog.rightX * scale;
                motion.y += p.analog.rightY * scale;
                break;
            case MotionSource::Dpad:
                motion.x += p.analog.dpadX * scale;
                motion.y += p.analog.dpadY * scale;
                break;
        }
    }

    // Move by the whole points; bank the fraction (toward zero, so the remainder keeps the sign) — a
    // slow drag accumulates across frames instead of stalling on integer window coordinates. The move
    // goes straight to the seam: automatic movement changes where the window actually is, not the
    // setter's last-set diff base.
    const float wholeX = std::trunc(motion.x);
    const float wholeY = std::trunc(motion.y);
    moveRemainder_ = Vec2{motion.x - wholeX, motion.y - wholeY};
    if (wholeX != 0.0f || wholeY != 0.0f) {
        const Vec2i pos = platform_.windowPosition();
        platform_.windowPosition(
            Vec2i{pos.x + static_cast<int>(wholeX), pos.y + static_cast<int>(wholeY)});
    }
}

}  // namespace retropp

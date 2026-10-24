#pragma once

#include <cmath>

namespace retropp {

// A 2D PROJECTIVE transform a layer or a sprite carries — arbitrary scale, rotation, skew,
// translation, AND perspective, about any pivot, with no per-console hardware ceiling (the same
// generalized posture as arbitrary layers / ViewportResolution / PaletteSize). The identity is the
// default, so content that sets no transform renders unchanged.
//
// Stored as the nine coefficients of a 3×3 homography (row-major)
//
//     | m00 m01 m02 |        x' = m00*x + m01*y + m02
//     | m10 m11 m12 |        y' = m10*x + m11*y + m12
//     | m20 m21 m22 |        w  = m20*x + m21*y + m22      → placement = (x'/w, y'/w)
//
// mapping a CONTENT-space coordinate (x, y) to a PLACEMENT-space coordinate, with the perspective
// divide by w. The bottom row (m20, m21) carries the perspective terms: zero ⇒ w is constant ⇒ a plain
// AFFINE transform (no divide needed); non-zero ⇒ foreshortening — the receding/rotating "Mode-7 style"
// floor (Mario-Kart map), done as honest per-pixel GPU geometry, NOT a per-scanline hardware idiom.
//
// The value IS the matrix — a hand-built Transform and one from a named constructor are interchangeable,
// the same "the value is the data" idiom as AssetDimensions / ViewportResolution. The constexpr subset is the
// unit-tested CPU mirror of the GPU math, like packTileCell / makeGpuSprite.
//
// Named constructors take a PIVOT in CONTENT-LOCAL PIXELS (e.g. a 160×144 layer rotates about its
// center with pivot (80, 72)); rotation/scale/skew about a pivot compose translate(pivot) · op ·
// translate(-pivot) internally. `rotation` is plain `inline` (not constexpr) because std::sin/std::cos
// are not constexpr in C++20 — the same concession displaceSourceUv made; every other member stays
// constexpr so the affine subset + compose/inverse fold at compile time and are static_assert-testable.
struct Transform {
    float m00 = 1.0f, m01 = 0.0f, m02 = 0.0f,
          m10 = 0.0f, m11 = 1.0f, m12 = 0.0f,
          m20 = 0.0f, m21 = 0.0f, m22 = 1.0f;

    [[nodiscard]] constexpr bool operator==(const Transform&) const noexcept = default;

    // ── Named constructors (the authoring surface) — all produce AFFINE transforms (bottom row 0,0,1) ─

    [[nodiscard]] static constexpr Transform identity() noexcept { return Transform{}; }

    [[nodiscard]] static constexpr Transform translation(float dx, float dy) noexcept {
        return Transform{1.0f, 0.0f, dx,
                         0.0f, 1.0f, dy,
                         0.0f, 0.0f, 1.0f};
    }

    // Scale (sx, sy) about (pivotX, pivotY). Non-uniform allowed; sx/sy may be negative (mirror).
    [[nodiscard]] static constexpr Transform
    scale(float sx, float sy, float pivotX = 0.0f, float pivotY = 0.0f) noexcept {
        return Transform{sx,   0.0f, pivotX * (1.0f - sx),
                         0.0f, sy,   pivotY * (1.0f - sy),
                         0.0f, 0.0f, 1.0f};
    }

    // Shear by (kx, ky) about (pivotX, pivotY): x' = x + kx*y, y' = ky*x + y (re-anchored to the pivot).
    [[nodiscard]] static constexpr Transform
    skew(float kx, float ky, float pivotX = 0.0f, float pivotY = 0.0f) noexcept {
        return Transform{1.0f, kx,   -kx * pivotY,
                         ky,   1.0f, -ky * pivotX,
                         0.0f, 0.0f, 1.0f};
    }

    // Rotation by `degrees` (clockwise in the platform's top-left-origin pixel space) about
    // (pivotX, pivotY). `inline`, not constexpr — std::sin/std::cos are not constexpr in C++20.
    [[nodiscard]] static inline Transform
    rotation(float degrees, float pivotX = 0.0f, float pivotY = 0.0f) noexcept {
        const float rad = degrees * 0.017453292519943295f;  // π / 180
        const float cs = std::cos(rad);
        const float sn = std::sin(rad);
        return Transform{cs,   -sn,  pivotX * (1.0f - cs) + pivotY * sn,
                         sn,    cs,  pivotY * (1.0f - cs) - pivotX * sn,
                         0.0f, 0.0f, 1.0f};
    }

    // Perspective foreshortening: the bottom-row terms (gx, gy) that make w vary across content space
    // (w = gx*x + gy*y + 1). Compose with rotation/scale/translation via then() to author a receding,
    // rotating "Mode-7 style" ground plane. Zero terms ⇒ affine (no foreshortening).
    [[nodiscard]] static constexpr Transform perspective(float gx, float gy) noexcept {
        return Transform{1.0f, 0.0f, 0.0f,
                         0.0f, 1.0f, 0.0f,
                         gx,   gy,   1.0f};
    }

    // ── Composition + queries ────────────────────────────────────────────────────────────────

    // Apply this transform, THEN `next` — i.e. (next ∘ this), the matrix product next · this. A point
    // goes through `this` first and `next` second: `a.then(b).apply(p) == b.apply(a.apply(p))`.
    [[nodiscard]] constexpr Transform then(const Transform& n) const noexcept {
        return Transform{
            n.m00 * m00 + n.m01 * m10 + n.m02 * m20,
            n.m00 * m01 + n.m01 * m11 + n.m02 * m21,
            n.m00 * m02 + n.m01 * m12 + n.m02 * m22,
            n.m10 * m00 + n.m11 * m10 + n.m12 * m20,
            n.m10 * m01 + n.m11 * m11 + n.m12 * m21,
            n.m10 * m02 + n.m11 * m12 + n.m12 * m22,
            n.m20 * m00 + n.m21 * m10 + n.m22 * m20,
            n.m20 * m01 + n.m21 * m11 + n.m22 * m21,
            n.m20 * m02 + n.m21 * m12 + n.m22 * m22};
    }

    // Map a content-space point to placement space, including the perspective divide.
    [[nodiscard]] constexpr float weight(float x, float y) const noexcept { return m20 * x + m21 * y + m22; }
    [[nodiscard]] constexpr float applyX(float x, float y) const noexcept {
        return (m00 * x + m01 * y + m02) / weight(x, y);
    }
    [[nodiscard]] constexpr float applyY(float x, float y) const noexcept {
        return (m10 * x + m11 * y + m12) / weight(x, y);
    }

    [[nodiscard]] constexpr bool isAffine() const noexcept {
        return m20 == 0.0f && m21 == 0.0f && m22 == 1.0f;
    }
    [[nodiscard]] constexpr bool isIdentity() const noexcept {
        return m00 == 1.0f && m01 == 0.0f && m02 == 0.0f &&
               m10 == 0.0f && m11 == 1.0f && m12 == 0.0f &&
               m20 == 0.0f && m21 == 0.0f && m22 == 1.0f;
    }

    // The placement→content inverse — the tile fragment's consumer (it maps a destination pixel back to
    // the content pixel to sample, perspective divide included). The inverse of a homography is a
    // homography (the 3×3 adjugate ÷ determinant; the overall scale is irrelevant under the projective
    // divide, but normalizing keeps the coefficients well-conditioned). A singular (zero-determinant)
    // transform has no inverse and returns the identity — a defined fallback, not NaNs / UB.
    [[nodiscard]] constexpr Transform inverse() const noexcept {
        const float c00 =  (m11 * m22 - m12 * m21);
        const float c01 = -(m01 * m22 - m02 * m21);
        const float c02 =  (m01 * m12 - m02 * m11);
        const float c10 = -(m10 * m22 - m12 * m20);
        const float c11 =  (m00 * m22 - m02 * m20);
        const float c12 = -(m00 * m12 - m02 * m10);
        const float c20 =  (m10 * m21 - m11 * m20);
        const float c21 = -(m00 * m21 - m01 * m20);
        const float c22 =  (m00 * m11 - m01 * m10);
        const float det = m00 * c00 + m01 * c10 + m02 * c20;
        if (det == 0.0f) return Transform{};  // singular — defined fallback
        const float inv = 1.0f / det;
        // adjugate (transpose of the cofactor matrix) × 1/det.
        return Transform{c00 * inv, c01 * inv, c02 * inv,
                         c10 * inv, c11 * inv, c12 * inv,
                         c20 * inv, c21 * inv, c22 * inv};
    }
};

}  // namespace retropp

#pragma once

// Vantium — the DRAW step, its own translation unit. Reads the game model + assets + feel and
// composes the frame:
//
//   z=0   stars    — the parallax field (scroll = camera/2, Repeat wrap, palette-twinkled).
//   z=10  deck     — the dreadnought band (scroll = camera, Blank-wrapped finite map whose
//                    index-0 holes reveal the stars; pod glows palette-cycled in place).
//   z=20  actors   — every sprite in world space on one camera-scrolled layer: the Manta (bank /
//                    turn frames, invulnerability alpha), fighters (livery palettes, path-flipped),
//                    spinning mines (per-sprite Transform), bolts, enemy shots, explosion clips.
//   z=30  popups   — the floating "+N" numbers (rich-font glyph sprites).
//   z=40  hud      — the fixed HUD: rich 16×16 glyphs over the bar, the rule, LAND NOW pulsing.
//
// Frame-level extras: the thrust glow and the destruct dim (Add / Multiply ColorFill regions),
// a Ripple region confined to the deck band while a scuttled ship tears itself apart, and the
// feel layer's shake as a post-effect. Every sprite key is stable; teleports re-key.

#include <vector>

#include "retropp/draw_state.h"
#include "retropp/renderer.h"

#include "assets.h"
#include "feel.h"
#include "game.h"

namespace vant {

class VantRenderer {
public:
    VantRenderer();
    void render(retropp::Renderer& renderer, VantGame& game, const VantAssets& assets,
                const VantFeel& feel);

private:
    std::vector<retropp::TileCell> hudCells_;      // the fixed 80×60 HUD/title layer
    std::vector<retropp::Sprite>   actorSprites_;
    std::vector<retropp::Sprite>   popupSprites_;
    int                            pulseTick_ = 0;  // drives the LAND NOW color pulse
};

}  // namespace vant

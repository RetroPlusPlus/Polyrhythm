# Ferryman

A complete little arcade game on Polyrhythm, and a worked example of building one: an open-sea
rescue-and-carry under a toned-down bullet hell. Collect stranded souls from the islets and sail
them home to the sanctuary — **every soul aboard slows you, pays more, and occasionally fires
back. There is no fire button: the crew fights, the ferryman sails.**

Three enemy craft classes FLY over the sea firing straight, readable bullets — an aimed bolt
(corsair), a six-bolt ring (warden), a three-bolt fan (dreadnought) — every trajectory
predictable: dodge by reading, never by luck. They fly OVER your boat (each casts a shadow on the
water) and never collide — only their bullets bite. An abductor saucer steals whoever waits (ram
its beam, or let your crew shoot it — both foil the theft and pay a bounty), and a soul carried
off the top comes back as a MUTANT — the one enemy that swims at your level and kills on contact.
Delivering n souls at once pays escalating (slot i pays i × 50), so the greed dial is how heavy —
how slow, how armed — you dare get.

## Build & run

The example builds with the engine (top-level builds turn examples on by default):

```sh
cmake --build build --target retropp-ferryman-demo
./build/ferryman_demo/retropp-ferryman-demo
```

Every asset is embedded, so `build/ferryman_demo/` holds the executable alone.

## How to play

| Action | Keyboard | Gamepad |
|---|---|---|
| Sail (8 directions) | Arrows or WASD | d-pad, left stick |
| Start | Enter or numpad Enter | Start |
| Fullscreen | Backspace | Back |

Rescuing and banking take no button — you dock against a coast and the transfer happens.

- **Rescue:** the islands are SOLID; you sail only in the water. DOCK against an islet's coast and
  its waiting souls come aboard automatically (deck cap 4) — there is no pickup button, and you
  never sail through a colonist. The only place cargo leaves the deck is the sanctuary: press its
  coast at the top with souls aboard and the whole deck banks — slot i pays i × 50, so four at
  once pays 500 where four singles pay 200.
- **The archipelago is rolled fresh every run** — islet count, positions, shapes (single,
  horizontal, VERTICAL, or a small block), and props randomize at game start, kept off the map
  edges and spacing-constrained so sea lanes stay open, so no two runs share a map.
- **The weight rule:** every passenger slows the ferry — and each one aboard fires a gold bolt
  at the nearest enemy on a shared clock (four aboard ≈ a bolt a second). Empty you are fast and
  helpless; full you are slow and armed.
- **The abductor** never harms you — it steals. Your crew's bolts are the counter: a hit while
  its beam is lit foils the theft (the stolen soul drops back, stunned but safe) for a 100
  bounty. A soul carried off the top returns as a mutant. (A hull body-block on the lit beam
  also foils, but in practice you scoop the colonist aboard before you reach the saucer.)
- **Waves:** rescue the quota and the wave clears; the next runs denser and faster. Waiting
  souls persist between waves. START pauses (RESUME / QUIT TO TITLE).

## The code

One translation unit per concern; `main.cpp` only wires them together.

| File | Owns |
|---|---|
| `layout.h` | Every constant and tuning knob: field/islet geometry, speeds, cadences, bounties, the slot/palette enums. Change the game here. |
| `game.{h,cpp}` | The simulation: ferry, colonists, enemy craft, bolts, banking, waves. Emits a `GameEvent` stream naming what happened each tick. |
| `abductor.{h,cpp}` | The thief: timed entries along a baked curve, hover → descend → carry, the flee. The sim picks its target; it only flies. |
| `assets.{h,cpp}` | Slices the four committed indexed PNGs (game sheets + the bespoke title set), loads EVERY palette from a palette image (`loadPaletteImage`, 32 of them), builds the shared animation clips. |
| `feel.{h,cpp}` | Presentation state the sim never sees: popups, the death shake, the beam/glow breaths, the WAVE round card, the animation cursors, pooled booms. |
| `audio.{h,cpp}` | Registers the eight SM83 chiptune SFX and cues them per `GameEvent`. |
| `render.{h,cpp}` | The draw step: two parallax sea planes, the terrain band + islets, the sprite layers, the beam/glow regions. |
| `assets/` | The committed PNGs, the palette images under `palettes/`, their generator (`gen_ferryman_assets.py`, stdlib-only, deterministic, richness-asserted), and the SFX sources. |

The sim advances on the fixed tick and only ever *names* what happened; audio and feel consume
the event stream, and the renderer reads state without mutating it.

## Engine techniques it demonstrates

| Technique | Where in Ferryman | Guide |
|---|---|---|
| Palette images | all 32 palettes are 16×1 RGBA PNGs via `loadPaletteImage` — no color table in C++; the floating-text backgrounds are alpha-0 ENTRIES | [tiles-and-color](../../docs/guide/tiles-and-color.md) |
| A bespoke title tileset | "FERRYMAN" as its own 32×32 glyph set — gold letterforms half-submerged behind a foaming waterline — riding per-sprite scale transforms | [images-and-transparency](../../docs/guide/images-and-transparency.md) |
| Two-plane parallax | the drift and swell water layers scroll at different rates, advanced on the sim tick | [draw-state](../../docs/guide/draw-state.md) |
| 32×32 macro-tiles | terrain tiles stamp as 4×4 groups of the engine's 8px cells (the rich-font 2×2 idiom, scaled up) | [tilemaps](../../docs/guide/tilemaps.md) |
| Cross-state identity keys | a colonist keeps ONE key through waiting → aboard → carried-by-the-thief → stunned — the interpolator never loses the soul | [draw-state](../../docs/guide/draw-state.md) |
| Palette animation | the whole sea's shimmer, the beacon's glow, and every enemy livery's running lights — the art holds, the colors breathe | [animation](../../docs/guide/animation.md) |
| One art, two liveries | enemy and cargo bolts are the same sprite under magenta/gold palettes | [tiles-and-color](../../docs/guide/tiles-and-color.md) |
| Add-blend regions | the abductor's tractor-beam capsule; the sanctuary glow that scales with your load | [blend-modes](../../docs/guide/blend-modes.md) |
| A custom BELOW-scope shader | the mutant's reality-warp comet tail (`shaders/wake_warp.frag.hlsl`) — a refraction + psychedelic hue-cycle run on a content-less layer just under the mutant, traced as tapering circles down its tracked path | [rendering](../../docs/guide/rendering.md) |
| `Curve` + `ArcLengthTable` | the abductor's constant-speed entry swoops | [curve](../../docs/guide/curve.md) |
| Per-sprite alpha | the respawn breath, the stun dim, the hit flash, the popup fade, the prompt pulse | [draw-state](../../docs/guide/draw-state.md) |
| Tweens as the feel layer | the beam and glow breaths, the popup progress, the shake decay, the round card's OutBack slide | [tween](../../docs/guide/tween.md) |
| Embedded assets | every PNG, palette image, and SFX rides `AssetPolicy::Embed` literals — the binary is self-contained | [assets-and-embedding](../../docs/guide/assets-and-embedding.md) |

## Tuning and art

- **Every gameplay number lives in `layout.h`** — speeds, fire cadences, bounties, islet
  positions, the weight rule. One constant per lever.
- **The art regenerates** from `assets/gen_ferryman_assets.py` (Python 3, standard library only,
  byte-identical on every run — the committed PNGs stay auditable):

  ```sh
  python3 examples/ferryman/assets/gen_ferryman_assets.py
  ```

  All sheet art is indexed with 16-entry palettes, and the generator ASSERTS every solid tile
  and sprite uses at least 12 distinct indices — the richness is enforced, not aspirational.
  Recoloring anything — a livery, the sea, the text — is a palette-image edit, never an art
  change.

## Gotchas

- **Controls are one `ActionMap`** (`main.cpp`): every physical source — arrows, WASD, the
  pad rows — is an ordinary row on a semantic action (`ferryman::Action` in `game.h`). Adding
  a control is adding a row (or a new action + its rows) and reading it in the tick; see
  [input](../../docs/guide/input.md).
- **One `AudioSystem` per SFX** (`audio.h` explains why): a chiptune system hosts one routine at
  a time today, and the per-SFX pool also lets effects overlap. Register audio once on the
  `AudioLibrary`; cue it from anywhere.
- **The sim owns all change.** Even the water planes' scroll advances in a tick
  (`FerrymanRenderer::tickScroll`) — anything advanced per-render would run at the display's
  refresh rate instead of game speed (see
  [run-loop-and-timing](../../docs/guide/run-loop-and-timing.md)).

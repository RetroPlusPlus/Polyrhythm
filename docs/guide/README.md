# Polyrhythm — Developer Guide

This is the developer/modder-facing guide to the platform's public surface. Read it if you're
building a game on top of the platform, forking it for your own port, or modifying its
behavior. Each runtime subsystem has its own page under this directory — see the index below.

**New here? Start with [getting-started.md](getting-started.md)** — a complete ~60-line program that
opens a window and draws a scrolling background, explained line by line. Then read
[concepts.md](concepts.md) for the mental model, and [how-to.md](how-to.md) for task recipes. The
per-subsystem pages below are the full reference for each part.

## What the platform is

Polyrhythm is a platform for building, porting, and extending **8-bit / 16-bit, tile-based
retro games and applications** — the Game Boy / Game Boy Color / NES / SNES / Genesis /
Master System family idiom, and original games made in that style. It supplies the generic
infrastructure such a game needs — a fixed-step run loop, an action-based input surface, a
platform/window/GPU boundary, an `SDL_GPU` render pipeline with layered compositing and a built-in
effect library, an audio chain that can host a game's own sound driver, a VM for the narrow set of
routines that must run as original hardware code, and persistent storage for saves and player
files — while each consuming game supplies its own logic, data, and assets. The namespace and include
paths are `retropp`.

**Every surface is console-parameterized.** The viewport, palette, and timing surfaces all ship
presets for the whole console family (`ViewportResolution::Snes`, `PaletteSize::Genesis`,
`TickPeriodNs::Hz60`, …) and accept arbitrary values, so you target an NES screen, a 16-colour
palette, or a custom resolution just as easily. The defaults are Game-Boy-flavoured; they are
defaults, not constraints.

The design posture, everywhere: **the platform mirrors the data *model* of the 8-/16-bit era,
not any one console's hardware mechanism.** There are no hardware-register variables, no scanline
interrupts, and no mid-frame register pokes anywhere in the public surface. A frame is computed
*whole* from the game's logical inputs and submitted as data; colour is a palette index plus a
palette selected at render time; timing is a host-selected profile, not a baked constant. Out of
the box, with no enhancements enabled, the platform reproduces a consuming game's original behavior
faithfully; enhancements (output scaling modes, world zoom, audio packs, display filters) are
opt-in and off by default.

The whole public API lives in the `retropp` namespace, in headers under `include/retropp/`. Public
enums are `PascalCase` (`PadButton::FaceSouth`, `LayerContentKind::Tiles`).

## How these docs are organized

One page per subsystem. Each page documents the **public surface** (the types and functions you
call), explains the **behavior and intent** behind it, and — where relevant — tells you **where
to change things** if you're modding or forking. The pages are written against the shipped code;
when a field or function is *declared but not yet realized* (a forward seam for planned
work), the page says so explicitly rather than implying it works today.

## Index

### Start here

| Page | Covers |
|---|---|
| [environment-setup.md](environment-setup.md) | Setting a machine up from nothing on macOS, Windows or Linux — toolchain, shader compilers, submodules, first build, and what to do when a step fails. Start here on a fresh machine. |
| [getting-started.md](getting-started.md) | A complete minimal program — clone → build → a window with a scrolling, steerable tile background, explained block by block. |
| [concepts.md](concepts.md) | The mental model: how the core objects fit, sim/render decoupling, the "a frame is data" idea, and a glossary. |
| [how-to.md](how-to.md) | Task recipes — scroll, walk-behind, HUD, sprites, fades, recolour, region-confined effects, PNG loading, atlas slicing, animation, tweening, menus, a draggable title bar, the retained-vs-rebuilt frame patterns, music and SFX, audio files, glow, blend modes, mask-exact hit testing, sprite paths, saves and migrations, the player's other files, embedding assets, fullscreen and window scale, controller rumble, and running a routine as original hardware code. |

### Subsystem reference

| Page | Covers | Headers |
|---|---|---|
| [build-and-consume.md](build-and-consume.md) | Build modes, CMake targets (`retropp::engine` / `retropp::testkit`), consuming the platform as a submodule via `add_subdirectory`, versioning, dependencies, and the license posture. | `version.h` |
| [run-loop-and-timing.md](run-loop-and-timing.md) | The fixed-step simulation loop, the injectable clock, sim/render decoupling + automatic interpolation (including `advancesEvery`, the per-layer declaration of how often a world advances), exiting the application (`exitRequest` + the `exitAction` close-out guard every exit source routes through), frame pacing, and the host-selected timing profile. | `run_loop.h`, `clock.h`, `timing.h` |
| [input.md](input.md) | The action-based input system: game-defined actions bound to any sources in an `ActionMap` value (keyboard, pad buttons positional/printed-letter/family-qualified, mouse, sticks, triggers), per-tick held/edge/value state, player slots, the active-device signal, the raw pointer/analog surface, and controller vibration (the pad's output channel — per-tick declarative motor state, animation-shaped patterns). | `input.h`, `input_actions.h`, `analog_input.h`, `vibration.h` |
| [platform-and-windowing.md](platform-and-windowing.md) | The host-OS boundary (`Platform` seam), the production `SdlPlatform` (window + GPU device + event pump + native-chrome suppression + high-DPI), the `Window` surface behind `platform.window()` (position/size/fullscreen noun pairs that apply only on change, drawn-`Region` drag handles the OS drags the window by, automatic any-input window movement, the aggregate `WindowState` declaration), the windowed-host driver, the `EngineConfig` startup bundle + `setActive`, and the headless `MockPlatform` testing seam. | `platform.h`, `sdl_platform.h`, `window.h`, `windowed_host.h`, `engine_config.h` |
| [rendering.md](rendering.md) | The **Rondo** renderer. The `Renderer` object: the internal viewport + window-filling blit + nearest/bilinear sampling, the evaluation grid, the per-frame submission model, offscreen capture (`captureViewport`), post-process effects and registering a custom shader stage, atlas/palette upload, the upload skip (the renderer transfers only what changed), the `renderStats` counters, and how shaders reach the GPU. | `renderer.h`, `viewport.h`, `geometry.h`, `output.h`, `shader_format.h` |
| [draw-state.md](draw-state.md) | The `FrameDrawState` / `DrawLayer` submission envelope: arbitrary Z-sorted layers, scroll/size/alpha, the content variant (tiles, sprites, or a hosted machine's picture as `GuestFrameContent`), per-layer & per-sprite geometric transforms, per-layer tilemap wrap modes, the layer-key collision contract, screen-space effects (whole-layer + whole-frame, plus shape-confined via a `Region` that owns its effects — polygons or smooth curves, including cubic / Catmull-Rom boundaries evaluated exactly by a baked SDF mask), the built-in effects (`RowDisplacement`, `Ripple`, `Swirl` — an angular twist about a centre, the whirlpool — `ColorFill` — paint a solid colour / drawn line into a region — `Gleam`, a luminance-keyed diagonal sheen sweep, `ColorSaturation`, a cross-channel colour drain, `Bloom`, a threshold-blur-add glow halo, and `Glow`, an authored-colour aura), the per-region `alpha`, the per-row effect data table (`paramTable` — an array input read per-row by a custom effect), the `stencil()` see-through helper, and whole-frame colour (a `ColorFill` region under a blend mode). | `draw_state.h` |
| [sprites.md](sprites.md) | The `Sprite` drawable in full: the required `key` reconciliation identity and keying rules, placement (`x`/`y`, `origin` vs `pivot`, `center(Space)`), the arbitrary-but-8-aligned `size` read + console presets, art (`atlas`/`tile`/`palette`), texture flips + 90° `rotation`, per-sprite `alpha` and `blend`, the geometric `transform`, the `anchors` articulation resolvers (`anchor(k, Space)` / `toLayer`), the effect carrier (`effects` chain + `regions` — whole-silhouette, quad-space, and Below-scope scene lensing), the sprite mask (`mask` borrow / `freezeMask` snapshot / `maskShape` geometry — the sprite's image as a mask, exact `contains`/`bounds` collision, in `Quad`/`Layer` space), and what interpolates vs snaps. | `draw_state.h`, `geometry.h`, `sprite_mask.h` |
| [anchors-and-articulation.md](anchors-and-articulation.md) | Multi-part figures from plain sprites: anchors (named art-space points a sprite publishes; `anchor(k, Space)` resolver, label-or-index addressing, flips/rotation mirror them with the art), origin & pivot (`Sprite::origin` — the point `x/y` place; `Sprite::pivot` — the transform spin centre; set both to one point for a joint), `Sprite::center(Space)`, attachment by re-feeding a parent's anchor into a child's position each tick, per-sprite `z` (non-unique within-layer stacking, stable ties), and pinning a `Curve` / any point consumer to an anchor. | `draw_state.h` |
| [blend-modes.md](blend-modes.md) | Container blend modes: a `BlendMode` (Normal / Add / Subtract / Multiply / Screen / Half) on `Region` / `DrawLayer` / `FrameDrawState` / `Sprite` beside `alpha` — how a container's pixels combine with what they sit on; then the per-sprite effect surface that rides it: the `effects` chain and `regions` on one sprite, sprite displacement, custom effects on a sprite, and Below-scope sprite effects (the refraction lens); the `applyBlendMode` CPU mirror. | `draw_state.h`, `postprocess.h` |
| [tiles-and-colour.md](tiles-and-colour.md) | The indexed-tile + runtime-palette colour model: indexed atlases, palette upload (`uploadPalette`, and `loadPaletteImage` — a palette authored as an image), each tile/sprite naming its own sheet + palette directly (no per-layer set), the `tiles()` helper, flip/rotate, and wrap. | `draw_state.h`, `palette.h` |
| [images-and-transparency.md](images-and-transparency.md) | Loading art from PNG (`loadPng` → index plane + embedded palette), source routing, opt-in per-source index-hole transparency, and atlas asset ingestion (`loadAtlas` / `sliceLayout` → an `AtlasManifest` of carved slots). | `image.h`, `renderer.h` |
| [tilemaps.md](tilemaps.md) | Building a tile layer from images: the map-PNG → `IndexGrid` → `TileCatalog` → `AssembledTilemap` → `TileContent` pipeline, one layer mixing several sheets (each cell names its sheet directly), and collision-map decode. | `tilemap.h`, `image.h`, `draw_state.h` |
| [animation.md](animation.md) | Frame-based animation over the immediate-mode model: the `Animation` / `AnimationFrame` data model, the pure tick→frame resolver (`sampleAnimation` / `sampleAnimationFrame`), the four `PlaybackMode`s, and the game-owned `AnimationPlayer` cursor (plus multi-clip sheets and palette cycling). | `animation.h` |
| [tween.md](tween.md) | Value animation over the immediate-mode model: the `Tween` / `TweenSegment` data model, the `Easing` curve set, the pure tick→value resolver (`sampleTween` / `sampleTweenValue`), and the game-owned `TweenPlayer` cursor (fades, ramps, yoyos, effect/shader-parameter animation). | `tween.h` |
| [curve.md](curve.md) | The curve primitive: a piecewise-Bézier `Curve` with three authoring forms (Bézier handles, Catmull-Rom through-points, Hermite point+tangent), the pure queries — `at` / `tangent` / arc length (`length` / `atDistance` / `tangentAtDistance`) / `signedDistance` — a bakeable `ArcLengthTable` for repeated queries, and how it composes with a tween (shape vs speed). | `curve.h` |
| [path-walker.md](path-walker.md) | Moving along a curve over time: the `PathPacing` driver (constant speed / eased / a `Tween<float>` distance profile), the pure tick→(position + facing) resolver (`sampleWalk`), and the game-owned `PathWalker` cursor (hold-last facing, quantize-at-the-write, re-pathing). Completes the data→player family beside `Tween`/`TweenPlayer` and `Animation`/`AnimationPlayer`. | `path_walker.h` |
| [sprite-path.md](sprite-path.md) | Driving a whole sprite along a **route**: `SpritePathNode` (a movement spec + pacing + a facing policy + rotation / scale tween tracks + an animation), the `SpritePath` cursor that composes them off one clock and plays a **sequence** of chained-origin legs (node-local clocks, the wait / sentinel idioms, the four sequence playback modes) with an **interrupt stack** on top (departs from the current position, auto-pops on finish, resumes per `ResumePolicy` — `Continue` drifts the route on from where the detour ended (default), `Return` snaps back — plus explicit `popInterrupt`), and the `applyTo(Sprite&)` write (position, transform, frame art, flip — a union envelope over all held content) beside the raw composed sample. The orchestrator over the path walker. | `sprite_path.h` |
| [assets-and-embedding.md](assets-and-embedding.md) | Per-asset delivery policy: bake an asset into the binary (`Embed`) or ship it beside the binary (`LoadFromPath`), chosen in the `loadAtlas` / `loadMapPng` / `registerData` call; the per-type defaults; logical paths + the asset root; the build bakes/copies automatically with no build rule. Also the **data** family — arbitrary bytes registered with `DataLibrary::registerData` / `uploadData` and resolved by `data(DataId)`, which the platform never interprets. | `asset_policy.h`, `asset_registry.h`, `data_library.h`, `engine_config.h` |
| [vm-and-routines.md](vm-and-routines.md) | **Conductor**, the VM layer — the routine surface. The runtime VM host: registering a surgically-extracted routine and calling it like a typed C++ function, the developer-declared I/O binding, system selection, the `Throttle` pacing seam, hosting a resident sound driver (`hostDriver` / `tickDriver` / `readSlot`, banked placement), and the `divRng` preset. Hosting a whole cartridge is its own page below. | `vm.h`, `gb.h`, `gb_routines.h`, `object_key.h` |
| [co-execution.md](co-execution.md) | **Conductor** at its deepest: a cartridge running inside your game, with native code woven into it: `hostRom` makes an image addressable, `registerRegions` names the places inside it and `read`/`write` move their bytes (declared or built on the spot, across bank boundaries), `run` / `speed(num, den)` / `stop` boot and pace it on a thread of its own with a per-step publish, `registerEscapes` names places in the cartridge's own code where control leaves the guest — observing with `.handler` or answering instead of a routine with `.replaces = routine(binding, fn)` — `registerWatches` names places in its memory whose reads and writes native code decides — `AccessVerdict::proceed()` / `veto()` / `instead(v)`, per direction and per access — and `bindRoutine` calls the cartridge's own routines in the guest's own context, nested to any depth. `video(on)` / `video()` put the frames a machine finishes on a layer, on either clock. Also the threading rule every verb obeys, what each costs, and the failure modes. Game Boy / Game Boy Color today; more consoles planned. | `vm.h`, `guest_escape.h`, `guest_watch.h`, `memory_region.h`, `gb.h` |
| [audio.md](audio.md) | Audio: register on the `AudioLibrary` and cue by handle on an `AudioSystem` (chiptune or PCM), the Music/Sfx/Vocals tag, hosting a game's own resident sound driver (`host()` → the durable `HostedDriver` handle — `play`/`stop`/`slots`/`restart`/`close`), the `AudioMixer` volume levels, the `AudioSink` output (`SdlAudioSink`), running many audio systems at once, and console selection. | `audio_system.h`, `audio_library.h`, `audio_mixer.h`, `gb_audio.h`, `driver_binding.h` |
| [persistence.md](persistence.md) | Durable storage in the player's directory: the `SaveStore` byte-document store — the platform save directory resolved from the application identity (`AppIdentity` on `EngineConfig::identity`), atomic writes (a crash never corrupts the prior document), the absent-vs-corrupt error split, and consumer schema versions with a registered migration chain applied on read — plus `UserFiles`, the same directory without the document machinery, for extracted assets and other files a game writes back verbatim. | `save_store.h`, `user_files.h`, `app_identity.h`, `engine_config.h` |

## Coverage / status

The guide tracks the platform's shipped surface. Each subsystem's page is written and updated **in the
same change that ships the code** — the page is part of the deliverable, not a later write-up. The
guide always describes what the library actually does today; planned surfaces are called out as
planned, never implied to work.

| Subsystem | Status | Guide page |
|---|---|---|
| Build / consume | available | build-and-consume.md |
| Run loop + interpolation | available | run-loop-and-timing.md |
| Timing profile | available | run-loop-and-timing.md |
| Action-based input (game-defined actions, multi-source ActionMap, presets, per-family rows, player slots, active-device signal) | available | input.md |
| Pointer & analog input (mouse cursor in viewport coordinates, wheel, gamepad sticks/triggers/d-pad vector) | available | input.md |
| Controller vibration (per-tick declarative `gamepad(player).vibration(MotorLevels)`, animation-shaped patterns, per-slot diff to the device) | available | input.md |
| Platform / window / GPU device + `EngineConfig` startup bundle | available | platform-and-windowing.md |
| The window surface (`platform.window()` — position/size/fullscreen noun pairs applying only on change, drawn-`Region` drag handles via the OS hit-test, automatic any-input window movement, the aggregate `WindowState` declaration) | available | platform-and-windowing.md |
| Internal viewport + scaling/letterbox blit + build-time shaders | available | rendering.md |
| Draw-state envelope + layer-key contract | available | draw-state.md |
| Tile compositor + indexed/palette colour | available | tiles-and-colour.md |
| Sprites (placement, art, `alpha`/`blend`, transform, anchors, the `effects`/`regions` carrier + Below-scope lensing, the sprite mask) | available | sprites.md |
| Sprite anchors + origin/pivot placement + per-sprite z-order (published points, mount-on-anchor attachment, within-layer stacking) | available | anchors-and-articulation.md |
| Frame-level whole-frame colour (`ColorFill` region + blend) + N-layer composition | available | draw-state.md |
| Container blend modes (`BlendMode` on `Region` / `DrawLayer` / `FrameDrawState` beside `alpha`; Add / Subtract / Multiply / Screen / Half + Normal) | available | blend-modes.md |
| Per-layer & per-sprite geometric transforms (scale/rotate/skew/perspective) | available | draw-state.md |
| Per-layer tilemap wrap mode (Repeat / Clamp / Blank) | available | draw-state.md / tiles-and-colour.md |
| Image ingestion (PNG) + per-source index-hole transparency | available | images-and-transparency.md |
| Atlas asset ingestion (slice an image → manifest; `Single`/`Tileset`/`SpriteSeries`; all 8 read orders) | available | images-and-transparency.md |
| Tilemap image import (map PNG → `IndexGrid`, id-keyed `TileCatalog` → `AssembledTilemap` → `TileContent`; 16-bit grayscale maps; collision-map decode) | available | tilemaps.md |
| Multi-sheet tile/sprite layers (one layer mixes several sheets — each cell/sprite names its sheet directly; flat atlas store + global region table) | available | tilemaps.md / tiles-and-colour.md |
| Frame-based animation (`Animation` + pure tick→frame resolver + `AnimationPlayer` cursor; multi-clip sheets, palette cycling) | available | animation.md |
| Value animation (`Tween` + `Easing` curve set + pure tick→value resolver + `TweenPlayer` cursor; fades, ramps, yoyos, effect/shader-parameter animation) | available | tween.md |
| Curve primitive (piecewise Bézier; Catmull-Rom / Hermite authoring; `at` / `tangent` / `atDistance` / `tangentAtDistance` / `signedDistance` queries; bakeable `ArcLengthTable`) | available | curve.md |
| Path walking (`PathWalker` cursor + pure `sampleWalk` resolver + `PathPacing` — constant speed / eased / `Tween<float>` distance profile; position + facing along a curve over time) | available | path-walker.md |
| Sprite paths (`SpritePath` cursor composing movement + rotation / scale tween tracks + an animation + a facing policy off one clock; chained-origin node **sequences** + sequence playback modes + the **interrupt stack** with `ResumePolicy` drift-on-resume (default) or snap-back; `applyTo(Sprite&)` write plus the raw sample) | available | sprite-path.md |
| Direct-RGBA image sources | deferred (gated on a consumer needing non-indexed art) | images-and-transparency.md (seam noted) |
| Window scaling (N× viewport, clamped to display) + nearest/bilinear sampling + native fullscreen + high-DPI | available | rendering.md / platform-and-windowing.md |
| Frame-level screen-space effects (row displacement, post-process chain) | available | rendering.md / draw-state.md |
| Per-layer screen-space effects (`Layer` isolated / `Below` adjustment-layer scope) | available | draw-state.md |
| Region-confined screen-space effects (a `Region` owns a shape — polygon, curve, or SDF — the effects applied inside it, and an `alpha` for their opacity; layers and the frame own a list of them) | available | draw-state.md |
| Built-in effect library (`RowDisplacement` wave, `Ripple` droplet, `Swirl` whirlpool, `ColorFill` solid colour fill / drawn line, `Gleam` diagonal sheen sweep, `ColorSaturation` colour drain, `Bloom` glow halo, `Glow` authored-colour aura — name the kind, the platform owns the shader) | available | draw-state.md |
| `stencil()` see-through helper (a `Transparency` effect in a region: make a layer transparent inside or outside a shape, feathered — reveal the layers below or the backdrop; each side an optional effect-able region) | available | draw-state.md |
| Custom shader-stage hook (game-registered fragment as a first-class effect) | available | rendering.md / draw-state.md |
| Per-row effect data table (`ScreenSpaceEffect::paramTable` — an array the game fills each frame, read per-row by a custom effect via `paramRow` / `paramRowAtUv`) | available | draw-state.md |
| Built-in post-process display filters (CRT, scanlines) | planned (author as a custom stage today) | rendering.md |
| VM host (run an extracted routine as a typed function) + Game Boy RNG presets | available | vm-and-routines.md |
| VM host hardware-speed throttle (audio-driver path) | available | audio.md |
| VM host multi-instance (anti-channel-stealing) | planned (seam present) | vm-and-routines.md |
| Audio system (register a sound-driver on the `AudioLibrary`, cue by handle on a system, many systems at once, `SdlAudioSink` output) | available | audio.md |
| Audio files (register a `.wav` / `.ogg` / `.flac` / `.mp3` and stream it on an `AudioKind::Pcm` system) | available | audio.md |
| Hosting a game's own resident sound driver (`host()` → a durable handle driven by the player's own verbs — `play` / `stop` / `slots` / `restart` / `close`) | available | audio.md / vm-and-routines.md |
| Hosting a whole cartridge (`hostRom` makes an image addressable; `MemoryRegion` names the places inside it; read/write them directly) | available | vm-and-routines.md |
| Running the hosted cartridge (`run` / `speed(num, den)` / `stop` — its own thread, holding the platform's own speed or any live-adjustable fraction of it, declared regions published per step) | available | vm-and-routines.md |
| Guest escapes (`registerEscapes` — native code at declared places in the cartridge's program: observe with `.handler`, or replace a routine with `.replaces = routine(binding, fn)` answering in its own calling convention) | available | co-execution.md |
| Access watches (`registerWatches` — native code decides the guest's own reads and writes of a declared place: `AccessVerdict::proceed()` / `veto()` / `instead(v)`, per direction, per access) | available | co-execution.md |
| Calling the cartridge's own routines (`bindRoutine` names one where it already sits and hands back a `Routine<Sig>`; the call runs in the guest's own context, at any depth, and gives every register back) | available | vm-and-routines.md |
| A hosted machine's video (`VmFeatures{.video = true}` or `video(on)`; `video()` hands the last complete frame to a `DrawLayer` as `GuestFrameContent`, composited by z among native layers, on either clock) | available | co-execution.md / draw-state.md |
| Anti-channel-stealing routing (splitting one driver's channel writes across parallel sound chips) | planned | audio.md |
| Persistence (`SaveStore` — atomic versioned byte documents at the platform save location; migration chain on read) | available | persistence.md |
| Player's own files (`UserFiles` — the same directory, atomic writes, relative paths with subdirectories, no envelope) | available | persistence.md |
| Data assets (`DataLibrary` — arbitrary bytes registered by path or handed over, resolved by `DataId`, never interpreted by the platform) | available | assets-and-embedding.md |

"Planned" means the surface does not exist in the platform library yet. Where a *type seam* for future
work is already present in shipped headers (e.g. `SpriteContent`, `ScreenSpaceEffect`), the relevant
page documents it as a declared seam and says what it does and does not do today.

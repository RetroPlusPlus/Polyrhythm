# Platform & windowing

The host-OS boundary: the window, the GPU device, the OS event pump, frame pacing, and the lifecycle.
Everything that touches the operating system lives behind one seam so the rest of Polyrhythm never
depends on a live device — plus the startup configuration that drives it all.

```cpp
#include "retropp/platform.h"        // Platform (the abstract seam) — includes the window surface
#include "retropp/sdl_platform.h"    // SdlPlatform (the production implementation)
#include "retropp/window.h"          // Window, WindowState, WindowMovement, MotionSource
#include "retropp/windowed_host.h"   // WindowedHost (the hosted-mode driver)
#include "retropp/engine_config.h"   // EngineConfig, WindowConfig, EnhancementToggles
```

## Contents

- [The seam: `Platform`](#the-seam-platform)
- [The production platform: `SdlPlatform`](#the-production-platform-sdlplatform)
- [The window: `Window`](#the-window-window)
- [The hosted-mode driver: `WindowedHost`](#the-hosted-mode-driver-windowedhost)
- [Frame pacing](#frame-pacing)
- [Startup configuration: `EngineConfig`](#startup-configuration-engineconfig)
- [Where to change things](#where-to-change-things)

## The seam: `Platform`

```cpp
class Platform {
public:
    // Lifecycle + input, once per host iteration.
    virtual void        pumpEvents()           = 0;  // drain OS events, rebuild the input sample
    virtual bool        quitRequested() const  = 0;  // user asked to close the window (a one-shot latch)
    virtual void        clearQuitRequest() noexcept = 0;  // clear the latch (WindowedHost, on an exit Veto)
    virtual const InputSample& input() const   = 0;  // per-slot action + analog sample, as of the last pump

    // Pointer modes.
    virtual void pointerCaptured(bool) = 0;       // relative (spinner / mouse-look) capture
    virtual bool pointerCaptured() const  = 0;
    virtual void cursorVisible(bool)   = 0;       // show/hide the OS cursor (independent of capture)
    virtual bool cursorVisible() const    = 0;

    // The window (full surface in "The window: Window" below).
    Window& window();                     // the window object
    void    window(const WindowState&);   // aggregate declaration: engaged fields applied, omitted untouched

    // Window / display seam primitives — Window fronts position/size/fullscreen.
    virtual PixelSize drawableSize()      const = 0; // window's physical pixel size, for letterboxing
    virtual PixelSize windowSize()        const = 0; // the window's size (LOGICAL points)
    virtual void      windowSize(PixelSize)     = 0;
    virtual PixelSize usableDisplaySize() const = 0; // display work area (logical), for scale clamping
    virtual void      fullscreen(bool)       = 0; // enter/leave OS-native fullscreen
    virtual bool      fullscreen() const      = 0;
    virtual void      suppressNativeWindowChrome(bool) = 0; // remove/restore the OS title bar + border
    virtual bool      suppressNativeWindowChrome() const = 0;
    virtual Vec2i     windowPosition() const = 0;    // the window's top-left (LOGICAL points, signed)
    virtual void      windowPosition(Vec2i) = 0;     // place it

    // Drag hit-test: Window registers its containment predicate here; the platform's OS hit-test
    // asks dragHit(viewportPos) on every drag query. Shapes never cross the seam — only the predicate.
    using DragTest = bool (*)(void* user, Vec2i viewportPos);
    void dragTest(DragTest test, void* user) noexcept;   // register (stored on the base)
    bool dragHit(Vec2i viewportPos) const;               // run it (false when none registered)

    // Frame pacing (used by WindowedHost — see "Frame pacing" below).
    virtual std::chrono::nanoseconds nowMonotonic()         const = 0;
    virtual std::chrono::nanoseconds displayRefreshPeriod() const = 0;
    virtual void                     sleepPrecise(std::chrono::nanoseconds) = 0;
};
```

`Platform` is the host-OS abstraction — window + GPU present + input + pacing + lifecycle — expressed as
an interface so Polyrhythm's scheduling and input logic can run with **no live device**. It is to the
platform what the run loop's injectable `Clock` is to time: the same seam discipline. The production
implementation is `SdlPlatform`; tests drive a `MockPlatform`, which keeps the whole windowed-host
interleave (input, pacing included) verifiable headlessly.

No hardware-register or scanline idioms cross this boundary. Input is one sampled `InputSample` —
per player slot, the active action set, per-action values, the analog/pointer surface, and the
active-device signal; a frame is presented whole. There are no per-line, per-register, or mid-frame
hooks.

**Input.** `input()` is the whole sample: the platform translates the game's `ActionMap` (handed
over via `SdlPlatform::actions`) against every connected device each pump, per player slot —
digital action levels, per-action vector/axis values, the raw pointer/analog surface (cursor mapped
into **viewport pixels**, relative motion, wheel, mouse buttons, sticks / triggers), and which
device last produced input. `pointerCaptured(true)` switches the pointer to relative
(spinner / mouse-look) mode — the OS cursor is hidden and confined and motion arrives as unbounded
deltas; there is no meaningful absolute cursor while captured. `cursorVisible(false)` hides the OS
cursor **without** capturing (for a game that draws its own reticle while keeping absolute tracking
live) — an orthogonal knob. Full analog surface in [input.md](input.md).

## The production platform: `SdlPlatform`

```cpp
class SdlPlatform : public Platform {
public:
    explicit SdlPlatform(const EngineConfig& config = EngineConfig::active);

    SDL_GPUDevice* device() const noexcept;      // the live device the Renderer draws with
    SDL_Window*    sdlWindow() const noexcept;   // the live SDL window the Renderer presents into

    PixelSize windowSize() const override;              // the window's size (logical points)
    void      windowSize(PixelSize size) override;      // resize to N× the viewport (logical points)
    PixelSize usableDisplaySize() const override;       // the window's display's usable area (logical)
    void      fullscreen(bool enabled) override;     // native fullscreen (a real macOS Space)
    bool      fullscreen() const override;

    void actions(const ActionMap& map);                    // the game's bindings, replaced wholesale
    const ActionMap& actions() const noexcept;

    void assignGamepad(SDL_JoystickID id, int player);        // route a pad to a player slot
    void assignKeyboard(int player);                          // the keyboard+mouse unit is one device
    std::vector<GamepadInfo> connectedGamepads() const;       // {id, family, slot} per connected pad
};
```

The constructor initializes SDL (video + gamepad + audio), **creates the window sized to
`viewport × enhancements.windowScale` logical points, clamped down to fit the display**, acquires the
GPU device, claims the window for it, and sets vsync present. The destructor releases them in reverse
order. It is single-threaded — every call runs on the platform thread.

The default constructor argument is `EngineConfig::active` (the set-once active config — see
[`EngineConfig`](#startup-configuration-engineconfig) below), so a bare `SdlPlatform platform;` opens
the window the host configured. With no `setActive()` call, `active` is the faithful Game Boy Color
baseline.

**It owns the window, device, and input — not the drawing.** Drawing is the `Renderer`'s job: the
renderer takes `device()` / `sdlWindow()` and submits frames against them (see [rendering.md](rendering.md)).
The platform hands out the live device rather than hiding it behind a `present()` method, so the
renderer owns the pipeline and viewport independently. SDL types appear in this header by design (this
is the SDL platform).

**Input.** The platform samples the game's `ActionMap` against every connected device each pump —
nothing is filtered; any bound source sets its action, and with no `actions` call no actions are
reported. Bindings target device classes ("a pad's south button"), so a pad connect/disconnect/swap
needs no re-registration: the platform opens every pad, detects each one's family, and resolves the
printed-letter aliases and family-qualified rows against it at sample time. Every device feeds
player slot 0 until `assignGamepad` / `assignKeyboard` routes it elsewhere. See
[input.md](input.md) for the whole binding surface.

**Window scale.** The presentation scale is *window size*, expressed as an integer multiple of the
viewport in **logical points**: the window opens at `viewport × enhancements.windowScale` (default
**4×**), and the renderer auto-fills it crisply. Logical points (not physical pixels) keep the
perceived size the same on any display density — "4×" reads the same on a Retina laptop and a 1080p
monitor. The size is always **clamped down to fit the usable display** (`fitWindowScale` in
[geometry.h](rendering.md), fed by `usableDisplaySize()`), so even a large viewport never opens a
window bigger than the screen — it steps down to the nearest ratio that fits (floor 1×). At runtime,
`window().size(viewport × N)` resizes to a new scale; a settings menu computes the clamped `N` the
same way and calls it.

**Native fullscreen.** `fullscreen(true)` puts the window into the host's *real* fullscreen
affordance — on macOS a fullscreen Space, elsewhere a borderless desktop fill — via
`SDL_SetWindowFullscreen` (the platform-native idiom, not a fake borderless window). It does **not**
make the window freely resizable; the renderer's fill blit absorbs the new target size with no
renderer change. `EngineConfig::enhancements.fullscreen` is applied once at construction (default
windowed), and the toggle is runtime-dynamic thereafter. `fullscreen()` reports the current state.

**Native window chrome.** By default the OS draws its native chrome around the window — the title bar,
border, and decorations. An app that draws its *own* chrome (a custom draggable title bar) wants the OS
chrome gone so the two don't stack. Set `EngineConfig::window.suppressNativeWindowChrome = true` and the
window is created **borderless from the first frame** (`SDL_WINDOW_BORDERLESS` is OR-ed into the creation
flags — never applied after the fact, so the native decorations never flash at launch). The runtime knob
`suppressNativeWindowChrome(bool)` flips it live afterward (`SDL_SetWindowBordered`), and the
argument-free `suppressNativeWindowChrome()` reads the current state — a noun that submits and reads, no
`set` verb. Making the custom bar actually drag the window is the [window](#the-window-window)'s
job — declare the drawn bar a drag handle and the OS drags the window by it.

## The window: `Window`

```cpp
// platform.window() — the one window, as an object. Noun setter/getter pairs; a setter is a no-op
// unless its value differs from the last one set.
class Window {
public:
    Vec2i     position() const;    void position(Vec2i);      // top-left (LOGICAL points, signed)
    PixelSize size() const;        void size(PixelSize);      // window size (LOGICAL points)
    bool      fullscreen() const;  void fullscreen(bool);     // OS-native fullscreen

    void dragHandles(std::span<const Region>);                // drawn Regions the OS drags the window by
    void dragHandles(std::initializer_list<Region>);          //   ({titleBar} / {titleBar, dockTab})
    std::span<const Region> dragHandles() const;

    void autoMove(WindowMovement);                            // the input-driven drag; None turns it off
    const std::optional<WindowMovement>& autoMove() const;    // the movement in effect (nullopt = none)
};

struct WindowMovement {
    ActionRef                 trigger;   // the game action that means "grab the window" (any game enum)
    std::vector<MotionSource> motion = {MotionSource::Pointer, MotionSource::LeftStick,
                                        MotionSource::Dpad};

    static const WindowMovement None;    // no movement — declare it to turn automatic movement off
};

enum class MotionSource : std::uint8_t { Pointer, LeftStick, RightStick, Dpad };

struct WindowState {                     // the aggregate declaration — platform.window(WindowState{...})
    std::optional<Vec2i>               position;
    std::optional<PixelSize>           size;
    std::optional<bool>                fullscreen;
    std::optional<std::vector<Region>> dragHandles;
    std::optional<WindowMovement>      autoMove;
};
```

The window is an object on the platform: `platform.window()` returns it, and everything about the
window — placement, size, fullscreen, drag handles, input-driven movement — reads and writes through
it (Polyrhythm is single-window by design; multiple windows are separate renderers). Making a
borderless window fully movable is two lines:

```cpp
platform.window().dragHandles({titleBar});               // the drawn bar IS the drag handle
platform.window().autoMove({.trigger = Action::Grab});   // any input can drag it too
```

**Setters apply only on change.** Each setter compares its value against the last one set through
this surface — the same value again is a no-op, with no OS call. The platform never re-asserts window
state on its own, so restating a value never fights a native drag or a user resize: declare
`position({40, 40})`, let the user drag the window elsewhere, declare `position({40, 40})` again —
the window stays where the user put it. Only a new value moves it.

**The aggregate form.** `platform.window(WindowState{...})` declares several fields in one value:
every engaged field is applied through the matching setter, every omitted field is untouched.
`WindowState{.size = PixelSize{640, 480}, .fullscreen = false}` sizes and windows the app and touches
nothing else.

**Native drag — `dragHandles`.** A drag handle is a drawn `Region`: build the title bar as a
`Region`, draw it in the frame, and declare the same value here. A real mouse press inside any declared
region hands the window to the OS window manager, which performs the drag natively (`SDL_SetWindowHitTest`
under the hood — pixel-perfect, zero per-frame work). The handle and the painted bar agree to the
pixel by construction; containment matches the drawn region exactly, curved boundaries included
(`hitsDragRegion` is the predicate, tested at the pixel's center — the platform's `dragHit` asks it on
every OS drag query). One call declares the whole set and replaces the previous one; an empty set
clears it. Pairs naturally with a borderless window (`suppressNativeWindowChrome`).

**Automatic movement — `autoMove`.** The path for input the OS cannot drag by: while the declared
`trigger` action is held, the window follows the declared `MotionSource` set — the pointer's raw
delta 1:1, a stick's deflection and the d-pad's unit vector at a fixed per-second rate, so drag speed
is identical on any display refresh; fractional motion carries across frames, so slow drags never
stall. This is how a gamepad-driven interface drags a window (a stick cursor plus a button "click").
The `motion` field is the complete set: specifying it replaces the default wholesale, so
`{MotionSource::Dpad}` means the mouse does not drive this drag, and a stick-cursor UI names the
stick the cursor is NOT on. All sources are window-independent quantities — following them cannot
feedback-jitter. `WindowMovement::None` declares the movement off — the getter reads none in effect,
the same state as never declaring (off is the default). The host drives the movement once per frame
through an internal `update()`; a game never calls it.

**Placement, size, fullscreen — the noun pairs.** `position` is the window's top-left in logical
points, signed (a window legitimately sits at negative coordinates on a multi-monitor desktop):
declare it to center on launch, snap, or restore a saved position; read it for where the window
actually is. `size` is the window's size in logical points — the read reflects user resizes and OS
clamps. `fullscreen` enters and leaves the OS-native fullscreen. A game that wants to drive movement
itself reads the inputs and writes `position` per tick — the same drag `autoMove` serves, written by
hand.

See `examples/window_drag` (native drag + automatic movement, and a toggle between the automatic
drag and the same drag hand-written over `position`) and `examples/Numberator` (the painted
classic-Mac title bar declared draggable).

**High-DPI.** The window is created with `SDL_WINDOW_HIGH_PIXEL_DENSITY`, so on a Retina/HiDPI display
its drawable is the true physical pixel resolution. `drawableSize()` reports physical pixels and the
renderer fills in physical pixels, so the art renders crisp at native resolution automatically (the
fill just picks a larger integer scale). See [rendering.md](rendering.md).

## The hosted-mode driver: `WindowedHost`

```cpp
class WindowedHost {
public:
    WindowedHost(RunLoop& loop, Platform& platform) noexcept;
    void run();   // pump → push input → advance → pace, until an exit resolves
};
```

`WindowedHost` is the windowed replacement for the run loop's headless `run()`. It owns nothing and
depends only on the two abstractions — a `RunLoop` and a `Platform` — so the whole interleave is
unit-testable against a `MockPlatform` with no live window or GPU device.

Each iteration:

1. `pumpEvents()` — drain OS events and rebuild the input sample.
2. push the platform's current input into the loop: `setRawInput(platform.input())` — per-slot
   actions, values, and analog/pointer in one sample.
3. `advance()` the simulation once. The present happens *inside* `advance()` via the consumer's render
   callback, so the run loop's "render once per advance with `alpha`" contract is preserved unchanged
   — the host owns only the scheduling.
4. pace to the next frame deadline (below).

The OS window-close button is unioned into the run loop's exit request, so it runs the loop's close-out
guard like a programmatic quit. A guard that vetoes the close clears the platform's quit latch
(`clearQuitRequest()`) and the app keeps running; otherwise the loop stops when the exit resolves
(`RunLoop::exitResolved()`). See the exit surface in
[run-loop-and-timing.md](run-loop-and-timing.md#exiting-the-application). A typical `main()` is just:
construct config → clock → loop → platform → renderer, wire the loop's tick/render callbacks, then
`WindowedHost{loop, platform}.run();`.

The host also drives the platform's [window](#the-window-window) once per frame — unconditionally,
since the pointer's raw delta is per-pump and a zero-tick frame must not drop automatic-movement
drag motion. A game without interactive window behavior never notices.

## Frame pacing

After each present, `WindowedHost` sleeps to a monotonic frame deadline spaced by the display's
current refresh period, so the loop runs at the monitor's cadence rather than relying on the vsync
present block to throttle it (not every platform reliably blocks on present while the window is idle).
When the present *does* block, the deadline is already reached and the computed sleep is ~0, so the two
compose rather than fight.

```cpp
struct FrameDeadline {
    std::chrono::nanoseconds nextDeadline;   // carry to the next iteration
    std::chrono::nanoseconds sleepFor;       // time to sleep now (never negative)
};

constexpr FrameDeadline nextFrameDeadline(std::chrono::nanoseconds prevDeadline,
                                          std::chrono::nanoseconds period,
                                          std::chrono::nanoseconds now) noexcept;
```

`nextFrameDeadline` (in [pacing.h](run-loop-and-timing.md)) is **pure** — no clock, no sleep, no SDL —
so the pacing decision is unit-testable. The OS-coupled half is three `Platform` methods:
`nowMonotonic()` (current monotonic time, same clock domain as the sleep), `displayRefreshPeriod()`
(queried live each iteration, so dragging the window to a different-refresh monitor re-paces with no
event handling; 60 Hz fallback), and `sleepPrecise()` (high-resolution sleep, no-op when ≤ 0). An
iteration that finishes late re-anchors the deadline to `now` rather than carrying the lateness, so a
stall's backlog is dropped and recovery never fast-forwards through unpaced frames. This is
presentation pacing only; the sim's own catch-up clamp is `kMaxFrameTime` (see
[run-loop-and-timing.md](run-loop-and-timing.md)).

## Startup configuration: `EngineConfig`

```cpp
struct WindowConfig {
    std::string title = "Polyrhythm";   // the window SIZE comes from windowScale × viewport
    bool suppressNativeWindowChrome = false;  // open borderless — no OS title bar/border (from frame one)
};

struct EnhancementToggles {        // faithful defaults at a sensible window size
    int          windowScale = 4;                      // window = viewport × this (LOGICAL points),
                                                       // clamped down to fit the display
    bool         fullscreen  = false;                  // native OS fullscreen toggle
    SamplingMode sampling    = SamplingMode::Nearest;  // blit sampler: Nearest (faithful) / Bilinear
};

struct EngineConfig {
    AppIdentity        identity{};                       // REQUIRED — setActive refuses it empty; first member
    WindowConfig       window{};
    ViewportResolution viewport     = ViewportResolution::GameBoyColor;  // 160×144 internal
    TimingProfile      timing       = TimingProfile::GameBoyColor;
    EnhancementToggles enhancements{};
    bool               interpolation  = true;                     // automatic render interpolation (rendering.md)
    EvaluationGrid     evaluationGrid = EvaluationGrid::Viewport; // analytic-path evaluation grid (rendering.md)
    std::filesystem::path assetRoot{};                            // runtime base for LoadFromPath assets;
                                                                  // empty = the executable directory
                                                                  // (assets-and-embedding.md)

    static EngineConfig active;                          // the set-once active config
    static void setActive(const EngineConfig& config);   // store it + seed the platform defaults
};
```

`EngineConfig` is the single value bundle a host hands the platform at startup — window + internal
viewport + render timing + enhancement toggles + the application identity. It holds
**platform-agnostic value types only** (no live device handles): the platform reads `window`, the
renderer reads `viewport`, the run loop reads `timing`, and a default-constructed `SaveStore` reads
the identity ([persistence.md](persistence.md)). Input carries no config field — the game's
`ActionMap` is a value handed to the platform directly (`SdlPlatform::actions`; see
[input.md](input.md)).

Every field defaults to the faithful Game Boy Color baseline — override only what you mean to
change — with one exception: **the identity is required.** Every program declares who it is
(organization + application) before it starts, exactly as every major platform demands of a
project; the identity also names the per-user save directory.

```cpp
EngineConfig config{
    .identity = {.organization = "MyStudio", .application = "My Game"},
    .window   = {.title = "My Game"}};
// everything else = faithful GBC
```

**`setActive` — the one-call startup.** `EngineConfig::setActive(config)` requires the identity
(it throws `std::invalid_argument` when either field is empty), stores the config as
`EngineConfig::active` **and** seeds its per-type defaults from it, so the bare
constructors inherit the configuration without you threading fields to each one:

```cpp
EngineConfig::setActive(config);   // do this once at startup
SdlPlatform platform;              // reads EngineConfig::active (window)
RunLoop     loop{clock};           // uses the timing setActive seeded
Renderer    renderer{platform.device(), platform.window()};  // uses the viewport setActive seeded
```

Concretely, `setActive` seeds these process-wide defaults from the config (what the bare constructors read):

- **`config.timing`** → `RunLoop::defaultTiming`, `AnimationPlayer::defaultTiming`, every interpolable
  `TweenPlayer<T>::defaultTiming` (`float`/`Vec2`/`Vec3`/`Vec4`), `PathWalker::defaultTiming`,
  `SpritePath::defaultTiming`.
- **renderer fields** → `Renderer::defaultViewport` (`config.viewport`), `defaultSamplingMode`
  (`config.enhancements.sampling`), `defaultInterpolation` (`config.interpolation`), `defaultEvaluationGrid`
  (`config.evaluationGrid`).
- **`config.identity`** → `SaveStore::defaultIdentity` ([persistence.md](persistence.md)).
- **`config.assetRoot`** → the asset root, resolved to an absolute path once (a relative/empty root joins
  onto the executable directory, an absolute one is used as-is) so every LoadFromPath asset resolves the
  same way ([assets-and-embedding.md](assets-and-embedding.md)).

Threading the fields explicitly still works and overrides the defaults per object —
`SdlPlatform{config}`, `RunLoop{clock, config.timing}`, `Renderer{dev, win, config.viewport}` — so a
program that needs two differently-configured objects can still construct them directly.

**Dynamic vs startup.** `viewport` and `timing` are consumed once at construction (startup-only).
The action map is runtime-dynamic (`SdlPlatform::actions`). The `enhancements`
toggles are read at startup (`windowScale` sizes the initial window in the platform ctor; `sampling`
seeds `Renderer::defaultSamplingMode`, a per-type default the renderer reads at construction — not a
setter call) and then driven live at runtime — `window().size()` / `window().fullscreen()` on the
platform, `samplingMode` on the renderer. `interpolation` and `evaluationGrid` are seeded the same way
(`Renderer::defaultInterpolation` / `defaultEvaluationGrid`).

## Where to change things

- **One-call startup:** build an `EngineConfig` and call `EngineConfig::setActive(config)` before
  constructing those objects; bare ctors inherit it.
- **Application identity (required):** `EngineConfig::identity` (`AppIdentity` — organization +
  application). Set it once, keep it stable forever; it names the per-user save directory
  ([persistence.md](persistence.md)).
- **Window title:** `EngineConfig::window.title`. The window *size* is `enhancements.windowScale ×
  viewport` (clamped to the display), not a `WindowConfig` field — change the scale, or the viewport
  (`EngineConfig::viewport`) to change what the game renders into.
- **Input bindings:** no config field — hand the game's `ActionMap` to the platform
  (`SdlPlatform::actions`; see [input.md](input.md)).
- **Presentation scale / fullscreen / sampling:** start from `EngineConfig::enhancements`
  (`windowScale` / `fullscreen` / `sampling`); toggle live via `window().size()` (size to
  `viewport × N`, clamp with `fitWindowScale` + `usableDisplaySize()`), `window().fullscreen()`, and
  the renderer's `samplingMode` (details in [rendering.md](rendering.md)).
- **Native window chrome:** `EngineConfig::window.suppressNativeWindowChrome = true` opens the window
  borderless from the first frame; toggle live via `SdlPlatform::suppressNativeWindowChrome(bool)` (read
  back with the argument-free `suppressNativeWindowChrome()`).
- **Pointer / cursor:** `pointerCaptured` for relative (spinner / mouse-look) mode,
  `cursorVisible` to hide the OS cursor while still tracking it; read the pointer off the input sample
  ([input.md](input.md)).
- **Porting to a non-SDL platform:** implement the `Platform` interface (the full virtual surface
  above — input, window/display, and the three pacing methods) and hand your device/window to the
  `Renderer`; everything above the seam is unchanged.
- **Headless / automated testing:** drive a `MockPlatform` (in the test tree) — the `WindowedHost`
  loop, pacing included, runs identically with no real window.

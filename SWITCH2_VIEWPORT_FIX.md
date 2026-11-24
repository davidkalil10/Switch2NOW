# Switch 2 UI viewport fix (local fork patch)

Base: `davidkalil10/Switch2NOW` v1.3.0 (commit `2c90cfe6`).

## Problem
After a GeForce NOW stream ends, the whole Switch2NOW UI is rendered into a
smaller viewport (typically 1280x720) until the app is closed and relaunched.
Dock/undock does not recover it.

## Cause
`app/src/stream/OpenGL/GLVideoRenderer.cpp:617` calls
`glViewport(0, 0, width, height)` with the stream view size on every rendered
video frame. On Switch 2, Borealis restores the viewport only inside
`SwitchVideoContext::resetFramebuffer()`, which `beginFrame()` calls only when a
display-resolution change event is observed. A missed event or the end of the
stream therefore leaves the shrunken viewport active for the UI.

## Patch
`extern/borealis/library/lib/platforms/switch/switch_video.cpp`
(`SwitchVideoContext::beginFrame`, inside `#ifdef __SWITCH2__`) now restores the
viewport to the current display resolution on **every** frame before NanoVG
starts drawing, with a fallback to the cached framebuffer size.

This is a one-behaviour change scoped to the Switch 2 OpenGL path. No gameplay,
network, authentication or decoding logic was modified.

## Second pass (video still small)
The first patch restored the UI after a stream, but on-console testing showed
the video itself still occupied only the top-left part of the display. Cause:
`GLVideoRenderer::drawLatest()` sets `glViewport(0, 0, width, height)` using the
**logical** Borealis view size (1280x720), while the Switch 2 EGL surface is the
full display resolution (1920x1080 docked).

The video renderer now also uses the real display resolution for its GL
viewport on `__SWITCH2__`, while keeping the logical view size only for the 16:9
UV aspect calculation. A one-shot `sdmc:/switch/SwitchNOW/viewport_fix.log`
records the view, display and chosen viewport sizes for verification.

## Third pass (fit inside the screen + transient display menu)
Testing of the second patch showed the video filling the screen but appearing
zoomed/cropped. The log recorded `view=1280x720 display=1920x1080` with a
`1280x720` decoded frame, so all aspect ratios were 16:9 and the old cover UV
math could not explain the crop. The likely cause is that
`appletGetDefaultDisplayResolution()` can report a resolution different from the
actual EGL surface after a dock change, so a viewport derived from it can be
larger than the surface.

The renderer now queries `eglQuerySurface()` for the authoritative physical
size, restores the full surface viewport in `SwitchVideoContext::beginFrame()`
from the same source, and supports a small set of modes:

- `FIT` (default): centred, aspect-correct viewport; the whole frame stays
  inside the screen with black bars on unused edges.
- `FILL`: cover mode, crops the longer edge.
- `STRETCH`: full frame stretched to the full surface.
- `ZOOM 1.25x`, `1.5x`, `2x`: centre crop at the selected zoom.

Hold **UP** on the controller for ~400 ms to reveal a small top-right panel.
Move the highlight with **LEFT/RIGHT**, confirm with **A**, or tap a row on the
touchscreen. The panel closes automatically 3 seconds after the last input;
there is no permanent on-screen control. The active mode is written to
`sdmc:/switch/SwitchNOW/viewport_fix.log` whenever it changes.

## Fourth pass (display modes in the Minus+Plus menu + quit to hbmenu)
The transient UP panel is useful, but the same display modes are now also
reachable from the existing **MINUS + PLUS** stream menu:

- New **Display mode >** action opens a submenu listing all six modes.
  UP/DOWN or LEFT/RIGHT moves the highlight, **A** applies it, **B** goes back.
  The applied mode is marked with `*`.
- New **Exit to hbmenu (quit app)** action stops the stream cleanly and calls
  `brls::Application::quit()`, so the process ends and hbmenu takes over. This
  is different from **Exit stream**, which only returns to the in-app launcher.
- New two-button chord **L + R** quits to hbmenu directly, without opening the
  menu. It is distinct from `MINUS + R3` (keyboard command layer) and
  `MINUS + ZL + ZR` (exit stream only).

`brls::Application::setGlobalQuit(false)` is unchanged; the explicit `quit()`
call still works because the main loop checks `quitRequested` directly.

## Final pass (full frame first, display modes only)
The user clarified that the problem was never the magnification: the decoded
game picture itself was a rectangular segment, and shrinking the viewport only
made that same segment smaller. Zoom was therefore removed completely.

Two real defects were fixed:

1. `GLShaders.hpp` used `uv = (tex_position - uv_data.xy) * uv_data.zw`. That
   subtracts the offset **before** scaling, so a centred window such as
   `u0 = 0.25, width = 0.5` became `-0.125 .. 0.375` instead of `0.25 .. 0.75`,
   sampling outside the texture and clamping at the edges. All four shaders now
   use the direct rectangle form `uv = uv_data.xy + tex_position * uv_data.zw`.
2. `checkAndUpdateScale()` applied an old aspect-derived UV crop before
   `drawLatest()` overwrote it. It now uploads `(0, 0, 1, 1)`; the GL viewport
   handles letterboxing and the display mode handles any intentional crop.

The contain (`FIT`) geometry was also corrected: for a wider frame the height
is reduced to `surfaceWidth / frameAspect`, and for a taller frame the width is
reduced to `surfaceHeight * frameAspect`. The earlier version had these two
branches reversed.

Only three display-shape modes remain, default **FIT**:

- **FIT (inside frame)** — whole frame inside the screen, letterboxed if needed.
- **FILL (crop edges)** — cover the screen, cropping only excess source pixels.
- **STRETCH** — full screen, full source rectangle, no aspect preservation.

Hold **UP** ~400 ms for the transient panel, or use **MINUS + PLUS** ->
**Display mode >**. UP/DOWN moves the highlight, A applies it, B goes back;
touch rows work in the transient panel. The **Exit to hbmenu** action and the
**L + R** quit chord from the fourth pass are unchanged.

`viewport_fix.log` records the applied mode, viewport, UV rectangle and plane
line sizes. On Switch 2 the first decoded frame is also dumped to
`sdmc:/switch/SwitchNOW/first_frame.ppm` so we can verify whether the decoded
frame itself contains the whole picture.

## Advertised viewport decoupled from encoded resolution (v9)
The 1080p Quality preset never reached signaling: `signaling.log` stayed
byte-identical to the last successful 720p session and no 3 s / 8 s probes were
created. The server's H.264 offer only declares Level 3.1 (`42001f`, `42e01f`),
which cannot encode 1920x1080, so a 1080p request is unsupported here. The saved
settings were restored to the verified Balanced 1280x720 preset. v9 keeps the
decoder and encoded stream at 1280x720 but advertises the actual 1920x1080
client viewport in `a=video.clientViewportWd/Ht`, testing whether the server was
cropping a top-left 1280x720 region of the 1920x1080 desktop.

## Frame-probe timing extension
The first frame can be a dark loading or transition frame, so it is not enough
to decide whether the crop exists in the decoded stream. The probe now also
writes `frame_probe_3s.ppm` and `frame_probe_8s.ppm`, captured from decoded
frames about 3 and 8 seconds after the first one.

## Status
Unofficial community patch. Not reviewed or released by the upstream author.
Build artifacts come from this fork's `switch2-build.yml` workflow.

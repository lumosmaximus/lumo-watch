# Ideas for next time

## Remote: ambient mode (asked 2026-10-12)

After 30 s without a touch, the remote's controls (knob, buttons, arrows,
level meter, text) fade away and the whole screen becomes a calm, slow,
full-screen visualiser while something plays. Any touch brings the controls
back (fade in); the first touch should only wake it, not press a button.

Notes for building it:
- Core 1 RAM is ~97% full: draw the visualiser procedurally (no frame
  buffer); budget per frame, full screen at 30 fps is ~65k px per update in
  bands, so keep the per-pixel maths cheap (polar lookup per band, or only
  redraw an annulus / a few moving shapes).
- Fades: blend the controls with the background over ~1 s (a global alpha in
  remote_px, redraw everything a few times), not instant.
- Paused: let the visualiser settle to a still, dim state (or the clock?).

## Other

- Start the Mac helper at login (launchd agent); Windows helper (media
  session gives any app's title and state, pycaw for volume/mute, WMI for
  brightness).
- Remember the last face across resets (core 0 settings in flash).
- Raw HID instead of serial for the helper link (product-grade).
- Free core 1 RAM (main stack, ring strip compression) before more features.

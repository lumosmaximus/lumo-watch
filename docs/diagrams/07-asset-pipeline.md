# Diagram 7: from Python drawings to pixels on the panel

Sources: `tools/watchface/watch.py`, `chrono.py`, `knob.py`,
`apps/watch/remote/src/*_img.h`, `remote/src/main.c` (ring stage).

Draw a left-to-right pipeline:

**1. Generators (Python + Pillow, on the computer)**
- `watch.py`: GMT dial (466x466, rendered 3x supersampled then downscaled),
  31 date-window images, and the **bezel ring in polar form**.
- `chrono.py`: the two chronograph dials (tachymeter text laid out letter by
  letter along the curve).
- `knob.py`: the remote's knob ring, same polar form as the bezel.

**2. The polar lookup (the clever part, draw it big)**
- The ring is unwrapped into a strip: 1440 angles x 43 radii (0.25 degree
  steps). Each strip entry is pre-blended onto what lies beneath (grey rehaut
  inside, black outside) so it is opaque, then quantised to a 256-colour
  palette: 62 KB, small enough to copy into core 1's internal RAM at boot.
- For every screen pixel of the ring (65,028 of them) a 16-bit index into the
  strip is precomputed (113 KB, stays in flash, read sequentially), plus
  per-row spans.
- Turning the bezel by k steps = subtract k x 43 from every index. Drawing a
  ring pixel = one table read + one palette read: ~5.6 ms for the whole ring.
- Swapping `knob.py`'s strip for the bezel's turns the GMT bezel into the
  remote's knob with the same code; a second palette makes it amber.

**3. Output: C arrays** (`watch_img.c`, `speed_img.c`, `knob_img.c`, ~6 MB of
source, ~1.7 MB of flash) linked into core 1's image; `*_img.h` carry the
geometry constants (bezel radii, date window position, strip dimensions).

**4. On the watch**: dials read from flash through the cache (~20 cycles per
pixel sequential), ring strip and palettes in RAM, hands and the remote's
buttons/arrows/visualiser drawn procedurally from shape functions (bars,
triangles, discs with analytic anti-aliasing), the now-playing text drawn on
the computer and streamed as a 2-bit image.

Show sizes on the arrows. Pink for the core 1 side, grey for the computer.

# Making a watch face glide: an optimization log

Board: Waveshare ESP32-S3-Touch-AMOLED-1.75 (466x466 round AMOLED, CO5300 over
QSPI, ESP32-S3 at 240 MHz), Zephyr, both cores used as AMP. Goal: a GMT watch
face whose second hand glides at the panel's 60 Hz refresh, with a bezel you
can turn by touch, clicking at 1-minute (6 degree) steps.

This is the order things actually happened, dead ends included. Numbers are
from the board, printed once a second by core 0 from stats core 1 sends it.

## 0. The setup: two cores, two images

Zephyr has no SMP for the ESP32-S3, so the two cores run AMP: two separate
Zephyr images, built together with sysbuild.

- **Core 0 (PROCPU):** I2C sensors, RTC, touch, buttons, (later) audio, USB,
  Wi-Fi. It boots first and loads core 1's image from its own flash partition.
- **Core 1 (APPCPU):** the display, nothing else. 128 KB of internal RAM of its
  own, the SPI2 controller and GDMA channel pair 0.
- **Between them:** IPM messages over a small shared-memory block (~12 us round
  trip). Core 0 sends the time on each RTC second edge and forwards touches;
  core 1 sends frame statistics back.

Core 1 lists only GDMA pair 0's interrupts in its devicetree, so it never
takes core 0's I2S DMA interrupts (pair 1).

## 1. LVGL: 12-26 fps

The first face was LVGL widgets with rotated image hands. 12 to 26 fps,
varying with the hand angle.

## 2. Quad flash: 23-57 fps, and a jiggling dot

Core 1 runs its code and reads the dial image from flash through the cache.
The flash was in dual mode; the W25Q128 supports quad I/O. Setting QIO in the
board's defaults doubled flash bandwidth: 23-57 fps.

The second hand (an LVGL line snapped to whole pixels) visibly jiggled: its
lume dot hopped between pixel positions. A gliding hand needs sub-pixel
positions and anti-aliasing.

## 3. LVGL image hand: down to 4 fps

Drawing the second hand as a rotated image with small invalidated areas
overflowed LVGL's invalidation list (`LV_INV_BUF_SIZE`, default 32). On
overflow LVGL redraws the whole screen: 4 fps. Raising it to 96 fixed the
cliff but it was still slow.

## 4. Own compositor: 4-10 fps, then measure instead of guess

LVGL went; core 1 got a small compositor of its own. Each dirty pixel is built
back to front: dial (RGB565 in flash), date window, rotated bezel, hands,
second hand. First result: 4-10 fps.

- `floorf`, `sqrtf`, `fmaxf` were library calls, not FPU instructions
  (`-fno-math-errno`, own inline min/max/floor).
- Hot loop in IRAM, helpers force-inlined. Still ~2.4 us per pixel.

A micro-benchmark on the device settled where time went: a flash read cost
only ~20 cycles per pixel, but a plain dial-only box cost **315 cycles per
pixel**. The per-pixel code was the problem, not memory.

Restructuring from "per pixel, ask every layer" to "per row, each layer does
its span" cut that to **107 cycles/pixel** and took the face from 20 to a
steady 30 fps.

## 5. Stage counters: the hands were the cost

Cycle counters around each stage of a row (background copy, bezel, hands,
second hand, byte swap) showed the rotated, bilinear-filtered image hands
costing 12-20 ms a frame.

- **Vector hands:** each hand is described in its own coordinates (s along
  it, p across it) as bars, triangles and discs, with analytic anti-aliasing.
  Disc coverage uses the squared distance with a linear ramp, so there is no
  `sqrt` per pixel.
- **Span solving:** for each row, solve where the row crosses a hand's
  footprint (a few linear inequalities) and shade only that x range.
- **Cheap path:** most second-hand pixels are on the plain shaft, so it has a
  fast path that skips the dot, counterweight and cap tests.

Result: **59 fps**.

## 6. Frame pacing and dirty bands

- **TE lock:** the panel's tearing-effect pin (GPIO13) pulses at the start of
  each refresh. Core 1 polls it and starts each frame there: no tearing, even
  cadence.
- **Dirty bands:** only what moved is redrawn: the second hand's old and new
  positions, other hands when their angle changes, the date when it changes.
  Areas are tracked as 8-row bands, each only as wide as needed, and each band
  is one window write to the panel.
- Time comes from core 0 at each RTC second; core 1 adds milliseconds from its
  own uptime. Angles are in 0.1 degree units, so at 60 fps the second hand
  moves exactly 0.1 degree per frame.

## 7. The bezel

- **Bilinear rotation:** first version. Turning the bezel repaints the whole
  ring, which took 20-90 ms a frame: dragging stutters.
- **Polar lookup table:** each ring pixel precomputed with its angle and
  radius index; turning the bezel just offsets the angle. Ring stage 0.4 ms.
  A bug at 9 o'clock (a negative window start, not clamped) left pixels that
  never updated.
- **Progressive repaint:** the ring repainted a few bands per frame within a
  time budget, so the second hand never waited. It looked worse: for a few
  frames parts of the ring showed the old angle.

## 8. The lesson: cadence beats average frame rate

By now the face ran 59 fps most of the time, but frames that took just over
16.7 ms (second hand crossing another hand, bezel moving) missed a refresh and
dropped to 30 fps for a second or two. It looked worse than earlier, slower
versions. The eye notices the change of rhythm, not the average.

Things tried that made it worse:

- **Async writer thread:** a second thread on core 1 sent band buffers while
  the main thread composed the next. Compositing got ~1.4 ms slower (thread
  switches, interrupts per transfer) and the write time did not overlap.
- **Start late frames immediately** instead of waiting for the next TE: the
  frame start drifts off the refresh, which shows as judder.
- **Locking to a steady 30 fps:** smooth but not what we wanted.

So we went back to the version before the bezel lookup table, which looked
best, and attacked the worst-case frame instead.

## 9. Direct SPI: write time 3.4 ms -> 0.5 ms

Each dirty band was a `display_write()`: three SPI driver transactions
(column address, row address, pixels), each reconfiguring the GDMA, with the
single-line command header "bit-expanded" into quad data. About 100 us of
overhead per window, ~30 windows a frame.

`qspi_fast.c` now drives the SPI2 registers directly after Zephyr has set
things up (pins, clock, GDMA channel connection):

- The SPI peripheral's own command (8 bit) and address (24 bit) phases send
  the CO5300 header on one line, then data on one line (parameters, from the
  CPU buffer) or four lines (pixels, by DMA). No bit expansion.
- Column/row address commands take about a microsecond each.
- **Double buffering without a thread:** two band buffers. The pixel DMA for
  one band is started, and the CPU composes the next band into the other
  buffer; before reusing a buffer it waits for that buffer's transfer to
  finish.
- The SPI's "TX FIFO ran empty" flag is counted every transfer: zero, so the
  DMA keeps up while the CPU works.

## 10. The hand cache that corrupted the screen

The hands stage still cost 4-9 ms when the second hand crossed other hands.
The other hands move at most once a second, so a cache kept the finished
pixels under them, refreshed only where a hand moved. Hands stage: 5.5 ms ->
0.5 ms, frames ~9 ms.

But the panel showed corrupt pixels. Bisecting on the device:

| Build | Result |
|---|---|
| direct SPI, no cache | clean |
| + hand cache | corrupt |
| cache compiled out | clean |
| cache on, every cached pixel re-checked against a full recompute | corrupt on screen, zero visible mismatches in memory |
| cache on, RAM overlap fixed (below) | corrupt |
| cache on, no DMA/compositing overlap | corrupt |
| cache logic off, cache memory kept (same buffer addresses) | clean |

The cache's pixels were right in memory, yet the screen was wrong only with
the cache logic running. Unsolved; the cache was removed, since direct SPI
alone keeps the frame under budget.

**A real bug found on the way:** core 0 and core 1 disagreed about where RAM
ends. Core 0's image (simple-boot format) took 0x3fce9704; core 1's (MCUboot
format) took the IPC area start, 0x3fce5000. In Zephyr's `memory.h` the
simple-boot case wins over the AMP case, so core 0 carved out core 1's 128 KB
from 18 KB too high: the top of core 0's heap overlapped the start of core
1's code. It stayed hidden because core 0 barely uses its heap. The board
config now reserves 0x4800 more on core 0.

## 11. Bezel, measured properly

A boot-time benchmark repaints the whole ring the way one bezel click does,
with the lookup-table ring and the direct SPI path: **65,028 pixels in
34.9 ms** (~128 cycles per pixel). One click needs that in under ~10 ms to
fit a frame next to the second hand, so the lookup table alone is not enough.
Parked for now (the face as it is refreshes well when the bezel is still).

## 12. A second face on the same engine

The compositor became face-driven: every hand has its own pivot (the
chronograph's subdial hands turn about points 104 px from the centre), a
shape, a footprint for the span solver and a dirty-marking extent. The
sweeping hand (GMT seconds / chrono seconds) takes its dimensions from a
table. The per-row span solver just measures x relative to each hand's pivot.

- GMT face after the change: 59 fps, sweep stage ~0.5 ms slower (dimensions
  read from a table instead of compile-time constants).
- Chronograph face, chrono running: solid 60 fps, ~10.5 ms per frame. It is
  cheaper than the GMT: fixed bezel, slim hands, and the moving subdial hand is
  ~50 px long.
- Buttons: core 0 turns BOOT (a GPIO input event) and PWR (polled over I2C
  from the I/O expander, whose interrupt pin is not wired) into IPM messages.

## 13. The bezel click, by the numbers

A click (6 degree turn) repaints the whole ring: 65,028 pixels in boxes.

| Step | Click | Notes |
|---|---|---|
| bilinear rotation (original) | 20-90 ms | per pixel: 4 taps from a 466x466 image |
| polar LUT, radius-major strip in flash | 34.9 ms | |
| same, angle-major, one u32 per entry | 37.0 ms | layout made no difference |

Splitting that 37 ms by stage showed the lookup itself at 21.2 ms (~92 cycles
per pixel): each ring pixel read the strip from flash at a position that,
whichever way the strip is laid out, jumps around (along a row the angle and
the radius both change, depending where on the ring the row is). Flash reads
that miss the cache are the cost, so the strip had to move into internal RAM,
and for that it had to shrink:

- **8-bit palette:** the bezel has few colours; 256 entries cover them and
  their anti-aliased mixes. Strip 248 KB -> 62 KB.
- **Pre-blended:** the ring's soft edges are blended in the generator onto
  what is beneath them (grey rehaut inside, black outside), so every entry is
  opaque: no blending and no dial copy under the ring.
- **u16 index per ring pixel** (113 KB, stays in flash, read sequentially).
- **Sweep stage gated** by the hand's bounding box: ring boxes the hand
  cannot reach skip it (3.7 ms -> 0.4 ms in a click).
- **Off-panel corners** of ring boxes are zero-filled instead of copied.

Result: **14.0 ms** per click (ring 5.6, swap 3.1, bg 1.5, write mostly
overlapped). Core 1's RAM is now 93% used.

## 14. The watch becomes a USB media remote

Core 0 now runs the chip's USB OTG controller as its own composite device:
a CDC-ACM serial port (the console) and HID Consumer Control (volume, mute,
play/pause, next/previous, display brightness). Windows and macOS act on
those keys with no driver or app. Core 1 draws the remote: the bezel's polar
lookup reused for a knob ring that follows the finger in 0.25 degree steps
and sends one key per 15 degrees (a couple of turns from silent to full,
like an iPod wheel).

Things that bit:

- **One USB port, two jobs.** The OTG controller and the built-in
  USB-Serial-JTAG share the PHY: with our device running, the flashing port
  is gone. `west flash` writes core 0's image, resets, and the new firmware
  takes the port before core 1's image is written. Fix: a console command
  that reboots into the ROM's download mode, and `tools/flash.sh` writing
  both images in one esptool run.
- **macOS asks to identify every keyboard.** A keyboard report (for F14/F15
  brightness) opened the Keyboard Setup Assistant on each plug-in; brightness
  now goes as Consumer usages only.
- **Messages between the cores went missing.** The ESP32 IPM driver has one
  mailbox per direction and does not wait for the other core to read it: a
  second send overwrites the first. The "open the remote" message, sent a few
  microseconds before a touch update, was overwritten almost every time;
  BOOT presses (sent alone) always worked. Now one sender at a time, with
  200 us for the other core to copy each message out.
- **And the mailbox is small.** It takes at most 512 bytes a message; the
  2240-byte now-playing image was refused ("Not enough memory in IPM
  channel"). It goes in 480-byte chunks now.

## 15. A helper app for the other direction

HID only goes from the watch to the computer. For the watch to show the real
volume, mute and play state, and what is playing, a small Python helper
(`tools/agent/agent_mac.py`) sends a status line over the serial port every
0.4 s: volume and mute (AppleScript), built-in display brightness
(DisplayServices), and the play state, title and channel of YouTube tabs in
Chrome (JavaScript through Apple Events), or Spotify/Music.

The watch has no fonts, and core 1 has no RAM for them, so the helper draws
the title and artist with Pillow into a 224x40 image at 2 bits per pixel
(2240 bytes) and sends it; long lines scroll, the helper sending ~12 frames a
second. Core 0 decodes the base64 line (its USB serial RX FIFO raised from
1 KB to 4 KB for it) and forwards the image to core 1 in chunks.

Serial is fine for a development tool; for a product the usual choice is a
vendor-defined HID interface (raw HID): no COM port to find, no drivers, and
the helper finds the device by its USB id.

## 16. Seeing it: an event tracer on both cores

Per-second averages hide what a frame looks like, so the watch got a tracer
(`tools/trace/README.md`): 8-byte events (timestamp, id, two arguments)
stamped from the one system timer both cores can read, staged on core 1 in
60-event batches (its RAM is nearly full), stored on core 0 (16k events) and
dumped over the USB console as base64. `animate.py` turns a trace into a
page you can play and scrub: TE edges, each box being composed, the CPU
stalling for the DMA, the bezel repaint, the SPI/DMA transfer, core 0's
touches and keys, the messages between the cores, and a map of which bands
of the screen were redrawn (`docs/data/traces/*.html`).

Things the tracer itself got wrong first: a batch filling up exactly at the
frame boundary dropped the frame markers (now batches are flushed with room
to spare, after every box); the dump went through the log buffer and lost
lines (now written straight to the port, which blocks instead of dropping);
scripted setup commands sent 7 s after flashing were lost because core 1 was
still in its boot benchmark (now 16 s); and `k_cycle_get_32()` is per core,
so the first traces had core 0's events 6 s away from core 1's.

## 17. Before and after, measured the same way

Each optimisation has a build switch (`apps/watch/remote/Kconfig`) so the
version before it can be rebuilt and traced on the same scripted scenarios
(`tools/trace/ab_build.sh`, `ab_capture.sh`). GMT face, 1.8 s captures;
"stall" is CPU time spent waiting for the previous DMA transfer, "slow" is
frames that missed the next refresh.

| Build | idle: work / stall ms | bezel click: ring repaint ms, worst frame | hands crossing: fps, slow frames |
|---|---|---|---|
| **as shipped** | 13.4 / 0.9 | 16.3, 32 ms | 36, 40 of 64 |
| display writes through the Zephyr SPI driver | 15.9 / 0 (the wait is inside the driver) | 25.4, 43 ms | 29, 52 of 52 |
| no DMA overlap (send each box, then compose the next) | 14.0 / 2.0 | 20.2, 37 ms | 32, 48 of 57 |
| bezel strip read from flash instead of RAM | 13.7 / 0.9 | 18.9, 35 ms | 41, 32 of 73 |
| second-hand stage in every box (no gating) | 13.2 / 0.9 | 18.1, 34 ms | 48, 19 of 86 |
| free-running (no TE lock) | the loop spins ~10,000 empty frames a second and redraws whenever the angle ticks, anywhere in the panel's refresh | 16.6 | worst frame 32 ms |

Reading it honestly:

- The direct SPI path is the biggest single win still measurable: 2.5 ms a
  frame and 9 ms per bezel click, because every one of the ~35 windows a frame
  paid the driver's per-transaction overhead.
- DMA overlap saves about 1.1 ms of stall a frame and 4 ms per bezel click.
- RAM vs flash for the ring strip is worth 2.6 ms per click here; the 37 to
  14 ms jump in section 13 was mostly the palette compression and
  pre-blending, which this switch cannot undo (both builds use the compact
  strip).
- Second-hand gating does nothing measurable for ordinary frames (the span
  solver already skips rows the hand misses); it saves ~2 ms on a bezel
  click, when 70 ring boxes would otherwise each run the stage. The
  hands-crossing columns differ more by *when* in the crossing the 1.8 s
  window fell than by the switch.
- A bezel click still costs one missed frame (ring 16 ms + a normal frame).
- The hands stage swings between 3.8 and 10 ms within every minute as the
  second hand passes over the other hands (seen with the stats, confirmed by
  `gmt_hands_cross.html`): the remaining dip, and the next thing to fix.

## Concurrency and timing techniques used

- AMP: one core per job, separate images, no shared scheduler.
- IPM messages with a small payload in shared memory; the interrupt handler
  only copies, the main loop acts.
- Frame start locked to the panel's TE signal (polled GPIO).
- Dirty regions instead of full frames.
- DMA transfer of one band overlapping CPU composition of the next (double
  buffering, no threads).
- Time from two sources: RTC second edges from core 0, sub-second from core
  1's uptime.
- Measurement built in: per-stage cycle counters, a boot-time micro-benchmark,
  SPI underrun counter, on-device self-check of cached pixels.
- Core-to-core messages serialised, with time for the receiver to copy each
  one out (the IPM mailbox does not queue).
- A composite USB device: serial console plus HID, with a host helper for
  the return path (state and an image of the now-playing text).

## Where it stands

| | ms per frame |
|---|---|
| background copy | ~0.5-1 |
| bezel (no turn) | ~1.3 |
| GMT/hour/minute hands | 4-9 |
| second hand | ~4.5 |
| byte swap | ~0.6 |
| SPI writes (mostly overlapped) | ~0.5-0.7 |
| **typical total** | **~12 of 16.7** |

59 fps locked to the panel. Remaining dips: turning the bezel (ring repaint
20-66 ms) and the second hand crossing the other hands (hands stage up to
~9 ms).

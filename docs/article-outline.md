# lumo-watch: the article (working outline)

Part 1 of two. Audience: embedded developers and curious makers. Tone:
"here is what happened, with the numbers", not a tutorial. Every number comes
from `docs/optimization-log.md` and `docs/data/`. Device photos and UI renders
are placeholders marked [PHOTO] / [RENDER]; traces are the interactive pages
in `docs/data/traces/`.

---

## 0. Hook

A 466x466 round AMOLED, two CPU cores, one goal: a second hand that glides
like a mechanical watch, 60 times a second, without tearing. Then the watch
turned into a volume knob for the laptop. [PHOTO: watch on the desk next to
the laptop, remote screen lit] [RENDER: GMT face, animated second hand]

## 1. The hardware (system level)

- The board and what is on it (one paragraph, then the diagram).
  Diagram: `docs/diagrams/01-hardware.md`.
- The chip: two cores, 512 KB SRAM, 8 MB PSRAM, 16 MB flash; the panel's QSPI
  bus at 80 MHz, 4 lines = 40 MB/s; a full frame is 434 KB = ~11 ms of bus
  time, so a full redraw every frame is impossible at 60 Hz. This single
  number drives the whole design.
- The one USB port that two controllers share (sets up part 5).
- [PHOTO: the back of the board / the module]

## 2. What it does (the menus)

Walk through the screens with renders, no code yet.
Diagram: `docs/diagrams/06-faces-and-ui.md` (state diagram).
- GMT face: bezel turns by touch, clicks at minutes, date, gliding seconds.
  [RENDER: bezel mid-turn] 
- Chronographs, black and white: PWR start/stop, BOOT reset, subdials.
  [RENDER: both, chrono running]
- The remote: knob, mute/prev/next/play, brightness, level meter, title
  scrolling, visualiser. [RENDER: remote playing] [PHOTO: knob being turned,
  laptop volume overlay visible]
- The picker, the gestures (two fingers / one finger still / BOOT 3 s).
- Setting the time over USB; no battery, so the RTC forgets.

## 3. Inside: two cores, two programs

Diagram: `docs/diagrams/02-cores-and-memory.md`.
- Why AMP and not SMP (Zephyr has no SMP for the S3; and we wanted the
  display core undisturbed).
- Two Zephyr images, how core 0 starts core 1, the carved-out 128 KB.
- The memory map, and the overlap bug (18 KB) found from the linker maps.
- Messages: diagram `docs/diagrams/03-messages.md`. The mailbox that does not
  queue (lost gesture), the 512-byte limit (chunked text).
- Trace: `docs/data/traces/gmt_idle.html` with the msgs lane visible: time
  edges, touches, stats.

## 4. Drawing a frame

Diagram: `docs/diagrams/04-render-pipeline.md`.
- TE lock and why cadence matters more than average fps.
- Dirty bands: the staircase. Trace: `chrono_white_idle.html` (ten small
  bands) vs `gmt_idle.html` (the full second-hand staircase).
- The compositor stages, per row; vector hands with analytic anti-aliasing
  (a short aside with the disc-coverage trick: no sqrt).
- The direct SPI path and DMA overlap: the red stalls in the trace are the CPU
  waiting for the previous box; the blue lane is the panel being fed while
  the CPU composes the next one. Trace: `gmt_idle.html`, window 35 ms.
- The bezel as a polar lookup; diagram `docs/diagrams/07-asset-pipeline.md`.
  Trace: `gmt_bezel.html` (the yellow ring repaint, 14 ms).

## 5. The optimisation story, with before/after traces

Follow `docs/optimization-log.md` sections 1-13, condensed to one figure per
step. For each A/B pair embed two trace pages side by side (same scenario,
one switch flipped):

| Step | Before | After | Trace pair |
|---|---|---|---|
| Display writes | Zephyr SPI driver, 3.4 ms/frame | direct registers, 0.5 ms | `ab_zephyr-spi_*` vs `gmt_*` |
| DMA overlap | compose, wait, send | compose next while sending | `ab_no-overlap_*` vs `gmt_*` |
| Bezel strip | from flash, 37 ms a click | in RAM, 14 ms | `ab_ring-flash_gmt_bezel` vs `gmt_bezel` |
| Second-hand gating | every box | only boxes it can reach | `ab_no-gate_*` vs `gmt_*` |
| Frame pacing | free-running | TE-locked | `ab_free-run_*` vs `gmt_*` |

Plus the history that cannot be rebuilt as a switch (LVGL at 12-26 fps, the
image hands, the hand cache that corrupted the screen): tell those from the
log's numbers and the captures in `docs/data/0*-*.txt`.

The remaining dip: `gmt_hands_cross.html` - the hands stage reaching 10 ms as
the second hand passes the other hands. Honest ending for part 1: 59-60 fps
nearly always, 30 fps for a few frames a minute, and the fix we know (a
tighter redraw area for the second hand).

## 6. The USB remote and the helper

Diagram: `docs/diagrams/05-usb-and-helper.md`.
- HID Consumer Control: why no app is needed for volume/mute/play.
- The PHY-sharing surprise and the flash script.
- The keyboard-prompt mistake on macOS.
- The helper: volume, brightness, YouTube state via Apple Events, text drawn
  on the laptop because the watch has no fonts. Trace: a `remote_*.html` with
  the core 0 lane showing host state and HID keys. [PHOTO: title scrolling
  on the watch while a video plays]
- Serial vs raw HID (what a product would do).

## 7. Tooling that made it possible

- Stage counters and the boot micro-benchmark (the 315 -> 107 cycles story).
- The tracer: 8-byte events, shared timer, batches over the mailbox, dump
  over USB, the HTML viewer. How to reuse it in another project
  (`tools/trace/README`).
- Python generators for the art; the Mac render build for the screenshots in
  this article [when done].

## 8. Where it stands, and part 2

- Numbers table from the log's "Where it stands".
- RAM at 97%: the next feature has to make room first.
- Part 2: BLE companion app replacing the USB helper (state, titles, time
  over the air), Wi-Fi for now-playing from the network, the ambient
  visualiser mode, remembering the face. [RENDER: concept of the ambient mode]

---

## Assets checklist

- [ ] Diagrams rendered from `docs/diagrams/01..07` (Claude Design)
- [ ] UI renders: Mac build of the compositor writing PNG/animated frames
- [ ] Device photos: desk shot, knob in hand, back of board, cable to laptop
- [x] Traces: scenarios + A/B pairs in `docs/data/traces/` (HTML + GIF)
- [x] Raw per-second captures: `docs/data/*.txt`
- [x] The log with the numbers: `docs/optimization-log.md`
- [ ] GIF exports of the key traces for platforms that cannot embed HTML

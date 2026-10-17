# Event tracing: firmware side and host tools

A small, reusable way to see what a device is doing on a timeline, with a
playable HTML page at the end. Used here for the watch; the same pieces work
for any Zephyr (or bare-metal) project.

## How it works

1. **Events are 8 bytes**: `u32 timestamp, u8 event id, u8 a, u16 b`.
   Small enough to record thousands per second; two arguments cover most
   needs (which band, how wide, which message, how long).
2. **One clock for everything.** Use a hardware counter every core can read.
   On the ESP32-S3 that is the system timer (16 MHz): `trace_now()` in
   `apps/watch/common/trace.h`. Per-core cycle counters are *not* aligned
   between cores.
3. **Record where the RAM is.** Core 1 has ~3 KB spare, so it stages 60
   events at a time and sends each batch to core 0 over the mailbox
   (`remote/src/trace.c`). Core 0 stores 12,288 events (96 KB) plus its own
   (`src/trace_store.c`). A single-core project just keeps one ring buffer.
4. **Dump on request** over the console: `T1` starts (clears), `T0` stops and
   prints base64 lines of 60 events, straight to the UART (not through the
   log subsystem, which drops lines when flooded).
5. **Host**: `capture.py` records (and can script actions by console
   command at given times), `tracelib.py` parses and groups events into
   frames, `animate.py` writes the interactive page (and a GIF),
   `index.py` rebuilds every page and the README table.

Cost when recording: one timer read and a store per event (~0.1 us), plus
one mailbox message per 60 events (~0.25 ms). When off: a load and a branch.

## Reusing it in another project

Firmware:
- Copy `common/trace.h` (edit the `TR_*` enum for your events and pick a
  clock in `trace_now()`), `remote/src/trace.{c,h}` (the staging buffer; on
  one core you can store directly) and `src/trace_store.c` (store + dump).
- Add `trace_ev(TR_X, a, b)` at the points you care about: frame start/end,
  each unit of work, waits, interrupts, messages. Keep events per frame
  under ~100 or raise the store.
- Put the `T1`/`T0` commands in your console handler.

Host:
- `tracelib.py`: update `EVENTS` (id -> name) and, if your "frame" concept
  differs, `Trace.frames()` which groups events into frames with boxes,
  stalls and spans. `summary()` prints the headline numbers.
- `animate.py`: `build_model()` maps frames/events to the viewer's lanes
  (TE, CPU, DMA, core 0, messages). Rename lanes in `HTML`'s `L` table and
  the legend. The screen map assumes a 466 px square panel (`PANEL`).

## Commands

```
uv run tools/trace/capture.py gmt_idle 3 --setup F0
uv run tools/trace/capture.py gmt_bezel 3 --setup F0 --at 0.5:Z1 --at 0.8:Z1
uv run tools/trace/animate.py docs/data/traces/gmt_bezel.trace          # -> .html
uv run tools/trace/animate.py docs/data/traces/gmt_bezel.trace --gif    # -> .gif too
uv run tools/trace/index.py                                             # all pages + README
sh tools/trace/ab_build.sh                                              # the A/B builds
```

Watch console commands used by scripts: `F<face>`, `Z<clicks>` (bezel),
`W<deg>` (knob), `T YYYY-MM-DD HH:MM:SS` (clock), `K<1-8>` (send a media
key), `B` (reboot to download mode), `T1`/`T0`.

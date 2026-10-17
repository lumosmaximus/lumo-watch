# lumo-watch

**One-line summary:** a desk widget built from a round AMOLED dev board that became a smooth 60 fps watch, then a volume knob for the laptop.

![Device photo: the watch on the desk next to the laptop](docs/photos/hero.jpg)
![The GMT face, second hand gliding](docs/renders/gmt.gif)

## The board

Waveshare **ESP32-S3-Touch-AMOLED-1.75**: a 1.75" round AMOLED, 466x466, CO5300 controller on a 4-line QSPI bus at 80 MHz; CST9217 touch; an ESP32-S3 with two 240 MHz cores, 512 KB of SRAM and 8 MB of PSRAM; 16 MB flash; AXP2101 power, PCF85063 RTC, QMI8658 IMU, two codecs, a TF-card slot, one USB-C port.

The number that shaped everything: a full frame is 434 KB, about **11 ms of bus time**, so redrawing the whole screen 60 times a second is not possible. Everything below follows from that.

![Device photo: the board, front and back](docs/photos/board.jpg)
![Hardware diagram](docs/diagrams/01-hardware.png)

Board support, drivers and the apps: [`boards/`](boards/), [`drivers/`](drivers/), [`apps/`](apps/) · how to build: [`docs/BUILD.md`](docs/BUILD.md)

## A simple watch first

Two cores, two jobs. Core 0 runs the sensors, buttons, time and USB. Core 1 runs only the display. They talk through the chip's mailbox.

![Cores and memory](docs/diagrams/02-cores-and-memory.png)

The first face used a GUI library: 12 to 26 fps, and a second hand that hopped between pixels. Eight steps later it was 59, steady:

![How smooth it got, step by step](docs/figures/fps-journey.webp)

## Making it glide

Only redraw what moved. The compositor keeps a list of dirty 8-row bands; a quiet chronograph is ten small bands a frame, the GMT's second hand is a staircase across the dial.

| ![chronograph: ten small bands](docs/data/clips/bands_small.gif) | ![GMT: the second hand's staircase](docs/data/clips/bands_gmt.gif) |
|---|---|

Start every frame on the panel's refresh pulse (TE). Free-running, the loop spins thousands of empty frames and redraws at random points in the refresh.

| ![free-running](docs/data/clips/te_before.gif) | ![locked to TE](docs/data/clips/te_after.gif) |
|---|---|

Drive the SPI from the registers instead of the driver (3.4 ms → 0.5 ms a frame), and let the DMA send one band while the CPU draws the next. Red is the CPU waiting for the DMA.

| ![Zephyr SPI driver](docs/data/clips/spi_before.gif) | ![direct SPI + DMA](docs/data/clips/spi_after.gif) |
|---|---|
| ![no overlap: 2 ms of waiting a frame](docs/data/clips/overlap_before.gif) | ![overlap: 0.9 ms](docs/data/clips/overlap_after.gif) |

Hands as vector shapes with analytic anti-aliasing; the bezel as a polar lookup table pre-blended and palette-compressed into internal RAM, so a turn is an index offset.

![Redrawing the turning ring: 90 ms to 14](docs/figures/bezel-click.webp)

| ![bezel strip read from flash](docs/data/clips/bezel_before.gif) | ![bezel strip in RAM](docs/data/clips/bezel_after.gif) |
|---|---|

![One frame on core 1](docs/diagrams/04-render-pipeline.png)

Result: **59-60 fps** locked to the panel, 9-13 ms of work in a 16.7 ms frame, and every "before" above measured by rebuilding without that one change:

![Each frame gets 16.7 ms; what happens if we undo each improvement](docs/figures/frame-budget.webp)

The dip that remains: the second hand passing over the other hands costs up to 10 ms of hand shading, a few frames a minute.

![the hands crossing](docs/data/clips/hands_cross.gif)

Every step with its numbers: [`docs/optimization-log.md`](docs/optimization-log.md) · all 21 traces, playable: [`docs/data/traces/`](docs/data/traces/README.md)

## A nicer human interface

| ![bezel turn](docs/renders/gmt_bezel_turn.gif) | ![chronograph](docs/renders/chrono_white.gif) | ![picker](docs/renders/picker.png) |
|---|---|---|

Touch turns the bezel and it clicks every minute. Two chronographs, black and white: PWR starts and stops, BOOT resets. Hold BOOT for the face picker. Hold two fingers to open the remote.

![Device photo: turning the bezel](docs/photos/bezel.jpg)

## Volume, play, skip

![Device photo: knob in hand, laptop volume overlay visible](docs/photos/knob.jpg)
![the remote](docs/renders/remote_playing.gif)

The watch is a USB HID media device, so Windows and macOS need no driver: the ring is a knob (one key per 15 degrees, a couple of turns from silent to full), with mute, previous, next, play/pause and brightness. A small helper on the laptop sends back the real volume, mute and play state and what is playing, which the watch shows as a level meter and scrolling text. [`apps/watch/src/usb_hid.c`](apps/watch/src/usb_hid.c) · [`tools/agent/agent_mac.py`](tools/agent/agent_mac.py)

![USB and the helper](docs/diagrams/05-usb-and-helper.png)

Things that bit on the way: the flashing port and the USB device share one PHY; a keyboard interface made macOS ask to identify the watch on every plug-in; the mailbox does not queue, so a gesture message was overwritten by the touch update sent microseconds later; and it takes 512 bytes at most, so the now-playing image is chunked.

## Seeing it: traces, renders, diagrams

Both cores carry an event tracer (8-byte events on one shared hardware clock), dumped over USB and turned into pages you can play and scrub. Every optimisation has a build switch, so each "before" above is a real build traced on the same scripted scenario.

[`tools/trace/`](tools/trace/README.md) · [`tools/render/`](tools/render/) (the display code compiled for the computer: every render above is pixel-exact) · [`docs/diagrams/`](docs/diagrams/) (the diagram prompts)

## Next

A BLE companion app instead of the USB helper, Wi-Fi for now-playing from the network, an ambient visualiser when idle, remembering the face. [`docs/ideas.md`](docs/ideas.md)

## Links

- Source: this repository (Apache-2.0) · [building and flashing](docs/BUILD.md)
- Numbers: [`docs/optimization-log.md`](docs/optimization-log.md) · raw captures: [`docs/data/`](docs/data/)

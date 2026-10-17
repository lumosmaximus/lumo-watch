# lumo-watch

![lumo-watch](docs/diagrams/00-hero.png)
![Device photo: the watch on the desk next to the laptop](docs/photos/hero.jpg)
![The GMT face, second hand gliding](docs/renders/gmt.gif)

This started as a desk widget. A round AMOLED dev board had been sitting in a drawer long enough to feel guilty about, so I put a clock on it. The clock turned into a watch, the watch turned into an argument with a 16.7 ms deadline, and the argument turned into a volume knob. Here is the whole slide, with the receipts.

## The board

Waveshare **ESP32-S3-Touch-AMOLED-1.75**. A 1.75" round AMOLED, 466x466, driven by a CO5300 over a four-wire QSPI bus at 80 MHz. Capacitive touch. An ESP32-S3 with two cores at 240 MHz, 512 KB of SRAM and 8 MB of PSRAM. Power management, an RTC with no battery to back it, an IMU, two audio codecs that mind their own business, a TF-card slot, and one USB-C port that turns out to have opinions.

One number ran the whole project: a full frame is 434 KB, about **11 ms of bus time**. Redrawing the screen 60 times a second is therefore not a plan, it is a wish. Everything below is what you do instead.

![Device photo: the board, front and back](docs/photos/board.jpg)
![Hardware diagram](docs/diagrams/01-hardware.png)

Zephyr RTOS rather than the vendor SDK, so that anyone can clone this and build it with the standard tools; that meant writing the board definition and four drivers first. The CO5300 wants its commands bit-expanded onto one data line inside quad transactions, which no existing driver does. The CST9217 touch chip only interrupts on first contact and about once a second after that, so the driver polls until the finger lifts; it also reports how many fingers, which the gestures use. Two codec drivers came along for a later life as a microphone.

Before any watch, two small apps proved the board: `amp_hello` boots both cores and pings between them (12 µs round trip), and `hw_test` checks every peripheral at boot, draws a boundary pattern to catch a cropped edge, and hammers the display with alternating bit patterns to catch corruption. The TF card slot taught the first lesson: one card never finished initialising, hours were spent on the driver, and the card was simply bad. Quad-I/O flash doubled the read bandwidth the display core lives on.

![Device photo: hw_test's boundary pattern on the panel](docs/photos/hwtest.jpg)

Board support, drivers, apps: [`boards/`](boards/) · [`drivers/`](drivers/) · [`apps/`](apps/) · how to build it: [`docs/BUILD.md`](docs/BUILD.md)

## A simple watch first

Two cores, so two jobs. Core 0 gets the sensors, the buttons, the time and the USB port. Core 1 gets the display and nothing else, like a chef who is not allowed to answer the phone. They pass notes through the chip's mailbox.

![Cores and memory](docs/diagrams/02-cores-and-memory.png)
![Messages between the cores](docs/diagrams/03-messages.png)

The first face used a GUI library. It ran at 12 to 26 fps depending on where the hands happened to point, and the second hand hopped from pixel to pixel like it was crossing a hot pavement. Eight steps later it glides at 59:

![How smooth it got, step by step](docs/figures/fps-journey.webp)

## Making it glide

**Redraw only what moved.** The compositor keeps a list of dirty 8-row bands. A quiet chronograph is ten small bands a frame; the GMT's second hand is a staircase across the dial.

| ![chronograph: ten small bands](docs/data/clips/bands_small.gif) | ![GMT: the second hand's staircase](docs/data/clips/bands_gmt.gif) |
|---|---|

**Start every frame when the panel does.** The display has a tearing-effect pulse that says "refreshing now". Ignore it and the loop spins thousands of empty frames a second and repaints at random moments mid-refresh. Listen to it and every frame lands in the same place.

| ![free-running](docs/data/clips/te_before.gif) | ![locked to TE](docs/data/clips/te_after.gif) |
|---|---|

**Stop asking the driver politely.** Every band went through three SPI driver transactions, each setting up the DMA from scratch: about 100 µs of ceremony, 35 times a frame. Driving the SPI peripheral's own command and address phases from the registers, with two band buffers so the DMA ships one while the CPU draws the next, took display writes from 3.4 ms to 0.5 ms. The red in the clips is the CPU standing around waiting for the DMA.

| ![Zephyr SPI driver](docs/data/clips/spi_before.gif) | ![direct SPI + DMA](docs/data/clips/spi_after.gif) |
|---|---|
| ![no overlap: 2 ms of waiting a frame](docs/data/clips/overlap_before.gif) | ![overlap: 0.9 ms](docs/data/clips/overlap_after.gif) |

**Draw hands as maths, not pictures.** Bars, triangles and discs with analytic anti-aliasing; no square roots were harmed. And the bezel, which has to turn, became a polar lookup table: pre-blended so every entry is opaque, squeezed into 256 colours so it fits in the chip's fast memory, and rotated by adding a number to an index.

![Redrawing the turning ring: 90 ms to 14](docs/figures/bezel-click.webp)

| ![bezel strip read from flash](docs/data/clips/bezel_before.gif) | ![bezel strip in RAM](docs/data/clips/bezel_after.gif) |
|---|---|

![One frame on core 1](docs/diagrams/04-render-pipeline.png)

**Where it landed:** 59-60 fps, locked to the panel, using 9-13 ms of each 16.7 ms frame. Every "before" above is a real build with that one change switched back off, traced on the same scripted scenario, because "it felt faster" is not a measurement.

![Each frame gets 16.7 ms; what happens if we undo each improvement](docs/figures/frame-budget.webp)

The one dip left: when the second hand sweeps over the other hands, shading them costs up to 10 ms, and a few frames a minute arrive late. The fix is known. It is also next.

![the hands crossing](docs/data/clips/hands_cross.gif)

Every step with its numbers: [`docs/optimization-log.md`](docs/optimization-log.md) · all 21 traces, playable: [`docs/data/traces/`](docs/data/traces/README.md)

## Dials drawn by code

No image editor was involved. Python scripts draw the dials, the bezel and the knob with Pillow at three times the resolution and shrink them down: `watch.py` for the GMT, `chrono.py` for both chronographs (the tachymeter laid out letter by letter along the curve), `knob.py` for the remote's ring. They emit C arrays, so changing a colour is a two-line edit and a rebuild. The hands are not images at all; they are a few shape functions in C.

![From Python drawings to pixels on the panel](docs/diagrams/07-asset-pipeline.png)

[`tools/watchface/`](tools/watchface/)

## A nicer human interface

![Faces and how you move between them](docs/diagrams/06-faces-and-ui.png)

| ![bezel turn](docs/renders/gmt_bezel_turn.gif) | ![chronograph](docs/renders/chrono_white.gif) | ![picker](docs/renders/picker.png) |
|---|---|---|

The bezel turns under your finger and clicks every minute, which is deeply satisfying for no good reason. Two chronographs, black and white: PWR starts and stops, BOOT resets. Hold BOOT for the face picker. Hold two fingers on the glass and the watch stops being a watch.

![Device photo: turning the bezel](docs/photos/bezel.jpg)

## Volume, play, skip

![Device photo: knob in hand, laptop volume overlay visible](docs/photos/knob.jpg)
![the remote](docs/renders/remote_playing.gif)

Plug it into a laptop and it is a USB media device, the same kind as the volume keys on a keyboard, so Windows and macOS need no driver and ask no questions. The ring is a knob: one key per 15 degrees, a couple of turns from silent to full, like a wheel from a music player you may remember. Mute (the button turns red and says so), previous, next, play/pause with a rainbow visualiser that bounces while something plays and fades when it stops, and a sun button that turns the knob amber and makes it adjust screen brightness until you press it again or leave it alone for 30 s. A small helper on the laptop sends back the real volume, mute and play state and what is playing, which the watch shows as a level meter and a scrolling title. [`apps/watch/src/usb_hid.c`](apps/watch/src/usb_hid.c) · [`tools/agent/agent_mac.py`](tools/agent/agent_mac.py)

![USB and the helper](docs/diagrams/05-usb-and-helper.png)

Two bugs that deserve their own paragraph. The two cores' images computed the end of RAM from two different boot formats, and their memory regions overlapped by 18 KB; nothing noticed until core 0's heap grew, and the linker maps told the story. And a cache of finished pixels under the hands, which cut the hands stage from 5.5 ms to 0.5 ms, corrupted the screen while an on-device check proved every cached pixel identical to a full recompute. It was removed. It is still not explained. [`docs/optimization-log.md`](docs/optimization-log.md#10-the-hand-cache-that-corrupted-the-screen)

Things the USB port had opinions about: it shares one PHY between the flashing port and our device, so once the watch was a USB device you could no longer flash it (a reboot-to-download command fixed that). A keyboard interface for the brightness keys made macOS open "identify your keyboard" on every plug-in. The inter-core mailbox does not queue, so the "open the remote" gesture kept being overwritten by the touch update sent microseconds later. And it holds 512 bytes, so the now-playing text travels in pieces.

## The cable does more than charge

The RTC has no backup battery, so every unplug is amnesia: the watch wakes up believing it is whenever the firmware was compiled. Rather than add a coin cell, the computer sets the clock: the same USB port carries a serial console, and `tools/settime.py` waits for the start of a second, then sends one line, `T 2026-10-17 10:30:00`. Core 0 writes it to the RTC and tells core 1 right on the next second boundary; core 1 adds milliseconds from its own uptime, which is how a 1-second RTC ends up driving a hand that moves every 16.7 ms. (On first boot core 1 asks for the time every second until it gets it, because the first attempt used to arrive before it was listening.)

The helper, `tools/agent/agent_mac.py`, is the rest of the return path. Four times a second it reads the Mac's volume and mute (AppleScript), the built-in display's brightness (a private DisplayServices call), and what is playing: YouTube tabs in Chrome through "JavaScript from Apple Events" (`video.paused`, the title, the channel) or Spotify/Music. It sends one line, `S v=63 m=0 b=85 p=1`. For the title it goes further, since the watch has no fonts and three kilobytes of RAM to spare: it draws the text with Pillow as a 224x40 image at 2 bits per pixel and streams it, twelve frames a second when the line scrolls. The watch just copies pixels.

Serial lines are fine for a desk; a product would use a raw HID report for the same traffic and never show a COM port. [`tools/settime.py`](tools/settime.py) · [`tools/agent/agent_mac.py`](tools/agent/agent_mac.py) · [`apps/watch/src/main.c`](apps/watch/src/main.c) (the console parser)

![Video: plugging in, setting the time, the title appearing](docs/photos/helper.jpg)

## Seeing it: traces, renders, diagrams

Averages lie. Both cores now carry an event tracer (8-byte events on one shared hardware clock), dumped over USB and turned into pages you can play and scrub; the viewer at the top of this page is one. The display code also compiles for the computer, which is where every render on this page came from, pixel for pixel.

[`tools/trace/`](tools/trace/README.md) · [`tools/render/`](tools/render/) · [`docs/diagrams/`](docs/diagrams/) (the diagram prompts) · [`docs/figures/`](docs/figures/)

## Next

A BLE companion app so the helper can retire. Wi-Fi for now-playing from the network. An ambient visualiser for when nobody is touching it. Remembering which face you chose, which it currently forgets with great consistency. [`docs/ideas.md`](docs/ideas.md)

## Links

- Source: this repository (Apache-2.0) · [building and flashing](docs/BUILD.md)
- Numbers: [`docs/optimization-log.md`](docs/optimization-log.md) · raw captures: [`docs/data/`](docs/data/)

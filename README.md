# lumo-watch

**One-line summary:** a dual-core smartwatch on Zephyr whose second hand glides at the panel's 60 Hz, with a touch-turned bezel, three faces, and a mode that turns it into a USB volume knob and media remote for a laptop.

![The GMT face, second hand gliding](docs/renders/gmt.gif)

## What problem it solves

Cheap round AMOLED boards are everywhere now, and the demos that ship with them all do the same thing: a GUI library redraws the screen at 12 to 25 fps and a second hand that hops from pixel to pixel. I wanted the opposite: a hand that moves 0.1 degree every frame, 60 times a second, with no tearing, on a chip with 128 KB of RAM to spare for the display. Then, since the thing sat on my desk next to the laptop, I wanted the bezel to be a volume knob.

The hardware is the Waveshare ESP32-S3-Touch-AMOLED-1.75: a 466x466 round AMOLED (CO5300 controller over QSPI), capacitive touch, a PMIC, codecs, an IMU, an RTC, and the ESP32-S3's two cores. Everything here runs on Zephyr RTOS, in C, with Python tooling on the computer. No brand marks: the faces are "in the style of" a GMT dive watch and a moon-watch chronograph.

## System architecture

The two cores run as **asymmetric multiprocessing**: two separate Zephyr images, built together, each with its own kernel.

- **Core 0** owns the I2C bus (touch, RTC, power, expander, IMU), the buttons, the time, and the USB port, which it turns into a composite device: a serial console plus a HID media-key interface.
- **Core 1** does one thing: the display. No GUI library. A small compositor redraws only the 8-row bands that changed, draws the hands as anti-aliased vector shapes, turns the bezel through a precomputed polar lookup, and drives the SPI peripheral and DMA from the registers so the transfer of one band overlaps the drawing of the next. Frames start on the panel's tearing-effect pulse.
- Between them: the chip's IPM mailbox (512 bytes a message, one slot per direction) carrying the time, touches, buttons, media keys, the computer's state, and the now-playing text.

![Architecture](docs/diagrams/02-cores-and-memory.png)
![One frame on core 1](docs/diagrams/04-render-pipeline.png)

The diagram prompts (and every number in them) are in [`docs/diagrams/`](docs/diagrams/).

## What it does

| | |
|---|---|
| ![GMT](docs/renders/gmt_bezel_turn.gif) | **GMT face.** Two-tone 24-hour bezel that turns by touch and clicks every minute, date, gliding seconds, a 24-hour hand. |
| ![Chronograph](docs/renders/chrono_white.gif) | **Chronographs**, black or white. Fixed tachymeter, running seconds at 9, 30-minute and 12-hour counters. PWR starts and stops, BOOT resets. |
| ![Remote](docs/renders/remote_playing.gif) | **The remote.** The ring is a knob (one key per 15 degrees, a couple of turns from silent to full), mute, previous, next, play/pause, brightness. With a small helper app on the computer it shows the real volume, mute and play state, and what is playing, scrolling when long. |
| ![Picker](docs/renders/picker.png) | **The picker**: hold BOOT 3 s. Two fingers held (or one finger still in the middle) opens and closes the remote. |

Those pictures are not photos: the display code is compiled for the computer as well (`tools/render/`), so every render is what the panel shows, pixel for pixel.

## Hardware used

- Waveshare ESP32-S3-Touch-AMOLED-1.75 (-B, without GPS): ESP32-S3R8 (two Xtensa LX7 cores at 240 MHz, 8 MB octal PSRAM), 16 MB flash
- 1.75" round AMOLED, 466x466, CO5300 over QSPI at 80 MHz; CST9217 touch; AXP2101 PMIC; PCF85063 RTC; QMI8658 IMU; TCA9554 expander; ES8311/ES7210 codecs (unused so far); TF card
- A USB-C cable to the computer, and that is all

## Firmware / software used

- Zephyr RTOS (C), sysbuild for the two images, out-of-tree board definition and drivers written for this board (CO5300 display, CST9217 touch, ES7210/ES8311)
- The Espressif HAL for the SPI, GDMA and system-timer registers
- Zephyr's USB device stack: CDC-ACM + HID Consumer Control
- Python with Pillow for the dial, bezel and knob generators; pyserial, AppleScript and DisplayServices for the macOS helper (`tools/agent/`); matplotlib for trace clips

## Challenges and solutions

**A full frame does not fit.** 466x466x2 bytes is 434 KB, about 11 ms of bus time per frame: a full redraw every 16.7 ms would leave 5 ms for everything else, and there is no RAM for a frame buffer on the display core anyway. So the compositor keeps a list of dirty 8-row bands, as wide as needed, and redraws only those. A quiet chronograph is ten small bands a frame, under 2 ms. ([trace](docs/data/traces/chrono_white_idle.html))

**Cadence beats average frame rate.** A version that averaged 59 fps looked worse than a steady 30, because frames just over budget missed a refresh now and then and the eye sees the change of rhythm. Every later step attacked the worst frame, not the mean, and measured it on the device.

**The driver was the bottleneck.** Each band went through three SPI driver transactions with the DMA reconfigured each time: ~100 us of overhead, 35 times a frame. Driving the SPI peripheral's own command and address phases from the registers, with two band buffers so the DMA sends one while the CPU composes the next, took display writes from 3.4 ms to 0.5 ms per frame. ([before](docs/data/traces/ab_zephyr-spi_gmt_idle.html) / [after](docs/data/traces/gmt_idle.html))

**Rotating the bezel.** Bilinear rotation of a 466x466 image cost 20 to 90 ms a turn. The ring is now an unwrapped polar strip (1440 angles x 43 radii), pre-blended onto what lies beneath so every entry is opaque, quantised to 256 colours (62 KB, small enough for internal RAM), plus a 16-bit index per ring pixel. Turning the bezel is an index offset; a click repaints the whole ring in 14 to 16 ms. The same code draws the remote's knob from a different strip.

**Two cores disagreeing about memory.** Core 0's image and core 1's image computed the end of RAM from two different boot formats, and the regions overlapped by 18 KB. Found in the linker maps after a corruption hunt; fixed in the board config.

**Lost messages.** The ESP32 mailbox driver does not queue: a second send overwrites an unread message. The "open the remote" gesture was overwritten by the touch update sent microseconds later, almost every time, while button presses (sent alone) always worked. Sends are now serialised with time for the other core to copy each one out. Then the now-playing image (2240 bytes) hit the mailbox's 512-byte limit and had to be chunked.

**One USB port, two controllers.** The chip's flashing port and the OTG controller share the USB pins. Once the watch is its own USB device, the flashing port is gone and the second of the two images never gets written. A console command reboots into the ROM's download mode and a script writes both images in one pass. A keyboard interface for brightness keys also made macOS open its keyboard-identification dialog on every plug-in; brightness goes as a Consumer Control key now.

**The watch has no fonts.** The display core has 3 KB of RAM to spare. The helper on the laptop draws the title and artist as a 224x40 image at 2 bits per pixel and streams it over the serial port, 12 frames a second when the line scrolls.

**Seeing a frame.** Per-second averages hid all of this, so both cores got an event tracer: 8-byte events stamped from the one hardware timer both cores can read, batched over the mailbox, dumped over USB, and turned into a page you can play and scrub. Every optimisation also has a build switch so the version before it can be traced on the same scripted scenario. The full table is in [the log](docs/optimization-log.md#17-before-and-after-measured-the-same-way).

## Final results

| | |
|---|---|
| frame rate | 59-60 fps, locked to the panel (LVGL first attempt: 12-26 fps) |
| typical frame work, GMT face | 9-13 ms of 16.7 |
| compositor cost | 315 -> 107 CPU cycles per pixel |
| display writes | 3.4 -> 0.5 ms per frame |
| bezel click | 37 -> 14 ms (one missed frame) |
| chronograph faces | 60 fps at ~11 ms per frame, chronograph running |
| remaining dip | the hands stage swings 3.8-10 ms within each minute as the second hand passes over the other hands: a few frames at 30 fps per minute. Next fix known (a tighter redraw area for the second hand). |
| RAM on the display core | 97% used |

The whole journey, step by step with the raw captures, is in [`docs/optimization-log.md`](docs/optimization-log.md) and [`docs/data/`](docs/data/). The interactive traces are in [`docs/data/traces/`](docs/data/traces/README.md).

**Part 2** will replace the USB helper with a BLE companion app (state, titles and time over the air), add Wi-Fi for now-playing information from the network, an ambient visualiser mode, and remembering the face across resets. Ideas live in [`docs/ideas.md`](docs/ideas.md).

## Links

- Source: this repository ([building and flashing](docs/BUILD.md))
- Traces, renders and diagrams: [`docs/`](docs/)
- Licence: Apache-2.0

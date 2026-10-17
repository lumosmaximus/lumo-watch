# Building and flashing lumo-watch

The write-up is in [README.md](../README.md); this is the how-to.

## The board

Zephyr board support, drivers and apps for the
[Waveshare ESP32-S3-Touch-AMOLED-1.75](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75)
(466x466 round AMOLED, CO5300 + CST9217, AXP2101, ES8311/ES7210, QMI8658,
PCF85063, TCA9554, TF card).

The main app is a watch with a gliding second hand at the panel's 60 Hz,
a touch-turned bezel, two chronograph faces, and a USB media remote for a
computer. How it got smooth, step by step with measurements, is in
[docs/optimization-log.md](docs/optimization-log.md).

Both cores are used, as AMP (two Zephyr images, built together with sysbuild):

- **core 0 (PROCPU)**: the shared I2C bus (power, RTC, touch, IMU, codecs),
  buttons, USB (console + media keys), and the time
- **core 1 (APPCPU)**: the display only, with its own small compositor (no
  LVGL): dirty bands, vector hands, a polar lookup for the bezel, SPI driven
  from the registers with DMA overlapping the drawing, frames locked to the
  panel's tearing-effect pin

The cores talk through the IPM mailbox (see `apps/watch/common/watch_msg.h`).

## Build

This repo is a Zephyr module; use it from an existing west workspace with the
Espressif HAL:

```
west build --sysbuild -d build-watch -b esp32s3_touch_amoled_1_75/esp32s3/procpu \
  path/to/amoled_175/apps/watch -- -DBOARD_ROOT=path/to/amoled_175
```

`--sysbuild` builds both images; the core-1 image goes into the appcpu
partition (0xbc0000).

## Flash

For `apps/watch` use the script, not `west flash`:

```
sh path/to/amoled_175/tools/flash.sh build-watch
```

The watch turns the USB-C port into its own USB device, which switches off the
chip's USB-Serial-JTAG (the usual flashing port). The script asks the running
watch to reboot into the ROM's download mode and writes both images in one
esptool run. (`west flash` resets between the two images, and the second one
then fails.) If the watch is not running, hold BOOT while plugging it in.

`apps/amp_hello` and `apps/hw_test` keep USB-Serial-JTAG: `west flash` works.

## Apps

- `apps/watch`: the watch.
  - Faces: GMT (rotating bezel, 1-minute clicks, date), and a chronograph in
    black or white (fixed tachymeter, subdials; PWR starts/stops, BOOT resets).
    Hold BOOT 3 s for the face picker.
  - Media remote (hold two fingers, or one finger still in the middle, or pick
    it in the picker): a knob for volume (or brightness) that takes a couple
    of turns from silent to full, mute, previous/next, play/pause. The keys
    are standard USB HID media keys: Windows and macOS need no app.
  - The time comes from the RTC; with no battery fitted it is lost when
    unplugged, so set it over USB with `tools/settime.py`.
- `apps/hw_test`: checks every peripheral at boot, then prints a live line:
  mic levels (ES7210), IMU, BOOT/PWR buttons, touch (CST9217), RTC, TF card
  (SDMMC), AXP2101 power, core 1 alive. Speaker output is not tested.
- `apps/amp_hello`: dual-core bring-up. Core 0 scans I2C, dumps the AXP2101
  rails and pings core 1, which answers with its uptime and core ID.

## Tools

- `tools/flash.sh`: flash both watch images (see above).
- `tools/settime.py`: set the watch's clock from the computer
  (`uv run --with pyserial tools/settime.py`).
- `tools/agent/agent_mac.py`: optional helper on a Mac so the remote follows
  the computer: volume/brightness level, mute and play state, and the title
  and artist of what is playing (YouTube in Chrome or Edge, Spotify, Music),
  scrolling when long (`uv run tools/agent/agent_mac.py`). For YouTube, turn
  on View > Developer > Allow JavaScript from Apple Events in the browser.
- `tools/watchface/`: generators for the dial, bezel and knob images
  (`watch.py`, `chrono.py`, `knob.py`; Pillow, run with `uv`).
- `tools/capture/cap.py`: record the watch's serial output.

## Drivers in this repo

- `chipone,co5300-qspi`: CO5300 AMOLED over QSPI (display subsystem)
- `hynitron,cst9217`: CST9217 touch controller (input subsystem; also reports
  the finger count)
- `everest,es7210`, `everest,es8311`: mic ADC and speaker codec (audio_codec)

## Notes

- Watch faces are generic styles: no brand names or logos.
- The board config reserves a little more RAM on core 0 than Zephyr does,
  because Zephyr's simple-boot image placed core 0's RAM 18 KB into core 1's
  (see `boards/.../Kconfig.defconfig`).
- TF card: some cards answer but never finish initialising (an 8 GB card
  stayed busy on ACMD41 forever); a 16 GB SDHC card works.
- The USB VID/PID is the pid.codes test pair (1209:0001): fine on your own
  desk, not for a product.

## Licence

Apache-2.0 (see [LICENSE](LICENSE)).

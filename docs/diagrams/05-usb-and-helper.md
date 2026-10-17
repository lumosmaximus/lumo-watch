# Diagram 5: the USB device and the helper app

Sources: `apps/watch/src/usb_hid.c`, `apps/watch/app.overlay`,
`apps/watch/src/main.c` (console parser), `tools/agent/agent_mac.py`.

Draw the watch on the left, a laptop on the right, one USB-C cable between
them carrying two logical interfaces (two coloured strands inside the cable):

**Strand A – HID (watch -> computer)**: a standard "Consumer Control"
device. Keys sent: volume up/down (0xE9/0xEA), mute (0xE2), play/pause
(0xCD), next/previous track (0xB5/0xB6), display brightness up/down
(0x6F/0x70). Note: "Windows and macOS act on these with no driver or app".
Show the path: knob turn on the panel -> core 1 computes clicks (one key per
15 degrees) -> MSG_KEY over IPM -> core 0 HID thread -> USB interrupt
endpoint -> OS volume changes.

**Strand B – CDC-ACM serial (both directions)**: the console. Watch -> host:
logs, stats once a second, trace dumps. Host -> watch, as one-line commands:
`T YYYY-MM-DD HH:MM:SS` set the clock; `S v=63 m=0 b=85 p=1` the computer's
state; `N <base64>` a now-playing text image (224x40, 2 bits/pixel, 2240 B);
`B` reboot to ROM download mode (flashing); `T1`/`T0` trace start/stop;
`F`/`Z`/`W` scripted actions.

**On the laptop: `agent_mac.py`** (a small Python process), with its sources:
- system volume and mute: AppleScript `get volume settings`
- built-in display brightness: DisplayServices framework via ctypes
- what is playing: Chrome/Edge YouTube tabs through "JavaScript from Apple
  Events" (`document.querySelector('video').paused`, title, channel), or
  Spotify / Music player state
- renders the title and artist with Pillow (any script, emoji), long lines
  scroll, ~12 frames/s -> strand B.
It finds the watch by USB id 1209:0001 and never grabs the flashing port.

**On the watch**: core 0 parses the lines, forwards state (MSG_STATE) and
text chunks (MSG_TEXT) to core 1; the remote screen shows the level meter,
mute/play state and the scrolling title.

**A footnote box – "one PHY, two controllers"**: the chip's USB-Serial-JTAG
(used for flashing) and the OTG controller (our device) share the USB pins.
While the watch runs, the flashing port is gone; `tools/flash.sh` sends `B`,
the watch reboots into the ROM's download mode, and both images are written
in one pass.

Teal for core 0 parts, pink for core 1, grey for the laptop.

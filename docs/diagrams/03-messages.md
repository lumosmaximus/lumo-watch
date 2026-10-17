# Diagram 3: messages between the cores

Source: `apps/watch/common/watch_msg.h`, `common/trace.h`.

Draw a sequence/flow diagram with two vertical lanes, core 0 (teal, left) and
core 1 (pink, right), and the IPM mailbox as a narrow channel between them.
Each message is an arrow with its id, payload size and when it is sent.

**Core 0 -> core 1**
- `MSG_TIME` (8 B): wall clock, sent exactly on an RTC second edge at boot,
  when core 1 asks, when the clock is set, and every minute. Core 1 adds
  milliseconds from its own uptime.
- `MSG_TOUCH` (6 B): x, y, down, finger count - every touch report (~50 Hz).
- `MSG_BUTTON` (1 B): PWR press, BOOT short press, BOOT held 3 s, two-finger
  hold 1.5 s (or one finger still 2 s).
- `MSG_STATE` (4 B): the computer's volume, mute, brightness, play state
  (from the helper app, every 0.4 s).
- `MSG_TEXT` (up to 484 B, in 480-byte chunks): the now-playing title/artist
  as a 224x40 2-bit image, 2240 B per frame, ~12 frames/s while scrolling.
- `MSG_TRACE_CTL` (1 B): trace on/off. `MSG_CMD` (3 B): scripted face / bezel /
  knob actions for repeatable traces.

**Core 1 -> core 0**
- `MSG_HELLO` (0 B): "I'm up, send the time", every second until it arrives.
- `MSG_KEY` (1 B): a media key to send to the computer (volume, mute, play,
  next, previous, brightness) - from the remote's knob and buttons.
- `MSG_STATS` (~56 B): once a second: frames, per-stage microseconds, boxes,
  pixels, SPI underruns.
- `MSG_BENCH`: boot-time micro-benchmark results.
- `MSG_TRACE` (481 B): a batch of 60 trace events.

**Rules shown as notes on the channel:**
- One mailbox per direction, 512 B max, no queue: `msg_send()` serialises
  sends and waits 200 us so the other core's interrupt copies the message
  out first. (Before that, the "open the remote" message was overwritten by
  the touch update sent microseconds later.)
- The receiving interrupt only copies; the main loop acts on the message.

Below the lanes, a small table of latencies: IPM round trip ~12 us; touch to
screen ~1 frame (16.7 ms); knob click to HID key on the computer ~5-20 ms.

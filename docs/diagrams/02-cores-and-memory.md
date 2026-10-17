# Diagram 2: two cores, two images, one chip

Sources: `apps/watch/sysbuild.cmake`, `boards/.../Kconfig.defconfig`,
`zephyr/soc/espressif/esp32s3/memory.h`, linker maps in `build-watch/*/zephyr/`.

Draw the ESP32-S3 as two side-by-side CPU cores with the memories between
and below them. This is asymmetric multiprocessing (AMP): two separate
Zephyr RTOS images, each with its own kernel, threads and drivers, no shared
scheduler.

**Core 0 – PROCPU – image `watch` (simple-boot format, flash 0x0)**
- Roles: I2C bus (touch, RTC, PMIC, expander, IMU), BOOT/PWR buttons, USB
  composite device (CDC-ACM console + HID), the time, trace store.
- Threads: main (RTC time edges, stats print), console RX, PWR poll, HID key
  sender, Zephyr USB and work queues.
- RAM: internal SRAM region 0x3fc88000-0x3fcc4f04 (~249 KB available, ~196 KB
  used), 8 MB PSRAM available (unused by the watch).

**Core 1 – APPCPU – image `watch_remote` (MCUboot format, flash 0xbc0000)**
- Role: the display only. Compositor, SPI2 + GDMA pair 0, TE pin polling.
- Threads: main only (plus idle).
- RAM: a carved-out 128 KB at 0x3fcc5000-0x3fce5000, 97% used: two 7.5 KB band
  buffers, the 62 KB bezel/knob ring strip, hand tables, stacks.
- Code and read-only data (dials, 1.7 MB) run from flash through the cache
  (IROM/DROM windows), hot loops in IRAM.

**Boot sequence (numbered arrows):** 1. ROM loads core 0's image. 2. Core 0's
Zephyr starts, maps core 1's flash partition into the cache, copies core 1's
RAM image, and releases core 1 from reset. 3. Core 1 boots its own Zephyr,
initialises SPI2/GDMA/display, says "hello" over IPM. 4. Core 0 answers with
the time.

**Shared between the cores** (draw as a thin strip between them):
- IPM mailbox: 1 KB shared memory + two software interrupts (one per
  direction), 512 bytes max per message, no queue (a new send overwrites an
  unread message, so sends are spaced).
- The system timer (one 16 MHz counter both cores read: the trace timebase).
- Flash/cache and the GDMA controller (core 0 uses pair 1 for I2S, core 1 pair 0).

**A bug worth a callout box:** Zephyr computed core 0's RAM end from the
simple-boot layout (0x3fce9704) but core 1's start from the MCUboot layout
(0x3fce5000) - the regions overlapped by 18 KB until the board config
reserved more on core 0.

Teal for core 0, pink for core 1, grey for shared. Memory sizes as labels.

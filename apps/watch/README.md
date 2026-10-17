# apps/watch

Two images, built together with sysbuild (`sysbuild.cmake`).

| | |
|---|---|
| `src/main.c` | core 0: RTC and time, touch and gestures, buttons, console commands, stats print |
| `src/usb_hid.c` | core 0: the USB composite device (CDC-ACM console + HID media keys), reboot-to-download |
| `src/trace_store.c` | core 0: stores trace events from both cores, dumps them on `T0` |
| `remote/src/main.c` | core 1: the compositor, faces, picker, remote, main loop |
| `remote/src/qspi_fast.c` | core 1: CO5300 windows driven from the SPI2 registers + GDMA |
| `remote/src/trace.c` | core 1: trace event staging, batches to core 0 |
| `remote/src/*_img.c` | generated art (tools/watchface) |
| `remote/Kconfig` | the A/B switches (each optimisation can be turned back off) |
| `common/watch_msg.h` | every message between the cores; `msg_send()` |
| `common/trace.h` | the trace event format |
| `app.overlay` | core 0's USB device tree |

Build and flash: `docs/BUILD.md`. How it works: `docs/diagrams/`, `docs/optimization-log.md`.

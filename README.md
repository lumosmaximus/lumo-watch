# Waveshare ESP32-S3-Touch-AMOLED-1.75 on Zephyr (dual core)

Zephyr board support, drivers and apps for the
[Waveshare ESP32-S3-Touch-AMOLED-1.75](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75)
(466x466 round AMOLED, CO5300 + CST9217, AXP2101, ES8311/ES7210, QMI8658,
PCF85063, TCA9554, TF card).

Both cores are used, as AMP (two Zephyr images):

- **core 0 (PROCPU)**: Wi-Fi/BLE, the shared I2C bus (power, sensors, touch,
  codec control), audio, TF card
- **core 1 (APPCPU)**: the display (LVGL), started by core 0 at boot; the two
  talk over shared memory with inter-processor interrupts

## Build

This repo is a Zephyr module; use it from an existing west workspace:

```
west build --sysbuild -b esp32s3_touch_amoled_1_75/esp32s3/procpu \
  path/to/amoled_175/apps/amp_hello -- -DBOARD_ROOT=path/to/amoled_175
west flash
```

`--sysbuild` builds both images; the core-1 image goes into the appcpu
partition (0xbc0000).

## Apps

- `apps/amp_hello`: dual-core bring-up. Core 0 scans I2C, dumps the AXP2101
  rails and pings core 1, which answers with its uptime and core ID.

# Diagram 1: the hardware

Sources: `boards/waveshare/esp32s3_touch_amoled_1_75/*.dts`, `*-pinctrl.dtsi`,
the Waveshare schematic.

Draw a hardware block diagram of the Waveshare ESP32-S3-Touch-AMOLED-1.75
board as used by lumo-watch. Centre: the ESP32-S3R8 module (two Xtensa LX7
cores at 240 MHz, 512 KB internal SRAM, 8 MB octal PSRAM in package, 16 MB
QSPI flash W25Q128 in quad-I/O mode). Around it, grouped by bus:

**QSPI display bus (SPI2 host, 80 MHz, 4 data lines)**
- CO5300 AMOLED controller driving the 1.75" round 466x466 panel.
- Pins: SCLK GPIO38, CS GPIO12, D0-D3 GPIO4-7, reset GPIO39 (through the I/O
  expander, pin 7), TE (tearing effect) GPIO13 -> input to core 1 (polled).
- Note on the link: "windows start on even columns, end on odd; column offset 6".

**I2C bus (I2C0, SDA GPIO15, SCL GPIO14, 400 kHz) – owned by core 0**
- CST9217 touch controller, addr 0x5A, INT GPIO11, reset GPIO40 (axes inverted)
- AXP2101 PMIC, addr 0x34 (rails: DCDC1 3.3 V, ALDO1-4, BLDO1-2; battery, PWR key)
- PCF85063 RTC, addr 0x51 (no backup battery fitted: time lost when unplugged)
- QMI8658 IMU, addr 0x6B
- TCA9554 I/O expander, addr 0x20, no INT line (polled): EXIO4 = PWR key state,
  EXIO3 RTC INT, EXIO5 AXP IRQ, EXIO6 IMU INT, EXIO7 display reset
- ES8311 codec (speaker DAC), addr 0x18; ES7210 4-channel mic ADC, addr 0x40

**I2S (I2S0): MCLK GPIO42, BCK GPIO9, WS GPIO45, DOUT GPIO8 -> ES8311,
DIN GPIO10 <- ES7210. PA enable GPIO46 (speaker amp, unused).**

**SDMMC slot 1, 1-bit: CMD GPIO1, CLK GPIO2, D0 GPIO3 (D3 not routed).**

**USB-C**: native USB on GPIO19/20. Two things share one PHY: the chip's
built-in USB-Serial-JTAG (flashing, default console) and the USB OTG
controller (our composite device: CDC-ACM serial + HID media keys). Show a
switch between them labelled "PHY select".

**Buttons**: BOOT GPIO0 (strap pin, active low) -> core 0 GPIO interrupt;
PWR -> AXP2101 PWRON, state readable as EXIO4.

Colour code: things core 0 owns in teal, the display path (core 1) in pink.
Keep pin labels monospace. Landscape, suitable for a blog header-width image.

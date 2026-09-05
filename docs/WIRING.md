# Wiring Guide

This guide applies to firmware release `0.8.3`.

## Parts

- Wemos D1 mini V3.0.0 (ESP8266)
- 0.91-inch SSD1306 OLED, 128×32 pixels, I²C
- USB cable and a stable USB power supply

## Connections

| OLED | Wemos D1 mini | ESP8266 GPIO | Purpose |
|---|---|---:|---|
| GND | G | — | Ground |
| VCC | 3V3 | — | 3.3 V supply |
| SCL | D1 | GPIO5 | I²C clock |
| SDA | D2 | GPIO4 | I²C data |

Connect the OLED to `3V3`, not to the 5 V USB rail. Check the pin labels on the specific OLED module before powering it; module pin order is not universal.

## OLED detection

The firmware probes both common SSD1306 I²C addresses:

- `0x3C`
- `0x3D`

No address change in the sketch is normally required. If neither address responds, the firmware continues in Serial-only mode and reports the problem at 115200 baud.

The OLED is a convenience display, not a boot requirement. The ESP8266 can
still connect, authenticate, fetch usage, and report diagnostics through the
Serial Monitor when no compatible OLED is connected.

## Assembly checklist

1. Disconnect USB power.
2. Verify GND and 3V3 with the board labels.
3. Connect SDA to D2 and SCL to D1.
4. Check for shorts between 3V3 and GND.
5. Connect USB power and look for the startup banner.
6. If the OLED is not detected, recheck the cable orientation and run an I²C scanner before changing firmware.

An optional compatible enclosure is listed in the [main README](../README.md).

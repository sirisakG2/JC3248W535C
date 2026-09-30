# JC3248W535C

Setup and spec notes for the Guition **JC3248W535C_I_Y** — ESP32-S3 3.5" capacitive touch display module.

## Specs

| Item | Value |
|---|---|
| MCU | ESP32-S3 (QFN56, rev v0.2), dual-core, WiFi + BLE |
| Flash | 16MB quad (QIO) |
| PSRAM | 8MB embedded |
| Crystal | 40MHz |
| USB | Native USB-Serial/JTAG (VID:PID 303A:1001), no CH340/CP210x |
| Display | 3.5" IPS, 320x480, AXS15231B controller, QSPI |
| Touch | Capacitive, I2C address 0x3B (integrated in AXS15231B) |

## Pin mapping (unverified against schematic — confirm on hardware)

| Function | GPIO |
|---|---|
| LCD CS | 45 |
| LCD SCK | 47 |
| LCD D0 / D1 / D2 / D3 | 21 / 48 / 40 / 39 |
| Backlight | 1 |
| Touch SDA / SCL | 4 / 8 |
| Touch INT | 3 |

## Build

PlatformIO with the pioarduino platform (Arduino core 3.x is required by
GFX Library for Arduino; the stock `espressif32` platform only has core 2.x).

```
pio run -t upload --upload-port /dev/cu.usbmodem101
pio device monitor
```

`src/main.cpp` draws RGB bands plus text and paints a dot at each touch point,
logging coordinates over serial.

## Status

Project scaffolded; first flash and display/touch verification pending.

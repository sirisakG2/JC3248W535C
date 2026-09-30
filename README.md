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

Verified on hardware: display, touch and pin mapping all work; touch response is fast.

## LVGL demo

`src/main.cpp` is an LVGL 9 demo (button counter, brightness slider, switch, arc). Verified on hardware: all touch works.

- Config is passed as build flags in `platformio.ini` (`-DLV_CONF_SKIP` plus a few `LV_*` defines), so there is no `lv_conf.h`.
- LVGL renders in PARTIAL mode into a 40-line internal-RAM buffer; `flush_cb` copies each area into the `Arduino_Canvas` and calls `gfx->flush()` once on the last area of a frame.
- Touch is exposed to LVGL as a pointer indev via `touch_cb`.

## Notes / gotchas

- Colour constants are `RGB565_BLACK`, `RGB565_RED`, etc. in current GFX Library versions.
- Drawing directly to the panel (no canvas) garbles the picture on this controller. Use `Arduino_Canvas` and `flush()`.
- Start the bus at 40MHz: `gfx->begin(40000000)`.
- Flush at most every ~30ms and only when something changed; flushing on every touch event is slow.
- Touch read: write `B5 AB A5 5A 00 00 00 08` to 0x3B, read 8 bytes; `b[1]` = touch count, x = `(b[2]&0x0F)<<8 | b[3]`, y = `(b[4]&0x0F)<<8 | b[5]`.
- If upload won't start: hold BOOT, tap RESET, release BOOT.

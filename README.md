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

## Samples

| Sample | What it is |
|---|---|
| [`examples/weather-clock`](examples/weather-clock/main.cpp) | WiFi clock + weather dashboard (NTP time, Open-Meteo weather, 3-day forecast). Settings tab has a WiFi scanner/connect with on-screen keyboard, city search, 12/24h switch and brightness. WiFi credentials are stored in on-board flash only, never in the repo. |
| [`examples/media-player`](examples/media-player/main.cpp) | Photo frame + MJPEG video player from a microSD card. Reads `/pic/*.jpg` and `/mjpeg/*.mjpeg` (also `/photos`, `/video`). Landscape media (e.g. 480x320) is rotated 90° to fill the portrait screen. Tap = pause, swipe = next/prev, long-press = switch photos/video. |
| [`examples/music-player`](examples/music-player/main.cpp) | MP3/WAV/FLAC/AAC player from `/music` on the SD card with an LVGL track list, prev/play/next, volume and progress bar; auto-advances. **Audio output UNVERIFIED** (no speaker was connected when tested; UI and track scan work). |
| [`examples/paint`](examples/paint/main.cpp) | Finger-paint app: 8 colours incl. eraser, brush size, clear, and SAVE to `/paint/paintN.bmp` on the SD card (16-bit RGB565 BMP, opens on Mac/PC). Screen display/touch verified; BMP save not yet checked on a computer. |
| [`examples/tictactoe`](examples/tictactoe/main.cpp) | Tic-tac-toe with 5 modes: CPU Hard (unbeatable minimax), CPU Easy, 2 Players, **Forever 2P** and **Forever CPU** (max 3 marks per player - the oldest vanishes on a 4th placement, shown faded - plus a 4 s per-move countdown, `TURN_MS`). Scores saved in flash. |
| [`examples/game2048`](examples/game2048/main.cpp) | 2048: swipe to slide/merge tiles, score + best, one-step UNDO, win screen with keep-going. Board, score and best are saved in flash and resume after power-off. |
| [`examples/snake`](examples/snake/main.cpp) | Snake on a 20x20 field: on-screen arrow pad (instant turns; swiping also works), PAUSE/NEW, speed ramps from 200 ms to 90 ms per step, best score saved in flash. |
| [`examples/connect4`](examples/connect4/main.cpp) | Connect Four (7x6): tap a column, falling-disc animation, win highlight. Modes: CPU Hard (negamax + alpha-beta, depth 7), CPU Easy, 2 Players. Scores saved in flash. |
| [`examples/tetris`](examples/tetris/main.cpp) | Tetris: 10x20 field, 7-bag randomiser, ghost piece, next preview, levels, on-screen buttons with hold-to-repeat, best score in flash. Touch is debounced (needs a full lift between presses). |
| [`examples/lvgl-basic`](examples/lvgl-basic/main.cpp) | Minimal LVGL 9 demo: button counter, slider, switch, arc. Good starting point for new UIs. |

## Build

PlatformIO with the pioarduino platform (Arduino core 3.x is required by
GFX Library for Arduino; the stock `espressif32` platform only has core 2.x).
Each sample is a PlatformIO environment:

```
pio run -e weather-clock -t upload --upload-port /dev/cu.usbmodem101
pio run -e media-player  -t upload --upload-port /dev/cu.usbmodem101
pio run -e music-player  -t upload --upload-port /dev/cu.usbmodem101
pio run -e paint         -t upload --upload-port /dev/cu.usbmodem101
pio run -e tictactoe     -t upload --upload-port /dev/cu.usbmodem101
pio run -e game2048      -t upload --upload-port /dev/cu.usbmodem101
pio run -e snake         -t upload --upload-port /dev/cu.usbmodem101
pio run -e connect4      -t upload --upload-port /dev/cu.usbmodem101
pio run -e tetris        -t upload --upload-port /dev/cu.usbmodem101
pio run -e lvgl-basic    -t upload --upload-port /dev/cu.usbmodem101
pio device monitor
```

To add a sample, create `examples/<name>/main.cpp` and an `[env:<name>]` entry
in `platformio.ini` with `build_src_filter = -<*> +<<name>/>`.

## Status

Verified on hardware: display, touch and pin mapping all work; touch response is fast.

## microSD

Verified pins (own SPI3/HSPI bus, separate from the display's SPI2): CS=10, MOSI=11, SCK=12, MISO=13. Use `SPIClass sdSpi(HSPI)`.
Video files are raw concatenated JPEG frames (`.mjpeg`), e.g. made with
`ffmpeg -i in.mp4 -vf "scale=480:320" -q:v 6 -r 20 out.mjpeg`.

## Audio (unverified)

The board has **no built-in speaker**: it has a 2-pin JST 1.25 connector for an external 4-8 ohm speaker, driven by an NS4168 mono I2S amp.
Pins used (from an ESPHome config for this board, not yet confirmed on hardware): BCLK=42, LRC=2, DOUT=41.
Uses `schreibfaul1/ESP32-audioI2S`, which needs `-DCORE_DEBUG_LEVEL=0` in PlatformIO. The audio loop runs on its own core so UI redraws do not cause dropouts.
The UI font has no CJK glyphs, so non-ASCII track names are shown as "Track N".

## LVGL demo

Both samples use LVGL 9 and are verified on hardware (touch works, keyboard is usable).

- Config is passed as build flags in `platformio.ini` (`-DLV_CONF_SKIP` plus a few `LV_*` defines), so there is no `lv_conf.h`.
- LVGL renders in PARTIAL mode into a 40-line internal-RAM buffer; `flush_cb` copies each area into the `Arduino_Canvas` and calls `gfx->flush()` once on the last area of a frame.
- On-screen keyboard: make it tall (320px) with a 20pt key font, or keys are too small to hit.
- Games: swipe-only controls are unreliable on this screen (a full-frame redraw can miss part of a swipe); on-screen buttons that act on press feel much better.
- Touch: the controller drops out briefly while a finger is down. For button UIs, require ~6 empty reads before treating it as a release, and a full lift between presses, or one tap registers as several.
- Touch is exposed to LVGL as a pointer indev via `touch_cb`.

## Notes / gotchas

- Colour constants are `RGB565_BLACK`, `RGB565_RED`, etc. in current GFX Library versions.
- Drawing directly to the panel (no canvas) garbles the picture on this controller. Use `Arduino_Canvas` and `flush()`.
- Start the bus at 40MHz: `gfx->begin(40000000)`.
- Flush at most every ~30ms and only when something changed; flushing on every touch event is slow.
- Touch read: write `B5 AB A5 5A 00 00 00 08` to 0x3B, read 8 bytes; `b[1]` = touch count, x = `(b[2]&0x0F)<<8 | b[3]`, y = `(b[4]&0x0F)<<8 | b[5]`.
- If upload won't start: hold BOOT, tap RESET, release BOOT.

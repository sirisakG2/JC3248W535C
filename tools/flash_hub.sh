#!/bin/bash
# Build and flash the game hub: launcher in ota_0, one game per ota slot.
#   tools/flash_hub.sh [serial-port]   (auto-detects /dev/cu.usbmodem*)
# Slot offsets come from hub_partitions.csv (ota_N = 0x1B0000 + (N-1) * 0x100000).
set -e
cd "$(dirname "$0")/.."
PORT=${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}   # first USB serial port if none given
ESPTOOL=~/.platformio/packages/tool-esptoolpy/esptool.py
PY=~/.platformio/penv/bin/python   # PlatformIO's Python has esptool's dependencies
GAMES=(tictactoe game2048 snake connect4 tetris minesweeper memory paint)   # ota_1 .. ota_8

echo "== launcher + bootloader + partition table"
pio run -e hub -t upload --upload-port "$PORT"

for i in "${!GAMES[@]}"; do
  g=${GAMES[$i]}
  off=$(printf '0x%X' $((0x1B0000 + i * 0x100000)))
  echo "== $g -> ota_$((i + 1)) @ $off"
  pio run -e "$g"
  "$PY" "$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 write_flash "$off" ".pio/build/$g/firmware.bin"
done
echo "Done. Reset the board to open the hub."

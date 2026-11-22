#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Flash a board plugged into another machine: build here, copy the binaries over
# SSH, run esptool there, then print the first seconds of the boot log.
#
#   scripts/flash-remote.sh <ssh-host> [serial-port] [log-seconds]
#
# The remote machine needs python3 with pyserial and esptool (`pip install esptool`)
# and your user in the serial group (e.g. dialout). For a board plugged into this
# machine, `pio run -t upload` is all you need.
set -euo pipefail
HOST=${1:?usage: $0 <ssh-host> [serial-port] [log-seconds]}
PORT=${2:-/dev/ttyUSB0}
SECS=${3:-12}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/.pio/build/cyd"
BOOT_APP0="$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
(cd "$ROOT" && pio run)
ssh "$HOST" 'mkdir -p ~/.bubble-screen-flash'
rsync -az "$B/bootloader.bin" "$B/partitions.bin" "$B/firmware.bin" "$BOOT_APP0" "$HOST:.bubble-screen-flash/"
ssh "$HOST" "cd ~/.bubble-screen-flash && python3 -m esptool --chip esp32 --port $PORT --baud 921600 write_flash -z \
  0x1000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin"
ssh "$HOST" "python3 - <<PY
import serial, time
s = serial.Serial('$PORT', 115200, timeout=0.2)
s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
t = time.time(); out = b''
while time.time() - t < $SECS: out += s.read(4096)
print(out.decode(errors='replace'))
PY"

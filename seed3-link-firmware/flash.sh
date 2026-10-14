#!/bin/bash
# Flashes the Seed3 over USB (DFU), always reading a backup of what's on it first.
#
#   bash seed3-link-firmware/flash.sh                    flash build/seed3-link/seed3-link.bin
#   bash seed3-link-firmware/flash.sh --restore FILE     put a backup (or Rocco's build) back
#
# Backups go to build/seed3-link/backups/ (git-ignored; they contain the model weights).
# Needs dfu-util. The Seed's app lives in internal flash at 0x08000000 (APP_TYPE BOOT_NONE).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
work="$root/build/seed3-link"
image="$work/seed3-link.bin"
if [[ "${1:-}" == "--restore" ]]; then
    image="${2:?usage: flash.sh --restore FILE}"
fi
[[ -f "$image" ]] || { echo "No $image: run seed3-link-firmware/build.sh first." >&2; exit 1; }
command -v dfu-util >/dev/null || { echo "Install dfu-util (brew install dfu-util)." >&2; exit 1; }

in_dfu() { dfu-util -l 2>/dev/null | grep -q '0483:df11'; }

# 1. Get the Seed into its bootloader. Its NAM firmware reboots into DFU when sent 'B' on its USB
#    console (Rocco's make upload does the same). The port is found by the Seed's USB id (0483:5740)
#    so nothing is sent to the touchscreen; SEED_PORT=/dev/... overrides. Otherwise: the buttons.
find_seed_port() {
    [[ -n "${SEED_PORT:-}" ]] && { echo "$SEED_PORT"; return; }
    local py
    for py in "$HOME"/.espressif/python_env/*/bin/python python3; do
        "$py" -c 'import serial.tools.list_ports as l
for p in l.comports():
    if p.vid == 0x0483 and p.pid == 0x5740: print(p.device); break' 2>/dev/null && return
    done
}
if ! in_dfu; then
    port=$(find_seed_port | head -1)
    if [[ -n "$port" ]]; then
        echo "Asking the Seed on $port to enter DFU..."
        # A Seed that has locked up never reads it, so don't let the write hang.
        (printf B > "$port") 2>/dev/null & writer=$!
        sleep 2; kill "$writer" 2>/dev/null || true
        for _ in $(seq 30); do in_dfu && break; sleep 0.2; done
    fi
fi
if ! in_dfu; then
    echo "Put the Seed in DFU mode: hold BOOT, tap RESET, release BOOT. Waiting..."
    until in_dfu; do sleep 0.5; done
fi

# 2. Back up what's on it now, so this can always be undone.
mkdir -p "$work/backups"
backup="$work/backups/seed-flash-$(date +%Y%m%d-%H%M%S).bin"
dfu-util -d 0483:df11 -a 0 -s 0x08000000:131072 -U "$backup" > "$work/backup.log" 2>&1 \
    || { cat "$work/backup.log"; echo "Backup failed; not flashing." >&2; exit 1; }
echo "Backed up the Seed to $backup"

# 3. Flash and restart. dfu-util reports an error after a good flash on this chip (the STM32 resets
#    on :leave before answering), so success is judged by the download line, as Rocco's Makefile does.
dfu-util -d 0483:df11 -a 0 -s 0x08000000:leave -D "$image" 2>&1 | tee "$work/flash.log" | grep -E 'Download|downloaded' | tail -1
grep -q 'File downloaded successfully' "$work/flash.log" || { echo "Flash failed; see $work/flash.log" >&2; exit 1; }
echo "Flashed $image. To undo: bash seed3-link-firmware/flash.sh --restore $backup"

#!/bin/bash
# Flashes the Seed3 over USB (DFU), always reading a backup of what's on it first.
#
#   bash seed3-link-firmware/flash.sh                    build/seed3-link/seed3-link-boot.bin into the
#                                                        QSPI flash, installing the Daisy bootloader
#                                                        in the internal flash first if it isn't there
#   bash seed3-link-firmware/flash.sh --internal         build/seed3-link/seed3-link.bin into the
#                                                        internal flash (no bootloader)
#   bash seed3-link-firmware/flash.sh --restore FILE     put an internal-flash backup (or Rocco's
#                                                        build) back; this also removes the bootloader
#
# Backups go to build/seed3-link/backups/ (git-ignored; they contain the model weights).
# Needs dfu-util. Two DFU bootloaders can answer: the STM32's own (internal flash at 0x08000000) and
# the Daisy bootloader (QSPI flash; apps at 0x90040000).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
work="$root/build/seed3-link"
mode=boot
image="$work/seed3-link-boot.bin"
case "${1:-}" in
    --restore) mode=internal; image="${2:?usage: flash.sh --restore FILE}" ;;
    --internal) mode=internal; image="$work/seed3-link.bin" ;;
esac
[[ -f "$image" ]] || { echo "No $image: run seed3-link-firmware/build.sh first (with --internal for that build)." >&2; exit 1; }
if [[ $mode == boot && ! -f "$work/daisy-bootloader.bin" ]]; then
    echo "Run seed3-link-firmware/build.sh first." >&2
    exit 1
fi
command -v dfu-util >/dev/null || { echo "Install dfu-util (brew install dfu-util)." >&2; exit 1; }
mkdir -p "$work/backups"

# Which DFU bootloader is answering: stm (internal flash), daisy (QSPI), or none.
dfu_kind() {
    local l
    l=$(dfu-util -l 2>/dev/null | grep '0483:df11' || true)
    if [[ "$l" == *"Internal Flash"* ]]; then echo stm
    elif [[ "$l" == *"0x90000000"* ]]; then echo daisy
    else echo none; fi
}
wait_dfu() { # up to $1 seconds
    for _ in $(seq $(($1 * 5))); do [[ $(dfu_kind) != none ]] && return; sleep 0.2; done
}

# Ask the running firmware to reboot into a bootloader: 'B' = the one it came from (the STM32's, or
# the Daisy bootloader for a bootloader build), 'U' = the STM32's (bootloader builds only).
# The port is found by the Seed's USB id (0483:5740), so nothing is sent to the touchscreen;
# SEED_PORT=/dev/... overrides.
find_seed_port() {
    [[ -n "${SEED_PORT:-}" ]] && { echo "$SEED_PORT"; return; }
    local py
    for py in "$HOME"/.espressif/python_env/*/bin/python python3; do
        "$py" -c 'import serial.tools.list_ports as l
for p in l.comports():
    if p.vid == 0x0483 and p.pid == 0x5740: print(p.device); break' 2>/dev/null && return
    done
}
send_key() {
    local port=$1 key=$2 py
    # Through pyserial when there is one (a plain shell write to the port didn't always arrive); a
    # Seed that has locked up never reads it, so don't let the write hang.
    for py in "$HOME"/.espressif/python_env/*/bin/python python3; do
        "$py" -c 'import serial, sys, time
s = serial.Serial(sys.argv[1], 115200, timeout=0.5, write_timeout=1)
time.sleep(0.2)
s.write(sys.argv[2].encode()); s.flush()' "$port" "$key" 2>/dev/null && return
    done
    (printf '%s' "$key" > "$port") 2>/dev/null &
    local writer=$!
    sleep 2
    kill "$writer" 2>/dev/null || true
}

# 1. Into a bootloader: the Daisy one when it's there and wanted, else the STM32's.
if [[ $(dfu_kind) == none ]]; then
    port=$(find_seed_port | head -1)
    if [[ -n "$port" ]]; then
        echo "Asking the Seed on $port to enter DFU..."
        if [[ $mode == boot ]]; then
            send_key "$port" B
            wait_dfu 6
        else
            send_key "$port" U # a bootloader build goes to the STM32's DFU; others ignore it
            wait_dfu 4
            port=$(find_seed_port | head -1)
            if [[ $(dfu_kind) == none && -n "$port" ]]; then
                send_key "$port" B
                wait_dfu 6
            fi
        fi
    fi
fi
if [[ $mode == internal && $(dfu_kind) == daisy ]]; then
    echo "The Daisy bootloader is waiting; this needs the STM32's own DFU instead."
    echo "Hold BOOT, tap RESET, release BOOT. Waiting..."
    until [[ $(dfu_kind) == stm ]]; do sleep 0.5; done
fi
if [[ $(dfu_kind) == none ]]; then
    echo "Put the Seed in DFU mode: hold BOOT, tap RESET, release BOOT. Waiting..."
    until [[ $(dfu_kind) != none ]]; do sleep 0.5; done
fi

# 2. Back up the internal flash (through the STM32's DFU), so this can always be undone.
stamp=$(date +%Y%m%d-%H%M%S)
backup=""
if [[ $(dfu_kind) == stm ]]; then
    backup="$work/backups/seed-flash-$stamp.bin"
    dfu-util -d 0483:df11 -a 0 -s 0x08000000:131072 -U "$backup" > "$work/backup.log" 2>&1 \
        || { cat "$work/backup.log"; echo "Backup failed; not flashing." >&2; exit 1; }
    echo "Backed up the Seed's internal flash to $backup"
fi

# dfu-util reports an error after a good flash (the STM32 resets on :leave before answering), so
# success is judged by the download line, as Rocco's Makefile does.
flash() { # address file
    { dfu-util -d 0483:df11 -a 0 -s "$1:leave" -D "$2" || true; } > "$work/flash.log" 2>&1
    grep -q 'File downloaded successfully' "$work/flash.log" || { echo "Flash failed; see $work/flash.log" >&2; exit 1; }
}

if [[ $mode == internal ]]; then
    flash 0x08000000 "$image"
    echo "Flashed $image. To undo: bash seed3-link-firmware/flash.sh --restore $backup"
    exit 0
fi

# 3. Bootloader build: install the Daisy bootloader if the STM32's DFU answered (no bootloader yet, or a
#    'U'), then back up the QSPI region the app goes into and flash the app through the bootloader.
if [[ $(dfu_kind) == stm ]]; then
    flash 0x08000000 "$work/daisy-bootloader.bin"
    echo "Installed the Daisy bootloader. To remove it and put the old firmware back:"
    echo "  bash seed3-link-firmware/flash.sh --restore $backup"
    wait_dfu 10
fi
[[ $(dfu_kind) == daisy ]] || { echo "The Daisy bootloader didn't answer; press RESET and run this again." >&2; exit 1; }
qspi_backup="$work/backups/qspi-0x90040000-512k-$stamp.bin"
dfu-util -d 0483:df11 -a 0 -s 0x90040000:524288 -U "$qspi_backup" > "$work/backup.log" 2>&1 \
    || { cat "$work/backup.log"; echo "QSPI backup failed; not flashing." >&2; exit 1; }
echo "Backed up the QSPI app region to $qspi_backup"
flash 0x90040000 "$image"
echo "Flashed $image. It starts 2 s after power-up (the bootloader's DFU window)."

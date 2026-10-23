#!/usr/bin/env bash
#
# Copy the flatsat controller firmware onto the QT Py's CIRCUITPY drive.
#
# Expects an fstab entry so that mounting needs no password and lands at a
# predictable path:
#
#   sudo mkdir -p /mnt/circuitpy
#   echo 'LABEL=CIRCUITPY /mnt/circuitpy vfat noauto,user,umask=000,flush 0 0' \
#       | sudo tee -a /etc/fstab
#
# Override the location with CIRCUITPY=/somewhere/else flatsat/deploy.sh

set -euo pipefail

MOUNT=${CIRCUITPY:-/mnt/circuitpy}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

if [ ! -e /dev/disk/by-label/CIRCUITPY ]; then
    echo "error: no CIRCUITPY volume found - is the QT Py plugged in?" >&2
    exit 1
fi

if ! mountpoint -q "$MOUNT"; then
    if [ ! -d "$MOUNT" ]; then
        echo "error: $MOUNT does not exist - add the fstab entry above" >&2
        exit 1
    fi
    echo "mounting CIRCUITPY at $MOUNT"
    mount "$MOUNT"
fi

cp "$HERE/boot.py" "$HERE/code.py" "$MOUNT/"

# FAT on a microcontroller needs the flush, or CircuitPython can start
# reloading a half-written file.
sync

echo
echo "  Copied to $MOUNT"
echo "  code.py auto-reloads on its own."
echo "  If boot.py changed, press the QT Py reset button - USB descriptors"
echo "  only change on a hard reset."
echo

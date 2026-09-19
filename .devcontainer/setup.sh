#!/usr/bin/env bash
#
# Assembles the west workspace inside the container.
#
# Codespaces clones this repository to /workspaces/silversat-fsw, so the west
# workspace root is /workspaces and Zephyr lands at /workspaces/zephyr as a
# sibling. west.yml's `self: path:` must match the directory name.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKSPACE_ROOT="$(dirname "$REPO_DIR")"
REPO_NAME="$(basename "$REPO_DIR")"

echo "==> workspace root: $WORKSPACE_ROOT"
echo "==> manifest repo:  $REPO_NAME"

cd "$WORKSPACE_ROOT"

if [ ! -w "$WORKSPACE_ROOT" ]; then
	echo "ERROR: $WORKSPACE_ROOT is not writable by $(id -un)." >&2
	echo "The Dockerfile should chown it to the container user." >&2
	exit 1
fi

if [ ! -d .west ]; then
	echo "==> west init"
	west init -l "$REPO_NAME"
else
	echo "==> west already initialized, skipping"
fi

# Shallow and narrow: we build against a pinned tag and never need history.
echo "==> west update (first run takes a few minutes)"
west update --narrow -o=--depth=1

echo "==> west zephyr-export"
west zephyr-export

echo "==> installing Zephyr python requirements"
pip3 install --break-system-packages --no-cache-dir \
	-r zephyr/scripts/requirements.txt

cat <<BANNER

==============================================================
 SilverSat FSW workspace ready.

   cd $REPO_DIR

   west twister -p native_sim -T tests --inline-logs
       run the full emulated suite

   west build -p -b native_sim tests/drivers/mag
       build one test directly (fastest way to chase a
       devicetree or Kconfig error)

   west build -p -b native_sim app && ./build/zephyr/zephyr.exe
       run the application under emulation

   west build -p -b nucleo_f446re app
       cross-compile for the flatsat target
==============================================================

BANNER

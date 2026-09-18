#!/usr/bin/env bash
#
# Assembles the west workspace inside the container.
#
# Codespaces clones this repository to /workspaces/silversat-fsw, so the west
# workspace root is /workspaces and Zephyr lands at /workspaces/zephyr as a
# sibling. west.yml's `self: path:` must match the directory name, which is
# why it says silversat-fsw rather than app.

set -euo pipefail

WORKSPACE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_NAME="$(basename "$REPO_DIR")"

echo "==> workspace root: $WORKSPACE_ROOT"
echo "==> manifest repo:  $REPO_NAME"

cd "$WORKSPACE_ROOT"

if [ ! -d .west ]; then
	echo "==> west init"
	west init -l "$REPO_NAME"
else
	echo "==> west already initialized, skipping init"
fi

# --narrow with a shallow fetch: we build against a pinned tag and never need
# the history. Saves several minutes and a couple of GB.
echo "==> west update (first run takes a few minutes)"
west update --narrow -o=--depth=1

echo "==> west zephyr-export"
west zephyr-export

cat <<'BANNER'

==============================================================
 SilverSat FSW workspace ready.

   cd /workspaces/silversat-fsw

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

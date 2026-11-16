#!/usr/bin/env bash
# Installs (or removes) the OS Control Center privileged helper.
#
#   bash privileged/install.sh             install
#   bash privileged/install.sh --uninstall remove
#
# Run it from the project folder, after building.

set -euo pipefail

HELPER_DIR="/usr/local/libexec/os-control-center"
HELPER_BIN="$HELPER_DIR/os-control-helper"
POLICY_DST="/usr/share/polkit-1/actions/org.oscontrol.helper.policy"

cd "$(dirname "$0")/.."

if [ "${1:-}" = "--uninstall" ]; then
    sudo rm -f "$HELPER_BIN" "$POLICY_DST"
    sudo rmdir "$HELPER_DIR" 2>/dev/null || true
    echo "Removed helper and policy."
    exit 0
fi

if [ ! -x build/os-control-helper ]; then
    echo "build/os-control-helper not found. Run:"
    echo "  cmake -S . -B build && cmake --build build"
    exit 1
fi

# Owned by root, not writable by anyone else. This matters: if a normal user
# could replace the helper, they could run their own code as root.
sudo install -d -o root -g root -m 0755 "$HELPER_DIR"
sudo install -o root -g root -m 0755 build/os-control-helper "$HELPER_BIN"
sudo install -o root -g root -m 0644 privileged/org.oscontrol.helper.policy "$POLICY_DST"

echo "Installed:"
ls -l "$HELPER_BIN" "$POLICY_DST"
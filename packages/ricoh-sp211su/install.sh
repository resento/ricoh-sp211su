#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEB_FILE="$SCRIPT_DIR/ricoh-sp211su-driver_1.0.0_amd64.deb"

echo "=== Ricoh Aficio SP 211SU Driver Installer ==="

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This installer must be run as root (sudo ./install.sh)."
    exit 1
fi

if [ -f "$DEB_FILE" ]; then
    echo "Installing Debian package: $DEB_FILE..."
    if command -v apt-get >/dev/null 2>&1; then
        apt-get update -qq || true
        apt-get install -y "$DEB_FILE"
    else
        dpkg -i "$DEB_FILE" || apt-get install -f -y
    fi
else
    echo "Error: Package $DEB_FILE not found. Run ./build.sh first."
    exit 1
fi

echo "=== Installation complete! ==="
echo "You can check status at any time with: sudo ricoh-sp211su-setup"

#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "=== Building Ricoh SP 211SU Driver Package ==="

# 1. Compile SANE backend from source
echo "Compiling SANE backend from source..."
make -C "$REPO_ROOT/drivers/sane-ricoh-sp211su" clean
make -C "$REPO_ROOT/drivers/sane-ricoh-sp211su"

# 2. Copy freshly compiled binary to debian tree
cp "$REPO_ROOT/drivers/sane-ricoh-sp211su/libsane-ricoh_sp211su.so.1.0.0" \
   "$SCRIPT_DIR/debian/usr/lib/x86_64-linux-gnu/sane/libsane-ricoh_sp211su.so.1.0.0"

# 3. Ensure permissions
chmod 0755 "$SCRIPT_DIR/debian/DEBIAN/postinst" \
           "$SCRIPT_DIR/debian/DEBIAN/prerm" \
           "$SCRIPT_DIR/debian/DEBIAN/postrm" \
           "$SCRIPT_DIR/debian/usr/bin/ricoh-sp211su-setup" \
           "$SCRIPT_DIR/debian/usr/lib/cups/filter/rastertolilo"

# 4. Build .deb package
DEB_NAME="ricoh-sp211su-driver_1.0.0_amd64.deb"
echo "Building $DEB_NAME..."
dpkg-deb --build --root-owner-group "$SCRIPT_DIR/debian" "$SCRIPT_DIR/$DEB_NAME"

echo "=== Package successfully built: $SCRIPT_DIR/$DEB_NAME ==="

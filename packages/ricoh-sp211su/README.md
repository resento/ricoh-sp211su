# Ricoh Aficio SP 211SU Multi-Function Linux Driver & Package

Complete, standalone driver package for the **Ricoh Aficio SP 211SU** (USB ID `05ca:044f`) on Linux (Debian/Ubuntu x86_64).

This package bundles both printing and scanning capabilities into a single `.deb` package and automated installer.

## Features

- **Printer Filter (`rastertolilo`)**: Proprietary JBIG/DDST raster filter required by Ricoh SP 200/210 series.
- **Printer PPD**: Official Ricoh PPD configured for 600/1200 DPI printing.
- **Custom SANE Backend (`ricoh_sp211su`)**:
  - Independent SANE dynamic library (`libsane-ricoh_sp211su.so.1.0.0`).
  - Implements the reverse-engineered Primax 32-bit protocol over USB interface 1.
  - Supports Lineart, Gray (8-bit), and Color (24-bit RGB) scanning up to 600 DPI.
  - Built-in device status polling (handles warm-up / sleep recovery without stalling).
  - 32 KB aligned USB buffer preventing `LIBUSB_ERROR_OVERFLOW` with frontends like `simple-scan`.
  - Registered in `/etc/sane.d/dll.d/ricoh-sp211su` without modifying or conflicting with system `libsane1`.
- **Udev Rules**: Sets 0666 permissions on USB device `05ca:044f` so any user in `scanner` / `lp` group can access both printer and scanner.
- **Auto-configuration Helper (`ricoh-sp211su-setup`)**: Automatically detects USB URI via `lpinfo -v` and registers the CUPS queue `Ricoh_SP211SU_Local`.

## Quick Install (Pre-built Package)

To install or reinstall on a fresh Ubuntu/Debian system:

```bash
cd packages/ricoh-sp211su
sudo ./install.sh
```

Or manually:

```bash
sudo dpkg -i ricoh-sp211su-driver_1.0.0_amd64.deb
sudo ricoh-sp211su-setup
```

## Rebuilding from Source

If you modify the SANE backend driver (`drivers/sane-ricoh-sp211su/`):

```bash
cd packages/ricoh-sp211su
./build.sh
```

This compiles `libsane-ricoh_sp211su.so.1.0.0` and regenerates `ricoh-sp211su-driver_1.0.0_amd64.deb`.

## Verification Commands

- **Check Printer**:
  ```bash
  lpstat -p Ricoh_SP211SU_Local -d
  lp -d Ricoh_SP211SU_Local /path/to/test.pdf
  ```

- **Check Scanner**:
  ```bash
  scanimage -L
  scanimage --device ricoh_sp211su:sp211su --format=png --output=test.png
  ```

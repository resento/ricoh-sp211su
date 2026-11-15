# Ricoh Aficio SP 211SU Multi-Function Linux Driver & Package

Complete, standalone driver and automated installer for the **Ricoh Aficio SP 211SU** (USB ID `05ca:044f`) on Linux (Debian / Ubuntu x86_64).

Bundles both printing (CUPS DDST filter & PPD) and scanning (reverse-engineered SANE backend) into a single `.deb` package and 1-click installer.

---

## Quick Installation

On any Ubuntu/Debian installation:

```bash
git clone https://github.com/resento/ricoh-sp211su.git
cd ricoh-sp211su
sudo ./install.sh
```

Or install the pre-built `.deb` directly:

```bash
sudo dpkg -i packages/ricoh-sp211su/ricoh-sp211su-driver_1.0.0_amd64.deb
sudo ricoh-sp211su-setup
```

The installer automatically:
1. Installs the CUPS raster filter (`rastertolilo`) and PPD.
2. Installs the custom SANE backend (`libsane-ricoh_sp211su.so.1.0.0`) in `/etc/sane.d/dll.d/ricoh-sp211su` without modifying or conflicting with system `libsane1`.
3. Installs udev rules granting standard user access (`MODE="0666"`).
4. Auto-detects the USB printer queue and sets `Ricoh_SP211SU_Local` as default.

---

## Directory Structure

```
driver_ricoh_sp211su/
├── install.sh                  # 1-click root installer
├── build.sh                    # Rebuild driver from C source & regenerate .deb
├── README.md                   # This documentation
├── drivers/
│   └── sane-ricoh-sp211su/     # Reverse-engineered SANE C backend source
│       ├── Makefile
│       └── ricoh_sp211su.c
└── packages/
    └── ricoh-sp211su/          # Debian package tree and prebuilt .deb
        ├── build.sh
        ├── install.sh
        ├── ricoh-sp211su-driver_1.0.0_amd64.deb
        └── debian/
            ├── DEBIAN/         # control, postinst, prerm, postrm
            ├── etc/            # SANE dll.d & udev rules
            └── usr/            # CUPS filter, PPD, sane lib, setup helper
```

---

## Rebuilding from Source

If you make modifications to the C backend in `drivers/sane-ricoh-sp211su/`:

```bash
./build.sh
```

---

## Verification

- **Check Printer**:
  ```bash
  lpstat -p Ricoh_SP211SU_Local -d
  lp -d Ricoh_SP211SU_Local document.pdf
  ```

- **Check Scanner**:
  ```bash
  scanimage -L
  scanimage -d "ricoh_sp211su:sp211su" --resolution 150 --mode Color --format=png -o test.png
  ```
  Or open **Document Scanner (`simple-scan`)**.


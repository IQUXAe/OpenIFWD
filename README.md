# OpenIFWD

Open-source firmware flasher and RAM boot tool for Samsung Star 3 (GT-S5222) and Intel/Infineon XMM2150 (X-Gold 215 / PMB8815) baseband devices.

Replaces the proprietary Windows FlashTool E2 utility and vendor EBL binaries with a clean C11 implementation and a lightweight bare-metal Rust Primary Stage Initializer (PSI).

---

## Key Features

- **Built-in Open Rust PSI (Default)**:
  - Embeds a tiny 4 KB bare-metal Rust loader (`psi_open.bin`).
  - Completely eliminates dependency on the proprietary 600 KB Samsung/Infineon `ebl.bin` loader.
  - Speaks the standard [OpenIFWD Wire Protocol](docs/OPENIFWD_PROTOCOL.md) (`'OFWD'` magic, 11-byte headers, CRC-16 integrity).
  - Custom PSI binaries can still be supplied using `--psi <path>`.

- **Safe NVRAM / EEPROM Backup (`dump-nvram`)**:
  - Dumps the 1 MB NVRAM partition (OneNAND blocks 4..11, `0x60080000`–`0x60180000`) containing device IMEI, board calibrations, and RF tuning data.
  - Hardware write protection in the Open PSI prevents accidental erasure or corruption of these blocks.

- **Direct RAM Booting (`ram-boot`)**:
  - Streams bare-metal code directly into Mobile DDR SDRAM (default `0x60000000`) and jumps to it.
  - Enables kernel/OS development without touching or wearing out flash memory.

- **Stock Firmware Flashing (`flash`)**:
  - Supports writing stock Samsung `.fls` containers (`_BSY.fls`, `_CDS.fls`) and filesystem dumps (`.dffs`).
  - Supports full OneNAND erase or selective partition writes.

- **Transports**:
  - Direct USB Bulk transfer via `libusb-1.0` (`VID: 058b`, `PID: 0041`).
  - Serial TTY (`/dev/ttyACM*`, `/dev/ttyUSB*`).

- **Interfaces**:
  - Fast, scriptable CLI: `openifwd`.
  - Desktop GTK 3 GUI: `openifwd-gui` (with port scanner, progress bar, NVRAM backup, and RAM boot controls).

---

## Dependencies & Building

### Dependencies

- **Arch Linux**:
  ```bash
  sudo pacman -S base-devel pkgconf libusb gtk3 pixman
  ```

- **Debian / Ubuntu**:
  ```bash
  sudo apt install build-essential pkg-config libusb-1.0-0-dev libgtk-3-dev libpixman-1-dev
  ```

### Build

```bash
make clean && make -j$(nproc)
```

Run test suite:
```bash
make test
```

This compiles:
- `openifwd`: Command-line tool.
- `openifwd-gui`: GTK 3 desktop application.
- `test_runner`: Unit test runner.

---

## Usage Examples

### 1. Connecting the Phone
1. Disconnect the USB cable, power off the phone, and remove the SIM card and battery.
2. Run your `openifwd` command (it will start polling for the device).
3. Connect the micro-USB cable to the phone while holding **Volume Down**, or insert the battery ~1 second after connecting the cable.
4. The silicon Boot ROM enumerates as `058b:0041` and begins handshaking.

### 2. Backup NVRAM (IMEI & RF Calibrations)
```bash
# Dump 1 MB NVRAM to file using direct USB:
./openifwd dump-nvram -p usb -o s5222_nvram_backup.bin
```

### 3. RAM Boot (Direct SDRAM Execution)
```bash
# Load a bare-metal kernel binary to SDRAM and execute:
./openifwd ram-boot -p usb --bin my_kernel.bin --addr 0x60000000
```

### 4. Flash Stock Firmware
```bash
# Using the built-in Open PSI (no stock PSI/SLB needed):
./openifwd flash -p usb \
    --bsy S5222XXMJ1_BSY.fls \
    --cds S5222XXMJ1_CDS.fls \
    --dffs S5222OXEMJ1_CSC.dffs

# Using stock vendor PSI/SLB containers if desired:
./openifwd flash -p usb \
    --psi S5222XXMJ1_psi.fls \
    --slb S5222XXMJ1_slb.fls \
    --bsy S5222XXMJ1_BSY.fls \
    --cds S5222XXMJ1_CDS.fls \
    --erase-all
```

### 5. Inspect or Unpack FLS Files
```bash
# Inspect container chunks:
./openifwd info S5222XXMJ1_BSY.fls

# Unpack all payload chunks to directory:
./openifwd unpack S5222XXMJ1_BSY.fls ./unpacked/
```

### 6. GTK 3 GUI
```bash
./openifwd-gui
```

---

## Documentation

- [OpenIFWD Protocol Specification (v1.0)](docs/OPENIFWD_PROTOCOL.md): Packet wire format, framing, and command codes.

---

## License

GPL-3.0-or-later. See [LICENSE](LICENSE) for details.

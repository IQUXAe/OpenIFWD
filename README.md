# OpenIFWD

**OpenIFWD** is an open-source Linux alternative to the proprietary Windows utility *FlashTool E2 (Infineon IFWD)*. Written in clean C (C11), it allows flashing and booting firmware on mobile devices powered by **Infineon X-GOLD / XMM** baseband platforms (such as the **Samsung Star 3 / GT-S5222**, Champ, Rex, and related featurephones and smartphones).

---

## Features

- **FLS Container Engine**:
  - Full support for Infineon `.fls` (TLV Tag-Length-Value) files.
  - Parses and extracts all core chunks: Tag 1000 (Header), Tag 11 (PSI), Tag 14 (SLB/EBL), Tag 12 (Main Code / Flash), Tag 16 (TOC), Tag 18 (NVM), and Tag 21.
  - Built-in chunk unpacker (`openifwd unpack <file.fls>`).
- **IFWD Protocol Implementation**:
  - Reverse-engineered Infineon IFWD packet framing (`SOT=0x0200`).
  - Implements XOR-16 and CRC-16 CCITT checksum verification.
  - Automated `ATAT` synchronization with DTR/RTS serial handshaking.
  - Dynamic baud rate switching (up to 921600 bps).
- **Dual Transport Layer**:
  - Standard Linux serial TTY (`/dev/ttyACM*`, `/dev/ttyUSB*`) via POSIX `termios`.
  - Direct USB transport via `libusb-1.0` (bulk endpoints).
- **RAM Booting**:
  - Ability to inject and execute code directly in **SDRAM** (`ram-boot`) without writing or erasing flash memory.
- **Full Flash Programming**:
  - Flash ID detection, full/selective erase, block-by-block download, CRC verification, and hardware reset.
- **Two User Interfaces**:
  - Scriptable command-line interface (`openifwd`).
  - Modern GTK 3 desktop GUI (`openifwd-gui`) with real-time log, progress bar, port scanner, and Wayland compatibility.

---

## Dependencies & Requirements

To build OpenIFWD on Debian/Ubuntu/Arch/Fedora:

### Debian / Ubuntu / Linux Mint
```bash
sudo apt update
sudo apt install build-essential pkg-config libusb-1.0-0-dev libgtk-3-dev libpixman-1-dev
```

### Arch Linux / Manjaro
```bash
sudo pacman -S base-devel pkgconf libusb gtk3 pixman
```

---

## Building

Clone and build both CLI and GUI tools:
```bash
make clean && make
```

Run test suite:
```bash
make test
```

This produces two binaries:
- `openifwd`: Command-line interface.
- `openifwd-gui`: GTK 3 desktop graphical interface.

---

## Usage

### 1. Graphical Interface (GUI)
Launch the GUI application:
```bash
./openifwd-gui
```
- Click **"Refresh Ports"** to detect connected phone ports (`/dev/ttyACM*`, `/dev/ttyUSB*`, or direct USB).
- Select your firmware files (`_psi.fls`, `_slb.fls`, `_BSY.fls`, `_CDS.fls`, `*.dffs`) using the **Browse...** buttons.
- Choose whether to erase the flash or use **"RAM Boot only"**.
- Press **"Start Download"**.

---

### 2. Command-Line Interface (CLI)

#### Inspect an FLS firmware file:
```bash
./openifwd info S5222XXMJ1_BSY.fls
```

#### Unpack chunks from an FLS container:
```bash
./openifwd unpack S5222XXMJ1_BSY.fls ./extracted_chunks
```

#### Scan for connected phone ports:
```bash
./openifwd scan
```

#### RAM Boot (Execute in SDRAM without touching flash):
```bash
./openifwd ram-boot -p /dev/ttyACM0 \
    --psi S5222XXMJ1_psi.fls \
    --bin custom_payload.bin \
    --addr 0x20000000
```

#### Full Flash (Samsung Star 3 / GT-S5222):
```bash
./openifwd flash -p /dev/ttyACM0 \
    --psi S5222XXMJ1_psi.fls \
    --slb S5222XXMJ1_slb.fls \
    --bsy S5222XXMJ1_BSY.fls \
    --cds S5222XXMJ1_CDS.fls \
    --dffs S5222OXEMJ1_CSC.dffs \
    --erase-all
```

---

## Connecting the Phone (GT-S5222 / Infineon)

1. Turn off the phone and remove SIM cards and MicroSD.
2. Connect standard micro-USB cable to your PC.
3. Start the flashing command in OpenIFWD (it will begin sending `ATAT` sync signals).
4. Connect the cable to the phone:
   - Either while holding **Volume Down**;
   - Or connect the cable with battery removed, and insert the battery 1–2 seconds later.
5. The bootloader will respond, load PSI and SLB into RAM, and proceed with the selected operation.

---

## License

This project is licensed under the GNU General Public License v3.0 (GPLv3) - see the [LICENSE](LICENSE) file for details.

# openIFWD

Low-level flashing and diagnostic utility for Infineon PMB8815 (ARMv5TEJ / ARM926EJ-S, X-Gold 215 / XMM2150 platform) and Samsung OneNAND flash memory (KFM1G16Q2C) on the Samsung GT-S5222 (Star 3 Duos).

Communicates under Linux via direct `libusb-1.0` calls (USB bulk endpoints of Boot ROM and PSI: `058b:0041`) or serial interface (UART / TTY).

---

## Architecture and Execution Flow

1. **Boot ROM Handshake (USB or UART):**
   - **USB Mode (`-p usb`):** When powered on with the Volume Down key held, the on-chip mask Boot ROM (`0x00400000`, 32 KB) enters USB download mode (`VID: 058b`, `PID: 0041`). The host synchronizes using an `ATAT` ping-pong handshake and uploads the Primary Stage Initializer (`psi_ram`) to internal SRAM (`0x00080000`).
   - **UART Mode (`-p /dev/ttyUSB*` / `-p /dev/ttyACM*`, default `-b 115200`):** The silicon Boot ROM polls the serial console (USIF0, 115200 8N1) during the first 20–50 ms after power-on (10 polling attempts). The host continuously transmits `"AT"` (`0x41 0x54`) bursts with 5 ms intervals. Upon matching, Boot ROM responds with a 12-byte telemetry frame (`0xF0 0x0A [telemetry] 0xFF`), reporting Chip ID, silicon revision, SCU configuration, PLL status, and reset cause.

2. **In-RAM Flasher Agent (`open_flasher`):**
   Once PSI initializes system clocks and Mobile DDR SDRAM, the host uploads the bare-metal `open_flasher` payload directly to SDRAM (`0x60000000`, entry point `0x60000400`). The agent takes control, initializes the OneNAND controller (`0x40000000`), and transmits an initialization beacon (`RESP` / `PONG`) to Bulk IN endpoint `EP 0x83`.

3. **OFLS Wire Protocol (OneNAND Flash Loader Service):**
   All memory and flash operations are executed by the in-RAM agent using a synchronous binary protocol:
   - Command packet: 16 bytes with magic signature `OFLS` (`0x534C464F`).
   - Response packet: 16 bytes with magic signature `RESP` (`0x50534552`), error code, byte transfer counter, and status register/CRC fields.

---

## OneNAND Physical Partition Map (Samsung GT-S5222 / 128 MB)

Block size: 131,072 bytes (128 KB = 64 pages of 2048 bytes). Total: 1024 blocks.

| Blocks | OneNAND Offset | Size | Function |
|---|---|---|---|
| 0 | `0x00000000` | 128 KB | BSR: Primary Stage Initializer (`psi_flash`) |
| 1 | `0x00020000` | 128 KB | Reserved / Boot Configuration |
| 2..4 | `0x00040000` | 384 KB | SLB (Second Level Bootloader / EBL) |
| 5..9 | `0x000A0000` | 640 KB | Bootloader partition reserve |
| 10..331 | `0x00140000` | 41.25 MB | CODE / BSY (system OS image) |
| 332..339 | `0x02980000` | 1 MB | CDS / CSC (customer data specifications and region) |
| 340..977 | `0x02A80000` | 79.75 MB | IFS / DFFS (user filesystem partition) |
| 978..985 | `0x07A40000` | 1 MB | `DYN_EEP` (NVRAM: RF calibrations, IMEI 1/2) |
| 986..997 | `0x07B40000` | 1.5 MB | `STATE_EEP` (system calibrations) |
| 998..1023 | `0x07CC0000` | 3.25 MB | XSR partition tables and metadata |

> **NVRAM Protection:** Blocks 978..1023 are hardware-protected in software by `openIFWD`. Erase and write operations targeting this address range are blocked by both the host and the flasher agent (`IFWD_ERR_NVRAM_PROTECTED` / `0xEE01`) unless overridden with `--force-dangerous-erase`.

---

## Capabilities

### 1. Firmware Flashing (`flash`)
- **FLS Container Parsing:** Extracts raw executable payloads from Tag 12 (`IMAGE_DATA`) chunks by stripping vendor 28-byte chunk subheaders.
- **Block 0 (`psi_flash`) Programming:** Automatically extracts Tag 12 from `psi.fls` and writes it to Block 0 when specified or via `--flash-psi` / `--bsr`.
- **Multi-Block SLB Flashing:** Computes flash geometry for 283,700-byte SLB payload (spanning 3 physical blocks: 2, 3, and 4; 139 pages), erases the target block range, streams pages sequentially, and pads the 139th page with `0xFF`.
- **Component Flashing:** Flashes system components: BSY (`--bsy`), CDS (`--cds`), and DFFS/CSC (`--dffs`).
- **Automatic File Discovery:** Automatically detects companion firmware files within the same directory unless disabled with `--no-auto`.
- **PSI Integrity Check:** Verifies PSI binary SHA-256 against known Samsung GT-S5222 stock hashes, with optional bypass flag (`--skip-psi-sha256`).

### 2. Direct OneNAND Operations (`flasher-*`)
- `flasher-id`: Reads and displays Manufacturer ID (`0x00EC`) and Device ID (`0x0055`).
- `flasher-erase`: Erases a specified range of physical blocks. On failure, decodes and prints hardware status registers:
  - `CTRL_STATUS (0x1E480)`: Erase failure, timeout, or write protection violation.
  - `WP_STATUS (0x1E49C)`: Hardware write protection bits (`US`, `LS`, `LTS`).
  - `INT_STATUS (0x1E482)`: Controller interrupt status flags.
- `flasher-write`:
  - **Streaming Mode** (file size > 2048 bytes or `--page` omitted): Computes required block count, erases target blocks, streams 2048-byte pages with progress display, and pads final page with `0xFF`.
  - **Targeted Page Mode** (`--page X` with file size $\le$ 2048 bytes): Writes directly to a single 2048-byte page without erasing the block.
- `flasher-read`: Reads a 2048-byte page to a binary file (`-o`) or prints formatted hex output to terminal (`--hex` / `-x`) with line limiting (`--lines N`, `--all`). Decodes OneNAND hardware ECC status.
- `flasher-reset`: Performs hardware reboot via DWC2 controller.
- `flasher-boot`: Uploads and executes `open_flasher` in RAM without touching flash memory.

### 3. Memory and Firmware Dumps (`dump-*`)
- `dump-nvram`: Dumps the 1 MB calibration and IMEI area (OneNAND blocks 978..985 / `DYN_EEP`, `0x07A40000`–`0x07B3FFFF`) to a file.
- `dump-bootrom`: Dumps the 32 KB on-chip mask Boot ROM from physical address `0x00400000`.
- `dump-nand`: Dumps a specified range of OneNAND physical blocks to a binary file.
- `dump-memory`: Dumps an arbitrary physical memory address range over USB.

### 4. Container Inspection (`info`, `unpack`)
- `info`: Displays FLS container headers, TOC entries, chunk tags, memory classes, and payload offsets.
- `unpack`: Extracts raw payloads from all chunks within an FLS file into a target directory.

### 5. Graphical Interface (`openifwd-gui`)
- Built on GTK+ 3.
- Automatic root privilege escalation via Polkit (`pkexec`) when launched by an unprivileged user (preserving Wayland and X11 display environment), with fallback to `sudo -E` and `su`.
- Real-time USB polling, partition file selection, flashing progress indicators, and one-click NVRAM/Boot ROM extraction.

---

## Build and Dependencies

### Dependencies

**Arch Linux:**
```bash
sudo pacman -S base-devel pkgconf libusb gtk3
```

**Debian / Ubuntu:**
```bash
sudo apt install build-essential pkg-config libusb-1.0-0-dev libgtk-3-dev
```

### Compilation

```bash
make clean && make -j$(nproc)
```

Generated binaries:
- `openifwd` — CLI utility.
- `openifwd-gui` — GTK+ 3 GUI.
- `test_runner` — Unit test suite.

Run test suite:
```bash
make test
```

---

## Usage Examples

### 1. Device Connection
- **USB Connection (`-p usb`):**
  1. Disconnect the USB cable, remove battery and SIM card.
  2. Launch `openifwd` command (it waits for the USB device).
  3. Hold **Volume Down**, insert the micro-USB cable, then reinsert the battery. The Boot ROM registers as `058b:0041`.
- **UART Connection (`-p /dev/ttyUSB0` / `-b 115200`):**
  1. Connect a 3.3V USB-UART adapter (TX/RX) to USIF0 (or micro-USB with JIG resistor 523 kΩ between ID and GND).
  2. Launch `openifwd` with `-p /dev/ttyUSB0` (it continuously broadcasts `"AT"` probe bursts).
  3. Power on the phone or insert the battery. The silicon Boot ROM intercepts the `"AT"` sequence, completes the handshake, prints chip telemetry, and enters download mode.

### 2. Full Firmware Flash
```bash
# Over USB:
sudo ./openifwd flash -p usb \
    --psi S5222XXMJ1_psi.fls \
    --fls S5222XXMJ1_BSY.fls

# Over UART (115200 baud):
./openifwd flash -p /dev/ttyUSB0 -b 115200 \
    --psi S5222XXMJ1_psi.fls \
    --fls S5222XXMJ1_BSY.fls
```
*(Companion files `SLB`, `CDS`, and `CSC` in the same directory are detected and queued automatically).*

### 3. Flashing Selected Partitions
```bash
sudo ./openifwd flash -p usb \
    --psi S5222XXMJ1_psi.fls \
    --slb S5222XXMJ1_slb.fls \
    --bsy S5222XXMJ1_BSY.fls \
    --cds S5222XXMJ1_CDS.fls \
    --dffs S5222OXEMJ1_CSC.dffs \
    --no-auto
```

### 4. Flashing Primary Bootloader to Block 0
```bash
sudo ./openifwd flash -p usb --psi S5222XXMJ1_psi.fls --flash-psi
```

### 5. Multi-Block Binary Dump Writing
```bash
# Erases Block 0 and writes the entire binary file across necessary pages:
sudo ./openifwd flasher-write -p usb -b 0 -i dump_block0.bin
```

### 6. Single-Page Writing
```bash
# Writes exactly 2048 bytes to page 5 of block 10 without erasing:
sudo ./openifwd flasher-write -p usb -b 10 --page 5 -i page_patch.bin
```

### 7. Reading a OneNAND Page with Hex Output
```bash
# Display first 8 lines (128 bytes) of page 0, block 2:
sudo ./openifwd flasher-read -p usb -b 2 --page 0 --hex

# Display full 2048-byte page:
sudo ./openifwd flasher-read -p usb -b 2 --page 0 --hex --all
```

### 8. Memory and Partition Dumps
```bash
# Dump NVRAM (RF calibrations and IMEI, OneNAND blocks 978..985, 1 MB):
sudo ./openifwd dump-nvram -p usb -o nvram_backup.bin

# Dump 32 KB hardware mask Boot ROM:
sudo ./openifwd dump-bootrom -p usb -o bootrom_pmb8815_32k.bin

# Dump first 10 blocks of OneNAND:
sudo ./openifwd dump-nand -p usb -b 0 -c 10 -o nand_blocks_0_9.bin
```

### 9. Inspect FLS Container
```bash
./openifwd info S5222XXMJ1_psi.fls
```

---

## License

GPL-3.0-or-later. See [LICENSE](LICENSE) for details.

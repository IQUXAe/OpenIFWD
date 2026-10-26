# OpenIFWD Wire Protocol Specification (v1.0)

## 1. Overview

**OpenIFWD** is an open, deterministic binary wire protocol designed to replace the proprietary Infineon IFWD / FlashTool E2 protocol on mobile SoCs (specifically targeting the Intel/Infineon **XMM2150 / X-GOLD 215** platform, e.g. Samsung GT-S5222, and adaptable to other basebands).

Unlike the legacy vendor protocol, OpenIFWD:
- Does **not** require a 600 KB proprietary EBL RTOS binary.
- Does **not** mandate arbitrary 16,392-byte padded bulk buffers.
- Does **not** rely on reverse-engineered in-memory patching of signature checks.
- Uses clean, variable-length packet framing with CRC-16 data integrity checks.
- Runs over both USB CDC / Bulk endpoints and asynchronous serial (UART).

```
 +---------------------------------------------------------+
 |                      Host (PC CLI / GUI)                |
 +---------------------------------------------------------+
                              |
                   [Boot ROM 0x30 Loader]
                              v
 +---------------------------------------------------------+
 |             Target RAM: OpenIFWD PSI Agent              |
 |              (ARM926EJ-S @ SRAM 0x00080000)             |
 +---------------------------------------------------------+
        |                     |                     |
 [SCU / Clocks]       [SDRAM Controller]    [OneNAND MMIO]
   208 MHz PLL            64 MB LPDDR          128 MB Flash
```

---

## 2. Bootstrapping Flow (Handshake & Injection)

The bootstrapping sequence from device power-on proceeds in two stages:

### Stage 1: Silicon Boot ROM Injection
1. Phone is connected in download mode (Volume Down held, or dead flash fallback).
2. Device enumerates on USB as `VID:058b PID:0041` (Infineon Flashloader).
3. Host sends autobaud sync probe: `"AT"` (or `"ATAT"`).
4. Boot ROM responds with sync token `0xF0` followed by chip ID (`0x17` for XMM2150).
5. Host sends 3-byte Boot ROM header:
   ```
   [0x30] [size_low] [size_high]
   ```
6. Host streams the PSI agent binary (up to 64 KB).
7. Host sends 1-byte XOR-8 checksum of the payload.
8. Boot ROM responds with ACK (`0x01`) and jumps to entry point at `0x00080000` in internal SRAM.

### Stage 2: OpenIFWD Session Handshake
1. Once executing in SRAM, the OpenIFWD PSI agent sets up CPU clocks, initializes SDRAM, and exposes the OpenIFWD protocol over USB or UART.
2. Host issues `CMD_PING` (`0x01`).
3. PSI agent replies with `RSP_PONG` (`0x81`), indicating target readiness and protocol capabilities.
4. Normal flash / memory operations can now be performed.

---

## 3. Packet Framing & Wire Format

Every OpenIFWD packet (both Host Request and Target Response) follows this layout:

### Packet Header (8 bytes)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       Magic: 'O' 'F'          |       Magic: 'W' 'D'          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|    Version    |  Cmd / Resp   |          Sequence ID          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         Payload Length        |       Header Checksum         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|                   Payload Data (0..4096 bytes)                |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   Payload CRC-16 (2 bytes)                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### Field Definitions

| Field | Size | Description |
|---|---|---|
| `Magic` | 4 bytes | Fixed preamble: `0x4F, 0x46, 0x57, 0x44` (ASCII `"OFWD"`) |
| `Version` | 1 byte | Protocol version (`0x01` for this standard) |
| `Cmd / Resp` | 1 byte | Command code (Host: `0x01..0x7F`) or Response code (Target: `0x80..0xFF`) |
| `Sequence ID` | 2 bytes | 16-bit monotonic counter, incremented per transaction (little-endian) |
| `Payload Length` | 2 bytes | Length of following payload in bytes (`0..4096`, little-endian) |
| `Header Checksum` | 1 byte | XOR-8 of bytes 0..6 (verifies header integrity before reading payload) |
| `Payload Data` | N bytes | Command-specific argument data or response body |
| `Payload CRC-16` | 2 bytes | CRC-16-CCITT (poly `0x1021`, init `0xFFFF`) calculated over Payload Data |

> If `Payload Length == 0`, the trailing `Payload CRC-16` is omitted.

---

## 4. Command Matrix

### 4.1. Overview

| Code | Command Name | Description |
|---|---|---|
| `0x01` | `CMD_PING` | Connectivity probe and heartbeat |
| `0x02` | `CMD_GET_HW_INFO` | Query hardware IDs, RAM size, and Flash geometry |
| `0x03` | `CMD_ERASE_BLOCK` | Erase one 128 KB OneNAND block |
| `0x04` | `CMD_WRITE_PAGE` | Write 2048-byte page (+ optional 64-byte spare/OOB) |
| `0x05` | `CMD_READ_PAGE` | Read 2048-byte page (+ optional 64-byte spare/OOB) |
| `0x06` | `CMD_MEM_WRITE` | Stream binary data directly to an address in SDRAM |
| `0x07` | `CMD_MEM_READ` | Read raw bytes from SDRAM or memory-mapped space |
| `0x08` | `CMD_EXEC_RAM` | Flush caches and execute code at specified RAM address |
| `0x09` | `CMD_REBOOT` | Trigger hardware SoC reboot via SCU watchdog |

### 4.2. Response Codes

| Code | Response Name | Meaning |
|---|---|---|
| `0x80` | `RSP_OK` | Operation completed successfully |
| `0x81` | `RSP_PONG` | Ping reply with target status word |
| `0x82` | `RSP_HW_INFO` | Hardware geometry payload returned |
| `0x85` | `RSP_PAGE_DATA` | Page data read from flash |
| `0x87` | `RSP_MEM_DATA` | Memory data read from RAM |
| `0xE0` | `ERR_GENERIC` | Unspecified error |
| `0xE1` | `ERR_BAD_CRC` | Payload CRC-16 mismatch |
| `0xE2` | `ERR_UNSUPPORTED` | Unknown command code |
| `0xE3` | `ERR_FLASH_LOCKED` | OneNAND block is write-protected |
| `0xE4` | `ERR_FLASH_IO` | Hardware flash controller timeout / ECC error |
| `0xE5` | `ERR_NVRAM_PROTECT` | Address falls within protected calibration range |
| `0xE6` | `ERR_BAD_ADDR` | Invalid memory alignment or out of bounds |

---

## 5. Detailed Command Specifications

### 5.1. `CMD_PING` (`0x01`)
- **Payload:** None (`Length = 0`).
- **Response:** `RSP_PONG` (`0x81`), Payload: 4 bytes:
  - `uint32_t status_flags`: bit 0 = SDRAM ready, bit 1 = OneNAND ready.

---

### 5.2. `CMD_GET_HW_INFO` (`0x02`)
- **Payload:** None.
- **Response:** `RSP_HW_INFO` (`0x82`), Payload (32 bytes):
  ```c
  struct hw_info_payload {
      uint32_t chip_id;         /* 0x00000017 (X-Gold 215) */
      uint32_t cpu_freq_hz;     /* 208000000 (208 MHz) */
      uint32_t sdram_base;      /* 0x60000000 */
      uint32_t sdram_size;      /* 0x04000000 (64 MB) */
      uint16_t onenand_mfr_id;  /* 0x00EC (Samsung) */
      uint16_t onenand_dev_id;  /* 0x0030 (1 Gb / 128 MB) */
      uint32_t block_size;      /* 131072 (128 KB) */
      uint16_t page_size;       /* 2048 bytes */
      uint16_t spare_size;      /* 64 bytes */
      uint32_t total_blocks;    /* 1024 blocks */
  };
  ```

---

### 5.3. `CMD_ERASE_BLOCK` (`0x03`)
- **Payload (4 bytes):**
  - `uint32_t block_index`: Index of block to erase (`0..1023`).
- **Safety Rule:**
  If `block_index` maps to the protected NVRAM/DYN_EEP range (blocks 4..11), the agent **must reject** the command with `ERR_NVRAM_PROTECT` (`0xE5`) unless the host has explicitly unlocked safe mode.
- **Response:** `RSP_OK` (`0x80`) on success, or error code.

---

### 5.4. `CMD_WRITE_PAGE` (`0x04`)
- **Payload:**
  - `uint32_t block_index`: Target block (`0..1023`).
  - `uint16_t page_index`: Target page (`0..63`).
  - `uint16_t flags`: bit 0 = 1 includes 64-byte spare/OOB data.
  - `uint8_t data[2048]` (or `2112` if spare is included).
- **Behavior:**
  1. Copies data to OneNAND BufferRAM 0.
  2. Issues `PROGRAM_PAGE` command to OneNAND controller.
  3. Polls Controller Ready interrupt bit.
  4. Returns `RSP_OK` or `ERR_FLASH_IO`.

---

### 5.5. `CMD_READ_PAGE` (`0x05`) — Used for NVRAM Backup
- **Payload (8 bytes):**
  - `uint32_t block_index`: Source block (`0..1023`).
  - `uint16_t page_index`: Source page (`0..63`).
  - `uint16_t flags`: bit 0 = 1 includes 64-byte spare/OOB data.
- **Response:** `RSP_PAGE_DATA` (`0x85`), Payload: 2048 bytes (or 2112 bytes).

#### NVRAM Backup Recipe:
NVRAM is located at blocks 4 through 11 (8 blocks = 1 MB = 512 pages).
The host simply calls `CMD_READ_PAGE` in a loop from `block = 4, page = 0` up to `block = 11, page = 63`, writing each page to disk. This gives a 100% bit-exact calibration backup in seconds without vendor EBL dependencies.

---

### 5.6. `CMD_MEM_WRITE` (`0x06`) & `CMD_EXEC_RAM` (`0x08`) — RAM Booting
To boot a kernel, U-Boot, or test OS directly in RAM without flashing:
1. Host streams chunks of the binary to SDRAM:
   `CMD_MEM_WRITE(addr = 0x60000000 + offset, data = chunk)`
2. Once complete, host calls:
   `CMD_EXEC_RAM(entry_addr = 0x60000000, r0 = 0, r1 = mach_type, r2 = atags)`
3. The PSI agent:
   - Flushes instruction and data caches via CP15 (`CP15 c7`).
   - Disables MMU and interrupts.
   - Jumps directly to `entry_addr` in ARM Supervisor mode (`SVC32`).

---

## 6. Implementing Custom PSI Agents

Developers creating a custom PSI for another board or chipset only need to implement:
1. Boot ROM entry header (64 bytes matching SoC Boot ROM expectation).
2. Minimal serial/USB receiver.
3. OpenIFWD packet dispatcher implementing the commands in Section 5.

All higher-level logic (file unpacking, flash layout tables, NVRAM image validation) is handled on the host side by OpenIFWD.

# Infineon PMB8815 (X-Gold 215) Boot ROM Architecture Specification

> **Document Status:** Reference Architecture Manual / Technical Specification  
> **Target Processor:** Infineon Technologies PMB8815 (X-Gold 215 / S-Gold family)  
> **Core Architecture:** ARM926EJ-S (ARMv5TEJ Architecture, 32-bit RISC)  
> **Scope:** Hardware Memory Mapping, On-Chip Mask ROM Execution, Reset Flow, and Image Validation  

---

## 1. System Architecture Overview

The Infineon PMB8815 baseband processor incorporates an on-chip, read-only physical silicon mask Boot ROM. The Boot ROM serves as the Stage 0 primary bootloader executed by the ARM926EJ-S core immediately upon system reset, power-on, or hardware watchdog trigger.

### 1.1 Core Attributes & Execution Environment
* **Core:** ARM926EJ-S with Jazelle technology (ARMv5TEJ instruction set architecture, Little-Endian).
* **Caches & Memory Management:**
  * 16 KB Harvard Instruction Cache (I-Cache).
  * 16 KB Harvard Data Cache (D-Cache).
  * Memory Management Unit (MMU) with single-level and two-level page translation (ARMv5 format).
  * Separate Instruction TCM (ITCM) and Data TCM (DTCM) interfaces.
* **Boot State:**
  * Initial Mode: **Supervisor Mode (`SVC32`)**.
  * Core Register `CPSR`: Initialized to **`0x000000D3`** (ARM state, IRQ disabled, FIQ disabled, SVC32 mode).
  * High Vectors Bit (`SCTLR.V` in `CP15 c1`): Initially cleared to `0` (Exception vectors at `0x00000000`).

---

## 2. Memory Map & Interconnect Topology

The PMB8815 internal system interconnect utilizes a multi-layer Advanced High-performance Bus (AHB) matrix. The physical mask Boot ROM is permanently routed at a dedicated physical window, independent of system remap registers.

### 2.1 System Memory Map

| Physical Address Range       | Size    | Memory Type / Target                | Attributes / Access Rules                                        |
|------------------------------|---------|-------------------------------------|------------------------------------------------------------------|
| `0x00000000 - 0x0000FFFF`    | 64 KB   | Aliased / Mirrored Window           | Pre-remap: Mirrors Boot ROM. Post-remap: Mirrors On-Chip SRAM.  |
| `0x00080000 - 0x0009FFFF`    | 128 KB  | Internal On-Chip SRAM (SYSRAM)     | Read/Write, 0-wait-state DMA buffer and execution arena.         |
| `0x00400000 - 0x00407FFF`    | 32 KB   | **Physical Mask Boot ROM**          | **Read-Only physical window (Silicon ROM macro).**               |
| `0x00408000 - 0x004FFFFF`    | 992 KB  | **Unmapped Interconnect Hole**      | **Bus Abort (`HRESP = ERROR`). Triggers ARM Data Abort.**        |
| `0x40000000 - 0x4FFFFFFF`    | 256 MB  | Mobile SDRAM Bank 0                 | External system execution memory (DDR/SDR SDRAM).                |
| `0x60000000 - 0x67FFFFFF`    | 128 MB  | OneNAND Flash Controller Window     | Samsung KFM1G16Q2C OneNAND (Boot code, NVRAM, partitions).       |
| `0xE0000000 - 0xEFFFFFFF`    | 256 MB  | Memory-Mapped I/O (MMIO) Window     | Peripherals (USIF, ICU, DMA, USB DWC2, DCC Display).            |
| `0xFFFE0000 - 0xFFFE7FFF`    | 32 KB   | Data Tightly Coupled Memory (DTCM)  | Core-coupled zero-wait-state fast stack & context storage.       |
| `0xFFFE8000 - 0xFFFEFFFF`    | 32 KB   | Instruction TCM (ITCM)              | Fast interrupt service routines and high-speed execution.        |
| `0xFFFF0000 - 0xFFFFFFFF`    | 64 KB   | High Exception Vector Window        | Active when `SCTLR.V = 1` in `CP15 c1`. Hardwired to ITCM.       |

### 2.2 Physical Boot ROM Geometry
* **Silicon Macro Size:** Exactly **32,768 bytes (32 KB / `0x8000` bytes)**.
* **Physical Base Address:** `0x00400000`.
* **Physical End Address:** `0x00407FFF`.
* **Bus Termination (`0x00408000+`):** The internal AHB decoder generates an immediate hardware slave error response (`HRESP = ERROR`) when any read or write access targets `0x00408000` or above within the `0x00400000` block. On the ARM926EJ-S core, this triggers a synchronous **Data Abort** (`0x00000010` / `0xFFFF0010`), causing an unrecoverable system hang or watchdog reset.
* **REMAP Independence:** Address space `0x00400000` accesses the silicon ROM directly via the AHB matrix regardless of whether the internal SRAM remap register (`0xE4900088`) is set or cleared.

---

## 3. Exception Vectors & Reset Sequence

### 3.1 Mask ROM Vector Table
The physical vector table located at offset `0x0000` of the Boot ROM (`0x00400000`):

| Vector Offset | Exception Type         | Machine Code | Assembly Instruction            | Resolved Target |
|---------------|------------------------|--------------|---------------------------------|-----------------|
| `0x00000000`  | Reset                  | `0xE59FF018` | `ldr pc, [pc, #0x18]`           | `0x00400054`    |
| `0x00000004`  | Undefined Instruction  | `0xE59FF018` | `ldr pc, [pc, #0x18]`           | `0x0040003C`    |
| `0x00000008`  | Software Interrupt     | `0xE59FF018` | `ldr pc, [pc, #0x18]`           | `0x00400040`    |
| `0x0000000C`  | Prefetch Abort         | `0xE59FF018` | `ldr pc, [pc, #0x18]`           | `0x00400044`    |
| `0x00000010`  | Data Abort             | `0xE59FF018` | `ldr pc, [pc, #0x18]`           | `0x00400048`    |
| `0x00000014`  | Reserved               | `0xE320F000` | `nop`                           | —               |
| `0x00000018`  | Interrupt (IRQ)        | `0xE59FF014` | `ldr pc, [pc, #0x14]`           | `0x0040004C`    |
| `0x0000001C`  | Fast Interrupt (FIQ)   | `0xE59FF014` | `ldr pc, [pc, #0x14]`           | `0x00400050`    |

Literal Pool for Vectors (`0x00400020 - 0x0040003B`):
* `0x00400020`: `0x00400054` (Reset Handler Entry Point)
* `0x00400024`: `0x0040003C` (Default Trap: `b .`)
* `0x00400028`: `0x00400040` (Default Trap: `b .`)
* `0x0040002C`: `0x00400044` (Default Trap: `b .`)
* `0x00400030`: `0x00400048` (Default Trap: `b .`)
* `0x00400034`: `0x0040004C` (Default Trap: `b .`)
* `0x00400038`: `0x00400050` (Default Trap: `b .`)

### 3.2 Reset Execution Sequence (`0x00400054`)

Upon releasing the CPU reset line, execution proceeds through five deterministic phases:

```
+------------------------------------------------------------------+
| Phase 1: CPU Supervisor Mode Initialization                      |
|          msr cpsr_c, #0xD3 (SVC32, IRQ/FIQ masked)               |
+------------------------------------------------------------------+
                                |
                                v
+------------------------------------------------------------------+
| Phase 2: System Interconnect & MMIO Window Setup                 |
|          mcr p15, 0, r0, c15, c2, 4 (Val: 0xE0000013)           |
+------------------------------------------------------------------+
                                |
                                v
+------------------------------------------------------------------+
| Phase 3: Hardware Strap Evaluation & Peripheral Clocking         |
|          Read GPIO / Strap register (Mask: 0x20000000)           |
+------------------------------------------------------------------+
                                |
                                v
+------------------------------------------------------------------+
| Phase 4: TCM Interface Initialization                            |
|          CP15 c9, c1, 1 (ITCM: 0xFFFE8000)                       |
|          CP15 c9, c1, 3 (DTCM: 0xFFFE0000)                       |
+------------------------------------------------------------------+
                                |
                                v
+------------------------------------------------------------------+
| Phase 5: Transport Binding & Image Validation Engine             |
|          USIF UART / USB DWC2 enumeration -> liHU verification   |
+------------------------------------------------------------------+
```

#### Detailed Assembly Walkthrough:
1. **Mode Enforcement & Initial Stack:**
   ```arm
   0x00400054:  ldr  r0, [pc, #0x74]          ; Load stack base (literal 0x004000D0: 0x000AF7FC)
   0x00400058:  msr  cpsr_c, #0xD3            ; Force SVC32 mode with interrupts disabled
   0x0040005C:  mov  sp, r0                   ; Initialize temporary boot stack pointer
   ```
2. **Peripheral Window Configuration (`CP15 c15`):**
   ```arm
   0x00400060:  mov  r0, #0xE0000000          ; Peripheral MMIO base
   0x00400064:  orr  r0, r0, #0x13            ; Size & enable attributes
   0x00400068:  mcr  p15, 0, r0, c15, c2, 4   ; Write Secondary AHB Window register
   ```
3. **Hardware Boot Strap Evaluation:**
   ```arm
   0x0040006C:  ldr  r2, [pc, #0x60]          ; Load strap register pointer (0xE4900010)
   0x00400070:  ldr  r0, [r2]                 ; Read strap pins
   0x00400074:  and  r2, r0, #0x20000000      ; Test boot strap bit (USB Download vs NAND)
   0x00400078:  cmp  r2, #0x20000000
   0x0040007C:  bne  nand_boot_path           ; Branch if strap indicates Flash boot
   ```
4. **TCM Initialization (`CP15 c9`):**
   ```arm
   0x00400088:  mov  r0, #0
   0x0040008C:  mcr  p15, 0, r0, c9, c1, 2    ; Clear TCM control
   0x00400094:  mcr  p15, 0, r0, c9, c1, 5
   0x004000A0:  mcr  p15, 0, r0, c9, c1, 7
   0x004000AC:  ldr  r1, [pc, #0x28]          ; ITCM base literal (0xFFFE8019)
   0x004000B0:  mcr  p15, 0, r1, c9, c1, 1    ; Enable ITCM at 0xFFFE8000 (32 KB)
   0x004000B8:  ldr  r1, [pc, #0x20]          ; DTCM base literal (0xFFFE0019)
   0x004000BC:  mcr  p15, 0, r1, c9, c1, 3    ; Enable DTCM at 0xFFFE0000 (32 KB)
   ```

---

## 4. Boot ROM Image Validation Engine

When invoked in USB Download mode (e.g. `Volume Down` held on the Samsung GT-S5222), the Boot ROM activates the Synopsys DesignWare USB 2.0 OTG Controller (DWC2) in internal DMA mode.

### 4.1 Wire Protocol & Handshake
1. **Baud / Protocol Synchronization:** The host sends a zero-byte burst (`0x00`) or standard ping. The Boot ROM replies with the hardware ID announcement packet.
2. **Transfer Descriptor:** All downloads follow the Infineon Stage 1 framing protocol:
   * **Header Byte:** `0x30` (Download Request).
   * **Payload Size:** 2-byte little-endian length ($L$).
   * **Binary Stream:** $L$ bytes of stage 1 executable code (PSI).
   * **Checksum Byte:** 1-byte trailing XOR-8 parity value.

### 4.2 Image Container Verification (`liHU` & `CJKT`)

The Boot ROM incorporates an internal parser that inspects container tags embedded in uploaded stages:

```
0x00 +-------------------------------------------------------------------+
     | Branch Instruction (4 bytes): e.g. 0xEA002AD7 (Jump to payload)   |
0x04 +-------------------------------------------------------------------+
     | Magic Identifier (4 bytes): 0x5548696C ("liHU")                   |
0x08 +-------------------------------------------------------------------+
     | Container Metadata / Flags (36 bytes)                             |
0x30 +-------------------------------------------------------------------+
     | Load Address (4 bytes): e.g. 0x00080000 (Internal SRAM Base)      |
0x34 +-------------------------------------------------------------------+
     | Execution Address / Entry (4 bytes): e.g. 0x000811E8              |
0x38 +-------------------------------------------------------------------+
     | Memory Limit (4 bytes): e.g. 0x0008FFFF                           |
0x3C +-------------------------------------------------------------------+
     | Tag Identifier (4 bytes): 0x544B4A43 ("CJKT")                     |
     +-------------------------------------------------------------------+
```

#### Verification Rules:
1. **Magic Field:** The 32-bit word at offset `0x04` must strictly equal `0x5548696C` (ASCII string `"liHU"`).
2. **Boundary Enforcement:** The `Load Address` and `Memory Limit` must lie entirely within valid writable on-chip SRAM (`0x00080000 - 0x0009FFFF`) or configured SDRAM (`0x60000000+`). If the range overlaps Boot ROM (`0x00400000`) or unmapped areas, download is rejected.
3. **Tag Identifier:** Tag `0x544B4A43` (`"CJKT"`) designates a certified BootCore executable (PSI / Primary System Initialization).
4. **Transmission Parity:** After streaming the payload, the host must transmit the XOR-8 sum:
   $$\text{Checksum} = \bigoplus_{i=0}^{N-1} \text{Payload}[i]$$
   If computed sum does not match, the Boot ROM terminates USB transmission with `NAK` and resets.

---

## 5. Direct Silicon Extraction via openIFWD

Because the Boot ROM physical silicon window is permanently active at `0x00400000`, extraction does not require modifying runtime memory maps, hooking entry points, or clearing SCU remap bits.

### 5.1 Extraction Procedure
* **Utility:** `openIFWD` with standalone SDRAM payload `ram_dumper.bin` (executing at `0x60000400`).
* **Command:** `MAGIC_CMD_DUMP_BOOTROM` (`0x42524F4D` / `"BROM"`).
* **Chunk Geometry:** 16 sequential chunks of 2,048 bytes (total 32,768 bytes / 32 KB).
* **Memory Copy:** The dumper performs 32-bit aligned word copies directly from physical memory `0x00400000` to internal SRAM DMA buffer `0x0008E200`, followed by USB Bulk IN transmission on endpoint `0x83`.

### 5.2 Command Line Invocation
```bash
# Extract physical 32 KB Boot ROM to binary file
sudo ./openIFWD/openifwd dump-bootrom -p usb -o bootrom_pmb8815_32k.bin -v

# Parse, validate exception vectors, and verify strings
python3 tools/parse_bootrom_dump.py -i bootrom_pmb8815_32k.bin
```

---

## 6. Cryptographic Hashes & Reference Artifacts

Verified dump of the Samsung GT-S5222 / Infineon PMB8815 Mask Boot ROM:
* **File Size:** Exactly 32,768 bytes (32 KB).
* **MD5:** `5f3e30c4cbe50f6c6fbd328b14b6b8bc`
* **SHA-256:** `23d9901bf4e8909732228243ccecc7f0d3e96d06ff875faf2f589833fc6acf6f`
* **Identified Signatures:**
  * Magic: `liHU` (`0x5548696C`) at offset `0x0BC8`, `0x37B8`
  * Tag: `CJKT` (`0x544B4A43`) at offset `0x0BCC`
  * C++ Runtime Diagnostics: Exception descriptors located at `0x71C0 - 0x770B`

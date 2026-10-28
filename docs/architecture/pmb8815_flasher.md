# Infineon PMB8815 Bare-Metal Flasher & Hardware Architecture

Author:  IQUXAe <iquxae@proton.me>
Target:  Samsung GT-S5222 / Infineon PMB8815 (X-Gold 215 / S-Gold Family)
Core:    ARM926EJ-S rev 5 (ARMv5TEJ Architecture, 32-bit Little-Endian)
Status:  Hardware Verified / Reference Architecture Document


1. SoC Overview & Memory Map
============================

The Infineon PMB8815 (X-Gold 215) baseband SoC integrates an ARM926EJ-S
processor core running the ARMv5TEJ instruction set, connected via a 32-bit
multilevel AHB interconnect matrix to internal on-chip SRAM, an External Bus
Unit (EBU) controlling external Mobile SDRAM and OneNAND flash, and peripheral
subsystems including a Synopsys DWC2 USB High-Speed OTG controller.

The physical address space is partitioned across the AHB bus as follows:

Address Range              Size      Target / Description
-----------------------    -------   --------------------------------------------------
0x00000000 - 0x0000FFFF    64 KB     Low memory mirror (Boot ROM pre-remap, SRAM post)
0x00080000 - 0x0009FFFF    128 KB    Internal on-chip SRAM (SYSRAM; PSI execution base)
0x00400000 - 0x00407FFF    32 KB     Silicon Mask Boot ROM (Hardware Stage 0)
0x40000000 - 0x4001FFFF    128 KB    Samsung KFM1G16Q2C OneNAND Controller / DataRAM
0x60000000 - 0x60FFFFFF    16 MB     External Mobile SDRAM (Bank 1, Stage 2 execution)
0xE0000000 - 0xEFFFFFFF    256 MB    Memory-Mapped I/O (MMIO peripheral registers)
0xE4800000 - 0xE480FFFF    64 KB     Synopsys DWC2 USB OTG Controller
0xE4900000 - 0xE4900FFF    4 KB      System Control Unit (SCU / REMAP / WDT registers)
0xE8000000 - 0xE800FFFF    64 KB     External Bus Unit (EBU SDRAM & OneNAND control)
0xFFFE0000 - 0xFFFE7FFF    32 KB     Data TCM (DTCM)
0xFFFE8000 - 0xFFFEFFFF    32 KB     Instruction TCM (ITCM)
0xFFFF0000 - 0xFFFFFFFF    64 KB     High exception vectors (active when SCTLR.V = 1)


1.1. SDRAM Entry Offset Architecture (The 0x400 Requirement)
------------------------------------------------------------

When bootstrapping Stage 2 binaries (the proprietary EBL or the open-source
`open_flasher` agent), the Stage 1 primary loader (`psi_ram.bin`) executes in
on-chip SRAM at `0x00080000`.

Analysis of the decompiled PSI routine (`FUN_0008148c` / `FUN_000815c8` in
`psi_ram.bin`) reveals the physical handover mechanics:

```
  Host (openIFWD)                     PSI (0x00080000)                SDRAM (0x60000000)
        |                                    |                                |
        | --- 1. Size (uint32_le) ---------> |                                |
        | <--- 0xCC 0xCC (ACK) ------------- |                                |
        | --- 2. Stream Binary + Checksum -> |                                |
        |                                    | -- FUN_000815c8(0x60000000) -> | [DMA into 0x60000000]
        | <--- 0x17 0xA5 (Handover ACK) ---- |                                |
        |                                    | -- Branch to *0x00081524 ----> | [PC = 0x60000400]
```

1. PSI configures USB DMA destination memory strictly to base `0x60000000`:
   `FUN_000815c8(0x60000000);`
2. All received binary bytes are written starting at physical offset `0` of
   Mobile SDRAM (`0x60000000`).
3. Upon validating the 1-byte XOR-8 checksum, PSI branches to the entry point
   stored at literal `0x00081524`, which contains strictly `0x60000400`:
   `pcVar1 = *(code **)0x00081524; /* 0x60000400 */`
   `(*pcVar1)();`

CRITICAL REQUIREMENT:
Because PSI writes bytes from `0x60000000` but executes from `0x60000400`, any
standalone binary MUST be structured with a 1024-byte (0x400) header offset:

```
Physical SDRAM
Address       Offset  Contents
----------    ------  -------------------------------------------------------------
0x60000000    0x0000  Section .head: "b _entry" (Relative branch to 0x60000400)
0x60000004    0x0004  Zero-padding / Reserved (0x3FC bytes of 0x00)
0x60000400    0x0400  Section .text.entry (_entry): Real executable code starts here
```

If a binary is linked with `ORIGIN = 0x60000400` without the 0x400 `.head`
padding, PSI writes the binary starting at `0x60000000`, and then jumps to
`0x60000400`—landing directly into an arbitrary offset of the binary. This
triggers an immediate synchronous Data Abort or Undefined Instruction exception.

The linker script (`linker.ld`) MUST conform to:

```ld
MEMORY
{
    SDRAM (rwx) : ORIGIN = 0x60000000, LENGTH = 16M
}

SECTIONS
{
    . = 0x60000000;

    .head : {
        KEEP(*(.head))
        . = 0x400; /* Pad binary so .text.entry lands at 0x60000400 */
    } > SDRAM

    .text.entry 0x60000400 : {
        KEEP(*(.text.entry))
        *(.text .text.*)
    } > SDRAM
}
```


2. CPU State & Interrupt Handling
=================================

The ARM926EJ-S processor core operates in 32-bit ARM state.

```
CPSR (Current Program Status Register) Layout:
[31:28] Condition Flags (N, Z, C, V)
[7]     I: IRQ Disable bit (1 = Masked / Disabled, 0 = Enabled)
[6]     F: FIQ Disable bit (1 = Masked / Disabled, 0 = Enabled)
[5]     T: State bit (0 = ARM state, 1 = Thumb state)
[4:0]   M[4:0]: Processor Mode (10011 = 0x13 = Supervisor Mode / SVC32)
```

Values used in bootloaders:
- `0xD3` (`1101 0011`b): SVC32 mode, IRQ disabled (bit 7=1), FIQ disabled (bit 6=1).
- `0x53` (`0101 0011`b): SVC32 mode, IRQ enabled (bit 7=0), FIQ disabled (bit 6=1).


2.1. The Interrupt Deadlock Trap (0xD3 vs 0x53)
-----------------------------------------------

Stock PSI executes with High Vectors enabled (`SCTLR.V = 1`, vector base
`0xFFFF0000`). PSI registers its own interrupt service routines in High
Vectors, specifically:
- Line IRQ #9: USB Controller Hardware Interrupt (`USB_HAL_ISR`).

When `open_flasher` (or `ram_dumper`) performs USB transfers via PSI driver
functions (`psi_usb_send` at `0x00083234`, `psi_usb_recv` at `0x000831E8`):
1. The agent programs the transfer descriptor in SYSRAM (`PSI_PTR_DESC`).
2. The agent sets `desc[0x10] = 0xFFFFFFFF` (TX) or `desc[0x0F] = 0xFFFFFFFF` (RX).
3. The agent calls the PSI transfer function, which arms the DWC2 hardware registers.
4. When DMA completion occurs, the DWC2 core asserts hardware interrupt IRQ #9.
5. The CPU vectors to `0xFFFF0018`, invoking PSI's `USB_HAL_ISR`.
6. `USB_HAL_ISR` checks the hardware endpoint flags, acknowledges the interrupt,
   and writes `desc[0x10] = 0` (TX complete) or `desc[0x0F] = 0` (RX complete).
7. The agent polling loop `while (desc[0x10] != 0 && --timeout)` observes the
   zero value and proceeds.

WARNING:
If the agent startup code configures `msr cpsr_c, #0xD3` (IRQ masked):
The CPU physically blocks IRQ #9. `USB_HAL_ISR` never executes. Consequently,
`desc[0x10]` and `desc[0x0F]` remain `0xFFFFFFFF` forever. The agent spins in
its polling loop until timeout, or hangs indefinitely.

The entry point MUST set `msr cpsr_c, #0x53` to preserve PSI USB interrupt
dispatching.


2.2. ARM926 Cache Invalidation
------------------------------

PSI executes with the MMU and Data Cache disabled (`SCTLR.M = 0`, `SCTLR.C = 0`).
Executing a Test-and-Clean D-Cache loop (`mrc p15, 0, pc, c7, c14, 3; bne 1b`)
when the data cache is disabled causes unpredictable behavior or infinite loops
on certain ARM926 silicon revisions because tag RAM clocks are gated.

The verified, hardware-safe startup sequence for PMB8815 SDRAM agents is:

```arm
    msr     cpsr_c, #0x53          /* SVC32 mode, IRQ enabled */
    ldr     sp, =0x60020000        /* SP at safe 128 KB offset in SDRAM */

    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 0  /* Invalidate I-Cache */
    mcr     p15, 0, r0, c7, c6, 0  /* Invalidate D-Cache */
    mcr     p15, 0, r0, c7, c10, 4 /* Drain write buffer */
    b       1f
    nop
    nop
1:
```


3. USB Subsystem & Host-Device Synchronization
==============================================

The PMB8815 integrates a Synopsys DesignWare Core 2 (DWC2) USB 2.0 controller
mapped at `0xE4800000`. In the pre-OS stage, endpoints are allocated as:
- Endpoint `0x02` (Bulk OUT): Host to Device (commands and flash page payloads).
- Endpoint `0x83` (Bulk IN):  Device to Host (status responses and page reads).


3.1. The Hardware NAK Mechanism & The "Beacon First" Requirement
----------------------------------------------------------------

In the USB protocol, an endpoint configured for Bulk OUT issues a hardware NAK
token whenever its FIFO or DMA transfer descriptor is not armed.

In the DWC2 core:
- The endpoint is armed ONLY when `DOEPCTL2.EPENA` (bit 31) is set to `1` and
  `DOEPCTL2.CNAK` (bit 26) is asserted.
- `psi_usb_recv()` is the function that writes to `DOEPCTL2` and sets `EPENA = 1`.

If the host tool (`openIFWD`) attempts to write to EP 0x02 via
`libusb_bulk_transfer()` BEFORE the device has invoked `psi_usb_recv()`:
1. The DWC2 hardware returns NAK to every OUT transaction.
2. The host xHCI/EHCI controller hardware repeatedly retries the packet.
3. Because the agent on the phone is still booting, executing BSS zeroing, or
   initializing hardware, the OUT transfer stalls until `libusb` expires,
   producing error:
   `libusb bulk write error on EP 0x02: LIBUSB_ERROR_TIMEOUT (code -7)`

To eliminate this race condition and ensure deterministic synchronization, the
architecture mandates the **"Beacon First"** protocol model:

```
  Host (openIFWD)                                Device (open_flasher)
        |                                                 |
        | [Read PSI handover ACK 0x17 0xA5]               | [Boot SDRAM at 0x60000400]
        |                                                 | [Execute start.S, clear BSS]
        |                                                 | [Enter main()]
        |                                                 |
        |                                                 | 1. usb_send(&ready_resp, 16)
        | <===== EP 0x83: BEACON ('RESP' / 'PONG') ====== |    (Transmits on EP 0x83)
        |                                                 |
        | [Host receives Beacon: Device is READY]         | 2. usb_recv(&cmd, 16)
        |                                                 |    (Arms DOEPCTL2.EPENA = 1)
        |                                                 |
        | ====== EP 0x02: CMD_PACKET (16 bytes) =======> | [Device receives packet instantly]
        |                                                 | (No NAK, zero timeout)
```

RULE: The device MUST transmit a 16-byte response beacon on EP 0x83 before
attempting to read from EP 0x02. The host MUST wait for and read this beacon
before issuing any write commands to EP 0x02.


3.2. SYSRAM DMA Buffer Boundaries
---------------------------------

PSI allocates its internal structures in on-chip SYSRAM (`0x00080000`–`0x0009FFFF`).
The active USB descriptor pointer is referenced via a double pointer at literal
`0x00081D54`:
`#define PSI_PTR_DESC (**(volatile uint32_t ***)0x00081D54)`

Buffer layout in SYSRAM:
- `0x0008E04C`: Stock PSI transfer buffer (restricted to 20 bytes maximum).
  WARNING: Transmitting more than 20 bytes into `0x0008E04C` corrupts adjacent
  PSI state structures and crashes the core.
- `0x0008E200`–`0x0008E9FF`: Dedicated Agent TX DMA Buffer (2048 bytes).
- `0x0008EA00`–`0x0008F1FF`: Dedicated Agent RX DMA Buffer (2048 bytes).

All USB transmissions must copy data into `0x0008E200` before invoking `psi_usb_send()`,
and copy data out of `0x0008EA00` after `psi_usb_recv()` completes.


4. Storage Subsystem (Samsung OneNAND KFM1G16Q2C)
=================================================

The Samsung GT-S5222 features a Samsung KFM1G16Q2C OneNAND memory device
connected to the EBU on chip select CS0.

Physical parameters:
- Density: 1 Gbit (128 MB)
- Array organization: 1024 blocks
- Block size: 128 KB (64 pages per block)
- Page size: 2048 bytes payload + 64 bytes spare area
- Base address: `0x40000000`
- DataRAM 0 buffer: `0x40000400`
- Spare Area buffer: `0x40010020`
- Controller registers: `0x4001E000`–`0x4001E482`


4.1. Hardware Bus Access Requirement
------------------------------------

IMPORTANT:
The OneNAND controller and internal BufferRAM macros are strictly 16-bit
peripherals mapped on a 16-bit EBU external data bus.
- All register accesses MUST be performed using 16-bit unsigned integers (`u16`)
  via ARM `ldrh` and `strh` instructions.
- 8-bit byte accesses (`ldrb`/`strb`) are ignored by the internal OneNAND
  state machine or write to incorrect byte lanes.
- 32-bit accesses (`ldr`/`str`) to controller registers can trigger bus faults
  or split-transaction errors on the EBU.


4.2. Identification & Geometry Registers
----------------------------------------

Register Address    Name            Expected Value    Description
----------------    ------------    --------------    ---------------------------
0x4001E000          MANUF_ID        0x00EC            Samsung Electronics
0x4001E002          DEV_ID          0x0030            1 Gb, Muxed, Single-Die (DDP=0)
0x4001E004          VERSION_ID      0x0020            Version ID
0x4001E006          DATA_BUF_SIZE   0x0800            2048 bytes DataRAM
0x4001E008          BOOT_BUF_SIZE   0x0200            512 bytes BootRAM


4.3. Hardware Safety Guard (NVRAM Protection Policy)
---------------------------------------------------

The OneNAND flash partition scheme allocates blocks 978 through 1023
(`0x07A40000`–`0x07FFFFFF`, 46 blocks total) for `DYN_EEP` (Dynamic EEPROM).
This region contains factory calibration matrices:
- GSM/EDGE RF transceiver calibration tables
- Power amplifier ramp profiles
- Baseband battery voltage curves
- Factory IMEI 1 and IMEI 2 descriptors

CAUTION: Erasing or overwriting blocks in range 978..1023 results in permanent,
unrecoverable loss of cellular network connectivity and invalidates the device IMEI.

Software Safety Rule (enforced in both `open_flasher` and `openIFWD`):
```c
#define ONENAND_PROTECTED_BLOCK_START   978U
#define ERR_NVRAM_PROTECTED             0xEE01

if (block >= ONENAND_PROTECTED_BLOCK_START ||
    (block + count) > ONENAND_PROTECTED_BLOCK_START) {
    return -ERR_NVRAM_PROTECTED;
}
```


5. Binary Protocol Specification (OFLS / RESP)
==============================================

Communication between `openIFWD` (host) and `open_flasher` (device) occurs over
USB Bulk EP 0x02 / EP 0x83 using fixed-size 16-byte binary frames.


5.1. Packet Structures
----------------------

All multi-byte fields are little-endian. Structures are packed with no padding.

Host Command Packet (`struct flasher_cmd`, 16 bytes):
```c
struct __attribute__((packed)) flasher_cmd {
    u32 magic;   /* 0x534C464F: 'OFLS' (Open Flasher Command) */
    u32 cmd;     /* Command identifier (CMD_*) */
    u32 addr;    /* Block address or Target physical address */
    u32 length;  /* Page index, block count, or byte length */
};
```

Device Response Packet (`struct flasher_resp`, 16 bytes):
```c
struct __attribute__((packed)) flasher_resp {
    u32 magic;      /* 0x50534552: 'RESP' (Device Response) */
    u32 status;     /* 0 = SUCCESS, !0 = Error code */
    u32 bytes_done; /* Transfer count, version, or return data */
    u32 crc32;      /* 0x474E4F50 ('PONG') or IEEE 802.3 CRC32 */
};
```


5.2. Command Reference Table
----------------------------

Command ID    Name              Input Parameters               Response Data
----------    ---------------   ----------------------------   ----------------------------------------
0x01          CMD_PING          addr = 0, length = 0           status=0, bytes_done=ver, crc32='PONG'
0x02          CMD_FLASH_ID      addr = 0, length = 0           status=0, bytes_done=(mfr<<16)|dev
0x03          CMD_BLOCK_ERASE   addr = block, length = count   status=0, bytes_done=erased_count
0x04          CMD_PAGE_WRITE    addr = block, length = page    [Payload 2048 B on EP 0x02] -> status, CRC
0x05          CMD_PAGE_READ     addr = block, length = page    status, CRC -> [Payload 2048 B on EP 0x83]
0x06          CMD_RESET         addr = 0, length = 0           status=0 (triggers DCTL.SftDiscon)


5.3. Error Codes
----------------

Code          Symbol                    Description
----------    -----------------------   -------------------------------------------------
0x00000000    OFLS_OK                   Operation completed successfully
0x00000001    OFLS_ERR_GENERIC          Generic execution failure
0x00000002    OFLS_ERR_ERASE_FAIL       OneNAND block erase hardware error
0x00000003    OFLS_ERR_USB_RX           Failed receiving USB page data from host
0x00000004    OFLS_ERR_WRITE_FAIL       OneNAND page program hardware error
0x00000005    OFLS_ERR_ECC_WARNING      OneNAND correctable ECC warning (data returned)
0x00000006    OFLS_ERR_READ_FAIL        OneNAND read or uncorrectable ECC error
0x00000007    OFLS_ERR_INIT_FAIL        OneNAND controller initialization failure
0x0000EE01    ERR_NVRAM_PROTECTED       Operation rejected: target block within NVRAM range
0xFFFFFFFF    OFLS_ERR_UNKNOWN_CMD      Invalid command ID received


6. Clean Room & Legal Compliance Architecture
=============================================

Proprietary firmware binaries (`psi_ram.bin`, `psi_flash.bin`, `ebl.bin`) are
copyrighted works of Infineon Technologies and Samsung Electronics. Redistributing
or embedding these raw binary artifacts in public open-source software is
strictly prohibited.

To maintain clean-room legal compliance while providing fully automated flashing:

1. Dynamic Container Parsing:
   The user supplies an official firmware container (such as `S5222XXMJ1_psi.fls`).
   The host flasher (`openIFWD`) dynamically parses the FLS file structures (Tag 18,
   Tag 12, Tag 15) to locate and extract the PSI payload at runtime.

2. Cryptographic Hardware Verification:
   The extracted PSI binary is hashed with SHA-256 and matched against known,
   public hardware checksums:
   - `652d144ec1466a776dd8c55e34ed4c3faa0fa2adbb52ecbeaab5acad66d1539a` (XXMJ1, 57644 B)
   - `8c77dc2095f70bb002e21e06f8976b914a2bc7569106096057a6eec95e7fae44` (XXLAF, 57644 B)
   This ensures that no corrupted, malicious, or incompatible stage 1 loader is
   injected into the target device.

3. Complete EBL Replacement:
   The proprietary, closed-source Second Level Bootloader (EBL) is never executed.
   `openIFWD` replaces EBL with the open-source `open_flasher` agent, providing
   100% transparent, auditable flash memory programming.

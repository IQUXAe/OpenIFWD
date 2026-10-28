# Infineon PMB8815 Boot ROM Reference

Author: openIFWD project
Target: Infineon PMB8815 (X-Gold 215 / S-Gold family)
Core:   ARM926EJ-S (ARMv5TEJ, 32-bit little-endian)


1. Overview and Hardware Identification
=======================================

The Infineon PMB8815 baseband processor integrates 32 KB of on-chip mask
ROM mapped at physical address 0x00400000. It contains the primary Stage 0
bootloader executed by the ARM926EJ-S core on power-up, hardware reset, or
watchdog trigger.

Key hardware parameters:
- Core: ARM926EJ-S (ARMv5TEJ instruction set, 32-bit RISC)
- Physical base address: 0x00400000
- Length: 32768 bytes (0x8000)
- Physical end address: 0x00407FFF
- Initial execution mode: Supervisor (SVC32), IRQ and FIQ disabled (CPSR = 0xD3)
- Vector table location: 0x00000000 (Low vectors, SCTLR.V = 0)


2. Address Space Layout
=======================

The internal AHB interconnect decodes the following memory regions:

Address Range              Size      Target / Description
-----------------------    -------   -------------------------------------------------
0x00000000 - 0x0000FFFF    64 KB     Mirrored window (Boot ROM pre-remap, SRAM post-remap)
0x00080000 - 0x0009FFFF    128 KB    Internal on-chip SRAM (SYSRAM)
0x00400000 - 0x00407FFF    32 KB     Physical mask Boot ROM
0x00408000 - 0x004FFFFF    992 KB    Unmapped AHB window (access generates Data Abort)
0x40000000 - 0x4FFFFFFF    256 MB    External Mobile SDRAM (Bank 0)
0x60000000 - 0x6FFFFFFF    256 MB    External Mobile SDRAM (Bank 1, code execution)
0xE0000000 - 0xEFFFFFFF    256 MB    Memory-Mapped I/O (MMIO peripherals)
0xFFFE0000 - 0xFFFE7FFF    32 KB     Data TCM (DTCM)
0xFFFE8000 - 0xFFFEFFFF    32 KB     Instruction TCM (ITCM)
0xFFFF0000 - 0xFFFFFFFF    64 KB     High exception vectors (active when SCTLR.V = 1)

Mapping notes:
- The 0x00400000 window directly addresses the physical silicon mask ROM macro.
  Reading from 0x00400000 does not require clearing or toggling SCU remap
  registers (such as 0xE4900088).
- Access to 0x00408000 or above within the 0x00400000 block returns an AHB
  error response (HRESP = ERROR), triggering a synchronous Data Abort.
- Memory at 0x60000000 is external Mobile SDRAM (Bank 1). Executable helper
  programs (including ram_dumper at 0x60000400) and runtime buffers are
  allocated here. Physical DRAM capacity spans 32 MB to 64 MB.


3. Reset and Hardware Initialization
====================================

3.1 Vector Table
----------------

The mask ROM contains an ARM exception vector table at offset 0x0000
(physical address 0x00400000):

Offset        Opcode        Instruction                 Resolved Target
----------    ----------    -------------------------   --------------------------------
0x00000000    0xE59FF018    ldr pc, [pc, #0x18]         0x00400054 (Reset entry)
0x00000004    0xE59FF018    ldr pc, [pc, #0x18]         0x0040003C (Undef trap: b .)
0x00000008    0xE59FF018    ldr pc, [pc, #0x18]         0x00400040 (SWI trap: b .)
0x0000000C    0xE59FF018    ldr pc, [pc, #0x18]         0x00400044 (Prefetch abort: b .)
0x00000010    0xE59FF018    ldr pc, [pc, #0x18]         0x00400048 (Data abort: b .)
0x00000014    0xE320F000    nop                         -
0x00000018    0xE59FF014    ldr pc, [pc, #0x14]         0x0040004C (IRQ trap: b .)
0x0000001C    0xE59FF014    ldr pc, [pc, #0x14]         0x00400050 (FIQ trap: b .)

Literal pool entries (0x00400020 - 0x0040003B):
- 0x00400020: 0x00400054 (Reset handler address)
- 0x00400024 - 0x00400038: 0x0040003C .. 0x00400050 (Trap targets)


3.2 Reset Execution Sequence
----------------------------

Execution begins at 0x00400054:

1. Mode Setup and Initial Stack:
   0x00400054:  ldr  r0, [pc, #0x74]          ; Load 0x000AF7FC from literal 0x004000D0
   0x00400058:  msr  cpsr_c, #0xD3            ; SVC32 mode, IRQ and FIQ disabled
   0x0040005C:  mov  sp, r0                   ; Initialize temporary stack pointer

2. Peripheral Port Remapping:
   0x00400060:  mov  r0, #0xE0000000
   0x00400064:  orr  r0, r0, #0x13
   0x00400068:  mcr  p15, 0, r0, c15, c2, 4   ; Map MMIO peripheral port to 0xE0000000

3. Boot Strap Evaluation:
   0x0040006C:  ldr  r2, [pc, #0x60]          ; Load strap register address (0xE4900010)
   0x00400070:  ldr  r0, [r2]
   0x00400074:  and  r2, r0, #0x20000000      ; Test bit 29 (boot mode strap pin)
   0x00400078:  cmp  r2, #0x20000000
   0x0040007C:  bne  0x00400084               ; Non-zero indicates USB download mode

4. TCM Configuration and Core Stack Setup:
   0x00400088:  mov  r0, #0
   0x0040008C:  mcr  p15, 0, r0, c9, c2, 0    ; Disable Data TCM
   0x00400090:  nop
   0x00400094:  mov  r0, #0
   0x00400098:  mcr  p15, 0, r0, c9, c1, 2    ; Disable Instruction TCM
   0x0040009C:  nop
   0x004000A0:  mcr  p15, 0, r0, c9, c1, 3    ; TCM Size / Boundary control
   0x004000A4:  nop
   0x004000A8:  ldr  r1, [pc, #0x28]          ; Load DTCM base from literal pool (0x004000D8)
   0x004000AC:  mcr  p15, 0, r1, c9, c1, 0    ; Enable DTCM
   0x004000B0:  nop
   0x004000B4:  ldr  r1, [pc, #0x20]          ; Load ITCM base from literal pool (0x004000DC)
   0x004000B8:  mcr  p15, 0, r1, c9, c1, 1    ; Enable ITCM
   0x004000BC:  nop
   0x004000C0:  ldr  r0, [pc, #0x18]          ; Load runtime stack base from literal pool (0x004000E0)
   0x004000C4:  msr  cpsr_c, #0xD3            ; Enforce SVC32 mode with interrupts disabled
   0x004000C8:  mov  sp, r0                   ; Set core SP


4. Stage 1 Loader Format
========================

The boot ROM validates downloaded binaries using a 64-byte container header (liHU).

Header structure:

Byte Offset    Length    Description
-----------    ------    -----------------------------------------------------
0x00 - 0x03    4         ARM branch instruction to entry point (e.g. 0xEA002AD7)
0x04 - 0x07    4         Magic identifier: 0x5548696C ("liHU")
0x08 - 0x2F    40        Reserved / header metadata
0x30 - 0x33    4         Target load address (e.g. 0x00080000)
0x34 - 0x37    4         Execution entry address (e.g. 0x000811E8)
0x38 - 0x3B    4         Target memory limit (e.g. 0x0008FFFF)
0x3C - 0x3F    4         Tag identifier: 0x544B4A43 ("CJKT")

Validation rules:
- Magic identifier at offset 0x04 must equal 0x5548696C ("liHU").
- Target address span [Load Address .. Memory Limit] must fall entirely
  within valid writable SRAM (0x00080000 - 0x0009FFFF) or configured SDRAM.
- Tag at offset 0x3C designates the component role ("CJKT" identifies Stage 1 PSI).
- Following payload transfer, the host sends a 1-byte XOR-8 parity value:
    Checksum = XOR(Payload[0 .. N-1])
  If the checksum does not match, the boot ROM aborts download and resets.


5. Memory Dumping Interface
===========================

The physical Boot ROM can be dumped over USB by running an execution agent
(ram_dumper) in external Mobile SDRAM at 0x60000400.

Operational parameters:
- Read base address: 0x00400000
- Length: 32768 bytes (0x8000)
- Chunk size: 2048 bytes (16 chunks total)
- Transfer endpoint: EP 0x83 (Bulk IN)

Accessing addresses at or above 0x00408000 generates an AHB bus error and
must be avoided.


6. Image Checksums
==================

Reference digests of the 32 KB Boot ROM binary (bootrom_pmb8815_32k.bin):

Size:    32768 bytes (0x8000)
MD5:     5f3e30c4cbe50f6c6fbd328b14b6b8bc
SHA-256: 23d9901bf4e8909732228243ccecc7f0d3e96d06ff875faf2f589833fc6acf6f

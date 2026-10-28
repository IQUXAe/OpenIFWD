#!/usr/bin/env python3
"""
Parse Boot ROM and register dump extracted from Samsung GT-S5222 / Infineon PMB8815.

Features:
1. Unpacks and decodes CPU handover registers (R0-R12, SP, LR, CPSR) saved at 0x0008F000.
2. Extracts pure 64 KB physical silicon mask Boot ROM (from 0x00090000 in dump).
3. Validates ARM Vector Table (Reset, SWI, Abort, IRQ, FIQ).
4. Scans and extracts Infineon Boot ROM strings, chip IDs, build metadata, and hashes.
"""

import argparse
import hashlib
import os
import re
import struct
import sys

ARM_MODES = {
    0x10: "USR (User)",
    0x11: "FIQ (Fast Interrupt)",
    0x12: "IRQ (Interrupt)",
    0x13: "SVC (Supervisor)",
    0x17: "ABT (Abort)",
    0x1B: "UND (Undefined)",
    0x1F: "SYS (System)"
}

ARM_VECTORS = [
    ("0x00", "Reset"),
    ("0x04", "Undefined Instruction"),
    ("0x08", "Software Interrupt (SWI)"),
    ("0x0C", "Prefetch Abort"),
    ("0x10", "Data Abort"),
    ("0x14", "Reserved"),
    ("0x18", "Interrupt (IRQ)"),
    ("0x1C", "Fast Interrupt (FIQ)"),
]

def decode_cpsr(cpsr: int) -> str:
    mode_val = cpsr & 0x1F
    mode_str = ARM_MODES.get(mode_val, f"Unknown (0x{mode_val:02X})")
    thumb = "Thumb" if (cpsr & (1 << 5)) else "ARM"
    fiq_dis = "Disabled" if (cpsr & (1 << 6)) else "Enabled"
    irq_dis = "Disabled" if (cpsr & (1 << 7)) else "Enabled"

    flags = []
    if cpsr & (1 << 31): flags.append("N")
    if cpsr & (1 << 30): flags.append("Z")
    if cpsr & (1 << 29): flags.append("C")
    if cpsr & (1 << 28): flags.append("V")
    flags_str = "".join(flags) if flags else "none"

    return f"{mode_str}, State={thumb}, IRQ={irq_dis}, FIQ={fiq_dis}, Flags=[{flags_str}]"

def decode_branch_target(insn: int, pc: int) -> int:
    """Decodes B / BL target address."""
    disp = insn & 0x00FFFFFF
    if disp & 0x00800000:
        disp -= 0x01000000
    return (pc + 8 + (disp << 2)) & 0xFFFFFFFF

def print_registers(reg_bytes: bytes):
    if len(reg_bytes) < 64:
        print(f"[-] Register buffer too small: {len(reg_bytes)} bytes (need 64 bytes)", file=sys.stderr)
        return

    words = struct.unpack("<16I", reg_bytes[:64])
    r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, sp, lr, cpsr = words

    print("=" * 80)
    print("CPU Handover Registers (Boot ROM -> PSI Entry 0x000811E8)")
    print("=" * 80)
    print(f"  R0 : 0x{r0:08X}    R1 : 0x{r1:08X}    R2 : 0x{r2:08X}    R3 : 0x{r3:08X}")
    print(f"  R4 : 0x{r4:08X}    R5 : 0x{r5:08X}    R6 : 0x{r6:08X}    R7 : 0x{r7:08X}")
    print(f"  R8 : 0x{r8:08X}    R9 : 0x{r9:08X}    R10: 0x{r10:08X}    R11: 0x{r11:08X}")
    print(f"  R12: 0x{r12:08X}    SP : 0x{sp:08X}    LR : 0x{lr:08X}    CPSR: 0x{cpsr:08X}")
    print("-" * 80)
    print(f"  CPSR Decoding: {decode_cpsr(cpsr)}")
    if 0x00080000 <= sp <= 0x0009FFFF:
        print(f"  Stack Pointer (SP): 0x{sp:08X} (within internal SRAM 128KB)")
    elif 0xFFFE0000 <= sp <= 0xFFFE7FFF:
        print(f"  Stack Pointer (SP): 0x{sp:08X} (within DTCM)")
    if lr < 0x00010000:
        print(f"  Handover Return (LR): 0x{lr:08X} (pointing into silicon Boot ROM)")
    print("=" * 80)

def analyze_bootrom(rom_bytes: bytes):
    print("=" * 80)
    print(f"Physical Silicon Boot ROM Analysis ({len(rom_bytes):,} bytes / {len(rom_bytes) // 1024} KB)")
    print("=" * 80)

    # Hashes
    md5 = hashlib.md5(rom_bytes).hexdigest()
    sha256 = hashlib.sha256(rom_bytes).hexdigest()
    print(f"  MD5    : {md5}")
    print(f"  SHA-256: {sha256}")
    print("-" * 80)

    # ARM Vector Table
    print("ARM Exception Vector Table (@ 0x00000000):")
    for i in range(8):
        v_addr, v_name = ARM_VECTORS[i]
        insn = struct.unpack_from("<I", rom_bytes, i * 4)[0]
        desc = f"0x{insn:08X}"

        # Check for B / BL
        if (insn & 0x0E000000) == 0x0A000000:
            target = decode_branch_target(insn, i * 4)
            desc += f" -> b 0x{target:04X}"
        # Check for LDR PC, [PC, #offset]
        elif (insn & 0xFFFFF000) == 0xE59FF000:
            off = insn & 0xFFF
            lit_addr = (i * 4 + 8 + off)
            if lit_addr + 4 <= len(rom_bytes):
                lit_val = struct.unpack_from("<I", rom_bytes, lit_addr)[0]
                desc += f" -> ldr pc, [0x{lit_addr:04X}] => 0x{lit_val:08X}"
            else:
                desc += f" -> ldr pc, [pc, #0x{off:x}]"

        print(f"  [{v_addr}] {v_name:<28}: {desc}")

    print("-" * 80)
    # Search for printable ASCII strings
    print("Extracted Boot ROM Strings & Descriptors:")
    pattern = rb"[\x20-\x7E]{4,}"
    found_strings = []
    for match in re.finditer(pattern, rom_bytes):
        s = match.group().decode("ascii", errors="replace")
        offset = match.start()
        found_strings.append((offset, s))

    # Highlight relevant hardware keywords
    keywords = ["infineon", "pmb", "xmm", "boot", "rom", "gold", "chip", "intel", "sgold"]
    notable = []
    other = []
    for off, s in found_strings:
        lower = s.lower()
        if any(k in lower for k in keywords):
            notable.append((off, s))
        else:
            other.append((off, s))

    if notable:
        print("[*] Key Hardware / Chipset Identifiers:")
        for off, s in notable:
            print(f"  0x{off:04X}: \"{s}\"")
        print()

    print(f"[*] Total ASCII strings found: {len(found_strings)}")
    if len(other) > 0 and len(notable) < 10:
        print("[*] Sample text occurrences:")
        for off, s in other[:15]:
            print(f"  0x{off:04X}: \"{s}\"")

    print("=" * 80)

def main():
    parser = argparse.ArgumentParser(description="Parse Boot ROM & register dump from Samsung GT-S5222 / Infineon PMB8815")
    default_in = "bootrom_pmb8815_32k.bin" if os.path.isfile("bootrom_pmb8815_32k.bin") else (
        "infineon_pmb8815_bootrom_64k.bin" if os.path.isfile("infineon_pmb8815_bootrom_64k.bin") else "bootrom_pmb8815_32k.bin"
    )
    parser.add_argument("-i", "--input", default=default_in,
                        help=f"Input dump file (default: {default_in})")
    parser.add_argument("-o", "--output", default="bootrom_pmb8815_32k.bin",
                        help="Output pure Boot ROM file (default: bootrom_pmb8815_32k.bin)")
    parser.add_argument("-r", "--regs", default=None,
                        help="Optional separate register dump file (if dumped separately)")
    parser.add_argument("--regs-only", action="store_true",
                        help="Only decode and print registers, do not write Boot ROM")
    args = parser.parse_args()

    if not os.path.isfile(args.input):
        print(f"[-] Input file not found: {args.input}", file=sys.stderr)
        return 1

    with open(args.input, "rb") as f:
        data = f.read()

    file_size = len(data)
    print(f"[*] Loaded dump file: {args.input} ({file_size} bytes / {file_size // 1024} KB)")

    reg_bytes = None
    rom_bytes = None

    # Determine layout based on file size:
    # 1. 69632 bytes (0x11000): Dumped from 0x0008F000 for 0x11000 bytes
    #    - Registers at offset 0x0000 (0x0008F000)
    #    - Boot ROM at offset 0x1000 (0x00090000) for 65536 bytes
    if file_size == 0x11000:
        print("[+] Detected full range dump from 0x0008F000 (length 0x11000):")
        print("    - Registers: offset 0x0000 - 0x003F (addr 0x0008F000)")
        print("    - Boot ROM : offset 0x1000 - 0x10FFF (addr 0x00090000, 64 KB)")
        reg_bytes = data[0:64]
        rom_bytes = data[0x1000:0x1000 + 65536]

    # 2. 65600 bytes (0x10040): Contiguous header + 64 KB ROM
    #    - Registers at offset 0x0000
    #    - Boot ROM at offset 0x0040 for 65536 bytes
    elif file_size == 0x10040:
        print("[+] Detected contiguous header dump (64 bytes regs + 65536 bytes ROM)")
        reg_bytes = data[0:64]
        rom_bytes = data[64:64 + 65536]

    # 3. 65536 bytes (0x10000): Pure 64 KB Boot ROM
    elif file_size == 0x10000:
        print("[+] Detected pure 64 KB Boot ROM dump")
        rom_bytes = data
        if args.regs and os.path.isfile(args.regs):
            with open(args.regs, "rb") as rf:
                reg_bytes = rf.read(64)

    # 4. 32768 bytes (0x8000): Pure 32 KB PMB8815 Physical Mask Boot ROM (0x00400000)
    elif file_size == 0x8000:
        print("[+] Detected pure 32 KB PMB8815 Physical Mask Boot ROM dump (0x00400000 - 0x00407FFF)")
        rom_bytes = data
        if args.regs and os.path.isfile(args.regs):
            with open(args.regs, "rb") as rf:
                reg_bytes = rf.read(64)

    # 5. 64 bytes: Pure register dump
    elif file_size == 64:
        print("[+] Detected 64-byte register dump")
        reg_bytes = data

    # 5. Fallback for larger dumps
    elif file_size > 65536:
        print(f"[*] Non-standard dump size ({file_size} bytes). Attempting heuristic extraction...")
        # Check if first 64 bytes look like registers (e.g. SP/LR non-zero)
        words = struct.unpack("<16I", data[:64])
        if words[13] != 0 or words[14] != 0:
            reg_bytes = data[:64]
        if file_size >= 0x1000 + 65536:
            rom_bytes = data[0x1000:0x1000 + 65536]
        else:
            rom_bytes = data[64:64 + 65536]
    else:
        print(f"[-] Dump size too small ({file_size} bytes). Minimum required is 64 bytes.", file=sys.stderr)
        return 1

    # Check external regs file if provided
    if args.regs and os.path.isfile(args.regs) and not reg_bytes:
        with open(args.regs, "rb") as rf:
            reg_bytes = rf.read(64)

    # Display registers
    if reg_bytes:
        print_registers(reg_bytes)
    else:
        print("[*] No register dump found in this file.")

    if args.regs_only:
        return 0

    # Save and analyze Boot ROM
    if rom_bytes:
        if len(rom_bytes) not in (32768, 65536):
            print(f"[-] Warning: ROM slice size is {len(rom_bytes)} bytes (expected 32768 or 65536)")

        with open(args.output, "wb") as f:
            f.write(rom_bytes)
        print(f"[+] Saved pure {len(rom_bytes) // 1024} KB Boot ROM to: {args.output}")

        analyze_bootrom(rom_bytes)
    else:
        print("[-] Boot ROM data could not be extracted from input file.", file=sys.stderr)
        return 1

    return 0

if __name__ == "__main__":
    sys.exit(main())

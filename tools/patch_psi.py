#!/usr/bin/env python3
"""
Patch Infineon stock psi_ram.bin (57,644 bytes) to dump the physical silicon
mask Boot ROM (64 KB @ 0x00000000 - 0x0000FFFF) before SCU remap and TCM/cache
initialization take place.

Architecture details:
- Base load address: 0x00080000
- Entry hook location: 0x000811E8 (file offset 0x011E8)
  Replaces stock setup instructions with 'b 0x00080900' + NOPs
- Hook routine: injected at 0x00080900 (file offset 0x00900, free padding)
  1. Saves CPU handover registers (R0-R12, SP, LR, CPSR) to SRAM @ 0x0008F000
  2. Copies 64 KB Boot ROM from 0x00000000 to SRAM @ 0x00090000 (0x00090000-0x0009FFFF)
  3. Replays replaced stock instructions (E4900088 / E490007C config)
  4. Resumes stock PSI at 0x00081200 normally
- Recalculates transmission XOR-8 checksum for Infineon Boot ROM protocol
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

PSI_DEFAULT_IN = "reverse_samsungstar3/psi/psi_ram.bin"
PSI_DEFAULT_OUT = "reverse_samsungstar3/psi/psi_ram_hooked.bin"

# Pre-assembled fallback machine code (108 bytes) for address 0x00080900:
HOOK_FALLBACK_BYTES = bytes([
    0x50, 0x00, 0x8f, 0xe5,  # str  r0, [pc, #80]      (saved_r0)
    0x50, 0x00, 0x9f, 0xe5,  # ldr  r0, [pc, #80]      (0x0008f004)
    0xfe, 0x7f, 0xa0, 0xe8,  # stmia r0!, {r1-r12, sp, lr}
    0x44, 0x10, 0x9f, 0xe5,  # ldr  r1, [pc, #68]      (saved_r0)
    0x8f, 0x2a, 0xa0, 0xe3,  # mov  r2, #0x8f000
    0x00, 0x10, 0x82, 0xe5,  # str  r1, [r2]           (r0 @ 0x8f000)
    0x00, 0x10, 0x0f, 0xe1,  # mrs  r1, CPSR
    0x3c, 0x10, 0x82, 0xe5,  # str  r1, [r2, #60]      (cpsr @ 0x8f03c)
    0x09, 0x08, 0xa0, 0xe3,  # mov  r0, #0x90000       (dest: 0x90000)
    0x00, 0x10, 0xa0, 0xe3,  # mov  r1, #0             (src: 0x00000)
    0x01, 0x29, 0xa0, 0xe3,  # mov  r2, #16384         (count: 64 KB)
    # copy_loop:
    0x04, 0x30, 0x91, 0xe4,  # ldr  r3, [r1], #4
    0x04, 0x30, 0x80, 0xe4,  # str  r3, [r0], #4
    0x01, 0x20, 0x52, 0xe2,  # subs r2, r2, #1
    0xfb, 0xff, 0xff, 0x1a,  # bne  copy_loop
    # Replay stock instructions:
    0x01, 0x10, 0xa0, 0xe3,  # mov  r1, #1
    0x18, 0x00, 0x9f, 0xe5,  # ldr  r0, [pc, #24]      (0xe4900088)
    0xb0, 0x10, 0xc0, 0xe1,  # strh r1, [r0]
    0x14, 0x00, 0x9f, 0xe5,  # ldr  r0, [pc, #20]      (0xe490007c)
    0x00, 0x10, 0xa0, 0xe3,  # mov  r1, #0
    0x00, 0x10, 0x80, 0xe5,  # str  r1, [r0]
    0x0c, 0xf0, 0x9f, 0xe5,  # ldr  pc, [pc, #12]      (0x00081200)
    # Literals:
    0x00, 0x00, 0x00, 0x00,  # saved_r0
    0x04, 0xf0, 0x08, 0x00,  # 0x0008f004
    0x88, 0x00, 0x90, 0xe4,  # 0xe4900088
    0x7c, 0x00, 0x90, 0xe4,  # 0xe490007c
    0x00, 0x12, 0x08, 0x00,  # 0x00081200
])

HOOK_ASM = """
    .syntax unified
    .arm
    .text
    .global _hook_start

_hook_start:
    str     r0, saved_r0
    ldr     r0, =0x0008F004
    stmia   r0!, {r1-r12, sp, lr}
    ldr     r1, saved_r0
    ldr     r2, =0x0008F000
    str     r1, [r2]
    mrs     r1, cpsr
    str     r1, [r2, #0x3C]

    ldr     r0, =0x00090000
    mov     r1, #0
    mov     r2, #0x4000
copy_loop:
    ldr     r3, [r1], #4
    str     r3, [r0], #4
    subs    r2, r2, #1
    bne     copy_loop

    mov     r1, #1
    ldr     r0, =0xE4900088
    strh    r1, [r0]
    ldr     r0, =0xE490007C
    mov     r1, #0
    str     r1, [r0]

    ldr     pc, =0x00081200

saved_r0:
    .word   0

    .ltorg
"""

def assemble_hook():
    """Assembles the ARM hook using arm-none-eabi-as if available, else uses fallback bytes."""
    try:
        with tempfile.TemporaryDirectory() as td:
            s_file = os.path.join(td, "hook.s")
            o_file = os.path.join(td, "hook.o")
            elf_file = os.path.join(td, "hook.elf")
            bin_file = os.path.join(td, "hook.bin")

            with open(s_file, "w") as f:
                f.write(HOOK_ASM)

            subprocess.check_call([
                "arm-none-eabi-as", "-mcpu=arm926ej-s", s_file, "-o", o_file
            ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.check_call([
                "arm-none-eabi-ld", "-Ttext=0x00080900", o_file, "-o", elf_file
            ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.check_call([
                "arm-none-eabi-objcopy", "-O", "binary", elf_file, bin_file
            ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

            with open(bin_file, "rb") as f:
                hook_bytes = f.read()

            print(f"[+] Hook assembled with arm-none-eabi-as: {len(hook_bytes)} bytes")
            return hook_bytes
    except Exception as e:
        print(f"[*] Toolchain assembly unavailable ({e}); using verified pre-assembled machine code")
        return HOOK_FALLBACK_BYTES

def compute_xor8(data: bytes) -> int:
    chk = 0
    for b in data:
        chk ^= b
    return chk

def patch_psi(src_path: str, dst_path: str):
    print("=" * 80)
    print("Infineon XMM2150 / PMB8815 PSI Entry Hook Injector")
    print("Target: Physical Silicon Boot ROM Dump before SCU REMAP & TCM init")
    print("=" * 80)

    if not os.path.isfile(src_path):
        print(f"[-] Source PSI not found: {src_path}", file=sys.stderr)
        return 1

    with open(src_path, "rb") as f:
        data = bytearray(f.read())

    print(f"[*] Loaded source PSI: {len(data)} bytes from {src_path}")
    if len(data) != 57644:
        print(f"[-] Warning: Expected 57644 bytes, got {len(data)} bytes")

    # Validate header fields
    w_branch = struct.unpack_from("<I", data, 0x00)[0]
    w_magic = struct.unpack_from("<I", data, 0x04)[0]
    w_load = struct.unpack_from("<I", data, 0x30)[0]
    w_limit = struct.unpack_from("<I", data, 0x38)[0]
    w_tag = struct.unpack_from("<I", data, 0x3C)[0]

    orig_chk = compute_xor8(data)
    print(f"[*] Header Analysis:")
    print(f"    - Word 0 (Entry Branch): 0x{w_branch:08X}")
    print(f"    - Word 1 (Magic):        0x{w_magic:08X} (ASCII: {bytes(data[4:8])})")
    print(f"    - Word 12 (Load Addr):   0x{w_load:08X}")
    print(f"    - Word 14 (Target Limit):0x{w_limit:08X}")
    print(f"    - Word 15 (Tag):         0x{w_tag:08X} (ASCII: {bytes(data[0x3C:0x40])})")
    print(f"    - Original XOR-8:        0x{orig_chk:02X}")

    if w_magic != 0x5548696C:
        print("[-] Warning: PSI magic mismatch (expected 0x5548696C / 'liHU')")

    # Verify original instructions at 0x000811E8 (file offset 0x011E8)
    entry_offset = 0x011E8
    orig_insns = struct.unpack_from("<6I", data, entry_offset)
    expected_insns = (
        0xE3A01001,  # mov  r1, #1
        0xE59F008C,  # ldr  r0, [pc, #0x8c] -> 0xE4900088
        0xE1C010B0,  # strh r1, [r0]
        0xE59F0088,  # ldr  r0, [pc, #0x88] -> 0xE490007C
        0xE3A01000,  # mov  r1, #0
        0xE5801000,  # str  r1, [r0]
    )
    if orig_insns == expected_insns:
        print("[+] Verified stock PSI entry point at 0x000811E8 (matching stock sequence)")
    else:
        print("[-] Warning: Instructions at 0x000811E8 do not match stock sequence:")
        for idx, insn in enumerate(orig_insns):
            print(f"    0x{0x000811E8 + idx*4:08X}: 0x{insn:08X}")

    # Assemble hook code
    hook_bytes = assemble_hook()
    assert len(hook_bytes) <= 512, "Hook code exceeds allocated space"

    # Inject hook routine into padding at 0x00080900 (file offset 0x00900)
    hook_offset = 0x00900
    print(f"[*] Injecting {len(hook_bytes)}-byte hook at file offset 0x{hook_offset:05X} (addr 0x00080900)...")
    # Verify target padding is zeroes
    pad_check = data[hook_offset:hook_offset + len(hook_bytes)]
    if any(b != 0 for b in pad_check):
        print("[-] Warning: Target padding contains non-zero bytes!")
    data[hook_offset:hook_offset + len(hook_bytes)] = hook_bytes

    # Inject branch at 0x000811E8 (file offset 0x011E8): b 0x00080900
    # disp = (0x00080900 - (0x000811E8 + 8)) >> 2 = -0x23C
    # opcode = 0xEA000000 | ((-0x23C) & 0x00FFFFFF) = 0xEAFFFDC4
    branch_opcode = 0xEAFFFDC4
    nop_opcode = 0xE1A00000

    print(f"[*] Injecting branch at file offset 0x{entry_offset:05X} (addr 0x000811E8):")
    print(f"    0x000811E8: 0x{branch_opcode:08X} (b 0x00080900)")
    struct.pack_into("<I", data, entry_offset, branch_opcode)

    # Pad remaining 5 replaced instructions with NOP
    for i in range(1, 6):
        off = entry_offset + i * 4
        struct.pack_into("<I", data, off, nop_opcode)
    print(f"    0x000811EC - 0x000811FC: NOP x 5 (0x{nop_opcode:08X})")

    # Compute new XOR-8 checksum
    new_chk = compute_xor8(data)
    print(f"[+] Recomputed XOR-8 transmission checksum: 0x{new_chk:02X} (original: 0x{orig_chk:02X})")

    # Ensure output directory exists and write patched file
    out_dir = os.path.dirname(dst_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(dst_path, "wb") as f:
        f.write(data)
    print(f"[+] Successfully saved patched PSI: {dst_path} ({len(data)} bytes)")

    print("-" * 80)
    print("Instructions to execute dump on hardware:")
    print("1. Start memory dump via openIFWD:")
    print("   sudo ./openIFWD/openifwd dump-memory -p usb \\")
    print(f"       --psi {dst_path} \\")
    print("       -a 0x0008F000 -l 0x11000 -o bootrom_dump_with_regs.bin -v")
    print()
    print("2. Parse dump and extract 64 KB Boot ROM:")
    print("   python3 tools/parse_bootrom_dump.py -i bootrom_dump_with_regs.bin -o bootrom_64k.bin")
    print("=" * 80)
    return 0

def main():
    parser = argparse.ArgumentParser(description="Inject physical Boot ROM extraction hook into stock psi_ram.bin")
    parser.add_argument("-i", "--input", default=PSI_DEFAULT_IN, help=f"Input stock PSI (default: {PSI_DEFAULT_IN})")
    parser.add_argument("-o", "--output", default=PSI_DEFAULT_OUT, help=f"Output hooked PSI (default: {PSI_DEFAULT_OUT})")
    args = parser.parse_args()

    return patch_psi(args.input, args.output)

if __name__ == "__main__":
    sys.exit(main())

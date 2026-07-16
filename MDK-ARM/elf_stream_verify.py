#!/usr/bin/env python3
"""
elf_stream_verify.py — 用 Python 解析 AXF，验证 elf_loader_stream 的预期行为
"""

import struct
import sys

SHT_NAMES = {0:'NULL',1:'PROGBITS',2:'SYMTAB',3:'STRTAB',4:'RELA',8:'NOBITS',9:'REL'}
SHF_W = 0x1; SHF_A = 0x2; SHF_X = 0x4

def main():
    axf = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\zyw\Desktop\OTA\USART\USART\MDK-ARM\USART\USART.axf"
    target_base = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x0802B800

    with open(axf, 'rb') as f:
        data = f.read()

    print(f"File: {axf}")
    print(f"Size: {len(data)} bytes")
    print(f"Target base: 0x{target_base:08X}")

    # Parse ELF header
    magic = struct.unpack_from('<I', data, 0)[0]
    assert magic == 0x464C457F, "Not ELF"
    ei_class = data[4]
    assert ei_class == 1, "Not 32-bit"
    e_machine = struct.unpack_from('<H', data, 18)[0]
    assert e_machine == 40, "Not ARM"

    e_entry   = struct.unpack_from('<I', data, 24)[0]
    e_shoff   = struct.unpack_from('<I', data, 32)[0]
    e_shentsize = struct.unpack_from('<H', data, 46)[0]
    e_shnum   = struct.unpack_from('<H', data, 48)[0]
    e_shstrndx = struct.unpack_from('<H', data, 50)[0]

    print(f"\nELF Header:")
    print(f"  e_entry    = 0x{e_entry:08X}")
    print(f"  e_shoff    = 0x{e_shoff:X}")
    print(f"  e_shnum    = {e_shnum}")
    print(f"  e_shstrndx = {e_shstrndx}")

    # Parse section headers
    shdrs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh = struct.unpack_from('<IIIIIIIIII', data, off)
        shdrs.append({
            'sh_name': sh[0], 'sh_type': sh[1], 'sh_flags': sh[2],
            'sh_addr': sh[3], 'sh_offset': sh[4], 'sh_size': sh[5],
            'sh_link': sh[6], 'sh_info': sh[7], 'sh_addralign': sh[8],
            'sh_entsize': sh[9]
        })

    # Read shstrtab
    shstrtab_off = shdrs[e_shstrndx]['sh_offset']
    shstrtab_size = shdrs[e_shstrndx]['sh_size']
    shstrtab = data[shstrtab_off:shstrtab_off + shstrtab_size]

    def get_name(idx):
        end = shstrtab.find(b'\x00', idx)
        return shstrtab[idx:end].decode('ascii', errors='replace')

    print(f"\n--- Section Table ({e_shnum} sections) ---")
    load_secs = []
    rel_secs = []
    sym_count = 0

    for i, s in enumerate(shdrs):
        name = get_name(s['sh_name'])
        tname = SHT_NAMES.get(s['sh_type'], f"TYPE{s['sh_type']}")
        flags = ""
        if s['sh_flags'] & SHF_X: flags += "X"
        if s['sh_flags'] & SHF_W: flags += "W"
        if s['sh_flags'] & SHF_A: flags += "A"

        print(f"  [{i}] %-16s %-8s addr=0x%08X off=0x%05X size=%-6u flags=%s" % (
            name, tname, s['sh_addr'], s['sh_offset'], s['sh_size'], flags))

        if s['sh_type'] in (1, 8) and (s['sh_flags'] & SHF_A) and s['sh_size'] > 0:
            load_secs.append((i, name, s))
        if s['sh_type'] in (4, 9):
            rel_secs.append((i, name, s))
        if s['sh_type'] == 2:
            sym_count = s['sh_size'] // 16

    print(f"\n  load_count = {len(load_secs)}")
    print(f"  rel_count  = {len(rel_secs)}")
    print(f"  sym_count  = {sym_count}")

    if len(rel_secs) == 0:
        print(f"\n  >>> 走 Path B（值域猜测重定向）")
    else:
        print(f"\n  >>> 走 Path A（精确 REL 重定向）")

    # Check ELF_STREAM_META_BUF_MIN
    meta_min = 52 + e_shnum * 40 + 4096  # ehdr + shdrs + shstrtab_max
    print(f"\n  ELF_STREAM_META_BUF_MIN = {meta_min} bytes")

    # Check buffer strategy for bootloader
    elf_size = len(data)
    avail = 40960 - elf_size
    print(f"\n  elf_buf 空间: ELF={elf_size}B, 剩余={avail}B, meta+work 需要约 {meta_min+4096}B")
    if avail >= meta_min + 4096:
        print(f"  [OK] 空间足够，stream buffer 可复用 elf_buf 末尾")
    else:
        print(f"  [FAIL] 空间不足！")

    # Show relocation preview
    offset = target_base
    print(f"\n--- Relocation Preview (offset=0x{offset:08X}) ---")
    print(f"  e_entry: 0x{e_entry:08X} -> 0x{e_entry + offset:08X}")

    for i, name, s in load_secs:
        new_addr = s['sh_addr'] + offset if s['sh_addr'] < 0x10000000 else s['sh_addr']
        bss = " (BSS)" if s['sh_type'] == 8 else ""
        is_exec = bool(s['sh_flags'] & SHF_X) and not (s['sh_flags'] & SHF_W)
        is_write = bool(s['sh_flags'] & SHF_W) and not (s['sh_flags'] & SHF_X)
        scan = ""
        if is_exec: scan = " -> scan_exec"
        if is_write: scan = " -> scan_write"
        print(f"  {name:16s} 0x{s['sh_addr']:08X} -> 0x{new_addr:08X}  size={s['sh_size']}{bss}{scan}")

    # Show vector table
    print(f"\n--- Vector Table (from first PROGBITS section) ---")
    for i, name, s in load_secs:
        if s['sh_type'] != 1: continue  # skip NOBITS
        words = struct.unpack_from('<4I', data, s['sh_offset'])
        print(f"  [0] 0x{words[0]:08X}  (MSP)")
        print(f"  [1] 0x{words[1]:08X}  (Reset_Handler) -> 0x{words[1]+offset:08X}")
        print(f"  [2] 0x{words[2]:08X}  (NMI_Handler) -> 0x{words[2]+offset:08X}")
        print(f"  [3] 0x{words[3]:08X}  (HardFault) -> 0x{words[3]+offset:08X}")

        # Verify MSP is in RAM
        msp = words[0]
        in_ram = (0x20000000 <= msp < 0x20010000) or (0x10000000 <= msp < 0x10004000)
        print(f"  MSP valid: {'OK' if in_ram else 'FAIL: NOT in RAM range!'}")
        break

    print(f"\n[OK] Analysis complete")

if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Decompresses a Majora's Mask US ROM for Zelda64Recomp's code generation (mm.us.rev1.rom_uncompressed.z64).

    decompress_mm.py <mm-us.z64|.n64|.v64> <out.z64>

Same steps as the MM decomp's tools/fixbaserom.py (file table, Yaz0, 0xFF padding, CIC-6105 CRC), in plain Python so
the build needs no extra tools. Checks the input and output against the decomp's published MD5s.
"""
import hashlib
import struct
import sys

COMPRESSED_MD5 = "2a0a8acb61538235bc1094d297fb6556"
UNCOMPRESSED_MD5 = "f46493eaa0628827dbd6ad3ecd8d65d6"
UNCOMPRESSED_SIZE = 0x2F00000
PAD_FROM = 0x2EE8000
FILE_TABLE = 0x1A500


def to_big_endian(rom: bytearray) -> bytearray:
    if rom[0] == 0x80:
        return rom
    if rom[0] == 0x37:  # byte-swapped (.v64)
        out = bytearray(len(rom))
        out[0::2], out[1::2] = rom[1::2], rom[0::2]
        return out
    if rom[0] == 0x40:  # little-endian words (.n64)
        n = len(rom) // 4
        return bytearray(struct.pack(f">{n}I", *struct.unpack(f"<{n}I", rom)))
    raise SystemExit("not an N64 ROM")


def yaz0(src: bytes) -> bytes:
    if src[:4] != b"Yaz0":
        raise SystemExit("bad Yaz0 header")
    size = struct.unpack_from(">I", src, 4)[0]
    dst = bytearray(size)
    s, d = 16, 0
    while d < size:
        header = src[s]
        s += 1
        for bit in range(7, -1, -1):
            if d >= size:
                break
            if header & (1 << bit):
                dst[d] = src[s]
                d += 1
                s += 1
            else:
                b1, b2 = src[s], src[s + 1]
                s += 2
                back = d - (((b1 & 0x0F) << 8) | b2) - 1
                count = b1 >> 4
                if count == 0:
                    count = src[s] + 0x12
                    s += 1
                else:
                    count += 2
                for _ in range(count):  # overlapping copies are intended
                    dst[d] = dst[back]
                    d += 1
                    back += 1
    return bytes(dst)


def crc_6105(rom: bytearray) -> bytes:
    mask = 0xFFFFFFFF
    t1 = t2 = t3 = t4 = t5 = t6 = 0xDF26F436
    for pos in range(0x1000, 0x101000, 4):
        d = struct.unpack_from(">I", rom, pos)[0]
        r = ((d << (d & 0x1F)) | (d >> (-(d & 0x1F) & 0x1F))) & mask
        t6d = (t6 + d) & mask
        if t6d < t6:
            t4 = (t4 + 1) & mask
        t6 = t6d
        t3 ^= d
        t5 = (t5 + r) & mask
        t2 = t2 ^ r if t2 > d else t2 ^ t6 ^ d
        t1 = (t1 + (struct.unpack_from(">I", rom, 0x0750 + (pos & 0xFF))[0] ^ d)) & mask
    return struct.pack(">II", t6 ^ t4 ^ t3, t5 ^ t2 ^ t1)


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    rom = to_big_endian(bytearray(open(sys.argv[1], "rb").read()))
    if hashlib.md5(rom).hexdigest() != COMPRESSED_MD5:
        raise SystemExit("not the Majora's Mask US ROM Zelda64Recomp needs (MD5 mismatch)")

    out = bytearray(UNCOMPRESSED_SIZE)
    table = bytearray()
    addr = FILE_TABLE
    while True:
        entry = struct.unpack_from(">IIII", rom, addr)
        if not any(entry):
            break
        v_start, v_end, p_start, p_end = entry
        addr += 16
        if p_start == 0xFFFFFFFF and p_end == 0xFFFFFFFF:
            table += struct.pack(">IIII", *entry)
            continue
        data = rom[p_start:p_start + v_end - v_start] if p_end == 0 else yaz0(rom[p_start:p_end])
        out[v_start:v_start + len(data)] = data
        table += struct.pack(">IIII", v_start, v_end, v_start, 0)
    out[FILE_TABLE:FILE_TABLE + len(table)] = table
    out[PAD_FROM:UNCOMPRESSED_SIZE] = b"\xff" * (UNCOMPRESSED_SIZE - PAD_FROM)
    out[0x10:0x18] = crc_6105(out)

    if hashlib.md5(out).hexdigest() != UNCOMPRESSED_MD5:
        raise SystemExit("decompressed ROM doesn't match the expected MD5")
    open(sys.argv[2], "wb").write(out)


if __name__ == "__main__":
    main()

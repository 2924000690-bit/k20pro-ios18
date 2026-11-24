#!/usr/bin/env python3
"""U26: force request_elements() to use the 5 GHz 8-rate table.

Usage: python3 patch_u26.py kernel.u24-5g-vht kernel.u26-5g-rates

Input is a raw kernel.macho from EFI/UniXNU/Payloads/kernel.macho.
The patch branches from request_elements to a code cave, loads a fake
band descriptor (rate table + n_bitrates=8), and forces basic rates
6/12/24 (mask 0x15).
"""
import hashlib
import struct
import sys

BASE = 0xfffffe0007004000
CAVE = 0xfffffe000754d3d8
FAKE = 0xfffffe000754d420
TABLE = 0xfffffe0007d393f0
RET = 0xfffffe0007c31760
SITE = 0xfffffe0007c31758


def off(va):
    return va - BASE


def b_enc(pc, target):
    d = target - pc
    assert d % 4 == 0 and -(1 << 27) <= d < (1 << 27)
    return 0x14000000 | ((d >> 2) & 0x03FFFFFF)


def adrp_enc(pc, target, rd):
    imm = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12
    assert -(1 << 20) <= imm < (1 << 20)
    i = imm & 0x1FFFFF
    return 0x90000000 | ((i & 3) << 29) | ((i >> 2) << 5) | rd


def add_imm_enc(rn, rd, imm):
    assert 0 <= imm < 4096
    return 0x91000000 | (imm << 10) | (rn << 5) | rd


def ldr_w_enc(rn, rt, imm):
    assert imm % 4 == 0 and 0 <= imm < 16384
    return 0xB9400000 | ((imm >> 2) << 10) | (rn << 5) | rt


src, dst = sys.argv[1], sys.argv[2]
b = bytearray(open(src, "rb").read())

assert bytes(b[off(SITE):off(SITE) + 4]).hex() == "191d40f9"
b[off(SITE):off(SITE) + 4] = struct.pack("<I", b_enc(SITE, CAVE))

words = [
    adrp_enc(CAVE, FAKE, 25),
    add_imm_enc(25, 25, FAKE & 0xFFF),
    ldr_w_enc(25, 27, 28),
    b_enc(CAVE + 12, RET),
]
b[off(CAVE):off(CAVE) + 16] = b"".join(struct.pack("<I", w) for w in words)

b[off(FAKE):off(FAKE) + 64] = b"\x00" * 64
b[off(FAKE) + 8:off(FAKE) + 16] = struct.pack("<Q", TABLE)
b[off(FAKE) + 28:off(FAKE) + 32] = struct.pack("<I", 8)

assert bytes(b[0xC2D7D4:0xC2D7D8]).hex() == "8e7240f9"
assert bytes(b[0xC2D7D8:0xC2D7DC]).hex() == "ce9541b9"
b[0xC2D7D4:0xC2D7D8] = struct.pack("<I", 0x528002AE)  # mov w14,#0x15
b[0xC2D7D8:0xC2D7DC] = struct.pack("<I", 0xD503201F)  # nop

open(dst, "wb").write(b)
print("sha256", hashlib.sha256(b).hexdigest().upper())

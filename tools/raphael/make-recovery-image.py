#!/usr/bin/env python3
"""Repack a v0 boot image as the v2 recovery image this ABL accepts.

The recovery partition on raphael boots header v2 images with a recovery_dtbo and
a dtb (the one-click package's ios-recovery.img is exactly that); a plain v0 boot
image that boots fine through `fastboot boot` is refused from the partition and
falls back to fastboot.

    python3 tools/raphael/make-recovery-image.py \\
        --boot ../wifi-test/boot-ios18-skywalk-uc.img \\
        --dtbo /tmp/oc-dtbo.img --dtb /tmp/oc-dtb.bin \\
        --out /tmp/rec.img --name iOS18REC

The kernel, ramdisk, addresses and cmdline come from the v0 image; dtbo and dtb
come from the working recovery image. The result is padded to 64 MiB and gets an
unsigned AVB hash footer (avbtool add_hash_footer --algorithm NONE).
"""
import argparse
import hashlib
import struct
import subprocess
import sys
from pathlib import Path

PAGE = 4096
DTB_ADDR = 0x1F00000
HEADER_V2 = 1660


def read_v0(path):
    d = Path(path).read_bytes()
    if d[:8] != b"ANDROID!":
        raise SystemExit(f"{path}: not an Android boot image")
    (ks, ka, rs, ra, ss, sa, tags, page) = struct.unpack_from("<IIIIIIII", d, 8)
    hv = struct.unpack_from("<I", d, 40)[0]
    if hv != 0:
        raise SystemExit(f"{path}: header version {hv}, this tool reads v0")
    cmdline = d[64:64 + 512].split(b"\0")[0]
    name = d[48:64].split(b"\0")[0]
    kernel = d[page:page + ks]
    ramdisk = d[page + ((ks + page - 1) // page) * page:][:rs]
    return dict(kernel=kernel, ramdisk=ramdisk, ka=ka, ra=ra, tags=tags, page=page,
                cmdline=cmdline, name=name)


def align(n, a=PAGE):
    return (n + a - 1) // a * a


def build(boot, dtbo, dtb, out, name, partition_size, avbtool=None):
    b = read_v0(boot)
    dtbo = Path(dtbo).read_bytes()
    dtb = Path(dtb).read_bytes()
    if dtbo[:4] != bytes.fromhex("d7b7ab1e"):
        raise SystemExit("dtbo: bad magic")
    if dtb[:4] != bytes.fromhex("d00dfeed"):
        raise SystemExit("dtb: bad magic")

    k_off = PAGE
    r_off = align(k_off + len(b["kernel"]))
    dtbo_off = align(r_off + len(b["ramdisk"]))
    dtb_off = align(dtbo_off + len(dtbo))
    total = align(dtb_off + len(dtb))

    digest = hashlib.sha1()
    for data in (b["kernel"], b["ramdisk"], b"", dtbo, dtb):
        digest.update(data)
        digest.update(struct.pack("<I", len(data)))
    image_id = digest.digest() + bytes(12)

    hdr = bytearray(HEADER_V2)
    hdr[0:8] = b"ANDROID!"
    struct.pack_into("<IIIIIIII", hdr, 8, len(b["kernel"]), b["ka"], len(b["ramdisk"]), b["ra"],
                     0, 0, b["tags"], PAGE)
    struct.pack_into("<I", hdr, 40, 2)          # header_version
    struct.pack_into("<I", hdr, 44, 0)          # os_version
    hdr[48:64] = name.encode()[:16].ljust(16, b"\0")
    hdr[64:64 + len(b["cmdline"])] = b["cmdline"]
    hdr[576:608] = image_id
    struct.pack_into("<I", hdr, 1632, len(dtbo))
    struct.pack_into("<Q", hdr, 1636, dtbo_off)
    struct.pack_into("<I", hdr, 1644, HEADER_V2)
    struct.pack_into("<I", hdr, 1648, len(dtb))
    struct.pack_into("<Q", hdr, 1652, DTB_ADDR)

    body = bytearray(total)
    body[0:PAGE] = hdr
    body[k_off:k_off + len(b["kernel"])] = b["kernel"]
    body[r_off:r_off + len(b["ramdisk"])] = b["ramdisk"]
    body[dtbo_off:dtbo_off + len(dtbo)] = dtbo
    body[dtb_off:dtb_off + len(dtb)] = dtb

    out = Path(out)
    out.write_bytes(bytes(body))
    if avbtool:
        salt = hashlib.sha256(bytes(body)).hexdigest()
        subprocess.run([sys.executable, "-B", str(avbtool), "add_hash_footer",
                        "--image", str(out), "--partition_name", "recovery",
                        "--partition_size", str(partition_size),
                        "--algorithm", "NONE", "--hash_algorithm", "sha256",
                        "--salt", salt], check=True)
    print(f"{out}: {out.stat().st_size} bytes, dtbo {len(dtbo)} @ {dtbo_off}, "
          f"dtb {len(dtb)} @ {dtb_off}, id {image_id.hex()[:16]}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--boot", required=True)
    p.add_argument("--dtbo", required=True)
    p.add_argument("--dtb", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--name", default="iOS18REC")
    p.add_argument("--partition-size", type=lambda v: int(v, 0), default=64 * 1024 ** 2)
    p.add_argument("--avbtool", default=None)
    a = p.parse_args()
    build(a.boot, a.dtbo, a.dtb, a.out, a.name, a.partition_size, a.avbtool)


if __name__ == "__main__":
    main()

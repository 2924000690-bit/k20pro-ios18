#!/usr/bin/env python3
"""Build the dual-boot boot image: UniXNU menu with the iOS 18 entry and an Android entry.

Same packing as wifi-test/mkboot-small.sh (header v0, the base image's UEFI shim kernel,
the FAT32 ramdisk replaced), plus:

  * Payloads/android/Image          the Android kernel (raw ARM64 Image, decompressed)
  * Payloads/android/Android.dtb    the dtb appended after it in the stock boot image
  * Payloads/android/Android.ramdisk the stock ramdisk (lz4/cpio as shipped)
  * a [[entry]] type = "linux" in unixnu.toml with the stock Android cmdline

The result is padded to the boot partition and gets the unsigned AVB hash footer.

    python3 tools/raphael/mkboot-dual.py --base BASE.img --kernel KERNEL --adt ADT \\
        --android-image Image --android-dtb dtb --android-ramdisk ramdisk \\
        --android-cmdline-file cmdline.txt --out boot.img [--fat-mib 96] [--avbtool avbtool.py]
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

PAGE = 4096
MTOOLS_ENV = dict()


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        raise SystemExit(f"{cmd[0]} failed: {r.stderr.strip()}")
    return r.stdout


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--base", required=True)
    p.add_argument("--kernel", required=True)
    p.add_argument("--adt", required=True)
    p.add_argument("--android-image", default=None)
    p.add_argument("--android-dtb", default=None)
    p.add_argument("--android-ramdisk", default=None)
    p.add_argument("--android-cmdline-file", default=None)
    p.add_argument("--loader", default=None, help="LinuxLoader.efi; boots the Android image from a payload file")
    p.add_argument("--android-boot", default=None, help="Android boot image file for the loader (file= mode)")
    p.add_argument("--out", required=True)
    p.add_argument("--title", default="Redmi K20 Pro")
    p.add_argument("--fat-mib", type=int, default=96)
    p.add_argument("--avbtool", default=None)
    p.add_argument("--partition-size", type=lambda v: int(v, 0), default=128 * 1024 ** 2)
    a = p.parse_args()

    work = Path(tempfile.mkdtemp())
    try:
        base = Path(a.base).read_bytes()
        if base[:8] != b"ANDROID!":
            raise SystemExit("base: not an Android boot image")
        ks, ka, rs, ra, ss, sa, tags, page, hv = struct.unpack_from("<9I", base, 8)
        if hv != 0 or ss != 0:
            raise SystemExit(f"base: expected v0 without second stage (v{hv}, second {ss})")
        roff = page + (ks + page - 1) // page * page
        fat = base[roff:roff + rs]
        if fat[0x52:0x5a] != b"FAT32   ":
            raise SystemExit("base: ramdisk is not FAT32")
        (work / "header.bin").write_bytes(base[:page])
        (work / "kernel.bin").write_bytes(base[page:page + ks])
        (work / "base-fat.img").write_bytes(fat)

        env = dict(os.environ)
        env["MTOOLS_SKIP_CHECK"] = "1"
        tree = work / "tree"
        tree.mkdir()
        run(["mcopy", "-s", "-i", str(work / "base-fat.img"), "::/EFI", str(tree)], env=env)
        toml = tree / "EFI/UniXNU/unixnu.toml"
        text = toml.read_text()
        if a.loader and a.android_boot:
            entry = '''
[[entry]]
id = "android"
title = "Android"
type = "efi"
path = "Payloads/LinuxLoader.efi"
args = "file=Android.img"
'''
        else:
            cmdline = Path(a.android_cmdline_file).read_text().strip()
            if '"' in cmdline:
                raise SystemExit("android cmdline contains a quote")
            entry = f'''
[[entry]]
id = "android"
title = "Android"
type = "linux"
path = "Payloads/android/Image"
initrd = "Payloads/android/Android.ramdisk"
dtb = "Payloads/android/Android.dtb"
args = "{cmdline}"
'''
        import re
        text = re.sub(r"(?m)^timeout = \d+$", "timeout = 5", text)
        toml.write_text(text.rstrip("\n") + "\n" + entry)

        newfat = work / "new-fat.img"
        with open(newfat, "wb") as fh:
            fh.truncate(a.fat_mib * 1024 * 1024)
        run(["mkfs.vfat", "-F", "32", "-n", "GUACESP", str(newfat)], env=env)
        run(["mcopy", "-s", "-i", str(newfat), str(tree / "EFI"), "::/"], env=env)
        run(["mcopy", "-o", "-i", str(newfat), a.kernel, "::/EFI/UniXNU/Payloads/kernel.macho"], env=env)
        run(["mcopy", "-o", "-i", str(newfat), a.adt, "::/EFI/UniXNU/Payloads/kernel.macho.adt"], env=env)
        if a.loader and a.android_boot:
            run(["mcopy", "-o", "-i", str(newfat), a.loader, "::/EFI/UniXNU/Payloads/LinuxLoader.efi"], env=env)
            run(["mcopy", "-o", "-i", str(newfat), a.android_boot, "::/Android.img"], env=env)
        else:
            run(["mmd", "-i", str(newfat), "::/EFI/UniXNU/Payloads/android"], env=env)
            for src, dst in ((a.android_image, "Image"), (a.android_dtb, "Android.dtb"),
                             (a.android_ramdisk, "Android.ramdisk")):
                run(["mcopy", "-o", "-i", str(newfat), src, f"::/EFI/UniXNU/Payloads/android/{dst}"], env=env)
        run(["mcopy", "-o", "-i", str(newfat), str(toml), "::/EFI/UniXNU/unixnu.toml"], env=env)
        back = run(["mtype", "-i", str(newfat), "::/EFI/UniXNU/unixnu.toml"], env=env)
        if ("type = \"linux\"" not in back) and ("LinuxLoader.efi" not in back):
            raise SystemExit("toml readback lost the linux entry")

        f = newfat.read_bytes()
        h = bytearray((work / "header.bin").read_bytes())
        k = (work / "kernel.bin").read_bytes()
        struct.pack_into("<I", h, 16, len(f))
        blob = bytes(h) + k + b"\0" * (-len(k) % page) + f
        blob += b"\0" * (-len(blob) % page)
        out = Path(a.out)
        out.write_bytes(blob)
        print(f"{out}: {len(blob)} bytes (fat {len(f)}, kernel {len(k)})")

        if a.avbtool:
            import hashlib
            subprocess.run([sys.executable, "-B", str(a.avbtool), "add_hash_footer",
                            "--image", str(out), "--partition_name", "boot",
                            "--partition_size", str(a.partition_size),
                            "--algorithm", "NONE", "--hash_algorithm", "sha256",
                            "--salt", hashlib.sha256(blob).hexdigest()], check=True)
            print(f"{out}: {out.stat().st_size} bytes with AVB footer")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Build the CPU accuracy test: build/cputest.img, a 16 MB disk image to
boot from a hard disk or a CF card."""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "build")

CFLAGS = [
    "-m32", "-march=i386", "-Os", "-std=gnu11",
    "-ffreestanding", "-fno-pic", "-fno-pie", "-fno-stack-protector",
    "-fno-asynchronous-unwind-tables", "-fno-builtin", "-nostdlib",
    "-Wall", "-Wextra", "-Wno-unused-parameter",
]


def dump_flags():
    """--dump, optionally followed by a group-name prefix: -DDUMP and
    -DDUMP_ONLY="prefix"."""
    if "--dump" not in sys.argv:
        return []
    i = sys.argv.index("--dump")
    if i + 1 < len(sys.argv) and not sys.argv[i + 1].startswith("-"):
        return ["-DDUMP", '-DDUMP_ONLY="%s"' % sys.argv[i + 1]]
    return ["-DDUMP"]


def run(cmd):
    print(" ".join(cmd))
    subprocess.run(cmd, check=True, cwd=OUT)


# The musl cross compiler CORSAC's own Linux programs are built with.
MUSL_GCC = os.path.expanduser(
    "~/projects/corsac86-gui/build/linuxbin/i686-linux-musl-cross/bin/i686-linux-musl-gcc")


def build_host():
    """The harness as a static i386 ELF: runs on this Linux host and on
    CORSAC alike, and on anything back to a 486 (no CMOV)."""
    extra = dump_flags()
    out = "cputest-dump" if extra else "cputest"
    run([MUSL_GCC, "-static", "-no-pie", "-fno-pie", "-march=i486", "-mtune=i486", "-O2", "-std=gnu11", "-DHOSTTEST"] + extra + [
         "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function",
         "-I", HERE, "-o", out, os.path.join(HERE, "harness.c")])


def main():
    os.makedirs(OUT, exist_ok=True)
    if "--host" in sys.argv:
        build_host()
        return
    run(["nasm", "-f", "elf32", "-o", "rt.o", os.path.join(HERE, "rt.asm")])
    extra = dump_flags()
    run(["gcc"] + CFLAGS + extra + ["-I", HERE, "-c", "-o", "harness.o", os.path.join(HERE, "harness.c")])
    run(["ld", "-m", "elf_i386", "-T", os.path.join(HERE, "payload.ld"), "-o", "payload.elf", "rt.o", "harness.o"])
    run(["objcopy", "-O", "binary", "payload.elf", "payload.bin"])

    payload = os.path.getsize(os.path.join(OUT, "payload.bin"))
    # The loader and payload, in whole 16 KB steps: the boot sector reads
    # 32 sectors at a time, and loads to 1000:0000, below 640 KB.
    size = (payload + 512 + 16383) // 16384 * 16384
    if size > 0x80000:
        sys.exit("payload too large: %d bytes" % payload)

    run(["nasm", "-f", "bin", "-DIMAGE_BLOCKS=%d" % min(size // 512, 255), "-I", OUT + "/",
         "-o", "stub.bin", os.path.join(HERE, "stub.asm")])
    loader = bytearray(open(os.path.join(OUT, "stub.bin"), "rb").read())
    loader += bytes(size - len(loader))
    print("loader and payload: %d bytes (payload %d)" % (size, payload))

    # Behind a boot sector, for a disk or a CF card: the boot sector loads
    # it and far-calls offset 3.
    run(["nasm", "-f", "bin", "-DIMAGE_SECTORS=%d" % (size // 512), "-o", "boot.bin",
         os.path.join(HERE, "boot.asm")])
    boot = open(os.path.join(OUT, "boot.bin"), "rb").read()
    assert len(boot) == 512 and boot[510:] == b"\x55\xaa"
    img = boot + bytes(loader)
    # 15 MB (30720 sectors): fits a card sold as 16 MB, which holds fewer.
    img += bytes((15 << 20) - len(img))
    path = os.path.join(OUT, "cputest.img")
    open(path, "wb").write(img)
    print("%s: %d bytes" % (path, len(img)))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Build the CPU accuracy test option ROM: cputest.rom (32 KB, or 64 KB if
the payload needs it), ready for an ISA ROM card at C8000h."""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "build")

CFLAGS = [
    "-m32", "-march=i386", "-O2", "-std=gnu11",
    "-ffreestanding", "-fno-pic", "-fno-pie", "-fno-stack-protector",
    "-fno-asynchronous-unwind-tables", "-fno-builtin", "-nostdlib",
    "-Wall", "-Wextra", "-Wno-unused-parameter",
]


def run(cmd):
    print(" ".join(cmd))
    subprocess.run(cmd, check=True, cwd=OUT)


# The musl cross compiler CORSAC's own Linux programs are built with.
MUSL_GCC = os.path.expanduser(
    "~/projects/corsac86-gui/build/linuxbin/i686-linux-musl-cross/bin/i686-linux-musl-gcc")


def build_host():
    """The harness as a static i386 ELF: runs on this Linux host and on
    CORSAC alike, and on anything back to a 486 (no CMOV)."""
    extra = ["-DDUMP"] if "--dump" in sys.argv else []
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
    extra = ["-DDUMP"] if "--dump" in sys.argv else []
    run(["gcc"] + CFLAGS + extra + ["-I", HERE, "-c", "-o", "harness.o", os.path.join(HERE, "harness.c")])
    run(["ld", "-m", "elf_i386", "-T", os.path.join(HERE, "payload.ld"), "-o", "payload.elf", "rt.o", "harness.o"])
    run(["objcopy", "-O", "binary", "payload.elf", "payload.bin"])

    payload = os.path.getsize(os.path.join(OUT, "payload.bin"))
    for size in (32768, 65536):
        if payload + 512 <= size:
            break
    else:
        sys.exit("payload too large: %d bytes" % payload)

    run(["nasm", "-f", "bin", "-DROM_BLOCKS=%d" % (size // 512), "-I", OUT + "/",
         "-o", "stub.bin", os.path.join(HERE, "stub.asm")])
    rom = bytearray(open(os.path.join(OUT, "stub.bin"), "rb").read())
    if len(rom) > size - 1:
        sys.exit("ROM too large: %d bytes" % len(rom))
    rom += bytes(size - len(rom))
    rom[-1] = (-sum(rom[:-1])) & 0xff
    assert sum(rom) & 0xff == 0
    path = os.path.join(OUT, "cputest.rom")
    open(path, "wb").write(rom)
    print("%s: %d bytes (payload %d)" % (path, size, payload))

    # The same image behind a boot sector, for a disk or a CF card: the boot
    # sector loads it and calls it the way a BIOS calls an option ROM.
    run(["nasm", "-f", "bin", "-DIMAGE_SECTORS=%d" % (size // 512), "-o", "boot.bin",
         os.path.join(HERE, "boot.asm")])
    boot = open(os.path.join(OUT, "boot.bin"), "rb").read()
    assert len(boot) == 512 and boot[510:] == b"\x55\xaa"
    img = boot + bytes(rom)
    img += bytes((1 << 20) - len(img))  # 1 MB; dd writes it to the start of the card
    path = os.path.join(OUT, "cputest.img")
    open(path, "wb").write(img)
    print("%s: %d bytes" % (path, len(img)))


if __name__ == "__main__":
    main()

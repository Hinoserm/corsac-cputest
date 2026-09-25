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


def build_host():
    """The harness as a 32-bit Linux program, to check it on real silicon."""
    run(["gcc", "-m32", "-march=i386", "-O2", "-std=gnu11", "-DHOSTTEST", "-no-pie", "-fno-pie",
         "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function",
         "-I", HERE, "-o", "cputest-host", os.path.join(HERE, "harness.c")])


def main():
    os.makedirs(OUT, exist_ok=True)
    if "--host" in sys.argv:
        build_host()
        return
    run(["nasm", "-f", "elf32", "-o", "rt.o", os.path.join(HERE, "rt.asm")])
    run(["gcc"] + CFLAGS + ["-I", HERE, "-c", "-o", "harness.o", os.path.join(HERE, "harness.c")])
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


if __name__ == "__main__":
    main()

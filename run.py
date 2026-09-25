#!/usr/bin/env python3
"""Run the CPU accuracy test ROM on a headless 86Box.

  run.py [--machine 54tdp] [--cpu FAMILY SPEED MULTI] [--cpus N]
         [--interp] [--box PATH] [--display N] [--timeout S]

Builds cputest.rom, writes a case folder under ~/src/86suite/machines
(cputest-<machine>, no disks, the ROM on an ISA ROM card at C8000h, COM1 to
this script), boots it through box86.py, and prints the ROM's report as it
arrives. Exits 0 on PASS, 1 on FAIL, 2 if the run never finished.
"""

import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SUITE = os.path.expanduser("~/src/86suite")
ROMS = os.path.expanduser("~/.local/share/86Box/roms")

# Machines this knows how to set up: board, NVR to start from, video card.
MACHINES = {
    "54tdp": dict(machine="54tdp", nvr=os.path.join(SUITE, "machines/w2ksuite/nvr.pristine"),
                  gfxcard="voodoo_banshee_migrated_pci",
                  cpu=("pentium_p55c", 233333333, "3.5")),
}

CFG = """[General]
emu_build_dynarec_type = new
force_10ms = 1
vid_renderer = qt_software

[Machine]
cpu_count = {cpus}
cpu_family = {family}
cpu_multi = {multi}
cpu_speed = {speed}
cpu_use_dynarec = {dynarec}
fpu_type = internal
machine = {machine}
mem_size = 65536
pit_mode = 1

[Video]
gfxcard = {gfxcard}

[Input devices]
keyboard_type = keyboard_at
mouse_type = none

[Ports (COM & LPT)]
lpt1_enabled = 0
serial1_device = pipe

[Named Pipe (COM) #1]
path = /dev/null
mode = 0

[Floppy and CD-ROM drives]
fdd_01_type = none
fdd_02_type = none

[Other peripherals]
isarom0_type = isarom

[Generic ISA ROM Board #1]
bios_fn = {rom}
bios_addr = C8000
bios_size = {romsize}
rom_writes_enabled = 0
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--machine", default="54tdp", choices=sorted(MACHINES))
    ap.add_argument("--cpu", nargs=3, metavar=("FAMILY", "SPEED", "MULTI"))
    ap.add_argument("--cpus", type=int, default=1)
    ap.add_argument("--interp", action="store_true", help="recompiler off (where the CPU allows it)")
    ap.add_argument("--box", default=os.path.expanduser("~/src/86Box/buildfast/src/86Box"))
    ap.add_argument("--display", type=int, default=91)
    ap.add_argument("--timeout", type=int, default=1200)
    ap.add_argument("--tag", default="")
    args = ap.parse_args()

    subprocess.run([sys.executable, os.path.join(HERE, "build.py")], check=True, stdout=subprocess.DEVNULL)
    rom = os.path.join(HERE, "build", "cputest.rom")

    m = MACHINES[args.machine]
    family, speed, multi = args.cpu if args.cpu else m["cpu"]
    name = "cputest-" + args.machine + (("-" + args.tag) if args.tag else "")
    case = os.path.join(SUITE, "machines", name)
    os.makedirs(case, exist_ok=True)
    # A fresh NVR every run: the ROM must not depend on what a last run left.
    if os.path.exists(os.path.join(case, "nvr")):
        shutil.rmtree(os.path.join(case, "nvr"))
    shutil.copytree(m["nvr"], os.path.join(case, "nvr"))
    if not os.path.islink(os.path.join(case, "roms")):
        os.symlink(ROMS, os.path.join(case, "roms"))
    # A private copy of the emulator, so a rebuild elsewhere can't swap it
    # underneath a run.
    box = os.path.join(case, "86Box.bin")
    shutil.copy2(args.box, box)
    romcopy = os.path.join(case, "cputest.rom")
    shutil.copy2(rom, romcopy)
    open(os.path.join(case, "86box.cfg"), "w").write(CFG.format(
        cpus=args.cpus, family=family, multi=multi, speed=speed, dynarec=0 if args.interp else 1,
        machine=m["machine"], gfxcard=m["gfxcard"], rom=romcopy, romsize=os.path.getsize(romcopy)))

    os.environ["BOX86"] = box
    sys.path.insert(0, SUITE)
    import box86

    b = box86.Box(case, display=args.display)
    print("case %s  emulator %s  cpu %s %s x%s  cpus %d  %s" % (
        case, args.box, family, speed, multi, args.cpus, "interpreter" if args.interp else "recompiler"), flush=True)
    b.start()
    result = 2
    try:
        start = time.time()
        shown = 0
        last_shot = time.time()
        while time.time() - start < args.timeout:
            b.pump(2.0)
            text = b.buf.decode("latin-1")
            if len(text) > shown:
                print(text[shown:], end="", flush=True)
                shown = len(text)
            if "DONE tests=" in text and text.rstrip().endswith(("PASS", "FAIL")):
                result = 0 if text.rstrip().endswith("PASS") else 1
                break
            if b.box.poll() is not None:
                print("\nemulator exited (%s)" % b.box.returncode)
                break
            if time.time() - last_shot > 60:
                b.shot("screen.png")
                last_shot = time.time()
        else:
            print("\ntimed out after %d s" % args.timeout)
        b.shot("screen.png")
        print("\nelapsed %.0f s" % (time.time() - start))
    finally:
        b.stop()
    sys.exit(result)


if __name__ == "__main__":
    main()

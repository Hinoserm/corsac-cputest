# corsac-cputest

A bare-metal x86 CPU accuracy test. I use it to check 86Box's recompiler
against its interpreter, and both against real CPUs, from the 486 up.

It runs with no OS at all, from either of these:

- **`cputest.img`**: a 16 MB disk image. Write it to a CF card or hard disk
  and boot it. The boot sector loads the test and calls it the way a BIOS
  calls an option ROM.
- **`build/cputest.rom`**: the same test as a 32 KB option ROM, for an ISA
  ROM card at C8000h (86Box: *Generic ISA ROM Board*).

Output goes to COM1 at 115200 8N1, and to the screen. The top line of the
screen shows live status.

## Running it on real hardware

```
dd if=cputest.img of=/dev/sdX bs=1M
```

Boot from the card. Each pass runs every group, then starts again, forever.
Every pass must print the same CRCs.

## What it does

Every test is a few bytes of code generated at run time:

1. load EFLAGS and seven registers from an input record;
2. optionally run a *producer* instruction (CMP, ADD, SHL, ...) that leaves
   the flags in one of the lazy states an emulator tracks, with or without a
   jump that starts a new block;
3. run the instruction under test;
4. store EFLAGS and the registers.

Each copy is placed at an address never used for code before, and run four
times on the same input. 86Box interprets a new block on its first run,
interprets it again while compiling it on the second, and runs the compiled
code from the third on. So runs 1-2 are the interpreter and runs 3-4 are the
recompiler, and any difference between them is reported as a `MISMATCH` with
the registers of all four runs. On real hardware all four runs are the same
by definition.

Memory operands point into a 16 KB sandbox that is refilled before every run
and CRC'd after it, so a wrong address shows up as a wrong result instead of
landing in the harness.

## Output

```
START  <group> tests=N
PROGRESS <group> done/total             (every 3 seconds)
GROUP  <group> tests= mismatches= faults= crc_interp= crc_comp= defined= crc_defined= crc_raw=
DONE   pass=N tests= mismatches= arena_wraps= PASS|FAIL
```

- `crc_interp` / `crc_comp`: every result from run 1 and from run 4.
- `crc_defined`: only what the architecture defines. Undefined flags (per
  producer and per instruction) are masked, and so are undefined register
  results. Compare this one between different CPU models, emulated or real.
- `crc_raw`: the same results with nothing masked. Compare this one only
  against the **same** CPU model; it catches undefined-flag behaviour that
  real software sometimes depends on.

Each group's inputs are seeded from its name, so a group's CRCs don't depend
on which groups ran before it.

## Reference values

`crc_defined` from a Zen 5 host running the ELF build. Upstream 86Box
master, emulating a Pentium MMX, gives the same values for every group but
`bt`. On an earlier version of this test, a real Pentium MMX matched the host
in every group.

| group       | crc_defined |
|-------------|-------------|
| alu.rr      | 8237e275    |
| setcc       | e82d1a9c    |
| lahf.sahf   | a4ccdc5b    |
| bswap       | e5c92df6    |
| movzx.movsx | 28f49335    |
| bt          | 78164abb    |

Two interpreter bugs in 86Box turned up this way, both in upstream master:

- **BT, BTS, BTR and BTC with a memory operand and a register bit offset**
  treated the offset as unsigned, so a negative offset addressed the wrong
  byte.
- **ADC** computed AF from the whole second operand instead of its low
  nibble.

## Building

Needs `nasm`, a GCC that can target i386 (`-m32`), and binutils.

```
python3 build.py              # build/cputest.rom and build/cputest.img
python3 build.py --dump       # also prints one line per defined result (D) and raw result (R)
python3 build.py --host       # build/cputest: a static i386 Linux ELF of the same harness
```

`--host` runs the tests on the host CPU as an ordinary process, with faults
arriving as signals. It uses an i686 musl cross compiler; the path is at the
top of `build.py`.

`diffdump.py REF.txt RUN.txt` compares two `--dump` outputs and groups the
differences by instruction, producer and field.

`run.py` boots the ROM or image in a headless 86Box. It relies on my own
headless runner (not published), so it won't work elsewhere as it stands.

## Files

| file         | what                                                        |
|--------------|-------------------------------------------------------------|
| `stub.asm`   | option ROM entry: A20, protected mode, copies the payload to 1 MB |
| `rt.asm`     | runtime: GDT, IDT stubs, `run_kernel`, fault capture        |
| `harness.c`  | emitter, runner, CRCs, serial and screen output             |
| `groups.inc` | the test groups                                             |
| `host.h`     | the Linux side of the `--host` build                        |
| `boot.asm`   | boot sector for the disk image                              |
| `payload.ld` | links the payload at 1 MB                                   |

## License

MIT, see `LICENSE`.

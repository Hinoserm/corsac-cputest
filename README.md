# corsac-cputest

A bare-metal x86 CPU accuracy test. I use it to check 86Box's recompiler
against its interpreter, and both against real CPUs, from the 486 up.

It runs with no OS at all, from either of these:

- **`build/cputest.img`**: a 16 MB disk image. Write it to a CF card or hard disk
  and boot it. The boot sector loads the test and calls it the way a BIOS
  calls an option ROM.
- **`build/cputest.rom`**: the same test as a 64 KB option ROM, for an ISA
  ROM card at C8000h (86Box: *Generic ISA ROM Board*).

Output goes to COM1 at 115200 8N1, and to the screen. The top line of the
screen shows live status. The first line out is `CPUTEST <version>`, then
the CPU's vendor, signature and feature flags, and CR0 as the BIOS left it
and as the tests run. The test turns the CPU's cache on (clears CR0.CD and
NW): BIOSes call option ROMs with it off, and 86Box doesn't compile code
while it is off, so without this a ROM-card run only tests the
interpreter.

## Running it on real hardware

```
dd if=build/cputest.img of=/dev/sdX bs=1M
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

## Groups

New groups only go at the end, so each group's CRCs stay comparable
between versions until the group itself changes.

| # | group          | what                                                            |
|---|----------------|-----------------------------------------------------------------|
| 1 | `alu.rr`       | ADD OR ADC SBB AND SUB XOR CMP, register forms, 8/16/32-bit     |
| 2 | `setcc`        | all 16 conditions, byte registers and memory, after every producer |
| 3 | `lahf.sahf`    | LAHF, SAHF and both orders                                      |
| 4 | `bswap`        | BSWAP, with and without an operand-size prefix                  |
| 5 | `movzx.movsx`  | byte and word sources, 16/32-bit destinations                   |
| 6 | `bt`           | BT BTS BTR BTC, register and immediate offsets, into memory     |
| 7 | `imul`         | IMUL r, r/m and IMUL r, r/m, imm8/imm                           |
| 8 | `mul.div`      | MUL IMUL DIV IDIV, 8/16/32-bit, #DE included                    |
| 9 | `shift.rotate` | all eight ops by CL, by 1 and by immediate, RCL/RCR included    |
| 10 | `shld.shrd`   | by CL and immediate, 16/32-bit, register and memory             |
| 11 | `bsf.bsr`     | zero, single-bit and random sources                             |
| 12 | `cmpxchg.xadd` | CMPXCHG, XADD, LOCK forms, #UD for LOCK on a register, CMPXCHG8B |
| 13 | `bcd`         | DAA DAS AAA AAS AAM AAD (any base, AAM 0 = #DE), SALC           |
| 14 | `string`      | MOVS CMPS STOS LODS SCAS, REP/REPE/REPNE, both directions, 16-bit addressing |
| 15 | `popf.pushf`  | POPF/POPFD of any flags but TF and IF, PUSHF of every lazy state |
| 16 | `stack`       | PUSH ESP, POP [ESP], PUSHA/POPA, POP SS, ENTER (nesting 0-33), LEAVE |
| 17 | `far.call`    | far CALL/JMP direct and indirect, RETF, RETF n, RET n, IRETD, in protected mode |
| 18 | `int.ud`      | INT3, INT n, INTO, ICEBP, INT past the IDT (#GP), invalid encodings (#UD) |
| 19 | `bound.arpl`  | BOUND inside, on and outside the bounds (#BR), ARPL             |
| 20 | `misc`        | XLAT, CMC CLC STC CLD STD, WAIT, PAUSE, XCHG with memory        |
| 21 | `mmx`         | every MMX instruction: arithmetic, packs, compares, shifts by register and immediate (with the #UD holes), MOVD/MOVQ |
| 22 | `mmx.x87`     | EMMS and the x87 tag and status words after MMX, EMMS and FLD   |
| 23 | `3dnow`       | 3DNow! and the K6-2+/K6-III+ extensions, FEMMS, PREFETCH(W)     |
| 24 | `cmov`        | CMOVcc, all conditions, 16/32-bit, register and memory, after every producer (P6 on) |
| 25 | `rdtsc`       | two RDTSCs back to back: the difference is positive and small (Pentium on) |
| 26 | `nop.p6`      | multi-byte NOP and the reserved NOPs 0F 19-1E: NOPs on P6, #UD before; its CRCs differ by family on purpose |
| 27 | `smc`         | self-modifying code: patched immediates and opcodes, and a loop that repatches its own block; without a jump only in `crc_raw` (a 486 may run stale prefetched bytes) |
| 28 | `segments`    | FS/GS loads of good, null, 16-bit, RPL-3, out-of-GDT and LDT selectors (#GP), limits, LFS/LGS, MOV from Sreg, LAR LSL VERR VERW |
| 29 | `exhaust8`    | every input of the 8-bit flag math, walked by a loop inside the test: ADD..CMP × AL × BL × CF, INC DEC NEG NOT, shifts and rotates × counts 0-31, DAA DAS AAA AAS × AF × CF, MUL IMUL, AAM AAD, SAHF |
| 30 | `lea`         | every 32-bit ModRM form, SIB with every base and every index×scale at each mod, every 16-bit form, displacement edges, ESP as the base |
| 31 | `alu.mem`     | the eight ALU ops on memory: both directions, 80/81/83 immediates at their edges, every size, LOCK (#UD on CMP), misaligned |
| 32 | `unary.mem`   | INC DEC NEG NOT TEST, register and memory, every size, LOCK, and the one-byte INC/DEC; INC/DEC after every producer (CF passes through) |
| 33 | `jcc.loop`    | all 16 Jcc, short and near, after every producer; JECXZ/JCXZ; LOOP/LOOPE/LOOPNE over a real body, 32- and 16-bit counts (CX = 0: 65536 times) |
| 34 | `mov.forms`   | moffs A0-A3, C6/C7 through [reg+disp32] (a form 86Box's recompiler interprets on purpose), MOV to/from memory with high bytes, SIB, misaligned, FS:, CBW CWDE CWD CDQ |
| 35 | `prefixes`    | repeated and ignored prefixes, prefix order, stacked segment overrides, branch hints, and the 15-byte limit (#GP at 16) |
| 36 | `code16`      | code in a 16-bit protected-mode segment (Windows 3.x/9x, DOS extenders): ALU at 16 and 32 bits, SETcc and Jcc after every producer, every 16-bit address form, MOVZX/MOVSX, PUSH/POP/PUSHF, MUL/DIV edges, string ops over SI/DI, LOOP over CX, near CALL/RET |
| 37-178 | `loop.<op><size>` | sampled loops, 4096 edge-biased operand sets each, results and flags folded into checksums: ADD OR ADC SBB AND SUB XOR CMP at 16 and 32 bits; INC DEC NEG NOT; ROL ROR RCL RCR SHL SHR SAR by every count; SHLD SHRD; MUL IMUL (one-operand), IMUL r,r/m and IMUL r,r/m,imm; DIV IDIV at 8, 16 and 32 bits, constrained to fit; BSF BSR (non-zero sources); BT BTS BTR BTC with a register offset; XADD CMPXCHG at 8, 16 and 32 bits (a quarter equal); BSWAP; MMX: every arithmetic, logic, compare, pack and unpack instruction; MMX shifts by register, counts at every lane edge; 3DNow! (exact ones in the defined CRC, the rest raw); the K6-2+/K6-III+ 3DNow! extensions |
| 179-194 | `opmap.0f<row>x` | every 0F xx of the row, register and memory form, behind a trailer that is harmless at any length: which encodings run and which #UD, per model (raw CRC only). INVD, MOV CR/DR/TR, the MSR and counter reads, LOADALL, SMINT (0F 38) and PUSH/POP FS/GS are left out, near Jcc runs over a MOV instead of the trailer, and the memory forms of LSS and the bit-string instructions with a register offset |
| 195 | `aliases` | undocumented aliases: 82h, TEST /1, SAL /6, LOCK 82h, PREFETCH /2; and the #UD sub-opcodes of 8F, C6, C7, FE, FF |
| 196 | `cpuid` | the standard, AMD/IDT extended and Centaur leaves, including the brand strings (raw CRC only) |
| 197 | `msr.map` | which MSRs a model has (P5 test registers and counters, AMD K5/K6, IDT, P6): whether RDMSR faults, values cleared |
| 198 | `tsc.write` | WRMSR to the TSC with the upper half set, read back: which models write all 64 bits |
| 199 | `cyrix.dir` | the Cyrix DIR0/DIR1 and CCR0-3 through ports 22h/23h, only on a CPU the 5/2 test or CPUID calls a Cyrix |
| 200 | `pg.basic`    | with paging on (identity map, 4 KB pages): loads, stores, RMW, XADD/XCHG and PUSH/POP to memory, misaligned across a page boundary |
| 201 | `pg.notpresent` | #PF from a missing page: loads, stores, RMW, LOCK, PUSH/POP to memory; error code and CR2 (RMW forms raw only) |

Groups 7 on are mostly instructions 86Box's recompiler still hands to the
interpreter. Faults are results too: the vector, the error code and where
it happened all go into the CRCs. Groups that need something the CPU lacks
(a 486, CMPXCHG8B, MMX, 3DNow!, CMOV, TSC) say so and are skipped.

In the 3DNow! group, only the exact operations go into `crc_defined`:
compares, min/max, truncating conversions, PMULHRW, PAVGUSB and PSWAPD.
Additions, multiplies and reciprocal estimates round differently on a K6-2
and an Athlon, so they're in `crc_raw` only.

The test never uses LOCK CMPXCHG8B with a register operand. That's the
Pentium F00F erratum, and it would hang the machine.

## How much it runs

This is an accuracy test, not a stress test: each case runs as few times as
it takes.

- **Known edge cases first, once each.** Where an instruction's edges are
  known, the first inputs of a variant are exactly those, in order: shift
  counts 0, 1, 7, 8, 9, 15, 16, 17, 31, 32, 33; divisor 0, 1 and the
  quotient one too big; the most negative dividend over -1 and its
  neighbours; CMPXCHG equal and not; REP with ECX 0, 1 and 5 in both
  directions; BSF/BSR of 0, bit 0 and the top bit; BOUND below, on, inside
  and above. Then one or two random inputs for what nobody listed.
- **Random where no edges are known**, biased toward edge values (0, 1,
  7F/80, FF, 7FFF/8000, ...), as for the ALU's own flag math at 16 and 32
  bits and MMX saturation.
- **Exhaustive where the input space is small**: the 8-bit flag math is
  walked completely by loops inside the test (see `exhaust8`).
- **A few representative registers**, not every pair: different registers,
  the same register twice, a high byte, memory.
- **Every flag producer, with and without a block boundary, only for
  instructions that read the flags** (SETcc, CMOVcc, ADC/SBB, RCL/RCR,
  PUSHF, the BCD adjusts, SALC, CMC, LAHF, INTO). The rest run once plain
  and once after an ADD across a block boundary.
- **Four runs** per test: two interpreted, two compiled. The second
  compiled run is where anything the first one left behind shows up.

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
  In `exhaust8` the loop folds only the defined flags into EBP, which goes
  into this CRC.
  With paging on, a #PF's `fault_err` is the error code plus CR2 (relative
  to the buffer) shifted left 8.
- `crc_raw`: the same results with nothing masked (in `exhaust8`, EDI's
  checksum of all the flags). Compare this one only
  against the **same** CPU model; it catches undefined-flag behaviour that
  real software sometimes depends on.

Each group's inputs are seeded from its name, so a group's CRCs don't depend
on which groups ran before it.

## Reference values

Every CRC changed in CPUTEST 3 (see *How much it runs*). References for it
will come from real CPUs; the values for CPUTEST 1 and 2 no longer apply.

Three interpreter bugs in 86Box turned up this way, all in upstream master:

- **BT, BTS, BTR and BTC with a memory operand and a register bit offset**
  treated the offset as unsigned, so a negative offset addressed the wrong
  byte.
- **BT, BTS, BTR and BTC with an immediate bit offset** (`0F BA`) didn't
  take the offset modulo the operand size.
- **ADC** computed AF from the whole second operand instead of its low
  nibble.

## Building

Needs `nasm`, a GCC that can target i386 (`-m32`), and binutils.

```
python3 build.py              # build/cputest.rom and build/cputest.img
python3 build.py --dump       # also prints one line per defined result (D) and raw result (R)
python3 build.py --dump bcd   # the same, only for groups whose name starts with bcd
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
| `groups.inc` | the first six groups, and the group table                   |
| `groups_ops.inc` | groups 7-20                                               |
| `groups_mmx.inc` | groups 21-23: MMX and 3DNow!                              |
| `groups_p6.inc` | groups 24-26: Pentium Pro and later                        |
| `groups_more.inc` | groups 27-28                                             |
| `groups_exhaust.inc` | group 29: exhaustive 8-bit flag math                  |
| `groups_mem.inc` | groups 30-35: addressing and memory forms                 |
| `groups_code16.inc` | group 36: 16-bit protected-mode code                   |
| `groups_loop.inc` | groups 37-178: sampled loops, one instruction each        |
| `groups_map.inc` | groups 179-194: the 0F opcode map                          |
| `groups_cpu.inc` | groups 195-199: aliases, CPUID, MSRs, the TSC, Cyrix       |
| `groups_paging.inc` | groups 200 on: paging                                   |
| `host.h`     | the Linux side of the `--host` build                        |
| `boot.asm`   | boot sector for the disk image                              |
| `payload.ld` | links the payload at 1 MB                                   |

## License

MIT, see `LICENSE`.

# corsac-cputest

A bare-metal x86 CPU accuracy test. I use it to check 86Box's recompiler
against its interpreter, and both against real CPUs, from the 486 up.

It runs with no OS at all, from **`build/cputest.img`**: a 15 MB disk
image (it fits a card sold as 16 MB). Write it to a CF card or a hard disk and boot it; the boot sector
loads the test and starts it. In 86Box, attach it as the primary IDE disk.

Output goes to COM1 at 115200 8N1, and to the screen. The top line of the
screen shows live status. The first line out is `CPUTEST <version>`, then
the CPU's vendor, signature and feature flags, and CR0 as found and as the
tests run. The test turns the CPU's cache on (clears CR0.CD and NW) if it
was off: 86Box doesn't compile code while it is, so the recompiler would
never be tested.

## CPUs

Everything common runs everywhere; the parts a model hasn't got are
skipped by CPUID (or the Cyrix 5/2 test), never assumed. The model-specific
parts cover every CPU 86Box has from the Pentium up: the P54C and P55C
and their OverDrives, the K5, K6, K6-2, K6-III and their + parts, the
Cyrix 6x86, 6x86MX and MII, the IDT WinChip and WinChip 2, the VIA
Cyrix III, and the P6 family: the Pentium Pro, Pentium II Klamath and
Deschutes, Celeron, Pentium II Xeon and the Pentium II OverDrive. For the
P6 that means SYSENTER/SYSEXIT (and their absence on the Pentium Pro),
the P6 MSRs and what WRMSR refuses, PAE paging, global pages, branch
single-step and last-branch recording, performance counters that count,
FXSAVE/FXRSTOR, CMOV and the local APIC's version. Tests for a quirk of
one model go into the group that covers the instruction, not a group of
their own.

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
recompiler. Any difference between them stops the test, with everything
needed to see why (see below). On real hardware all four runs must be the
same too.

Memory operands point into a 16 KB sandbox that is refilled before every run
and CRC'd after it, so a wrong address shows up as a wrong result instead of
landing in the harness.

## Groups

A group's CRCs stay comparable between versions until the group itself
changes (its `def=` fingerprint says when). A new test for an instruction
goes into the group that already covers it, not into a group of its own.

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
| 14 | `string`      | MOVS CMPS STOS LODS SCAS, REP/REPE/REPNE, both directions, 16-bit addressing; overlapping REP MOVS both ways, long runs across pages, overrides moving only the source (FS past its limit, null ES), REP with ECX 0 checking nothing |
| 15 | `popf.pushf`  | POPF/POPFD of any flags but TF and IF, PUSHF of every lazy state; IRETD at CPL 0 with IOPL, AC, ID, RF, VIF and VIP in the image |
| 16 | `stack`       | PUSH ESP, POP [ESP], PUSHA/POPA, POP SS, ENTER (nesting 0-33), LEAVE; a 16-bit stack segment under 32-bit code: SP-only PUSH/POP/CALL/ENTER/PUSHAD/PUSHFD, ESP's upper half kept |
| 17 | `far.call`    | far CALL/JMP direct and indirect, RETF, RETF n, RET n, IRETD, in protected mode |
| 18 | `int.ud`      | INT3, INT n, INTO, ICEBP, INT past the IDT (#GP), invalid encodings (#UD) |
| 19 | `bound.arpl`  | BOUND inside, on and outside the bounds (#BR), ARPL             |
| 20 | `misc`        | XLAT, CMC CLC STC CLD STD, WAIT, PAUSE, XCHG with memory; XLAT with ES:/CS: overrides, a null FS (#GP) and 16-bit addressing |
| 21 | `mmx`         | every MMX instruction: arithmetic, packs, compares, shifts by register and immediate (with the #UD holes), MOVD/MOVQ; the Cyrix extended MMX instructions (0F 50-5E) with CCR7 enabling them, and #UD without |
| 22 | `mmx.x87`     | EMMS and the x87 tag and status words after MMX, EMMS and FLD; FXSAVE/FXRSTOR on the Pentium II Deschutes on: the whole 512-byte area (reserved bytes untouched), misaligned (#GP), a changed MM0 restored, 0F AE /0 register and /7 (#UD) |
| 23 | `3dnow`       | 3DNow! and the K6-2+/K6-III+ extensions, FEMMS, PREFETCH(W)     |
| 24 | `cmov`        | CMOVcc, all conditions, 16/32-bit, register and memory, after every producer (P6 on); CMOVZ from a bad FS operand with the condition false (the P6 reads it anyway) |
| 25 | `rdtsc`       | two RDTSCs back to back: the difference is positive and small (Pentium on) |
| 26 | `nop.p6`      | multi-byte NOP and the reserved NOPs 0F 19-1E: NOPs on P6, #UD before; its CRCs differ by family on purpose |
| 27 | `smc`         | self-modifying code: patched immediates and opcodes, and a loop that repatches its own block; without a jump only in `crc_raw` (a 486 may run stale prefetched bytes) |
| 28 | `segments`    | FS/GS loads of good, null, 16-bit, RPL-3, out-of-GDT and LDT selectors (#GP), limits, LFS/LGS, MOV from Sreg, LAR LSL VERR VERW; LDS LES LFS LGS LSS with 16/32-bit offsets and data, null, code, 64 KB and out-of-GDT selectors; MOV r32/r16/m16 from every Sreg and PUSH Sreg's write width |
| 29 | `exhaust8`    | every input of the 8-bit flag math, walked by a loop inside the test: ADD..CMP × AL × BL × CF, INC DEC NEG NOT, shifts and rotates × counts 0-31, DAA DAS AAA AAS × AF × CF, MUL IMUL, AAM AAD, SAHF |
| 30 | `lea`         | every 32-bit ModRM form, SIB with every base and every index×scale at each mod, every 16-bit form, displacement edges, ESP as the base |
| 31 | `alu.mem`     | the eight ALU ops on memory: both directions, 80/81/83 immediates at their edges, every size, LOCK (#UD on CMP), misaligned |
| 32 | `unary.mem`   | INC DEC NEG NOT TEST, register and memory, every size, LOCK, and the one-byte INC/DEC; INC/DEC after every producer (CF passes through) |
| 33 | `jcc.loop`    | all 16 Jcc, short and near, after every producer; JECXZ/JCXZ; LOOP/LOOPE/LOOPNE over a real body, 32- and 16-bit counts (CX = 0: 65536 times) |
| 34 | `mov.forms`   | moffs A0-A3, C6/C7 through [reg+disp32] (a form 86Box's recompiler interprets on purpose), MOV to/from memory with high bytes, SIB, misaligned, FS:, CBW CWDE CWD CDQ |
| 35 | `prefixes`    | repeated and ignored prefixes, prefix order, stacked segment overrides, branch hints, and the 15-byte limit (#GP at 16); LOCK on XCHG and BTS/BTR/BTC memory, #UD on every other LOCK form; CS: writes (#GP), null FS, SS:/ES:, LODS from CS:, the last of two overrides |
| 36 | `code16`      | code in a 16-bit protected-mode segment (Windows 3.x/9x, DOS extenders): ALU at 16 and 32 bits, SETcc and Jcc after every producer, every 16-bit address form, MOVZX/MOVSX, PUSH/POP/PUSHF, MUL/DIV edges, string ops over SI/DI, LOOP over CX, near CALL/RET; running up to and off the end of the 64 KB segment (#GP), 66/67 on CALL rel32, RET, PUSH imm, LOOP and PUSHFD |
| 37-178 | `loop.<op><size>` | sampled loops, 4096 edge-biased operand sets each, results and flags folded into checksums: ADD OR ADC SBB AND SUB XOR CMP at 16 and 32 bits; INC DEC NEG NOT; ROL ROR RCL RCR SHL SHR SAR by every count; SHLD SHRD; MUL IMUL (one-operand), IMUL r,r/m and IMUL r,r/m,imm; DIV IDIV at 8, 16 and 32 bits, constrained to fit; BSF BSR (non-zero sources); BT BTS BTR BTC with a register offset; XADD CMPXCHG at 8, 16 and 32 bits (a quarter equal); BSWAP; MMX: every arithmetic, logic, compare, pack and unpack instruction; MMX shifts by register, counts at every lane edge; 3DNow! (exact ones in the defined CRC, the rest raw); the K6-2+/K6-III+ 3DNow! extensions |
| 179-194 | `opmap.0f<row>x` | every 0F xx of the row, register and memory form, behind a trailer that is harmless at any length: which encodings run and which #UD, per model (raw CRC only). INVD, MOV CR/DR/TR, the MSR and counter reads, LOADALL, SMINT (0F 38), BSWAP ESP (0F CC) and PUSH/POP FS/GS are left out, near Jcc runs over a MOV instead of the trailer, and the memory forms of LSS and the bit-string instructions with a register offset |
| 195 | `aliases` | undocumented aliases: 82h, TEST /1, SAL /6, LOCK 82h, PREFETCH /2; and the #UD sub-opcodes of 8F, C6, C7, FE, FF |
| 196 | `cpuid` | the standard, AMD/IDT extended and Centaur leaves, including the brand strings (raw CRC only); the local APIC's version register where CPUID reports an APIC |
| 197 | `msr.map` | which MSRs a model has (P5 test registers and counters, AMD K5/K6, IDT, P6): whether RDMSR faults, values cleared; the P6's L2, SYSENTER, counter, LBR, MTRR, PAT and MC bank MSRs, WinChip MCR, Cyrix III FCR, and the TSC with high ECX bits (an alias or #GP) |
| 198 | `msr.write` | WRMSR to the TSC with the upper half set, read back: which models write all 64 bits; RDMSR of the TSC agrees with RDTSC; on a P6, #GP for WRMSR to MTRRcap, MCG_CAP, an LBR MSR and a reserved MTRR type, and PerfCtr0 sign-extending 32 bits to 40 |
| 199 | `cyrix.dir` | the Cyrix DIR0/DIR1 and CCR0-3 through ports 22h/23h, only on a CPU the 5/2 test or CPUID calls a Cyrix |
| 200 | `e820` | the BIOS memory map (INT 15h E820) the loader collected: status (read, no such call, not SMAP, too many or short entries, corrupt, no usable RAM), entries, RAM above 4 GB. The board's answer, not the CPU's: no reference, never a stop; a bad or missing table only turns off PSE-36 |
| 201 | `pg.basic`    | with paging on (identity map, 4 KB pages): loads, stores, RMW, XADD/XCHG and PUSH/POP to memory, misaligned across a page boundary; PAE paging (Pentium Pro on) with WP: reads, writes and their A/D bits in the 64-bit PTE, a missing, a read-only and a reserved-bit page (RSVD), a 2 MB page and its PDE |
| 202 | `pg.notpresent` | #PF from a missing page: loads, stores, RMW, LOCK, PUSH/POP to memory; error code and CR2 (RMW forms raw only); the alias page through a missing PDE, and through a present PDE and missing PTE |
| 203 | `pg.readonly.wp0` | a read-only page with CR0.WP clear: supervisor writes go through; CMPXCHG (equal and not), CMPXCHG8B, XADD, locked BTS/ADD and BT; the alias page through a read-only PDE |
| 204 | `pg.readonly.wp1` | the same with CR0.WP set: supervisor writes fault (486 on) |
| 205 | `pg.cross` | accesses split across a present and a missing page: which half faults, CR2, nothing written (raw) |
| 206 | `pg.ad` | the accessed and dirty bits in the PTE after a read, a write, RMW, and a write after a read; after CMPXCHG equal and not (a failed compare still writes), CMPXCHG8B and BT; the alias page's PDE and PTE after a read and a write |
| 207 | `pg.tlb` | a PTE changed under a cached translation: stale read (raw), then INVLPG or a CR3 reload; CR4.PGE: a global page survives a CR3 reload and INVLPG removes it; without G or PGE it doesn't |
| 208 | `pg.pse` | a 4 MB page (CR4.PSE) aliasing low memory: reads, writes and the PDE's accessed/dirty bits; with PSE-36 and usable RAM above 4 GB in the E820 map, a 4 MB page onto that RAM: a dword written and read back, the PDE's flag bits |
| 209 | `pg.string` | REP MOVS/STOS/LODS running into a missing page part-way, both directions: registers at the fault and the memory done |
| 210 | `pg.exec` | a jump and a call into a missing page: #PF on the fetch |
| 211 | `pg.smc.alias` | code patched through a second mapping of its own page |
| 212 | `r3.basic` | code at CPL 3: arithmetic, memory, the stack, and what it can see (CS, SS, DS, PUSHFD, SMSW, STR, SLDT, SGDT) |
| 213 | `r3.priv` | every privileged instruction at CPL 3: #GP(0) |
| 214 | `r3.io` | port 80h by OUT, OUT DX, OUTSB with the TSS I/O bitmap allowing and denying, at IOPL 0 and 3; CLI by IOPL |
| 215 | `r3.popf` | POPF at CPL 3: IOPL never changes, IF only at IOPL 3, AC always |
| 216 | `r3.ac` | the alignment check (CR0.AM, EFLAGS.AC) at CPL 3: #AC(0) for misaligned words, dwords and stack pushes |
| 217 | `r3.seg` | segment loads at CPL 3 (DPL and RPL rules, SS, system descriptors, null) and LAR/VERR/VERW |
| 218 | `r3.gates` | call gates of every DPL (and into ring 1), parameter copying, conforming code, direct and JMP transfers that must #GP |
| 219 | `r3.int` | INT through gates of DPL 0 and 1 (#GP), INT3 and INT 3 (#GP), ICEBP (no DPL check), INTO |
| 220 | `r3.tsd` | CR4.TSD: RDTSC faults outside ring 0, whatever IOPL; RDPMC without CR4.PCE; RDPMC of counters 0-2 at CPL 0-3 with and without CR4.PCE (Pentium MMX, P6); counter 0 counting instructions (P6 EvtSel0, Pentium MMX CESR) at CPL 0 and 3: RDPMC must move |
| 221 | `r3.pf` | paging at CPL 3: supervisor pages, read-only pages, missing pages; U/S in the error code; the alias page with its PDE or PTE supervisor-only, the PDE read-only or missing |
| 222 | `r1.basic` | code at CPL 1 (CORSAC's sub-kernels): the same as r3.basic |
| 223 | `r1.priv` | every privileged instruction at CPL 1: #GP(0) |
| 224 | `r1.iopl1` | ring 1 with IOPL 1, as CORSAC runs it: ports past the bitmap, CLI, POPF; still no privileged instructions |
| 225 | `r1.io` | the I/O bitmap and IOPL matrix at CPL 1 |
| 226 | `r1.popf` | POPF at CPL 1 |
| 227 | `r1.seg` | segment loads at CPL 1: data of DPL 1-3, SS only DPL 1, RPL raising the check; a JMP to ring 3 code |
| 228 | `r1.gates` | call gates, conforming code and forbidden transfers from ring 1 |
| 229 | `r1.int` | software interrupts from ring 1: the DPL-1 gate is allowed, DPL 0 not |
| 230 | `r1.ac` | CR0.AM and EFLAGS.AC at CPL 1: never #AC |
| 231 | `r1.pf` | paging at CPL 1: supervisor access to every page, read-only pages writable with WP clear |
| 232 | `r1.pf.wp` | the same with CR0.WP: ring 1 writes to read-only pages fault |
| 233 | `r1.outer` | IRETD and RETF from ring 1 to rings 2 and 3: DS and ES of DPL 1 loaded null; IRETD within ring 1 keeps them; IRETD from ring 0 through bad frames (null, data, RPL-0, out-of-GDT CS; null, mismatched, DPL-0, read-only, not-present, TSS SS) and to execute-only and conforming code |
| 234 | `r1.stack` | the stack switch into ring 1 through gates from rings 2 and 3 (SS1:ESP1 from the TSS), none from ring 1 |
| 235 | `r1.lar` | LAR, LSL and VERR of every descriptor at CPL 1, RPL 0 and 3 (raw); and at CPL 0 over each of the 16 system descriptor types |
| 236 | `r2.basic` | code and I/O checks at CPL 2 |
| 237 | `r2.gates` | call gates and far transfers from ring 2, including the DPL-2 gate into ring 1 |
| 238 | `v86.basic` | V86 mode at IOPL 3: 16-bit and 32-bit arithmetic, segment arithmetic, the stack, PUSHF, CLI; what stays privileged |
| 239 | `v86.iopl0` | V86 at IOPL 0 without VME: CLI STI PUSHF POPF INT IRET #GP, ports by the bitmap |
| 240 | `v86.io` | the I/O bitmap in V86 mode, at IOPL 3 too |
| 241 | `v86.ud` | protected-mode-only instructions in V86 (#UD), an address past 64 KB (#GP) |
| 242 | `v86.vme` | VME: CLI/STI on VIF, PUSHF/POPF with VIF, INT 60h redirected through the V86 vector table or faulting; CR4.PVI at CPL 3: CLI/STI on VIF, STI with VIP #GP, POPFD leaves IF and VIF, IOPL 3 and rings 1-2 unaffected |
| 243 | `r3.limits` | FS on read/write, read-only, expand-down (32 and 16-bit), execute/read and execute-only code, page-granular, DPL-1 and missing descriptors at CPL 3, accessed either side of the limit; each of the 16 code/data types at DPL 0 from ring 0 and DPL 3 from ring 3: VERR, VERW, FS load, read and write |
| 244 | `r2.limits` | the same at CPL 2 |
| 245 | `r1.limits` | the same at CPL 1; operands straddling a byte-granular FS limit must #GP whatever the instruction: shifts and rotates by 0 and CL=0, BT/BTS with register and immediate offsets, MOVQ/MOVD/PADDB (MMX), CMPXCHG8B |
| 246 | `r3.ss` | SS on those descriptors at CPL 3, a push and pop either side of the limit: #SS, #GP, expand-down and 16-bit stacks |
| 247 | `r1.ss` | the same at CPL 1 |
| 248 | `conforming` | far calls to conforming code of DPL 0, 1 and 3 from rings 1-3: runs at the caller's CPL, or #GP |
| 249 | `intgate` | interrupt and trap gates into ring 1, 2 and 3 handlers from every ring; outward and DPL violations #GP; gates that must fail (not present, type 0, data or null selector) and a 386 trap gate from rings 0 and 3; INT past a shortened IDT and into a gate half inside it |
| 250 | `r1.inner` | IRETD and RETF from ring 1 to ring 0 and to a mismatched CS (#GP); IRETD in ring 1 can't raise IOPL; RETF 8 |
| 251 | `r3.code16` | 16-bit code at CPL 3 (Win16): arithmetic, SETcc, memory, the stack, CALL/RET, far calls through 32-bit gates, I/O |
| 252 | `r1.code16` | 16-bit code at CPL 1 |
| 253 | `r3.popseg` | POP DS/ES/FS/GS/SS at CPL 3 of good, other-ring, DPL-0, null, TSS, code, execute-only, read-only and missing selectors |
| 254 | `r1.popseg` | the same at CPL 1 |
| 255 | `gateparams` | call gates copying three parameters into ring 1 from rings 3, 2, 1; gates to ring-3 code; a DPL-1 gate outward |
| 256 | `tf.step` | single-step (TF) after 16 kinds of instruction: where the trap lands, DR6.BS, and what beats it (INT3, ICEBP, UD2); on a P6, DEBUGCTL.BTF (the trap waits for a taken JMP, CALL or Jcc) and LBR (LastBranchFromIP/ToIP after a JMP) |
| 257 | `tf.shadow` | MOV SS and POP SS hold off the single-step trap for one instruction; STI and MOV DS don't |
| 258 | `tf.rep` | REP MOVS/STOS/CMPS/SCAS/LODS under TF: one iteration per trap, counts 5, 1 and 0 |
| 259 | `dr.exec` | instruction breakpoints in each of DR0-DR3, on a prefixed instruction's first byte and past the prefix; globally enabled, not enabled |
| 260 | `dr.data` | data breakpoints for writes and for reads/writes, 1, 2 and 4 bytes, hit, straddled and missed by a byte |
| 261 | `dr.gd` | DR7.GD: the next MOV to or from a debug register faults with DR6.BD |
| 262 | `dr.alias` | DR4/DR5 as DR6/DR7 with CR4.DE clear, #UD with it set |
| 263 | `dr.io` | I/O breakpoints (CR4.DE, RW=10) on port 80h by OUT imm8, OUT DX and OUTSB; another port; RW=10 without DE (raw) |
| 264 | `dr.bits` | what DR6 and DR7 read back after all zeros and all ones, and DR0/DR3 round trips (raw) |
| 265 | `cr0.write` | each CR0 flag set or cleared alone and read back; NW without CD (#GP); CD with and without NW; reserved bits (raw) |
| 266 | `cr4.bits` | every CR4 bit alone, read back: which a model has, and whether one it lacks is ignored or #GP (raw) |
| 267 | `cr0.ts` | CR0 EM/TS/MP combinations against WAIT, FNOP, FNINIT, FLD1, EMMS, MOVD and FEMMS: #NM, #UD or run |
| 268 | `lmsw` | LMSW never clears PE and reaches only its four bits; SMSW to a register and to memory; SGDT/SIDT at 32 and 16 bits, SLDT/STR to r32, r16 and memory |
| 269 | `cr23.rw` | CR2 and CR3 written and read back (CR3's low bits raw) |
| 270 | `desc.accessed` | segment loads set a descriptor's accessed bit; LAR, LSL, VERR and VERW don't (the GDT read back, CPL 0 and 3) |
| 271 | `ldt.load` | LLDT of the LDT, null, a TSS, data, a TI=1 selector, from memory; SLDT; TI=1 loads with no LDT (#GP) |
| 272 | `ldt.seg` | LAR/LSL, FS and SS loads through LDT selectors at CPL 0, 1 and 3: DPL, not present, past the LDT's limit |
| 273 | `ldt.gate` | a call gate and a DPL-3 code segment in the LDT, called from rings 3, 1 and 0 |
| 274 | `task.call` | task switches by CALL to a TSS, a GDT task gate and an IDT task gate (from rings 0 and 3): the new task's registers, TR, NT and back link; IRET back, TR and busy bits after |
| 275 | `task.jmp` | task switches by JMP to a TSS and a task gate and back: no nesting, no back link |
| 276 | `task.errors` | CALL to a busy TSS, a too-short TSS (#TS), IRET with NT and no back link, a DPL-0 TSS from ring 3, LTR of a busy TSS or data |
| 277 | `syscall` | fast system calls: SYSENTER/SYSEXIT/SYSCALL/SYSRET, RSM, GETSEC, 0F FF, UD1 and MOV from TR6 at CPL 3 with nothing set up; AMD SYSCALL/SYSRET through STAR and EFER.SCE from rings 0, 1 and 3; Intel SYSENTER/SYSEXIT (Pentium II on) through MSRs 174h-176h, CS MSR 0 and 4, SYSEXIT outside ring 0 |
| 278 | `mp.table` | the processors and I/O APICs the BIOS lists: MP floating pointer and configuration table (1.1/1.4), an MP default configuration, or the ACPI MADT; status (bad checksum, length, entry, extended checksum...), counts, spec revision, IMCR. The board's answer: no reference, never a stop; a bad table only turns off what needs it |
| 279 | `apic.regs` | the boot processor's local APIC, registers only: version, TPR/LDR/DFR/SVR/divider/LVT write masks, software disable forcing and holding the LVT masks, the timer counting down and stopping at 0, the error register after an illegal self-IPI vector (read the P5 or P6 way); the BIOS's settings put back |
| 280 | `apic.irq` | interrupts through the boot processor's local APIC, with the APIC groups' IDT: self-IPIs taken at once and in priority order, TPR holding a vector pending (PPR), the timer one-shot and periodic, HLT woken by it, the STI shadow, NMI to its own ID and through port 70h's NMI-disable bit |
| 281 | `smp.start` | starting every other processor the tables list (INIT, INIT de-assert, two STARTUPs, the MP spec's delays, every wait bounded): how many listed and started, each arrival's EDX (its signature) and CR0; each then waits in a mailbox loop with its own IDT (a fault there stops only it). Machine facts: no reference |
| 282 | `smp.ipi` | IPIs from the boot processor: physical fixed and NMI to each other processor, all-but-self, logical flat to all of them in one; each taken exactly once, the sender excluded |
| 283 | `smp.lock` | every processor at once, 100000 times each: LOCK INC, LOCK XADD, a LOCK CMPXCHG loop, a plain increment under an XCHG spinlock, LOCK ADD across two cache lines; each total exact |

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

## When something doesn't match

If the four runs of a test disagree, the test **stops there**. The top line
of the screen turns red, and COM1 gets everything about that test:
- where it was: group, test number, pass, CPU, CR0/CR2/CR3/CR4;
- the variant's settings and the producer before it;
- the whole input: flags, registers, buffers, MMX registers;
- every byte of the generated code, with addresses;
- each run's complete result;
- a list of what differs from run 1, field by field and byte by byte.

A few tests have more than one right answer on a real CPU, and a real CPU
gives different ones from run to run: code that overwrites the next
instruction without a jump may run it as it was or as it became. Such a
test lists every allowed outcome and folds each into one canonical result
before the runs are compared, so they agree whenever the CPU gave any of
them. An outcome outside the list still stops the test.

The same happens when a group's results differ from what a real CPU of
the same model gave. `refs.inc` holds those results, one line per group,
made by `refs.py` from the real CPU's serial log. Each group line carries
`def=`, a fingerprint of the group's own definition; a reference is only
compared while that fingerprint matches, so changing a group makes its old
reference stale (it says so and goes on), never wrong.

A fault inside the harness itself (not in a test) stops it the same way,
with the vector, error code, EIP, CR2, the registers and what was running,
instead of crashing on.

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
  A ring or V86 test ends with INT 30h, so its normal result has fault
  vector 30h; any other vector is a fault on the way.
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
python3 build.py              # build/cputest.img
python3 build.py --dump       # also prints one line per defined result (D) and raw result (R)
python3 build.py --dump bcd   # the same, only for groups whose name starts with bcd
python3 build.py --host       # build/cputest: a static i386 Linux ELF of the same harness
```

`--host` runs the tests on the host CPU as an ordinary process, with faults
arriving as signals. It uses an i686 musl cross compiler; the path is at the
top of `build.py`.

`diffdump.py REF.txt RUN.txt` compares two `--dump` outputs and groups the
differences by instruction, producer and field.

`run.py` boots the image in a headless 86Box. It relies on my own
headless runner (not published), so it won't work elsewhere as it stands.

## Files

| file         | what                                                        |
|--------------|-------------------------------------------------------------|
| `stub.asm`   | the loader: A20, protected mode, copies the payload to 1 MB |
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
| `groups_cpu.inc` | groups 195-200: aliases, CPUID, MSRs, the TSC, Cyrix, E820 |
| `groups_paging.inc` | groups 201-211: paging |
| `groups_ring.inc` | groups 212-242: rings 1-3 and V86 mode |
| `groups_ring2.inc` | groups 243-255: limits, stacks, gates, 16-bit code in rings |
| `groups_debug.inc` | groups 256-264: single-step and the debug registers |
| `groups_sys.inc` | groups 265-269: control registers; 277: fast system calls |
| `groups_desc.inc` | groups 270-276: descriptors, the LDT, task switches |
| `groups_smp.inc` | groups 278 on: MP/ACPI tables, the APICs, the other processors |
| `mp.inc`     | reads the MP and ACPI tables at start-up; PIT delays     |
| `host.h`     | the Linux side of the `--host` build                        |
| `boot.asm`   | boot sector for the disk image                              |
| `payload.ld` | links the payload at 1 MB                                   |
| `refs.inc`   | real CPUs' results per group (made by `refs.py`)            |
| `refs.py`    | makes `refs.inc` from serial logs                           |

## License

MIT, see `LICENSE`.

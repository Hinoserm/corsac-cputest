/*
 * Opcode maps and aliases: which encodings a CPU executes and which raise
 * #UD, the way Windows setup and CPU detection code probe them. Results
 * are per model, so these go into the raw CRC only. Included into
 * groups.inc after groups_loop.inc; append only.
 */

/* 0F xx opcodes never run here, in either form:
   - INVD (loses the cache's dirty lines) and LOADALL/SYSRET (07);
   - MOV to a control, debug or test register, and WRMSR;
   - RDTSC, RDMSR and RDPMC, whose results change between runs;
   - the near Jcc forms, which would jump into the trailer (row 8 runs
     them over a MOV instead);
   - PUSH/POP FS/GS, which unbalance the stack the test returns on;
   - 0F 38, SMINT on a Cyrix 6x86: into SMM if the BIOS enabled SMIs.
   And in the memory form only: the bit-string instructions with a
   register offset (they reach anywhere in memory) and LSS (a random SS). */
static int
opmap_skip(int op, int mem)
{
    if (op == 0x07 || op == 0x08 || (op >= 0x22 && op <= 0x27) || (op >= 0x30 && op <= 0x33) || (op >= 0x80 && op <= 0x8f))
        return 1;
    if (op == 0xa0 || op == 0xa1 || op == 0xa8 || op == 0xa9 || op == 0x38)
        return 1;
    return mem && (op == 0xa3 || op == 0xab || op == 0xb3 || op == 0xbb || op == 0xb2);
}

/* Every 0F xx of one row, in a register form and a memory form. After the
   opcode comes a trailer that is harmless however much of it the
   instruction takes: ModRM F8 is (EDI, EAX) as operands and CLC as an
   instruction; ModRM 03 is [ebx], and 03 F8 alone is ADD EDI, EAX; then
   NOPs for any immediate. */
static void
group_opmap(unsigned row)
{
    static const char *const names[16] = {
        "opmap.0f0x", "opmap.0f1x", "opmap.0f2x", "opmap.0f3x", "opmap.0f4x", "opmap.0f5x", "opmap.0f6x", "opmap.0f7x",
        "opmap.0f8x", "opmap.0f9x", "opmap.0fax", "opmap.0fbx", "opmap.0fcx", "opmap.0fdx", "opmap.0fex", "opmap.0ffx",
    };
    const char    *name = names[row];
    struct variant v    = { .group = name, .faults_ok = 1, .fx = FX_CUSTOM, .defmask = dm_arg3, .arg = MEM_OFF };
    group_begin(name);
    if (row == 8) { /* the near Jcc: over MOV AL, 1, so the jump lands on real code */
        for (int lo = 0; lo < 16; lo++) {
            static const uint8_t jcc[8] = { 0x0f, 0x80, 0x02, 0x00, 0x00, 0x00, 0xb0, 0x01 };
            memcpy(v.target, jcc, sizeof(jcc));
            v.target[1] |= lo;
            v.target_len = sizeof(jcc);
            v.fix_input  = 0;
            run_plain(&v, 1);
        }
    }
    for (int lo = 0; lo < 16; lo++) {
        int op = (row << 4) | lo;
        for (int mem = 0; mem < 2; mem++) {
            if (opmap_skip(op, mem))
                continue;
            static const uint8_t trailer[6] = { 0xf8, 0x90, 0x90, 0x90, 0x90, 0x90 };
            int                  n          = 0;
            v.target[n++]                   = 0x0f;
            v.target[n++]                   = op;
            if (mem)
                v.target[n++] = 0x03;
            memcpy(v.target + n, trailer, sizeof(trailer));
            v.target_len = n + sizeof(trailer);
            v.fix_input  = mem ? fix_mem_ebx : 0;
            run_plain(&v, 1);
        }
    }
    seg_restore(); /* LFS/LGS (0F B4, B5) load whatever they find */
    group_end(name);
}

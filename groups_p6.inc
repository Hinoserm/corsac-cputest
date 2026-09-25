/*
 * Pentium Pro (P6) and later: CMOVcc, the multi-byte NOP, and RDTSC.
 * Included into groups.inc after groups_mmx.inc; append only.
 */

#define CPUID_TSC   (1u << 4)
#define CPUID_CMOV  (1u << 15)

/* CMOVcc: all sixteen conditions, 16- and 32-bit, from a register, the
   same register, and memory (aligned and misaligned; the load happens
   whether or not the move does), after every flag producer. */
static void
group_cmov(void)
{
    static const struct {
        uint8_t dst, src, disp;
    } forms[] = {
        { R_EAX, R_ECX, 0 },
        { R_EDX, R_EDX, 0 },
        { R_ESI, R_EDI, 0 },
        { R_EBP, RM_MEM, 0 },
        { R_EDI, RM_MEM, 31 }, /* [ebx+31] */
    };
    const char    *name = "cmov";
    struct variant v    = { .group = name, .fx = FX_SETCC };

    if (!(g_cpu.features & CPUID_CMOV)) {
        group_skip(name, "no CMOV");
        return;
    }
    group_begin(name);
    for (int cc = 0; cc < 16; cc++) {
        for (int size = 16; size <= 32; size += 16) {
            for (unsigned f = 0; f < sizeof(forms) / sizeof(forms[0]); f++) {
                int n = 0;
                if (size == 16)
                    v.target[n++] = 0x66;
                v.target[n++] = 0x0f;
                v.target[n++] = 0x40 | cc;
                if (forms[f].disp) {
                    v.target[n++] = 0x43 | (forms[f].dst << 3);
                    v.target[n++] = forms[f].disp;
                } else
                    n = put_modrm(v.target, n, forms[f].dst, forms[f].src);
                v.target_len = n;
                v.fx_arg     = cc;
                v.fix_input  = forms[f].src == RM_MEM ? fix_mem_ebx : 0;
                v.arg        = MEM_OFF;
                run_all_contexts(&v, 2);
            }
        }
    }
    group_end(name);
}

/* RDTSC twice: the difference must be positive and under 2^28 (the second
   read can't come before the first, nor a second or so later). The code
   turns that into EAX = 1 and zeroes everything else it used, so every run
   gives the same result. Pentium on (CPUID TSC). */
static void
group_rdtsc(void)
{
    static const uint8_t seq[] = {
        0x0f, 0x31,                   /* rdtsc */
        0x89, 0xc6,                   /* mov esi, eax */
        0x89, 0xd7,                   /* mov edi, edx */
        0x0f, 0x31,                   /* rdtsc */
        0x29, 0xf0,                   /* sub eax, esi */
        0x19, 0xfa,                   /* sbb edx, edi: EDX:EAX = the difference */
        0x85, 0xd2,                   /* test edx, edx */
        0x0f, 0x94, 0xc1,             /* setz cl */
        0x3d, 0x00, 0x00, 0x00, 0x10, /* cmp eax, 10000000h */
        0x0f, 0x92, 0xc5,             /* setb ch */
        0x83, 0xf8, 0x01,             /* cmp eax, 1 */
        0x0f, 0x93, 0xc2,             /* setae dl */
        0x20, 0xe9,                   /* and cl, ch */
        0x20, 0xd1,                   /* and cl, dl */
        0x0f, 0xb6, 0xc1,             /* movzx eax, cl */
        0x31, 0xd2,                   /* xor edx, edx */
        0x31, 0xf6,                   /* xor esi, esi */
        0x31, 0xff,                   /* xor edi, edi */
        0x31, 0xc9,                   /* xor ecx, ecx */
    };
    const char    *name = "rdtsc";
    struct variant v    = { .group = name };

    if (!(g_cpu.features & CPUID_TSC)) {
        group_skip(name, "no TSC");
        return;
    }
    group_begin(name);
    memcpy(v.target, seq, sizeof(seq));
    v.target_len = sizeof(seq);
    run_ctx(&v, ctx_some, 3, 8);
    group_end(name);
}

/* The multi-byte NOP (0F 1F /0) and the reserved-NOP opcodes 0F 19-1E:
   NOPs from the Pentium Pro on, #UD on the Pentium and the K6. Faults are
   results here, so this group's CRCs tell the two families apart; they
   never touch the memory their ModRM names. */
static void
group_nop_p6(void)
{
    static const struct {
        uint8_t len;
        uint8_t b[9];
    } seqs[] = {
        { 3, { 0x0f, 0x1f, 0x00 } },                                     /* nop [eax] */
        { 4, { 0x0f, 0x1f, 0x40, 0x00 } },                               /* nop [eax+0] */
        { 5, { 0x0f, 0x1f, 0x44, 0x00, 0x00 } },                         /* nop [eax+eax*1+0] */
        { 6, { 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00 } },                   /* nop word [...] */
        { 7, { 0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00 } },             /* nop [eax+0L] */
        { 8, { 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 } },       /* nop [eax+eax*1+0L] */
        { 9, { 0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 } }, /* nop word [...+0L] */
        { 3, { 0x0f, 0x1f, 0xc0 } },                                     /* nop eax */
        { 3, { 0x0f, 0x19, 0x00 } },                                     /* reserved NOP 0F 19 */
        { 3, { 0x0f, 0x1c, 0xc1 } },                                     /* reserved NOP 0F 1C */
        { 3, { 0x0f, 0x1e, 0x03 } },                                     /* reserved NOP 0F 1E */
    };
    const char    *name = "nop.p6";
    struct variant v    = { .group = name, .faults_ok = 1 };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        run_ctx(&v, ctx_some, 3, 2);
    }
    group_end(name);
}

/*
 * Pentium Pro (P6) and later: CMOVcc, the multi-byte NOP, and RDTSC.
 * Included into groups.inc after groups_mmx.inc.
 */

#define CPUID_TSC   (1u << 4)
#define CPUID_CMOV  (1u << 15)

/* CMOVZ from memory through FS (the 64 KB segment, past its limit, or
   null) or DS, with the condition true and false, 32- and 16-bit: the
   P6 reads the operand whatever the condition, so a bad one faults even
   when nothing moves. The false-condition faults are raw only. */
static void
cmov_fault(const char *name)
{
    static const struct {
        uint8_t len, b[7];
    } segs[] = { { SEQ(0x66, 0xb8, 0x20, 0x00, 0x8e, 0xe0, 0x64) },
                 { SEQ(0x66, 0xb8, 0x00, 0x00, 0x8e, 0xe0, 0x64) },
                 { 0, { 0 } } };
    struct variant v = { .group = name, .faults_ok = 1, .fix_input = fix_mem_ebx, .arg = MEM_OFF,
                         .fx = FX_CUSTOM, .defmask = dm_fxarg };
#ifdef HOSTTEST
    return; /* segment loads */
#endif
    for (int s = 0; s < 3; s++) {
        for (int cond = 1; cond >= 0; cond--) {
            for (int o16 = 0; o16 < 2; o16++) {
                int      n = 0;
                uint8_t *t = v.target;
                t[n++]     = 0x31; /* xor ecx, ecx: ZF set */
                t[n++]     = 0xc9;
                if (!cond)
                    t[n++] = 0x41; /* inc ecx: ZF clear */
                if (s < 2) {
                    memcpy(t + n, segs[s].b, segs[s].len - 1);
                    n += segs[s].len - 1;
                }
                if (o16)
                    t[n++] = 0x66;
                if (s < 2)
                    t[n++] = 0x64;
                t[n++]       = 0x0f; /* cmovz eax, [ebx] */
                t[n++]       = 0x44;
                t[n++]       = 0x03;
                v.target_len = n;
                v.fx_arg     = cond || s == 2;
                run_plain(&v, 1);
            }
        }
    }
}

/* CMOVcc: all sixteen conditions, 16- and 32-bit, from a register and from
   memory (the load happens whether or not the move does), after every flag
   producer. */
static void
group_cmov(void)
{
    static const struct {
        uint8_t dst, src, disp;
    } forms[] = {
        { R_EAX, R_ECX, 0 },
        { R_EBP, RM_MEM, 0 },
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
                run_flags(&v, 1);
            }
        }
    }
    cmov_fault(name);
    group_end(name);
}

/* A changing TSC, without assuming an upper scheduler-latency bound.
   The modular 64-bit difference must be nonzero; the independent
   predicate prevents four identically frozen reads from passing. */
static const char *
expect_rdtsc(const struct variant *v, const struct kin *in, const struct kout *o)
{
    return o->fault != 0xff || o->eax != 1 ? "RDTSC did not advance" : 0;
}

static void
group_rdtsc(void)
{
    static const uint8_t seq[] = {
        0x0f,0x31, 0x89,0xc6, 0x89,0xd7, /* first sample */
        0x0f,0x31, 0x29,0xf0, 0x19,0xfa, /* 64-bit difference */
        0x09,0xd0, 0x0f,0x95,0xc0, 0x0f,0xb6,0xc0, /* EAX = delta != 0 */
        0x31,0xd2, 0x31,0xf6, 0x31,0xff, 0x31,0xc9,
    };
    const char *name = "rdtsc";
    struct variant v = {.group=name,.fx=FX_ALU,.fx_arg=6,.expect=expect_rdtsc};
    if (!(g_cpu.features & CPUID_TSC)) {group_skip(name,"no TSC");return;}
    group_begin(name);
    memcpy(v.target,seq,sizeof(seq));v.target_len=sizeof(seq);
    run_plain(&v,2);
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
        { SEQ(0x0f, 0x1f, 0x00) },                                     /* nop [eax] */
        { SEQ(0x0f, 0x1f, 0x40, 0x00) },                               /* nop [eax+0] */
        { SEQ(0x0f, 0x1f, 0x44, 0x00, 0x00) },                         /* nop [eax+eax*1+0] */
        { SEQ(0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00) },                   /* nop word [...] */
        { SEQ(0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00) },             /* nop [eax+0L] */
        { SEQ(0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00) },       /* nop [eax+eax*1+0L] */
        { SEQ(0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00) }, /* nop word [...+0L] */
        { SEQ(0x0f, 0x1f, 0xc0) },                                     /* nop eax */
        { SEQ(0x0f, 0x19, 0x00) },                                     /* reserved NOP 0F 19 */
        { SEQ(0x0f, 0x1c, 0xc1) },                                     /* reserved NOP 0F 1C */
        { SEQ(0x0f, 0x1e, 0x03) },                                     /* reserved NOP 0F 1E */
    };
    const char    *name = "nop.p6";
    struct variant v    = { .group = name, .faults_ok = 1 };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        run_plain(&v, 1);
    }
    group_end(name);
}

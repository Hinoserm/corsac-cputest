/*
 * MMX (P55C, Pentium II, K6 on), the MMX/x87 handover (EMMS, FEMMS), and
 * AMD 3DNow! and its K6-2+/K6-III+ extensions. Included into groups.inc
 * after groups_ops.inc.
 *
 * Every variant loads MM0-MM7 from g_mmx_in after an FNINIT and stores them
 * to g_mmx_out afterwards; the stored registers go into the memory CRC.
 */

#define CPUID_MMX     (1u << 23)
#define EXT_3DNOW     (1u << 31)
#define EXT_3DNOWEXT  (1u << 30)

/* The MMX register operand (0-7) or [ebx] (RM_MEM, 8 bytes at the buffer
   offset MEM_OFF). */
static int
put_mm_modrm(uint8_t *t, int n, int reg, int rm)
{
    return put_modrm(t, n, reg, rm);
}

static void
set_mm(int r, uint32_t lo, uint32_t hi)
{
    if (r == RM_MEM) {
        put_le(g_in.buf + MEM_OFF, lo, 4);
        put_le(g_in.buf + MEM_OFF + 4, hi, 4);
    } else {
        g_mmx_in[2 * r]     = lo;
        g_mmx_in[2 * r + 1] = hi;
    }
}

/* Shift counts from an MMX register or memory: every edge for words,
   dwords and the qword in turn, then one with the upper half set (which
   shifts everything out), then one random. */
static const uint32_t mm_counts[] = { 0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 255 };
#define N_MM_COUNTS (sizeof(mm_counts) / sizeof(mm_counts[0]))

static void
fix_mm_count(const struct variant *v)
{
    int      e  = edge(N_MM_COUNTS + 1);
    uint32_t lo = e >= 0 && e < (int) N_MM_COUNTS ? mm_counts[e] : rnd() & 0x3f;
    uint32_t hi = e == (int) N_MM_COUNTS ? 1 + (rnd() & 0xff) : 0;
    if (v->arg2 == RM_MEM)
        g_in.ebx = (uint32_t) g_buf + MEM_OFF;
    set_mm(v->arg2, lo, hi);
}

/* ---- MMX arithmetic, logic, compares, packs and unpacks --------------- */

static void
group_mmx(void)
{
    /* Two-operand mm, mm/m64 instructions (0F xx). */
    static const uint8_t ops[] = {
        0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, /* unpack low, pack, pcmpgt, unpack high */
        0x74, 0x75, 0x76,                                                       /* pcmpeq */
        0xd5, 0xd8, 0xd9, 0xdb, 0xdc, 0xdd, 0xdf,                               /* pmullw psubus pand paddus pandn */
        0xe5, 0xe8, 0xe9, 0xeb, 0xec, 0xed, 0xef,                               /* pmulhw psubs por padds pxor */
        0xf5, 0xf8, 0xf9, 0xfa, 0xfc, 0xfd, 0xfe,                               /* pmaddwd psub padd */
    };
    /* Shifts by an MMX register or memory. */
    static const uint8_t shifts[] = { 0xd1, 0xd2, 0xd3, 0xe1, 0xe2, 0xf1, 0xf2, 0xf3 };
    /* (destination, source) pairs; 3,3 is the same register twice. */
    static const uint8_t pairs[3][2] = { { 0, 1 }, { 3, 3 }, { 5, RM_MEM } };
    const char          *name        = "mmx";
    struct variant       v           = { .group = name, .mmx = 1 };

    if (!(g_cpu.features & CPUID_MMX)) {
        group_skip(name, "no MMX");
        return;
    }
    group_begin(name);
    for (unsigned o = 0; o < sizeof(ops); o++) {
        for (int p = 0; p < 3; p++) {
            v.target[0]  = 0x0f;
            v.target[1]  = ops[o];
            v.target_len = put_mm_modrm(v.target, 2, pairs[p][0], pairs[p][1]);
            v.fix_input  = pairs[p][1] == RM_MEM ? fix_mem_ebx : 0;
            v.arg        = MEM_OFF;
            run_plain(&v, 2);
        }
    }
    for (unsigned o = 0; o < sizeof(shifts); o++) {
        for (int p = 0; p < 3; p += 2) { /* a register, memory */
            v.target[0]  = 0x0f;
            v.target[1]  = shifts[o];
            v.target_len = put_mm_modrm(v.target, 2, pairs[p][0], pairs[p][1]);
            v.fix_input  = fix_mm_count;
            v.arg2       = pairs[p][1];
            v.reg_fix    = pairs[p][1] != RM_MEM;
            v.norm       = pairs[p][1] == RM_MEM ? 1 << R_EBX : 0;
            run_plain(&v, N_MM_COUNTS + 2);
        }
    }
    v.reg_fix = 0;
    v.norm    = 0;

    /* Shifts by an immediate: 0F 71 (words), 72 (dwords), 73 (qword), with
       /2 right logical, /4 right arithmetic, /6 left. 73 /4 (no PSRAQ) and
       71 /0 are #UD. */
    static const uint8_t imms[] = { 0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 255 };
    for (int op = 0x71; op <= 0x73; op++) {
        for (int sub = 0; sub < 8; sub += 2) {
            int valid = sub != 0 && !(op == 0x73 && sub == 4);
            for (int dst = 0; dst < 8; dst += 6) { /* MM0, MM6 */
                for (unsigned k = 0; k < (valid ? sizeof(imms) : 1); k++) {
                    v.target[0]  = 0x0f;
                    v.target[1]  = op;
                    v.target[2]  = MODRM_RR(sub, dst);
                    v.target[3]  = valid ? imms[k] : 4;
                    v.target_len = 4;
                    v.fix_input  = 0;
                    v.faults_ok  = !valid;
                    run_plain(&v, 1);
                }
            }
        }
    }
    v.faults_ok = 0;

    /* MOVD and MOVQ, both directions, registers and memory (one misaligned). */
    static const struct {
        uint8_t mem, len;
        uint8_t b[5];
    } moves[] = {
        { 0, SEQ(0x0f, 0x6e, 0xc0) },       /* movd mm0, eax */
        { 0, SEQ(0x0f, 0x6e, 0xe6) },       /* movd mm4, esi */
        { 0, SEQ(0x0f, 0x6e, 0xcd) },       /* movd mm1, ebp */
        { 1, SEQ(0x0f, 0x6e, 0x13) },       /* movd mm2, [ebx] */
        { 0, SEQ(0x0f, 0x7e, 0xc8) },       /* movd eax, mm1 */
        { 0, SEQ(0x0f, 0x7e, 0xff) },       /* movd edi, mm7 */
        { 1, SEQ(0x0f, 0x7e, 0x1b) },       /* movd [ebx], mm3 */
        { 0, SEQ(0x0f, 0x6f, 0xea) },       /* movq mm5, mm2 */
        { 1, SEQ(0x0f, 0x6f, 0x33) },       /* movq mm6, [ebx] */
        { 1, SEQ(0x0f, 0x6f, 0x4b, 0x1f) }, /* movq mm1, [ebx+31] */
        { 0, SEQ(0x0f, 0x7f, 0xd5) },       /* movq mm5, mm2 (store form) */
        { 1, SEQ(0x0f, 0x7f, 0x1b) },       /* movq [ebx], mm3 */
        { 1, SEQ(0x0f, 0x7f, 0x63, 0x1f) }, /* movq [ebx+31], mm4 */
    };
    for (unsigned i = 0; i < sizeof(moves) / sizeof(moves[0]); i++) {
        memcpy(v.target, moves[i].b, moves[i].len);
        v.target_len = moves[i].len;
        v.fix_input  = moves[i].mem ? fix_mem_ebx : 0;
        v.arg        = MEM_OFF;
        run_plain(&v, 2);
    }
    group_end(name);
}

/* ---- the MMX/x87 handover ------------------------------------------------- */

/* arg2 bit 0: the tag word is defined (every register empty or holding an
   ordinary x87 value). Otherwise it holds registers MMX wrote, whose tags
   FNSTENV works out from the contents, one way per model. */
static int
dm_fenv(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    if (!(v->arg2 & 1))
        d->esi = 0;
    return 1;
}

/* fnstenv [g_fenv]; movzx esi, word [tag word]; movzx edi, word [status word] */
static int
put_fenv(uint8_t *t, int n)
{
    t[n++] = 0xd9;
    t[n++] = 0x35;
    put_le(t + n, (uint32_t) &g_fenv[0], 4);
    n += 4;
    t[n++] = 0x0f;
    t[n++] = 0xb7;
    t[n++] = 0x35;
    put_le(t + n, (uint32_t) &g_fenv[2], 4);
    n += 4;
    t[n++] = 0x0f;
    t[n++] = 0xb7;
    t[n++] = 0x3d;
    put_le(t + n, (uint32_t) &g_fenv[1], 4);
    return n + 4;
}

static void
group_mmx_x87(void)
{
    static const struct {
        uint8_t tw_defined, len;
        uint8_t b[6];
    } pre[] = {
        { 0, 0, { 0 } },                                /* straight after MMX */
        { 1, SEQ(0x0f, 0x77) },                       /* emms */
        { 0, SEQ(0x0f, 0x77, 0x0f, 0x6f, 0xc1) },     /* emms; movq mm0, mm1 */
        { 0, SEQ(0x0f, 0x77, 0x0f, 0x7e, 0xc0) },     /* emms; movd eax, mm0 */
        { 1, SEQ(0x0f, 0x77, 0xd9, 0xe8) },           /* emms; fld1 */
        { 0, SEQ(0xd9, 0xe8) },                       /* fld1 onto a full stack: masked #IS */
        { 1, SEQ(0x0f, 0x77, 0xd9, 0xe8, 0xd9, 0xee) }, /* emms; fld1; fldz */
    };
    const char    *name = "mmx.x87";
    struct variant v    = { .group = name, .mmx = 1, .fx = FX_CUSTOM, .defmask = dm_fenv };

    if (!(g_cpu.features & CPUID_MMX)) {
        group_skip(name, "no MMX");
        return;
    }
    group_begin(name);
    for (unsigned i = 0; i < sizeof(pre) / sizeof(pre[0]); i++) {
        memcpy(v.target, pre[i].b, pre[i].len);
        v.target_len = put_fenv(v.target, pre[i].len);
        v.arg2       = pre[i].tw_defined;
        run_plain(&v, 1);
    }
    group_end(name);
}

/* ---- 3DNow! --------------------------------------------------------------- */

/* Single-precision inputs: zeros, ones and ordinary numbers, no NaNs,
   infinities or denormals (their handling differs between models). */
static uint32_t
rnd_float(void)
{
    uint32_t r    = rnd();
    uint32_t sign = r & 0x80000000u;
    switch (r & 7) {
        case 0: return sign;              /* +-0 */
        case 1: return sign | 0x3f800000; /* +-1 */
        default: return sign | ((100 + rnd() % 56) << 23) | (rnd() & 0x7fffff);
    }
}

static void
fix_float(const struct variant *v)
{
    for (int i = 0; i < 16; i++)
        g_mmx_in[i] = rnd_float();
    if (v->arg2 == RM_MEM) {
        g_in.ebx = (uint32_t) g_buf + MEM_OFF;
        put_le(g_in.buf + MEM_OFF, rnd_float(), 4);
        put_le(g_in.buf + MEM_OFF + 4, rnd_float(), 4);
    }
}

/* Only exact operations are comparable between models: compares, min and
   max, truncating conversions, integer averages and rounding multiplies,
   swaps. The rest (add, multiply, reciprocal estimates, int to float)
   round one way on the K6-2 and another on the Athlon: raw CRC only. */
static int
dm_3dnow(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    return v->arg3;
}

static void
group_3dnow(void)
{
    static const struct {
        uint8_t op, exact, ext, fl;
    } ops[] = {
        { 0x0d, 0, 0, 0 }, /* PI2FD */
        { 0x1d, 1, 0, 1 }, /* PF2ID */
        { 0x90, 1, 0, 1 }, /* PFCMPGE */
        { 0x94, 1, 0, 1 }, /* PFMIN */
        { 0x96, 0, 0, 1 }, /* PFRCP */
        { 0x97, 0, 0, 1 }, /* PFRSQRT */
        { 0x9a, 0, 0, 1 }, /* PFSUB */
        { 0x9e, 0, 0, 1 }, /* PFADD */
        { 0xa0, 1, 0, 1 }, /* PFCMPGT */
        { 0xa4, 1, 0, 1 }, /* PFMAX */
        { 0xa6, 0, 0, 1 }, /* PFRCPIT1 */
        { 0xa7, 0, 0, 1 }, /* PFRSQIT1 */
        { 0xaa, 0, 0, 1 }, /* PFSUBR */
        { 0xae, 0, 0, 1 }, /* PFACC */
        { 0xb0, 1, 0, 1 }, /* PFCMPEQ */
        { 0xb4, 0, 0, 1 }, /* PFMUL */
        { 0xb6, 0, 0, 1 }, /* PFRCPIT2 */
        { 0xb7, 1, 0, 0 }, /* PMULHRW */
        { 0xbf, 1, 0, 0 }, /* PAVGUSB */
        { 0x0c, 0, 1, 0 }, /* PI2FW */
        { 0x1c, 1, 1, 1 }, /* PF2IW */
        { 0x8a, 0, 1, 1 }, /* PFNACC */
        { 0x8e, 0, 1, 1 }, /* PFPNACC */
        { 0xbb, 1, 1, 0 }, /* PSWAPD */
    };
    static const uint8_t pairs[2][2] = { { 0, 1 }, { 7, RM_MEM } };
    const char          *name        = "3dnow";
    struct variant       v           = { .group = name, .mmx = 1, .fx = FX_CUSTOM, .defmask = dm_3dnow };

    if (!(g_cpu.ext_features & EXT_3DNOW)) {
        group_skip(name, "no 3DNow!");
        return;
    }
    group_begin(name);
    for (unsigned o = 0; o < sizeof(ops) / sizeof(ops[0]); o++) {
        if (ops[o].ext && !(g_cpu.ext_features & EXT_3DNOWEXT))
            continue;
        for (int p = 0; p < 2; p++) {
            int n        = 0;
            v.target[n++] = 0x0f;
            v.target[n++] = 0x0f;
            n             = put_mm_modrm(v.target, n, pairs[p][0], pairs[p][1]);
            v.target[n++] = ops[o].op;
            v.target_len  = n;
            v.arg2        = pairs[p][1];
            v.arg3        = ops[o].exact;
            v.fix_input   = ops[o].fl ? fix_float : pairs[p][1] == RM_MEM ? fix_rm : 0;
            v.reg_fix     = pairs[p][1] != RM_MEM;
            v.norm        = pairs[p][1] == RM_MEM ? 1 << R_EBX : 0;
            run_plain(&v, 2);
        }
    }
    /* FEMMS, and PREFETCH/PREFETCHW (no visible effect but must decode). */
    {
        static const struct {
            uint8_t mem, len;
            uint8_t b[3];
        } seqs[] = {
            { 0, SEQ(0x0f, 0x0e) },       /* femms */
            { 1, SEQ(0x0f, 0x0d, 0x03) }, /* prefetch [ebx] */
            { 1, SEQ(0x0f, 0x0d, 0x0b) }, /* prefetchw [ebx] */
        };
        v.arg3 = 1;
        for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
            int n = seqs[i].len;
            memcpy(v.target, seqs[i].b, n);
            if (i == 0) /* FEMMS: the tag word after it, all empty */
                n = put_fenv(v.target, n);
            v.target_len = n;
            v.fix_input  = seqs[i].mem ? fix_mem_ebx : 0;
            v.arg        = MEM_OFF;
            v.reg_fix    = 0;
            v.norm       = 0;
            run_plain(&v, 1);
        }
    }
    group_end(name);
}

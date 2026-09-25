/*
 * Control registers from ring 0: which CR0 and CR4 writes a model takes
 * or faults on, TS/EM/MP against the x87 and MMX instructions, LMSW and
 * SMSW, CR2 and CR3, the TSC through RDMSR. Each test saves the register,
 * writes, reads back what stuck and restores it; an after-run hook puts
 * CR0, CR3 and CR4 back too, in case the write or what followed faulted.
 * Included into groups.inc after groups_debug.inc; append only.
 */

static uint32_t g_saved_cr0 RING_UNUSED, g_saved_cr3 RING_UNUSED, g_saved_cr4 RING_UNUSED;

static void
sys_before(const struct variant *v)
{
#ifndef HOSTTEST
    __asm__ volatile("mov %%cr0, %0\n\tmov %%cr3, %1" : "=r"(g_saved_cr0), "=r"(g_saved_cr3));
    if (g_cpu.has_cpuid)
        __asm__ volatile("mov %%cr4, %0" : "=r"(g_saved_cr4));
#endif
}

static void
sys_after(const struct variant *v)
{
#ifndef HOSTTEST
    __asm__ volatile("mov %0, %%cr0\n\tjmp 1f\n1:\n\tmov %1, %%cr3" : : "r"(g_saved_cr0), "r"(g_saved_cr3) : "memory");
    if (g_cpu.has_cpuid)
        __asm__ volatile("mov %0, %%cr4" : : "r"(g_saved_cr4));
#endif
}

static int
sys_begin(const char *name)
{
#ifdef HOSTTEST
    group_skip(name, "control registers (host build)");
    return 0;
#else
    group_begin(name);
    return 1;
#endif
}

static struct variant
sys_variant(const char *name)
{
    struct variant v = { .group = name, .faults_ok = 1, .fx = FX_CUSTOM, .defmask = dm_fxarg, .fx_arg = 1,
                         .before_run = sys_before, .after_run = sys_after };
    return v;
}

/* mov esi, crN; mov eax, esi; (or eax, set) (and eax, ~clear); mov crN, eax;
   mov ecx, crN; mov crN, esi. */
static int
put_crwrite(uint8_t *t, int cr, uint32_t set, uint32_t clear)
{
    int n    = 0;
    t[n++]   = 0x0f; /* mov esi, crN */
    t[n++]   = 0x20;
    t[n++]   = 0xc6 | (cr << 3);
    t[n++]   = 0x89; /* mov eax, esi */
    t[n++]   = 0xf0;
    if (set) {
        t[n++] = 0x0d; /* or eax, set */
        put_le(t + n, set, 4);
        n += 4;
    }
    if (clear) {
        t[n++] = 0x25; /* and eax, ~clear */
        put_le(t + n, ~clear, 4);
        n += 4;
    }
    t[n++] = 0x0f; /* mov crN, eax */
    t[n++] = 0x22;
    t[n++] = 0xc0 | (cr << 3);
    t[n++] = 0x0f; /* mov ecx, crN */
    t[n++] = 0x20;
    t[n++] = 0xc1 | (cr << 3);
    t[n++] = 0x0f; /* mov crN, esi */
    t[n++] = 0x22;
    t[n++] = 0xc6 | (cr << 3);
    return n;
}

/* CR0: each flag set or cleared alone (read back in ECX), NW without CD
   (#GP), CD with and without NW, and reserved bits. PE and PG are left
   alone: clearing one or setting the other takes the harness with it. */
static void
group_cr0_write(void)
{
    static const struct {
        uint32_t set, clear;
        uint8_t  defined;
    } cases[] = {
        { 1u << 1, 0, 1 }, { 1u << 2, 0, 1 }, { 1u << 3, 0, 1 }, { 1u << 5, 0, 1 }, { 1u << 16, 0, 1 }, { 1u << 18, 0, 1 },
        { 0, 1u << 1, 1 }, { 0, 1u << 5, 1 }, { 0, 1u << 16, 1 },
        { 0, 1u << 4, 0 },                     /* ET: hard-wired on some models */
        { 1u << 29, 1u << 30, 1 },             /* NW without CD: #GP */
        { 1u << 30, 1u << 29, 1 },             /* CD alone */
        { (1u << 30) | (1u << 29), 0, 1 },     /* CD and NW */
        { 1u << 6, 0, 0 }, { 1u << 17, 0, 0 }, { 1u << 28, 0, 0 }, /* reserved */
    };
    const char    *name = "cr0.write";
    struct variant v    = sys_variant(name);
    if (!sys_begin(name))
        return;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        v.target_len = put_crwrite(v.target, 0, cases[i].set, cases[i].clear);
        v.fx_arg     = cases[i].defined;
        run_plain(&v, 1);
    }
    group_end(name);
}

/* CR4: every bit alone, read back. Which ones a model has, and whether a
   bit it lacks is ignored or #GP, is the model's own: raw. */
static void
group_cr4_bits(void)
{
    const char    *name = "cr4.bits";
    struct variant v    = sys_variant(name);
    if (!sys_begin(name))
        return;
    v.fx_arg = 0;
    for (int b = 0; b < 32; b++) {
        if (b > 11 && b != 13 && b != 16 && b != 20 && b != 31)
            continue;
        v.target_len = put_crwrite(v.target, 4, 1u << b, 0);
        run_plain(&v, 1);
    }
    group_end(name);
}

/* CR0.EM, TS and MP against the x87 and MMX instructions: which ones #NM,
   which #UD (MMX with EM), which run. */
static void
group_cr0_ts(void)
{
    static const uint8_t combos[] = { 0x0, 0x2, 0x8, 0xa, 0x4, 0xc }; /* MP, TS, TS+MP, EM, EM+TS */
    static const struct {
        uint8_t mmx, now, len, b[4];
    } insns[] = {
        { 0, 0, SEQ(0x9b) },             /* wait */
        { 0, 0, SEQ(0xd9, 0xd0) },       /* fnop */
        { 0, 0, SEQ(0xdb, 0xe3) },       /* fninit */
        { 0, 0, SEQ(0xd9, 0xe8) },       /* fld1 */
        { 1, 0, SEQ(0x0f, 0x77) },       /* emms */
        { 1, 0, SEQ(0x0f, 0x6e, 0xc0) }, /* movd mm0, eax */
        { 1, 1, SEQ(0x0f, 0x0e) },       /* femms */
    };
    const char    *name = "cr0.ts";
    struct variant v    = sys_variant(name);
    if (!sys_begin(name))
        return;
    for (unsigned c = 0; c < sizeof(combos); c++) {
        for (unsigned i = 0; i < sizeof(insns) / sizeof(insns[0]); i++) {
            if (insns[i].mmx && !(g_cpu.features & CPUID_MMX))
                continue;
            if (insns[i].now && !(g_cpu.ext_features & EXT_3DNOW))
                continue;
            int      n = 0;
            uint8_t *t = v.target;
            t[n++]     = 0x0f; /* mov esi, cr0 */
            t[n++]     = 0x20;
            t[n++]     = 0xc6;
            t[n++]     = 0x89; /* mov eax, esi */
            t[n++]     = 0xf0;
            t[n++]     = 0x83; /* and eax, ~0Eh */
            t[n++]     = 0xe0;
            t[n++]     = 0xf1;
            t[n++]     = 0x83; /* or eax, combo */
            t[n++]     = 0xc8;
            t[n++]     = combos[c];
            t[n++]     = 0x0f; /* mov cr0, eax */
            t[n++]     = 0x22;
            t[n++]     = 0xc0;
            memcpy(t + n, insns[i].b, insns[i].len);
            n += insns[i].len;
            t[n++]       = 0x0f; /* mov cr0, esi */
            t[n++]       = 0x22;
            t[n++]       = 0xc6;
            v.target_len = n;
            run_plain(&v, 1);
        }
    }
    group_end(name);
}

/* LMSW: it can set PE but never clear it, and reaches only MP EM TS PE;
   SMSW to a register (its upper half the model's own: raw) and to memory
   (16 bits). */
static void
group_lmsw(void)
{
    static const uint16_t vals[] = { 0x0000, 0x0001, 0x0003, 0x000f, 0x0010, 0xfff0, 0xffff };
    const char           *name   = "lmsw";
    struct variant        v      = sys_variant(name);
    if (!sys_begin(name))
        return;
    for (unsigned i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        int      n = 0;
        uint8_t *t = v.target;
        t[n++]     = 0x0f; /* mov esi, cr0 */
        t[n++]     = 0x20;
        t[n++]     = 0xc6;
        t[n++]     = 0x66; /* mov ax, value */
        t[n++]     = 0xb8;
        t[n++]     = vals[i];
        t[n++]     = vals[i] >> 8;
        t[n++]     = 0x0f; /* lmsw ax */
        t[n++]     = 0x01;
        t[n++]     = 0xf0;
        t[n++]     = 0x0f; /* mov ecx, cr0 */
        t[n++]     = 0x20;
        t[n++]     = 0xc1;
        t[n++]     = 0x0f; /* mov cr0, esi */
        t[n++]     = 0x22;
        t[n++]     = 0xc6;
        v.target_len = n;
        v.fix_input  = 0;
        run_plain(&v, 1);
    }
    static const struct {
        uint8_t defined, len, b[8];
    } more[] = {
        { 0, SEQ(0x0f, 0x01, 0xe2) },             /* smsw edx: the upper half */
        { 1, SEQ(0x66, 0x0f, 0x01, 0xe2) },       /* smsw dx */
        { 1, SEQ(0x0f, 0x01, 0x23) },             /* smsw [ebx]: 16 bits */
    };
    for (unsigned i = 0; i < sizeof(more) / sizeof(more[0]); i++) {
        memcpy(v.target, more[i].b, more[i].len);
        v.target_len = more[i].len;
        v.fix_input  = fix_mem_ebx;
        v.arg        = MEM_OFF;
        v.fx_arg     = more[i].defined;
        run_plain(&v, 1);
    }
    group_end(name);
}

/* CR2 and CR3 round trips: CR2 takes anything; CR3 keeps PWT and PCD and
   the model decides the other low bits (raw). */
static void
group_cr23(void)
{
    static const uint32_t vals[] = { 0x12345678, 0xffffffff, 0x00000000, 0x00200018, 0x00200fff };
    const char           *name   = "cr23.rw";
    struct variant        v      = sys_variant(name);
    if (!sys_begin(name))
        return;
    for (int cr = 2; cr <= 3; cr++) {
        for (unsigned i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
            if (cr == 2 && i > 1)
                break;
            int      n = 0;
            uint8_t *t = v.target;
            t[n++]     = 0x0f; /* mov esi, crN */
            t[n++]     = 0x20;
            t[n++]     = 0xc6 | (cr << 3);
            t[n++]     = 0xb8; /* mov eax, value */
            put_le(t + n, vals[i], 4);
            n += 4;
            t[n++]       = 0x0f; /* mov crN, eax */
            t[n++]       = 0x22;
            t[n++]       = 0xc0 | (cr << 3);
            t[n++]       = 0x0f; /* mov ecx, crN */
            t[n++]       = 0x20;
            t[n++]       = 0xc1 | (cr << 3);
            t[n++]       = 0x0f; /* mov crN, esi */
            t[n++]       = 0x22;
            t[n++]       = 0xc6 | (cr << 3);
            v.target_len = n;
            v.fx_arg     = cr == 2;
            run_plain(&v, 1);
        }
    }
    group_end(name);
}

/* RDMSR of the TSC (10h) against RDTSC just after: the two agree to within
   a little, so EAX becomes 1; EDX is the upper halves' difference (0). */
static void
group_tsc_msr(void)
{
    static const uint8_t seq[] = {
        0xb9, 0x10, 0x00, 0x00, 0x00, /* mov ecx, 10h */
        0x0f, 0x32,                   /* rdmsr */
        0x89, 0xc6,                   /* mov esi, eax */
        0x89, 0xd7,                   /* mov edi, edx */
        0x0f, 0x31,                   /* rdtsc */
        0x29, 0xf0,                   /* sub eax, esi */
        0x19, 0xfa,                   /* sbb edx, edi */
        0x3d, 0x00, 0x00, 0x01, 0x00, /* cmp eax, 10000h */
        0x0f, 0x92, 0xc0,             /* setb al */
        0x0f, 0xb6, 0xc0,             /* movzx eax, al */
        0x31, 0xf6,                   /* xor esi, esi */
        0x31, 0xff,                   /* xor edi, edi */
        0x31, 0xc9,                   /* xor ecx, ecx */
    };
    const char    *name = "tsc.msr";
    struct variant v    = sys_variant(name);
    if (!(g_cpu.features & CPUID_MSR) || !(g_cpu.features & CPUID_TSC)) {
        group_skip(name, "no MSRs or TSC");
        return;
    }
    if (!sys_begin(name))
        return;
    memcpy(v.target, seq, sizeof(seq));
    v.target_len = sizeof(seq);
    run_plain(&v, 2);
    group_end(name);
}

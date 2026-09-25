/*
 * Control registers from ring 0: which CR0 and CR4 writes a model takes
 * or faults on, TS/EM/MP against the x87 and MMX instructions, LMSW and
 * SMSW, CR2 and CR3, the TSC through RDMSR. Each test saves the register,
 * writes, reads back what stuck and restores it; an after-run hook puts
 * CR0, CR3 and CR4 back too, in case the write or what followed faulted.
 * Included into groups.inc after groups_debug.inc.
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

/* SGDT and SIDT with 32- and 16-bit operand size: the limit stays in
   memory, the base is read back into ESI and cleared there (it is where
   this build put the table), and made relative to the real one; the byte
   after the six is the buffer's own. SLDT and STR to a 32-bit register
   (the upper half), a 16-bit one, and memory. */
static void
canon_sgdt(const struct variant *v, const struct kin *in, struct kout *o)
{
#ifndef HOSTTEST
    struct {
        uint16_t limit;
        uint32_t base;
    } __attribute__((packed)) t;
    if (v->arg2)
        __asm__ volatile("sidt %0" : "=m"(t));
    else
        __asm__ volatile("sgdt %0" : "=m"(t));
    o->esi -= t.base;
#endif
}

static void
fix_sgdt(const struct variant *v)
{
#ifdef HOSTTEST
    g_skip = 1; /* UMIP, and a table of Linux's */
#endif
    fix_mem_ebx(v);
}

static void
lmsw_sgdt(const char *name)
{
    static const struct {
        uint8_t len, b[16];
    } seqs[] = {
        { SEQ(0x0f, 0x01, 0x03, 0x8b, 0x73, 0x02, 0xc7, 0x43, 0x02, 0, 0, 0, 0) },       /* sgdt */
        { SEQ(0x66, 0x0f, 0x01, 0x03, 0x8b, 0x73, 0x02, 0xc7, 0x43, 0x02, 0, 0, 0, 0) }, /* o16 sgdt */
        { SEQ(0x0f, 0x01, 0x0b, 0x8b, 0x73, 0x02, 0xc7, 0x43, 0x02, 0, 0, 0, 0) },       /* sidt */
        { SEQ(0x66, 0x0f, 0x01, 0x0b, 0x8b, 0x73, 0x02, 0xc7, 0x43, 0x02, 0, 0, 0, 0) }, /* o16 sidt */
        { SEQ(0x0f, 0x00, 0xc1) },       /* sldt ecx */
        { SEQ(0x66, 0x0f, 0x00, 0xc1) }, /* sldt cx */
        { SEQ(0x0f, 0x00, 0x03) },       /* sldt [ebx] */
        { SEQ(0x0f, 0x00, 0xc9) },       /* str ecx */
        { SEQ(0x66, 0x0f, 0x00, 0xc9) }, /* str cx */
        { SEQ(0x0f, 0x00, 0x0b) },       /* str [ebx] */
    };
    struct variant v = { .group = name, .faults_ok = 1, .fx = FX_CUSTOM, .defmask = dm_fxarg,
                         .fix_input = fix_sgdt, .arg = MEM_OFF };
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.canon      = i < 4 ? canon_sgdt : 0;
        v.arg2       = i >= 2 && i < 4;
        v.fx_arg     = i != 4 && i != 7; /* a 32-bit register's upper half is the model's own */
        run_plain(&v, 1);
    }
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
    lmsw_sgdt(name);
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

/* ---- fast system calls: AMD SYSCALL/SYSRET, Intel SYSENTER/SYSEXIT ------- */

/* STAR: SYSCALL to 08h (SS 10h) at the variant's landing point, SYSRET to
   48h (SS 50h), both ring 3. EFER.SCE per variant (setup 100000h). What
   SYSCALL leaves: ECX the return address (made relative to it), CS, SS
   and EFLAGS (IF, RF and VM cleared); what SYSRET leaves: ring 3 with
   CS 4Bh, SS 53h and IF set (every IRQ is masked). SYSRET outside ring 0
   is #GP, both are #UD without SCE. arg2: 0 SYSCALL, 1 SYSRET. */
static uint32_t g_star_eip;

static void
sysc_before(const struct variant *v)
{
    ring_setup(v);
#ifndef HOSTTEST
    wrmsr_(MSR_STAR, g_star_eip, (SEL_CODE3 << 16) | 0x08);
    uint32_t efer = rdmsr_lo(MSR_EFER);
    wrmsr_(MSR_EFER, (v->arg3 & 0x100000) ? efer | 1 : efer & ~1u, 0);
#endif
}

static void
sysc_after(const struct variant *v)
{
#ifndef HOSTTEST
    wrmsr_(MSR_EFER, rdmsr_lo(MSR_EFER) & ~1u, 0);
    wrmsr_(MSR_STAR, 0, 0);
#endif
}

static void
emit_sysc_state(struct emit *e)
{
    static const uint8_t b[] = {
        0x8c, 0xce,                         /* mov esi, cs */
        0x8c, 0xd7,                         /* mov edi, ss */
        0x9c, 0x5a,                         /* pushfd; pop edx */
        0x81, 0xe2, 0x00, 0xff, 0xff, 0xff, /* and edx, ~0FFh: not the producer's flags */
    };
    ebytes(e, b, sizeof(b));
}

static void
emit_sysc(struct emit *e, const struct variant *v)
{
    if (v->arg2 == 0) {
        e8(e, 0x0f); /* syscall */
        e8(e, 0x05);
        g_star_eip = (uint32_t) e->p;
        e8(e, 0x81); /* sub ecx, here: 0 if ECX is the return address */
        e8(e, 0xe9);
        e32(e, g_star_eip);
        emit_sysc_state(e);
        return;
    }
    uint8_t *m = 0;
    if (v->ring == 0) {
        e8(e, 0xb9); /* mov ecx, M */
        m = e->p;
        e32(e, 0);
    }
    e8(e, 0x0f); /* sysret */
    e8(e, 0x07);
    if (m) {
        put32_at(m, (uint32_t) e->p); /* M: in ring 3 */
        emit_sysc_state(e);
        e8(e, 0x31); /* xor ecx, ecx: no address in the result */
        e8(e, 0xc9);
        e8(e, 0xcd); /* int 30h */
        e8(e, 0x30);
    }
}

/* SYSENTER and SYSEXIT (Pentium II on: the Pentium Pro reports SEP but
   hasn't got them). The SYSENTER MSRs: CS (setup 100000h: 08h; 200000h:
   04h; neither: 0, the one value that is #GP), ESP the ring-0
   stack's top, EIP the landing point. SYSENTER lands at CPL 0 with CS 08h,
   SS 10h and IF, VM and RF clear; SYSEXIT, only from CPL 0, lands at CPL
   3 with CS 1Bh, SS 23h (the MSR plus 16 and 24), ESP from ECX and EIP
   from EDX. Where they land: CS, SS and EFLAGS, and ESP made relative to
   the stack it should be, then INT 30h. arg2: 2 SYSENTER, 3 SYSEXIT. */
static uint32_t g_sysenter_eip, g_sysenter_saved[3] RING_UNUSED;

static void
sysenter_before(const struct variant *v)
{
    ring_setup(v);
#ifndef HOSTTEST
    wrmsr_(0x174, (v->arg3 & 0x100000) ? 0x08 : (v->arg3 & 0x200000) ? 0x04 : 0, 0);
    wrmsr_(0x175, (uint32_t) g_rstack[0] + sizeof(g_rstack[0]), 0);
    wrmsr_(0x176, g_sysenter_eip, 0);
#endif
}

static void
sysenter_after(const struct variant *v)
{
#ifndef HOSTTEST
    for (int i = 0; i < 3; i++)
        wrmsr_(0x174 + i, g_sysenter_saved[i], 0);
#endif
}

/* Where a fast call lands: CS, SS, ESP less the stack top it should be,
   EFLAGS without the producer's, and ECX cleared (an address); INT 30h. */
static void
emit_landing(struct emit *e, uint32_t top)
{
    static const uint8_t b[] = {
        0x8c, 0xce,                         /* mov esi, cs */
        0x8c, 0xd7,                         /* mov edi, ss */
        0x9c, 0x5a,                         /* pushfd; pop edx */
        0x81, 0xe2, 0x00, 0xff, 0xff, 0xff, /* and edx, ~0FFh */
        0x31, 0xc9,                         /* xor ecx, ecx */
        0x89, 0xe5,                         /* mov ebp, esp */
    };
    ebytes(e, b, sizeof(b));
    e8(e, 0x81); /* sub ebp, top */
    e8(e, 0xed);
    e32(e, top);
    e8(e, 0xcd); /* int 30h */
    e8(e, 0x30);
}

static void
emit_sysenter(struct emit *e, const struct variant *v)
{
    if (v->arg2 == 2) {
        e8(e, 0x0f); /* sysenter */
        e8(e, 0x34);
        g_sysenter_eip = (uint32_t) e->p;
        emit_landing(e, (uint32_t) g_rstack[0] + sizeof(g_rstack[0]));
        return;
    }
    uint8_t *m   = 0;
    uint32_t top = (uint32_t) g_rstack[3] + sizeof(g_rstack[3]);
    if (v->ring == 0) {
        e8(e, 0xba); /* mov edx, M */
        m = e->p;
        e32(e, 0);
        e8(e, 0xb9); /* mov ecx, the ring-3 stack */
        e32(e, top);
    }
    e8(e, 0x0f); /* sysexit */
    e8(e, 0x35);
    if (m) {
        put32_at(m, (uint32_t) e->p);
        emit_landing(e, top);
    }
}

static int
has_sysenter(void)
{
    unsigned family = (g_cpu.signature >> 8) & 0xf, model = (g_cpu.signature >> 4) & 0xf;
    return is_vendor("GenuineIntel") && (g_cpu.features & CPUID_SEP) && family == 6 && model >= 3;
}

/* The fast-call opcodes at CPL 3 with nothing set up (CPL 0 is in the
   opcode map): #UD where a model hasn't got one, #GP where it has
   (SYSEXIT and SYSRET outside ring 0, SYSENTER with its CS MSR 0, SYSCALL
   without EFER.SCE is #UD), and RSM, GETSEC, 0F FF, UD1 and MOV from TR6
   alongside. A variant that could really run is left out: SYSENTER and
   SYSEXIT where the SYSENTER CS MSR is set, SYSCALL and SYSRET where
   EFER.SCE is. Then SYSENTER/SYSEXIT and SYSCALL/SYSRET for real, where
   the model has them. */
static void
group_syscall(void)
{
    static const struct {
        uint8_t kind, len, b[3];
    } ops[] = {
        { 1, SEQ(0x0f, 0x34) }, { 1, SEQ(0x0f, 0x35) }, { 2, SEQ(0x0f, 0x05) }, { 2, SEQ(0x0f, 0x07) },
        { 0, SEQ(0x0f, 0xaa) }, { 0, SEQ(0x0f, 0x37) }, { 0, SEQ(0x0f, 0xff) }, { 0, SEQ(0x0f, 0xb9, 0xc0) },
        { 0, SEQ(0x0f, 0x24, 0xf0) },
    };
    static const struct {
        uint8_t ring, mode, sce;
    } amd[] = { { 0, 0, 1 }, { 1, 0, 1 }, { 3, 0, 1 }, { 0, 0, 0 }, { 3, 0, 0 },
                { 0, 1, 1 }, { 1, 1, 1 }, { 3, 1, 1 }, { 3, 1, 0 } };
    static const struct {
        uint8_t  ring, mode;
        uint32_t cs;
    } intel[] = { { 3, 2, 0x100000 }, { 1, 2, 0x100000 }, { 0, 2, 0x100000 }, { 0, 2, 0x200000 }, { 3, 2, 0 },
                  { 0, 3, 0x100000 }, { 1, 3, 0x100000 }, { 3, 3, 0x100000 }, { 0, 3, 0 } };
    const char    *name    = "syscall";
    struct variant v       = { .group = name, .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
                               .fix_input = fix_ring, .arg2 = RF_NONE, .fx = FX_CUSTOM, .defmask = dm_fxarg,
                               .fx_arg = 1, .ring = 3 };
    int            sep_set = 0, sce_set = 0, amd_sc = is_vendor("AuthenticAMD") && (g_cpu.ext_features & EXT_SYSCALL);
#ifndef HOSTTEST
    if (has_sysenter())
        sep_set = rdmsr_lo(0x174) != 0;
    if (amd_sc)
        sce_set = rdmsr_lo(MSR_EFER) & 1;
#endif
    if (!ring_begin(name, 0, 0))
        return;
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        if ((ops[i].kind == 1 && sep_set) || (ops[i].kind == 2 && sce_set))
            continue;
        memcpy(v.target, ops[i].b, ops[i].len);
        v.target_len = ops[i].len;
        run_plain(&v, 1);
    }
    v.fix_input = 0;
    if (amd_sc) {
        v.before_run = sysc_before;
        v.after_run  = sysc_after;
        v.emit       = emit_sysc;
        for (unsigned i = 0; i < sizeof(amd) / sizeof(amd[0]); i++) {
            v.target[0]  = 0x0f; /* for the reports */
            v.target[1]  = amd[i].mode ? 0x07 : 0x05;
            v.target[2]  = amd[i].ring;
            v.target[3]  = amd[i].sce;
            v.target_len = 4;
            v.ring       = amd[i].ring;
            v.arg2       = amd[i].mode;
            v.arg3       = amd[i].sce ? 0x100000 : 0;
            run_plain(&v, 1);
        }
    }
    if (has_sysenter()) {
#ifndef HOSTTEST
        if (!g_counting)
            for (int i = 0; i < 3; i++)
                g_sysenter_saved[i] = rdmsr_lo(0x174 + i);
#endif
        v.before_run = sysenter_before;
        v.after_run  = sysenter_after;
        v.emit       = emit_sysenter;
        for (unsigned i = 0; i < sizeof(intel) / sizeof(intel[0]); i++) {
            v.target[0]  = 0x0f; /* for the reports */
            v.target[1]  = intel[i].mode == 2 ? 0x34 : 0x35;
            v.target[2]  = intel[i].ring;
            v.target[3]  = intel[i].cs >> 20;
            v.target_len = 4;
            v.ring       = intel[i].ring;
            v.arg2       = intel[i].mode;
            v.arg3       = intel[i].cs;
            run_plain(&v, 1);
        }
    }
    ring_end(name, 0, 0);
}

/*
 * More system corners: AMD SYSCALL/SYSRET, CR4.PVI, RDPMC and CR4.PCE,
 * read-modify-write instructions on read-only and clean pages, the page
 * directory entry's own bits, global pages, the Cyrix EMMI extension,
 * segment overrides on string instructions, IRETD's checks on the frame
 * it returns through, the fast system-call opcodes where they don't
 * exist, and CMOV's memory read when the condition is false. Included
 * into groups.inc after groups_prot.inc; append only.
 */

#define EXT_SYSCALL (1u << 11)
#define MSR_EFER    0xc0000080u
#define MSR_STAR    0xc0000081u
#define CR4_PVI     (1u << 1)
#define CR4_PGE     (1u << 7)
#define CR4_PCE     (1u << 8)
#define CPUID_PGE   (1u << 13)
#define EF_VIF      0x80000u
#define EF_VIP      0x100000u
#define ALIAS       0x00, 0x00, 0x00, 0x40 /* PG_ALIAS_LIN, little-endian */

/* A ring sequence with any EFLAGS on the way in and a 32-bit setup (arg3:
   ring_setup's bitmaps in bits 0-1, pg_setup's pages from 128 up, a
   group's own bits from 100000h). */
struct rx_seq {
    uint8_t  ring, iopl, defined, len, b[24];
    uint32_t flags;
    uint32_t setup;
};

static __attribute__((unused)) void
rx_run(struct variant *v, const struct rx_seq *seqs, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        memcpy(v->target, seqs[i].b, seqs[i].len);
        v->target_len = seqs[i].len;
        v->ring       = seqs[i].ring;
        v->ring_flags = IOPL(seqs[i].iopl) | seqs[i].flags;
        v->arg3       = seqs[i].setup;
        v->fx_arg     = seqs[i].defined;
        run_plain(v, 1);
    }
}

#ifndef HOSTTEST
static __attribute__((unused)) void
wrmsr_(uint32_t msr, uint32_t lo, uint32_t hi)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static __attribute__((unused)) uint32_t
rdmsr_lo(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return lo;
}

static __attribute__((unused)) void
cr4_set(uint32_t bits, int on)
{
    uint32_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 = on ? cr4 | bits : cr4 & ~bits;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}
#endif

static int
is_vendor(const char *v)
{
    return g_cpu.has_cpuid && !memcmp_(g_cpu.vendor, v, 12);
}

/* ---- AMD SYSCALL and SYSRET (K6-2, K6-III) ------------------------------------ */

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

static void
group_syscall(void)
{
    static const struct {
        uint8_t ring, mode, sce;
    } cases[] = { { 0, 0, 1 }, { 1, 0, 1 }, { 3, 0, 1 }, { 0, 0, 0 }, { 3, 0, 0 },
                  { 0, 1, 1 }, { 1, 1, 1 }, { 3, 1, 1 }, { 3, 1, 0 } };
    struct variant v = { .group = "syscall", .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = sysc_before,
                         .after_run = sysc_after, .emit = emit_sysc, .fx = FX_CUSTOM, .defmask = dm_fxarg,
                         .fx_arg = 1 };
    if (!is_vendor("AuthenticAMD") || !(g_cpu.ext_features & EXT_SYSCALL)) {
        group_skip(v.group, "no AMD SYSCALL");
        return;
    }
    if (!ring_begin(v.group, 0, 0))
        return;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        v.target[0]  = 0x0f; /* for the reports */
        v.target[1]  = cases[i].mode ? 0x07 : 0x05;
        v.target[2]  = cases[i].ring;
        v.target[3]  = cases[i].sce;
        v.target_len = 4;
        v.ring       = cases[i].ring;
        v.arg2       = cases[i].mode;
        v.arg3       = cases[i].sce ? 0x100000 : 0;
        run_plain(&v, 1);
    }
    ring_end(v.group, 0, 0);
}

/* ---- CR4.PVI ------------------------------------------------------------------------ */

/* Protected-mode virtual interrupts: at CPL 3 with IOPL < 3, CLI and STI
   change VIF instead of faulting, and STI with VIP set is #GP; POPFD
   changes neither IF nor VIF; at IOPL 3 CLI is the real one; outside
   ring 3 nothing changes (#GP). */
static void
group_r3_pvi(void)
{
    static const struct rx_seq seqs[] = {
        { 3, 0, 1, SEQ(0xfa, 0x9c, 0x5e), EF_VIF, 0 },                         /* cli: VIF clear */
        { 3, 0, 1, SEQ(0xfb, 0x9c, 0x5e), 0, 0 },                              /* sti: VIF set */
        { 3, 0, 1, SEQ(0xfb), EF_VIP, 0 },                                     /* sti with VIP: #GP */
        { 3, 0, 1, SEQ(0xfa, 0x9c, 0x5e), EF_VIP | EF_VIF, 0 },                /* cli with VIP: fine */
        { 3, 0, 1, SEQ(0x9c, 0x5e), EF_VIF, 0 },                               /* pushfd shows VIF */
        { 3, 0, 1, SEQ(0x68, 0x00, 0x02, 0x00, 0x00, 0x9d, 0x9c, 0x5e), EF_VIF, 0 }, /* popfd with IF */
        { 3, 0, 1, SEQ(0x68, 0x00, 0x00, 0x08, 0x00, 0x9d, 0x9c, 0x5e), 0, 0 }, /* popfd with VIF */
        { 3, 3, 1, SEQ(0xfa, 0x9c, 0x5e), EF_VIF, 0 },                         /* IOPL 3: the real CLI */
        { 1, 0, 1, SEQ(0xfa), 0, 0 },                                          /* ring 1: #GP */
        { 2, 0, 1, SEQ(0xfb), 0, 0 },                                          /* ring 2: #GP */
    };
    struct variant v = { .group = "r3.pvi", .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
                         .fx = FX_CUSTOM, .defmask = dm_fxarg };
    if (!(g_cpu.features & CPUID_VME)) {
        group_skip(v.group, "no VME");
        return;
    }
    if (!ring_begin(v.group, CR4_PVI, 0))
        return;
    rx_run(&v, seqs, sizeof(seqs) / sizeof(seqs[0]));
    ring_end(v.group, CR4_PVI, 0);
}

/* ---- RDPMC and CR4.PCE ---------------------------------------------------------------- */

/* RDPMC of counters 0, 1 and 2 (#GP: there are two) at CPL 0, and outside
   it with and without CR4.PCE (setup 100000h). The counts are cleared
   straight after: only whether it faults counts. Intel with MMX, and the
   Pentium Pro. */
static void
pce_before(const struct variant *v)
{
    ring_setup(v);
#ifndef HOSTTEST
    cr4_set(CR4_PCE, v->arg3 & 0x100000);
#endif
}

static void
pce_after(const struct variant *v)
{
#ifndef HOSTTEST
    cr4_set(CR4_PCE, 0);
#endif
}

#define PMC(r, c, pce) { r, 0, 1, SEQ(0xb9, c, 0, 0, 0, 0x0f, 0x33, 0x31, 0xc0, 0x31, 0xd2), 0, (pce) ? 0x100000 : 0 }

static void
group_rdpmc_pce(void)
{
    static const struct rx_seq seqs[] = {
        PMC(0, 0, 0), PMC(0, 1, 0), PMC(0, 2, 0), PMC(3, 0, 0), PMC(3, 0, 1),
        PMC(3, 1, 1), PMC(3, 2, 1), PMC(1, 0, 0), PMC(1, 0, 1), PMC(2, 1, 1),
    };
    struct variant v      = { .group = "rdpmc.pce", .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = pce_before,
                              .after_run = pce_after, .fx = FX_CUSTOM, .defmask = dm_fxarg };
    unsigned       family = (g_cpu.signature >> 8) & 0xf;
    if (!is_vendor("GenuineIntel") || !(family == 6 || (family == 5 && (g_cpu.features & CPUID_MMX)))) {
        group_skip(v.group, "not a Pentium MMX or Pentium Pro");
        return;
    }
    if (!ring_begin(v.group, 0, 0))
        return;
    rx_run(&v, seqs, sizeof(seqs) / sizeof(seqs[0]));
    ring_end(v.group, 0, 0);
}

/* ---- read-modify-write on read-only and clean pages ------------------------------------- */

/* With CR0.WP: CMPXCHG and CMPXCHG8B on a read-only page fault whether
   the compare succeeds or not (a failed compare writes the old value
   back), XADD and locked BTS and ADD fault, BT only reads. On a clean
   writable page, the PTE read back after: a failed compare still sets
   D. The failed-compare cases are raw only. */
static void
group_pg_rmw(void)
{
    static const struct {
        uint8_t setup, defined, pte, len, b[12];
    } t[] = {
        { 4, 1, 0, SEQ(0x8b, 0x43, 0x18, 0x0f, 0xb1, 0x4b, 0x18) },                   /* cmpxchg, equal */
        { 4, 0, 0, SEQ(0x8b, 0x43, 0x18, 0xf7, 0xd0, 0x0f, 0xb1, 0x4b, 0x18) },       /* cmpxchg, not */
        { 4, 0, 0, SEQ(0x8a, 0x43, 0x18, 0xf6, 0xd0, 0x0f, 0xb0, 0x4b, 0x18) },       /* byte, not */
        { 4, 1, 0, SEQ(0x8b, 0x43, 0x18, 0x8b, 0x53, 0x1c, 0x0f, 0xc7, 0x4b, 0x18) }, /* cmpxchg8b, equal */
        { 4, 0, 0, SEQ(0x8b, 0x43, 0x18, 0xf7, 0xd0, 0x0f, 0xc7, 0x4b, 0x18) },       /* cmpxchg8b, not */
        { 4, 1, 0, SEQ(0x0f, 0xc1, 0x43, 0x18) },                                     /* xadd */
        { 4, 1, 0, SEQ(0xf0, 0x0f, 0xba, 0x6b, 0x18, 0x05) },                         /* lock bts */
        { 4, 1, 0, SEQ(0x0f, 0xba, 0x63, 0x18, 0x05) },                               /* bt: a read */
        { 4, 1, 0, SEQ(0xf0, 0x83, 0x43, 0x18, 0x00) },                               /* lock add 0 */
        { 0, 1, 1, SEQ(0x8b, 0x43, 0x18, 0x0f, 0xb1, 0x4b, 0x18) },                   /* equal: D */
        { 0, 0, 1, SEQ(0x8b, 0x43, 0x18, 0xf7, 0xd0, 0x0f, 0xb1, 0x4b, 0x18) },       /* not: D too */
        { 0, 0, 1, SEQ(0x8b, 0x43, 0x18, 0xf7, 0xd0, 0x0f, 0xc7, 0x4b, 0x18) },       /* cmpxchg8b not */
        { 0, 1, 1, SEQ(0x0f, 0xba, 0x63, 0x18, 0x05) },                               /* bt: A only */
    };
    static struct pg_seq seqs[sizeof(t) / sizeof(t[0])];
    uint32_t             pte = (uint32_t) pte_of((uint32_t) SANDBOX + 0x2000);
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        int n = t[i].len;
        memcpy(seqs[i].b, t[i].b, n);
        if (t[i].pte) {
            seqs[i].b[n++] = 0x8b; /* mov esi, [pte] */
            seqs[i].b[n++] = 0x35;
            put_le(seqs[i].b + n, pte, 4);
            n += 4;
        }
        seqs[i].len     = n;
        seqs[i].setup   = t[i].setup;
        seqs[i].defined = t[i].defined;
    }
    if (!g_cpu.is486) {
        group_skip("pg.rmw", "386 (no CR0.WP)");
        return;
    }
    pg_run_seqs("pg.rmw", seqs, sizeof(seqs) / sizeof(seqs[0]), CR0_WP);
}

/* ---- the page directory entry's own bits --------------------------------------------------- */

/* The alias page at 1 GB onto the buffer's second page, through a PDE
   made read-only or not present (CR0.WP): reads and writes, #PF with CR2
   in the alias area, and the PDE and PTE read back (A in both, D only in
   the PTE). */
#define RD_ALIAS  0xa1, 0x18, ALIAS                   /* mov eax, [40000018h] */
#define WR_ALIAS  0x89, 0x0d, 0x18, ALIAS             /* mov [40000018h], ecx */
#define GET_PDE   0x8b, 0x35, 0x00, 0x04, 0x20, 0x00  /* mov esi, [PG_DIR + 400h] */
#define GET_APTE  0x8b, 0x3d, 0x00, 0x10, 0x24, 0x00  /* mov edi, [PG_ALIAS] */

static void
group_pg_pde(void)
{
    static const struct pg_seq seqs[] = {
        { 128, 1, SEQ(RD_ALIAS, GET_PDE, GET_APTE), 0 },
        { 128, 1, SEQ(WR_ALIAS, GET_PDE, GET_APTE), 0 },
        { 128, 1, SEQ(RD_ALIAS, GET_PDE), 256 },      /* PDE read-only: reads work */
        { 128, 1, SEQ(WR_ALIAS), 256 },               /* writes don't */
        { 128, 1, SEQ(RD_ALIAS), 512 },               /* PDE not present */
        { 128, 1, SEQ(WR_ALIAS), 512 },
        { 0, 1, SEQ(RD_ALIAS, GET_PDE), 0 },          /* PDE present, PTE not */
    };
    if (!g_cpu.is486) {
        group_skip("pg.pde", "386 (no CR0.WP)");
        return;
    }
    pg_run_seqs("pg.pde", seqs, sizeof(seqs) / sizeof(seqs[0]), CR0_WP);
}

/* ---- global pages ------------------------------------------------------------------------------ */

/* The alias page read, its PTE pointed at the next page, CR3 reloaded,
   read again (ECX: a global entry survives the reload, with CR4.PGE; raw
   only), then INVLPG and a third read (ESI: the new page). Variants: PGE
   and G, PGE without G, G without PGE (arg2 bit 0: PGE). PGE is toggled
   off and on before each run, which flushes the global entries too. */
static void
pgg_before(const struct variant *v)
{
#ifndef HOSTTEST
    cr4_set(CR4_PGE, 0);
    pg_setup(v);
    if (v->arg2)
        cr4_set(CR4_PGE, 1);
#endif
}

static void
pgg_after(const struct variant *v)
{
#ifndef HOSTTEST
    cr4_set(CR4_PGE, 0);
    pg_reset(v);
#endif
}

static void
group_pg_global(void)
{
    static const struct {
        uint8_t pge, g;
    } cases[] = { { 1, 1 }, { 1, 0 }, { 0, 1 } };
    const char    *name = "pg.global";
    struct variant v    = { .group = name, .paging = 1, .faults_ok = 1, .fix_input = fix_mem_ebx, .arg = MEM_OFF,
                            .fx = FX_CUSTOM, .defmask = dm_tlb, .before_run = pgg_before, .after_run = pgg_after };
    if (!(g_cpu.features & CPUID_PGE)) {
        group_skip(name, "no PGE");
        return;
    }
    if (!pg_begin(name, 0))
        return;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t *t = v.target;
        int      n = 0;
        static const uint8_t rd[] = { 0xa1, ALIAS };
        memcpy(t + n, rd, sizeof(rd));
        n += sizeof(rd);
        t[n++] = 0xc7; /* mov dword [alias PTE], the next page */
        t[n++] = 0x05;
        put_le(t + n, PG_ALIAS, 4);
        n += 4;
        put_le(t + n, ((uint32_t) SANDBOX + 0x3000) | PTE_P | PTE_W | (cases[i].g ? 0x100 : 0), 4);
        n += 4;
        static const uint8_t rest[] = {
            0x0f, 0x20, 0xda,             /* mov edx, cr3 */
            0x0f, 0x22, 0xda,             /* mov cr3, edx */
            0x8b, 0x0d, ALIAS,            /* mov ecx, [alias] */
            0x0f, 0x01, 0x3d, ALIAS,      /* invlpg [alias] */
            0x8b, 0x35, ALIAS,            /* mov esi, [alias] */
        };
        memcpy(t + n, rest, sizeof(rest));
        n += sizeof(rest);
        v.target_len = n;
        v.arg2       = cases[i].pge;
        v.arg3       = 128 | (cases[i].g ? 4096 : 0);
        run_plain(&v, 1);
    }
    pg_end(name);
}

/* ---- the PDE's bits from the rings ---------------------------------------------------------------- */

/* The alias page from rings 3, 2 and 1 (CR0.WP clear) with the PDE or the
   PTE made supervisor-only, the PDE read-only or not present: U/S and R/W
   are the stricter of the two levels; supervisor code ignores U/S and,
   without WP, R/W. */
#define PDE_RD(r, hi) { r, 0, 1, SEQ(RD_ALIAS), 0, 128 | (hi) }
#define PDE_WR(r, hi) { r, 0, 1, SEQ(WR_ALIAS), 0, 128 | (hi) }

static void
group_r3_pde(void)
{
    static const struct rx_seq seqs[] = {
        PDE_RD(3, 0), PDE_WR(3, 0), PDE_RD(3, 1024), PDE_RD(3, 2048), PDE_RD(3, 256), PDE_WR(3, 256),
        PDE_RD(3, 512), PDE_RD(1, 1024), PDE_WR(1, 256), PDE_WR(1, 2048), PDE_RD(2, 1024), PDE_WR(2, 256),
    };
    const char    *name = "r3.pde";
    struct variant v    = { .group = name, .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
                            .fix_input = fix_ring, .arg2 = RF_SANDBOX, .fx = FX_CUSTOM, .defmask = dm_fxarg,
                            .paging = 1 };
#ifndef HOSTTEST
    v.after_run = pg_reset;
#endif
    if (!ring_begin(name, 0, 0))
        return;
#ifndef HOSTTEST
    if (!g_counting) {
        g_pg_user = PTE_U;
        pg_on(0);
    }
#endif
    rx_run(&v, seqs, sizeof(seqs) / sizeof(seqs[0]));
#ifndef HOSTTEST
    if (!g_counting) {
        pg_off();
        g_pg_user = 0;
    }
#endif
    ring_end(name, 0, 0);
}

/* ---- the Cyrix EMMI extension (6x86MX, MII) --------------------------------------------------------- */

/* The extended MMX instructions at 0F 50-5E, enabled by CCR7 bit 0 (with
   MAPEN in CCR3 to reach it) and #UD without it. Register forms where
   they exist, memory for all. arg2: enabled. */
#ifndef HOSTTEST
static uint8_t
cx_read(uint8_t i)
{
    outb(0x22, i);
    return inb(0x23);
}

static void
cx_write(uint8_t i, uint8_t val)
{
    outb(0x22, i);
    outb(0x23, val);
}

static void
emmi_set(int on)
{
    uint8_t ccr3 = cx_read(0xc3);
    cx_write(0xc3, (ccr3 & 0x0f) | 0x10); /* MAPEN */
    uint8_t ccr7 = cx_read(0xeb);
    cx_write(0xeb, on ? ccr7 | 1 : ccr7 & ~1);
    cx_write(0xc3, ccr3);
}

static void
emmi_before(const struct variant *v)
{
    emmi_set(v->arg2);
}
#endif

static void
group_cyrix_emmi(void)
{
    static const uint8_t regops[] = { 0x50, 0x51, 0x52, 0x55, 0x59, 0x5d }; /* paveb paddsiw pmagw psubsiw pmulhrw pmulhriw */
    static const uint8_t memops[] = { 0x54, 0x58, 0x5a, 0x5b, 0x5c, 0x5e }; /* pdistib pmvzb pmvnzb pmvlzb pmvgezb pmachriw */
    static const uint8_t pairs[3][2] = { { 0, 1 }, { 3, 5 }, { 6, RM_MEM } };
    const char          *name        = "cyrix.emmi";
    struct variant       v           = { .group = name, .mmx = 1, .faults_ok = 1, .arg = MEM_OFF };
    if (!g_cpu.cyrix || !(g_cpu.features & CPUID_MMX)) {
        group_skip(name, "not a Cyrix with MMX");
        return;
    }
#ifndef HOSTTEST
    v.before_run = emmi_before;
#endif
    group_begin(name);
    for (int on = 1; on >= 0; on--) {
        v.arg2 = on;
        for (unsigned o = 0; o < sizeof(regops) + sizeof(memops); o++) {
            int mem_only = o >= sizeof(regops);
            for (int p = mem_only ? 2 : 0; p < 3; p++) {
                v.target[0]  = 0x0f;
                v.target[1]  = mem_only ? memops[o - sizeof(regops)] : regops[o];
                v.target_len = put_mm_modrm(v.target, 2, pairs[p][0], pairs[p][1]);
                v.fix_input  = pairs[p][1] == RM_MEM ? fix_mem_ebx : 0;
                run_plain(&v, on ? 2 : 1);
                if (!on)
                    break; /* disabled: one form is enough */
            }
        }
    }
#ifndef HOSTTEST
    if (!g_counting)
        emmi_set(0);
#endif
    group_end(name);
}

/* ---- segment overrides on string instructions ---------------------------------------------------------- */

/* An override moves the source (DS:ESI) but never the destination
   (ES:EDI): with FS the 64 KB segment, MOVS, LODS and CMPS through it
   fault and STOS and SCAS don't; with ES null, STOS and MOVS fault and
   LODS doesn't, and an ES: override on the source faults; a REP with
   ECX = 0 checks nothing. ES is put back in the sequence. arg2: ECX 0. */
static void
fix_strseg(const struct variant *v)
{
    g_in.esi = (uint32_t) g_buf;
    g_in.edi = (uint32_t) g_buf + 32;
    g_in.ecx = v->arg2 ? 0 : 4;
    g_in.flags &= ~0x400u;
}

#define FS20 0x66, 0xb8, 0x20, 0x00, 0x8e, 0xe0 /* fs = 64 KB data */
#define ES00 0x66, 0xb8, 0x00, 0x00, 0x8e, 0xc0 /* es = null */
#define ES20 0x66, 0xb8, 0x20, 0x00, 0x8e, 0xc0 /* es = 64 KB data */
#define ES10 0x6a, 0x10, 0x07                   /* push 10h; pop es */

static void
group_string_segs(void)
{
    static const struct {
        uint8_t ecx0, len, b[16];
    } seqs[] = {
        { 0, SEQ(FS20, 0x64, 0xa4) },             /* fs: movsb */
        { 0, SEQ(FS20, 0x64, 0xaa) },             /* fs: stosb: ES:EDI still */
        { 0, SEQ(FS20, 0xf3, 0x64, 0xa4) },       /* rep fs: movsb */
        { 1, SEQ(FS20, 0xf3, 0x64, 0xa4) },       /* ECX = 0 */
        { 0, SEQ(FS20, 0x64, 0xae) },             /* fs: scasb */
        { 0, SEQ(FS20, 0x64, 0xa6) },             /* fs: cmpsb */
        { 0, SEQ(FS20, 0x64, 0xac) },             /* fs: lodsb */
        { 0, SEQ(ES00, 0xac, ES10) },             /* lodsb with ES null */
        { 0, SEQ(ES00, 0xaa, ES10) },             /* stosb */
        { 0, SEQ(ES00, 0xa4, ES10) },             /* movsb */
        { 1, SEQ(ES00, 0xf3, 0xaa, ES10) },       /* rep stosb, ECX = 0 */
        { 0, SEQ(ES00, 0x26, 0xac, ES10) },       /* es: lodsb */
        { 0, SEQ(ES20, 0xf3, 0xaa, ES10) },       /* rep stosb past ES's limit */
    };
    const char    *name = "string.segs";
    struct variant v    = { .group = name, .faults_ok = 1, .fix_input = fix_strseg,
                            .norm = (1 << R_ESI) | (1 << R_EDI) };
#ifdef HOSTTEST
    group_skip(name, "segment loads (host build)");
    return;
#endif
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.arg2       = seqs[i].ecx0;
        run_keep(&v, 1);
    }
    group_end(name);
}

/* ---- IRETD's checks on the frame --------------------------------------------------------------------------- */

/* IRETD at CPL 0 through a hand-built frame to an outer ring: a good one
   (DS, ES and FS of DPL 0 come out null), then a null, data, RPL-0 or
   out-of-GDT CS, a null, RPL-mismatched, DPL-0, read-only, not-present
   or TSS SS; execute-only and conforming code are fine to land in.
   arg3: SS << 16 | CS. */
static void
emit_iretfr(struct emit *e, const struct variant *v)
{
    static const uint8_t land[] = {
        0x8c, 0xde, /* mov esi, ds */
        0x8c, 0xc7, /* mov edi, es */
        0x8c, 0xe5, /* mov ebp, fs */
        0x8c, 0xca, /* mov edx, cs */
        0xcd, 0x30, /* int 30h */
    };
    e8(e, 0x68); /* push ss */
    e32(e, v->arg3 >> 16);
    e8(e, 0x68); /* push esp */
    e32(e, (uint32_t) g_rstack[3] + sizeof(g_rstack[0]));
    e8(e, 0x6a); /* push eflags: nothing but bit 1 */
    e8(e, 0x02);
    e8(e, 0x68); /* push cs */
    e32(e, v->arg3 & 0xffff);
    e8(e, 0x68); /* push the landing point */
    uint8_t *l = e->p;
    e32(e, 0);
    e8(e, 0xcf); /* iretd */
    put32_at(l, (uint32_t) e->p);
    ebytes(e, land, sizeof(land));
}

static void
group_iret_frames(void)
{
    static const uint16_t frames[][2] = {
        { SEL_CODE3 | 3, SEL_DATA3 | 3 },   /* good */
        { 0, SEL_DATA3 | 3 },               /* CS null */
        { SEL_DATA3 | 3, SEL_DATA3 | 3 },   /* CS data */
        { SEL_CODE3, SEL_DATA3 | 3 },       /* CS RPL 0, DPL 3 */
        { 0x1f8 | 3, SEL_DATA3 | 3 },       /* CS past the GDT */
        { SEL_CODE3 | 3, 0 },               /* SS null */
        { SEL_CODE3 | 3, SEL_DATA3 },       /* SS RPL 0 */
        { SEL_CODE3 | 3, 0x10 | 3 },        /* SS DPL 0 */
        { SEL_CODE3 | 3, SEL_RO3 | 3 },     /* SS read-only */
        { SEL_CODE3 | 3, SEL_NP3 | 3 },     /* SS not present: #SS */
        { SEL_CODE3 | 3, SEL_TSS | 3 },     /* SS a TSS */
        { SEL_XO3 | 3, SEL_DATA3 | 3 },     /* execute-only code */
        { SEL_CONF3 | 3, SEL_DATA3 | 3 },   /* conforming, DPL 3 */
        { SEL_CONF0 | 3, SEL_DATA3 | 3 },   /* conforming, DPL 0, RPL 3 */
        { SEL_CODE1 | 1, SEL_DATA1 | 1 },   /* to ring 1 */
        { SEL_CODE1 | 1, SEL_DATA3 | 3 },   /* ring 1 CS, ring 3 SS */
    };
    struct variant v = { .group = "iret.frames", .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
                         .emit = emit_iretfr, .fx = FX_CUSTOM, .defmask = dm_fxarg, .fx_arg = 1 };
    if (!ring_begin(v.group, 0, 0))
        return;
    for (unsigned i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
        v.target[0]  = 0xcf; /* for the reports */
        put_le(v.target + 1, frames[i][0], 2);
        put_le(v.target + 3, frames[i][1], 2);
        v.target_len = 5;
        v.arg3       = (uint32_t) frames[i][1] << 16 | frames[i][0];
        run_plain(&v, 1);
    }
    ring_end(v.group, 0, 0);
}

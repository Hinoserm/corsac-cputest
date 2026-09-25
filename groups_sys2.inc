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

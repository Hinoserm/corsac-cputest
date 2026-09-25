/*
 * More system corners: AMD SYSCALL/SYSRET, CR4.PVI, RDPMC and CR4.PCE,
 * read-modify-write instructions on read-only and clean pages, the page
 * directory entry's own bits, global pages, the Cyrix EMMI extension,
 * segment overrides on string instructions, IRETD's checks on the frame
 * it returns through, the fast system-call opcodes where they don't
 * exist, and CMOV's memory read when the condition is false. Included
 * into groups.inc after groups_prot.inc.
 */

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


/* ---- RDPMC and CR4.PCE ---------------------------------------------------------------- */


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


/* ---- IRETD's checks on the frame --------------------------------------------------------------------------- */


/* ---- the fast system-call opcodes where they aren't ---------------------------------------------------------- */

/* SYSENTER, SYSEXIT, SYSCALL, SYSRET, RSM outside SMM, GETSEC, 0F FF,
   UD1, and MOV from TR6 (a 486 and Cyrix test register), at CPL 0 and 3,
   with nothing set up: which of them a model knows (#GP) and which it
   doesn't (#UD). A variant that could really run is left out: SYSENTER
   and SYSEXIT where the SYSENTER CS MSR is set, SYSCALL and SYSRET where
   EFER.SCE is. */
static void
group_sysops_ud(void)
{
    static const struct {
        uint8_t kind, len, b[3];
    } ops[] = {
        { 1, SEQ(0x0f, 0x34) }, { 1, SEQ(0x0f, 0x35) }, { 2, SEQ(0x0f, 0x05) }, { 2, SEQ(0x0f, 0x07) },
        { 0, SEQ(0x0f, 0xaa) }, { 0, SEQ(0x0f, 0x37) }, { 0, SEQ(0x0f, 0xff) }, { 0, SEQ(0x0f, 0xb9, 0xc0) },
        { 0, SEQ(0x0f, 0x24, 0xf0) },
    };
    static struct ring_seq seqs[2 * sizeof(ops) / sizeof(ops[0])];
    unsigned               n = 0;
    int                    sep_set = 0, sce_set = 0;
#ifndef HOSTTEST
    unsigned family = (g_cpu.signature >> 8) & 0xf, model = (g_cpu.signature >> 4) & 0xf;
    if (is_vendor("GenuineIntel") && family == 6 && model >= 3)
        sep_set = rdmsr_lo(0x174) != 0;
    if (is_vendor("AuthenticAMD") && (g_cpu.ext_features & EXT_SYSCALL))
        sce_set = rdmsr_lo(MSR_EFER) & 1;
#endif
    for (int r = 0; r <= 3; r += 3) {
        for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
            if ((ops[i].kind == 1 && sep_set) || (ops[i].kind == 2 && sce_set))
                continue;
            struct ring_seq q = { r, 0, 0, 0, RF_NONE, 1, ops[i].len, { 0 } };
            memcpy(q.b, ops[i].b, ops[i].len);
            seqs[n++] = q;
        }
    }
    ring_run("sysops.ud", seqs, n, 0, 0, 0);
}

/* ---- CMOV's memory read ------------------------------------------------------------------------------------------ */


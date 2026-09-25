/*
 * Paging: page tables, #PF, the accessed and dirty bits, the TLB, 4 MB
 * pages. Paging is on only inside these groups, with an identity map of
 * the first 256 MB in 4 KB pages, plus an alias area at 1 GB whose page
 * table the tests point wherever they need. Before each run the tables
 * are put back and the TLB flushed, so every run starts the same. The
 * Linux build skips these groups. Included into groups.inc after
 * groups_cpu.inc.
 */

#define PG_DIR    0x200000u  /* the page directory */
#define PG_TABLES 0x201000u  /* 64 page tables: 0-256 MB */
#define PG_ALIAS  0x241000u  /* the page table for 1 GB up */
#define PG_ALIAS_LIN 0x40000000u
#define PTE_P  0x001u
#define PTE_W  0x002u
#define PTE_U  0x004u
#define PTE_A  0x020u
#define PTE_D  0x040u
#define PDE_PS 0x080u
/* The alias page's first dword (at 40000018h), its PDE and first PTE. */
#define ALIAS    0x00, 0x00, 0x00, 0x40                 /* PG_ALIAS_LIN, little-endian */
#define RD_ALIAS 0xa1, 0x18, ALIAS                      /* mov eax, [40000018h] */
#define WR_ALIAS 0x89, 0x0d, 0x18, ALIAS                /* mov [40000018h], ecx */
#define GET_PDE  0x8b, 0x35, 0x00, 0x04, 0x20, 0x00     /* mov esi, [PG_DIR + 400h] */
#define GET_APTE 0x8b, 0x3d, 0x00, 0x10, 0x24, 0x00     /* mov edi, [PG_ALIAS] */
#define CR0_PG (1u << 31)
#define CR0_WP (1u << 16)
#define CR4_PSE (1u << 4)
#define CPUID_PSE (1u << 3)

/* The code page a pg.smc.alias variant patches, set when it is emitted. */
static uint32_t g_smc_page;
/* Set by the ring groups: every page user-accessible, for code at CPL 3. */
static uint32_t g_pg_user;

static uint32_t *
pte_of(uint32_t lin)
{
    if (lin >= PG_ALIAS_LIN)
        return (uint32_t *) PG_ALIAS + ((lin - PG_ALIAS_LIN) >> 12);
    return (uint32_t *) PG_TABLES + (lin >> 12);
}

/* The tables as they start: identity, present and writable, A and D
   clear; the alias area not present. */
static void
pg_build(void)
{
    uint32_t *pd = (uint32_t *) PG_DIR;
    for (int i = 0; i < 1024; i++)
        pd[i] = 0;
    for (int i = 0; i < 64; i++)
        pd[i] = (PG_TABLES + i * 4096) | PTE_P | PTE_W | g_pg_user;
    pd[PG_ALIAS_LIN >> 22] = PG_ALIAS | PTE_P | PTE_W | g_pg_user;
    uint32_t *pt = (uint32_t *) PG_TABLES;
    for (uint32_t i = 0; i < 64 * 1024; i++)
        pt[i] = (i << 12) | PTE_P | PTE_W | g_pg_user;
    uint32_t *al = (uint32_t *) PG_ALIAS;
    for (int i = 0; i < 1024; i++)
        al[i] = 0;
}

#ifndef HOSTTEST
static void
pg_flush(void)
{
    uint32_t cr3;
    __asm__ volatile("mov %%cr3, %0\n\tmov %0, %%cr3" : "=r"(cr3) : : "memory");
}

/* Before each run: the pages the tests touch back as they started. */
static void
pg_reset(const struct variant *v)
{
    uint32_t base = (uint32_t) SANDBOX;
    for (uint32_t a = base; a < base + SANDBOX_SIZE; a += 4096)
        *pte_of(a) = a | PTE_P | PTE_W | g_pg_user;
    uint32_t *al = (uint32_t *) PG_ALIAS;
    for (int i = 0; i < 16; i++)
        al[i] = 0;
    ((uint32_t *) PG_DIR)[PG_ALIAS_LIN >> 22] = PG_ALIAS | PTE_P | PTE_W | g_pg_user; /* a PSE test takes it */
    pg_flush();
}


/* pg_reset, then what the variant needs (arg3):
   1: the buffer's second page (sandbox + 2000h) not present;
   2: its first page (sandbox + 1000h) not present;
   4: the second page read-only;
   8: the page after it (sandbox + 3000h) not present;
   16: 1 GB up a 4 MB page onto physical 0 (PSE);
   32: the first alias page onto g_smc_page;
   64: the second page supervisor-only (with g_pg_user);
   128: the first alias page (1 GB) onto the buffer's second page;
   256: the alias area's PDE read-only; 512: not present; 1024: not user;
   2048: the first alias PTE not user; 4096: global (G). */
static void
pg_setup(const struct variant *v)
{
    uint32_t a = v->arg3, s = (uint32_t) SANDBOX;
    pg_reset(v);
    if (a & 1)
        *pte_of(s + 0x2000) &= ~PTE_P;
    if (a & 2)
        *pte_of(s + 0x1000) &= ~PTE_P;
    if (a & 4)
        *pte_of(s + 0x2000) &= ~PTE_W;
    if (a & 8)
        *pte_of(s + 0x3000) &= ~PTE_P;
    if (a & 16)
        ((uint32_t *) PG_DIR)[PG_ALIAS_LIN >> 22] = PTE_P | PTE_W | PDE_PS;
    if (a & 32) {
        ((uint32_t *) PG_ALIAS)[0] = g_smc_page | PTE_P | PTE_W;
        smc_restore(v);
    }
    if (a & 64)
        *pte_of(s + 0x2000) &= ~PTE_U;
    uint32_t *pde = (uint32_t *) PG_DIR + (PG_ALIAS_LIN >> 22), *al = (uint32_t *) PG_ALIAS;
    if (a & 128)
        al[0] = (s + 0x2000) | PTE_P | PTE_W | g_pg_user;
    if (a & 256)
        *pde &= ~PTE_W;
    if (a & 512)
        *pde &= ~PTE_P;
    if (a & 1024)
        *pde &= ~PTE_U;
    if (a & 2048)
        al[0] &= ~PTE_U;
    if (a & 4096)
        al[0] |= 0x100;
    pg_flush();
}

static void
pg_on(uint32_t cr0_extra)
{
    uint32_t cr0;
    pg_build();
    __asm__ volatile("mov %0, %%cr3" : : "r"(PG_DIR) : "memory");
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_PG | cr0_extra;
    __asm__ volatile("mov %0, %%cr0\n\tjmp 1f\n1:" : : "r"(cr0) : "memory");
}

static void
pg_off(void)
{
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(CR0_PG | CR0_WP);
    __asm__ volatile("mov %0, %%cr0\n\tjmp 1f\n1:" : : "r"(cr0) : "memory");
}
#endif

/* The start of a paging group: skipped in the Linux build. */
static int
pg_begin(const char *name, uint32_t cr0_extra)
{
#ifdef HOSTTEST
    group_skip(name, "paging (host build)");
    return 0;
#else
    group_begin(name);
    if (!g_counting)
        pg_on(cr0_extra);
    return 1;
#endif
}

static void
pg_end(const char *name)
{
#ifndef HOSTTEST
    if (!g_counting)
        pg_off();
#endif
    group_end(name);
}

/* PAE (CR4.PAE, the Pentium Pro on): a page-directory-pointer table at
   PAE_PDPT, one page directory for the first GB in 2 MB pages, but the
   2 MB around the sandbox in 4 KB pages (PAE_PT), all identity, with
   CR0.WP. Each run builds the tables afresh and switches paging to them,
   and back to the 32-bit tables after. arg3: 1 the buffer's second page
   not present, 4 read-only, 8 with a reserved bit (40) set. */
#define PAE_PDPT 0x250000u
#define PAE_PD   0x251000u
#define PAE_PT   0x252000u
#define CR4_PAE  (1u << 5)
#define CPUID_PAE (1u << 6)

#ifndef HOSTTEST
static uint32_t *
pae_pte(uint32_t lin)
{
    return (uint32_t *) PAE_PT + ((lin - 0x200000) >> 12) * 2;
}

static void
pae_before(const struct variant *v)
{
    uint32_t *pdpt = (uint32_t *) PAE_PDPT, *pd = (uint32_t *) PAE_PD, *pt = (uint32_t *) PAE_PT;
    uint32_t  s    = (uint32_t) SANDBOX, cr0, cr4;
    for (int i = 0; i < 8; i++)
        pdpt[i] = 0;
    pdpt[0] = PAE_PD | PTE_P;
    for (int i = 0; i < 512; i++) {
        pd[2 * i]     = (i << 21) | PTE_P | PTE_W | PDE_PS;
        pd[2 * i + 1] = 0;
        pt[2 * i]     = (0x200000 + (i << 12)) | PTE_P | PTE_W;
        pt[2 * i + 1] = 0;
    }
    pd[2] = PAE_PT | PTE_P | PTE_W; /* 200000h-3FFFFFh in 4 KB pages */
    uint32_t *e = pae_pte(s + 0x2000);
    if (v->arg3 & 1)
        e[0] &= ~PTE_P;
    if (v->arg3 & 4)
        e[0] &= ~PTE_W;
    if (v->arg3 & 8)
        e[1] |= 1u << 8;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0\n\tjmp 1f\n1:" : : "r"(cr0 & ~CR0_PG) : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 | CR4_PAE));
    __asm__ volatile("mov %0, %%cr3" : : "r"(PAE_PDPT) : "memory");
    __asm__ volatile("mov %0, %%cr0\n\tjmp 1f\n1:" : : "r"(cr0 | CR0_PG | CR0_WP) : "memory");
}

static void
pae_after(const struct variant *v)
{
    uint32_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0\n\tjmp 1f\n1:" : : "r"(cr0 & ~(CR0_PG | CR0_WP)) : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 & ~CR4_PAE));
    __asm__ volatile("mov %0, %%cr3" : : "r"(PG_DIR) : "memory");
    __asm__ volatile("mov %0, %%cr0\n\tjmp 1f\n1:" : : "r"(cr0 & ~CR0_WP) : "memory");
    pg_reset(v);
}
#endif

static void
pg_pae(const char *name)
{
#ifndef HOSTTEST
    struct variant v   = { .group = name, .paging = 1, .faults_ok = 1, .fix_input = fix_mem_ebx, .arg = MEM_OFF,
                           .before_run = pae_before, .after_run = pae_after };
    uint32_t       pte = (uint32_t) pae_pte((uint32_t) SANDBOX + 0x2000);
    if (!(g_cpu.features & CPUID_PAE))
        return;
    static const struct {
        uint8_t setup, pte, len, b[8];
    } seqs[] = {
        { 0, 1, SEQ(0x8b, 0x43, 0x18) }, /* mov eax, [ebx+24]: then the PTE (A) */
        { 0, 1, SEQ(0x89, 0x4b, 0x18) }, /* mov [ebx+24], ecx: D */
        { 0, 0, SEQ(0x8b, 0x43, 0x16) }, /* across the page boundary */
        { 1, 0, SEQ(0x8b, 0x43, 0x18) }, /* not present */
        { 4, 0, SEQ(0x89, 0x4b, 0x18) }, /* read-only: #PF with WP */
        { 4, 0, SEQ(0x8b, 0x43, 0x18) },
        { 8, 0, SEQ(0x8b, 0x43, 0x18) }, /* a reserved bit: #PF, RSVD in the error code */
        { 0, 2, SEQ(0xa1, 0x10, 0x00, 0x60, 0x00, 0x31, 0xc0) }, /* a 2 MB page, then its PDE (A) */
    };
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        int n = seqs[i].len;
        memcpy(v.target, seqs[i].b, n);
        if (seqs[i].pte) {
            uint32_t at = seqs[i].pte == 1 ? pte : PAE_PD + 3 * 8;
            v.target[n++] = 0x8b; /* mov esi, [entry]; mov edi, [entry+4] */
            v.target[n++] = 0x35;
            put_le(v.target + n, at, 4);
            n += 4;
            v.target[n++] = 0x8b;
            v.target[n++] = 0x3d;
            put_le(v.target + n, at + 4, 4);
            n += 4;
        }
        v.target_len = n;
        v.arg3       = seqs[i].setup;
        run_plain(&v, 1);
    }
#endif
}

/* Ordinary memory work with paging on: loads, stores, read-modify-write,
   a misaligned dword across the page boundary in the buffer, string
   moves, PUSH/POP. */
static void
group_pg_basic(void)
{
    static const struct {
        uint8_t len, b[8];
    } seqs[] = {
        { SEQ(0x8b, 0x03) },             /* mov eax, [ebx] */
        { SEQ(0x89, 0x0b) },             /* mov [ebx], ecx */
        { SEQ(0x01, 0x13) },             /* add [ebx], edx */
        { SEQ(0x8b, 0x43, 0x16) },       /* mov eax, [ebx+22]: across the page boundary */
        { SEQ(0x89, 0x53, 0x17) },       /* mov [ebx+23], edx: across it */
        { SEQ(0x0f, 0xc1, 0x4b, 0x15) }, /* xadd [ebx+21], ecx: across it */
        { SEQ(0x87, 0x43, 0x16) },       /* xchg [ebx+22], eax: across it */
        { SEQ(0xff, 0x33, 0x8f, 0x43, 0x18) }, /* push [ebx]; pop [ebx+24] */
    };
    const char    *name = "pg.basic";
    struct variant v    = { .group = name, .paging = 1, .faults_ok = 1, .fix_input = fix_mem_ebx, .arg = MEM_OFF };
#ifndef HOSTTEST
    v.before_run = pg_reset;
    v.after_run  = pg_reset;
#endif
    if (!pg_begin(name, 0))
        return;
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        run_plain(&v, 2);
    }
    pg_pae(name);
    pg_end(name);
}

/* A table of byte sequences run with one page setup each. */
struct pg_seq {
    uint8_t  setup, defined, len, b[20];
    uint16_t setup_hi; /* pg_setup bits from 256 up, added to setup */
};

static void
pg_run_seqs(const char *name, const struct pg_seq *seqs, unsigned n, uint32_t cr0_extra)
{
    struct variant v = { .group = name, .paging = 1, .faults_ok = 1, .fix_input = fix_mem_ebx, .arg = MEM_OFF,
                         .fx = FX_CUSTOM, .defmask = dm_fxarg };
#ifndef HOSTTEST
    v.before_run = pg_setup;
    v.after_run  = pg_reset; /* before the harness reads the buffer */
#endif
    if (!pg_begin(name, cr0_extra))
        return;
    for (unsigned i = 0; i < n; i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.arg3       = seqs[i].setup | seqs[i].setup_hi;
        v.fx_arg     = seqs[i].defined;
        run_plain(&v, 1);
    }
    pg_end(name);
}

/* [ebx+24] is the first byte of the buffer's second page. Read-modify-
   write forms report their error code one way per model: raw only. */
static void
group_pg_notpresent(void)
{
    static const struct pg_seq seqs[] = {
        { 1, 1, SEQ(0x8b, 0x43, 0x18) },             /* mov eax, [ebx+24] */
        { 1, 1, SEQ(0x89, 0x4b, 0x18) },             /* mov [ebx+24], ecx */
        { 1, 0, SEQ(0x01, 0x53, 0x18) },             /* add [ebx+24], edx */
        { 1, 0, SEQ(0xff, 0x43, 0x18) },             /* inc dword [ebx+24] */
        { 1, 0, SEQ(0x0f, 0xb1, 0x4b, 0x18) },       /* cmpxchg [ebx+24], ecx */
        { 1, 0, SEQ(0x0f, 0xba, 0x6b, 0x18, 0x05) }, /* bts dword [ebx+24], 5 */
        { 1, 0, SEQ(0xf0, 0x01, 0x53, 0x18) },       /* lock add [ebx+24], edx */
        { 1, 1, SEQ(0xff, 0x73, 0x18) },             /* push dword [ebx+24] */
        { 1, 1, SEQ(0x8f, 0x43, 0x18) },             /* pop dword [ebx+24] */
        { 1, 1, SEQ(0x8b, 0x43, 0x10) },             /* mov eax, [ebx+16]: the present page */
        { 1, 1, SEQ(0x0f, 0xb6, 0x43, 0x18) },       /* movzx eax, byte [ebx+24] */
        { 128, 1, SEQ(RD_ALIAS), 512 },              /* the alias page through a missing PDE */
        { 128, 1, SEQ(WR_ALIAS), 512 },
        { 0, 1, SEQ(RD_ALIAS) },                     /* the PDE there, the PTE not */
    };
    pg_run_seqs("pg.notpresent", seqs, sizeof(seqs) / sizeof(seqs[0]), 0);
}

/* The buffer's second page read-only: supervisor writes succeed with
   CR0.WP clear, and fault (error code 3) with it set. */
static const struct pg_seq pg_ro_seqs[] = {
    { 4, 1, SEQ(0x8b, 0x43, 0x18) },       /* mov eax, [ebx+24]: reads always work */
    { 4, 1, SEQ(0x89, 0x4b, 0x18) },       /* mov [ebx+24], ecx */
    { 4, 1, SEQ(0x01, 0x53, 0x18) },       /* add [ebx+24], edx */
    { 4, 1, SEQ(0x87, 0x43, 0x18) },       /* xchg [ebx+24], eax */
    { 4, 1, SEQ(0x0f, 0xb1, 0x4b, 0x18) }, /* cmpxchg [ebx+24], ecx */
    { 4, 1, SEQ(0x89, 0x4b, 0x16) },       /* mov [ebx+22], ecx: half on each page */
    { 4, 1, SEQ(0x88, 0x4b, 0x17) },       /* mov [ebx+23], cl: the writable page */
    /* Read-modify-write: CMPXCHG and CMPXCHG8B write whether the compare
       succeeds or not (a failed one writes the old value back), so both
       fault with WP; the failed compares are raw only. */
    { 4, 1, SEQ(0x8b, 0x43, 0x18, 0x0f, 0xb1, 0x4b, 0x18) },                   /* cmpxchg, equal */
    { 4, 0, SEQ(0x8b, 0x43, 0x18, 0xf7, 0xd0, 0x0f, 0xb1, 0x4b, 0x18) },       /* cmpxchg, not */
    { 4, 0, SEQ(0x8a, 0x43, 0x18, 0xf6, 0xd0, 0x0f, 0xb0, 0x4b, 0x18) },       /* byte, not */
    { 4, 1, SEQ(0x8b, 0x43, 0x18, 0x8b, 0x53, 0x1c, 0x0f, 0xc7, 0x4b, 0x18) }, /* cmpxchg8b, equal */
    { 4, 0, SEQ(0x8b, 0x43, 0x18, 0xf7, 0xd0, 0x0f, 0xc7, 0x4b, 0x18) },       /* cmpxchg8b, not */
    { 4, 1, SEQ(0x0f, 0xc1, 0x43, 0x18) },                                     /* xadd */
    { 4, 1, SEQ(0xf0, 0x0f, 0xba, 0x6b, 0x18, 0x05) },                         /* lock bts */
    { 4, 1, SEQ(0x0f, 0xba, 0x63, 0x18, 0x05) },                               /* bt: a read */
    { 4, 1, SEQ(0xf0, 0x83, 0x43, 0x18, 0x00) },                               /* lock add 0 */
    /* The alias page through a read-only PDE: the stricter level counts. */
    { 128, 1, SEQ(RD_ALIAS, GET_PDE), 256 },
    { 128, 1, SEQ(WR_ALIAS), 256 },
};

static void
group_pg_readonly_wp0(void)
{
    pg_run_seqs("pg.readonly.wp0", pg_ro_seqs, sizeof(pg_ro_seqs) / sizeof(pg_ro_seqs[0]), 0);
}

static void
group_pg_readonly_wp1(void)
{
    if (!g_cpu.is486) {
        group_skip("pg.readonly.wp1", "386 (no CR0.WP)");
        return;
    }
    pg_run_seqs("pg.readonly.wp1", pg_ro_seqs, sizeof(pg_ro_seqs) / sizeof(pg_ro_seqs[0]), CR0_WP);
}

/* Accesses split across a present and a not-present page: which half
   faults, CR2, and that the half on the present page is not written.
   CR2 for a split access is per model: raw only. */
static void
group_pg_cross(void)
{
    static const struct pg_seq seqs[] = {
        { 1, 0, SEQ(0x8b, 0x43, 0x16) },       /* mov eax, [ebx+22]: into the missing page */
        { 1, 0, SEQ(0x89, 0x4b, 0x16) },       /* mov [ebx+22], ecx */
        { 1, 0, SEQ(0x66, 0x89, 0x4b, 0x17) }, /* mov [ebx+23], cx */
        { 1, 0, SEQ(0x01, 0x53, 0x15) },       /* add [ebx+21], edx */
        { 2, 0, SEQ(0x8b, 0x43, 0x16) },       /* the first page missing instead */
        { 2, 0, SEQ(0x89, 0x4b, 0x16) },
    };
    pg_run_seqs("pg.cross", seqs, sizeof(seqs) / sizeof(seqs[0]), 0);
}

/* The accessed and dirty bits: the tests read the second page's PTE back
   after touching the page, or the alias page's PDE and PTE (A in both, D
   only in the PTE). A failed CMPXCHG still writes, so it sets D: raw
   only, like everything about failed compares. */
static void
group_pg_ad(void)
{
    static const struct {
        uint8_t setup, defined, pte, len, b[19];
    } t[] = {
        { 0, 1, 1, SEQ(0x8b, 0x43, 0x18) },                               /* mov eax, [ebx+24] */
        { 0, 1, 1, SEQ(0x89, 0x4b, 0x18) },                               /* mov [ebx+24], ecx */
        { 0, 1, 1, SEQ(0x01, 0x53, 0x18) },                               /* add [ebx+24], edx */
        { 0, 1, 1, SEQ(0x8b, 0x43, 0x18, 0x89, 0x4b, 0x18) },             /* read, then write: D once the TLB has it */
        { 0, 1, 1, SEQ(0x85, 0x43, 0x18) },                               /* test [ebx+24], eax: a read that writes nothing */
        { 0, 1, 1, SEQ(0x8b, 0x43, 0x18, 0x0f, 0xb1, 0x4b, 0x18) },       /* cmpxchg, equal */
        { 0, 0, 1, SEQ(0x8b, 0x43, 0x18, 0xf7, 0xd0, 0x0f, 0xb1, 0x4b, 0x18) }, /* cmpxchg, not */
        { 0, 0, 1, SEQ(0x8b, 0x43, 0x18, 0xf7, 0xd0, 0x0f, 0xc7, 0x4b, 0x18) }, /* cmpxchg8b, not */
        { 0, 1, 1, SEQ(0x0f, 0xba, 0x63, 0x18, 0x05) },                   /* bt: A only */
        { 128, 1, 0, SEQ(RD_ALIAS, GET_PDE, GET_APTE) },
        { 128, 1, 0, SEQ(WR_ALIAS, GET_PDE, GET_APTE) },
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
    pg_run_seqs("pg.ad", seqs, sizeof(seqs) / sizeof(seqs[0]), 0);
}

/* The TLB: read the second page, point its PTE at the next physical page,
   read again (a CPU may still use the old translation: raw only, ECX),
   then INVLPG or reload CR3 and read a third time (the new one). And a
   page never read before, remapped: no stale entry to find. */
static int
dm_tlb(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    d->ecx = 0;
    return 1;
}

/* Global pages: the alias page read, its PTE pointed at the next page, CR3 reloaded,
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
tlb_global(const char *name)
{
    static const struct {
        uint8_t pge, g;
    } cases[] = { { 1, 1 }, { 1, 0 }, { 0, 1 } };
    struct variant v = { .group = name, .paging = 1, .faults_ok = 1, .fix_input = fix_mem_ebx, .arg = MEM_OFF,
                         .fx = FX_CUSTOM, .defmask = dm_tlb, .before_run = pgg_before, .after_run = pgg_after };
    if (!(g_cpu.features & CPUID_PGE))
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
}

static void
group_pg_tlb(void)
{
    const char    *name = "pg.tlb";
    struct variant v    = { .group = name, .paging = 1, .faults_ok = 1, .fix_input = fix_mem_ebx, .arg = MEM_OFF,
                            .fx = FX_CUSTOM, .defmask = dm_tlb };
    uint32_t       pte2 = (uint32_t) pte_of((uint32_t) SANDBOX + 0x2000);
    uint32_t       pte3 = (uint32_t) pte_of((uint32_t) SANDBOX + 0x3000);
#ifndef HOSTTEST
    v.before_run = pg_setup;
    v.after_run  = pg_reset; /* before the harness reads the buffer */
#endif
    if (!g_cpu.is486) {
        group_skip(name, "386 (no INVLPG)");
        return;
    }
    if (!pg_begin(name, 0))
        return;
    for (int k = 0; k < 3; k++) {
        int n = 0;
        uint8_t *t = v.target;
        if (k < 2) {
            t[n++] = 0x8b; t[n++] = 0x43; t[n++] = 0x18; /* mov eax, [ebx+24] */
        }
        t[n++] = 0xc7; t[n++] = 0x05;                     /* mov dword [pte], next page */
        put_le(t + n, k < 2 ? pte2 : pte3, 4);
        n += 4;
        put_le(t + n, (k < 2 ? (uint32_t) SANDBOX + 0x3000 : (uint32_t) SANDBOX + 0x1000) | PTE_P | PTE_W, 4);
        n += 4;
        if (k < 2) {
            t[n++] = 0x8b; t[n++] = 0x4b; t[n++] = 0x18; /* mov ecx, [ebx+24]: old or new */
            if (k == 0) {
                t[n++] = 0x0f; t[n++] = 0x01; t[n++] = 0x7b; t[n++] = 0x18; /* invlpg [ebx+24] */
            } else {
                t[n++] = 0x0f; t[n++] = 0x20; t[n++] = 0xde; /* mov esi, cr3 */
                t[n++] = 0x0f; t[n++] = 0x22; t[n++] = 0xde; /* mov cr3, esi */
            }
            t[n++] = 0x8b; t[n++] = 0x53; t[n++] = 0x18; /* mov edx, [ebx+24]: new */
        } else {
            /* page 3 is [ebx+1018h]: never read, so no stale entry */
            t[n++] = 0x8b; t[n++] = 0x93; /* mov edx, [ebx+1018h] */
            put_le(t + n, 0x1018, 4);
            n += 4;
        }
        v.target_len = n;
        v.arg3       = 0;
        run_plain(&v, 1);
    }
    tlb_global(name);
    pg_end(name);
}

/* A 4 MB page (CR4.PSE) at 1 GB onto physical 0: the buffer read and
   written through it and through the identity map, and the directory
   entry's accessed and dirty bits. */
static void
group_pg_pse(void)
{
    static struct pg_seq seqs[4];
    const char          *name = "pg.pse";
    uint32_t             pde  = PG_DIR + (PG_ALIAS_LIN >> 22) * 4;
    if (!(g_cpu.features & CPUID_PSE)) {
        group_skip(name, "no PSE");
        return;
    }
    static const uint8_t acc[4][10] = {
        { 6, 0x8b, 0x83, 0x00, 0x00, 0x00, 0x40 },             /* mov eax, [ebx+40000000h] */
        { 8, 0x89, 0x8b, 0x00, 0x00, 0x00, 0x40, 0x8b, 0x13 }, /* mov [ebx+40000000h], ecx; mov edx, [ebx] */
        { 8, 0x89, 0x0b, 0x8b, 0x93, 0x00, 0x00, 0x00, 0x40 }, /* mov [ebx], ecx; mov edx, [ebx+40000000h] */
        { 6, 0x01, 0x83, 0x00, 0x00, 0x00, 0x40 },             /* add [ebx+40000000h], eax */
    };
    for (int i = 0; i < 4; i++) {
        int n = acc[i][0];
        memcpy(seqs[i].b, acc[i] + 1, n);
        seqs[i].b[n++] = 0x8b; /* mov esi, [pde] */
        seqs[i].b[n++] = 0x35;
        put_le(seqs[i].b + n, pde, 4);
        n += 4;
        seqs[i].len     = n;
        seqs[i].setup   = 16;
        seqs[i].defined = 1;
    }
#ifndef HOSTTEST
    uint32_t cr4 = 0;
    if (!g_counting) {
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 | CR4_PSE));
    }
#endif
    pg_run_seqs(name, seqs, 4, 0);
#ifndef HOSTTEST
    if (!g_counting)
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
#endif
}

/* String instructions that run into a missing page part-way: the
   registers at the fault say how far they got (and must let the
   instruction restart), the memory how much was done. arg2: the case. */
static void
fix_pgstr(const struct variant *v)
{
    uint32_t s = (uint32_t) SANDBOX;
    g_in.flags &= ~0x400u;
    switch (v->arg2) {
        case 0: g_in.esi = (uint32_t) g_buf; g_in.edi = s + 0x2ff8; g_in.ecx = 6; break;  /* movsd into page 3 */
        case 1: g_in.edi = s + 0x2ffc; g_in.ecx = 10; break;                              /* stosb into page 3 */
        case 2: g_in.esi = s + 0x2ff8; g_in.ecx = 4; break;                               /* lodsd from page 3 */
        default:                                                                          /* movsd down into page 1 */
            g_in.flags |= 0x400u;
            g_in.esi = (uint32_t) g_buf + 40;
            g_in.edi = s + 0x2004;
            g_in.ecx = 6;
            break;
    }
}

static void
group_pg_string(void)
{
    static const struct {
        uint8_t setup, len, b[2];
    } seqs[] = { { 8, SEQ(0xf3, 0xa5) }, { 8, SEQ(0xf3, 0xaa) }, { 8, SEQ(0xf3, 0xad) }, { 2, SEQ(0xf3, 0xa5) } };
    const char    *name = "pg.string";
    struct variant v    = { .group = name, .paging = 1, .faults_ok = 1, .fix_input = fix_pgstr,
                            .norm = (1 << R_ESI) | (1 << R_EDI) };
#ifndef HOSTTEST
    v.before_run = pg_setup;
    v.after_run  = pg_reset; /* before the harness reads the buffer */
#endif
    if (!pg_begin(name, 0))
        return;
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.arg2       = i;
        v.arg3       = seqs[i].setup;
        run_plain(&v, 1);
    }
    pg_end(name);
}

/* A jump and a call into a missing page: #PF on the fetch, CR2 and EIP
   the target. */
static void
group_pg_exec(void)
{
    static struct pg_seq seqs[2];
    uint32_t             target = (uint32_t) SANDBOX + 0x2000;
    for (int i = 0; i < 2; i++) {
        seqs[i].b[0] = 0xb8; /* mov eax, target */
        put_le(seqs[i].b + 1, target, 4);
        seqs[i].b[5]    = 0xff;
        seqs[i].b[6]    = i ? 0xd0 : 0xe0; /* call eax / jmp eax */
        seqs[i].len     = 7;
        seqs[i].setup   = 1;
        seqs[i].defined = 1;
    }
    pg_run_seqs("pg.exec", seqs, 2, 0);
}

/* Self-modifying code through a second mapping of the code page: the
   store goes to the alias at 1 GB, the jump to the real address. Without
   the jump a 486 may run what it prefetched: raw only. */
static void
emit_smc_alias(struct emit *e, const struct variant *v)
{
    uint8_t *store = e->p;
    e8(e, 0x88); /* mov [alias of L+1], cl */
    e8(e, 0x0d);
    e32(e, 0);
    if (v->arg2 == 0) {
        e8(e, 0xeb);
        e8(e, 0x00);
    }
    uint8_t *l = e->p;
    e8(e, 0xb0); /* L: mov al, imm8 */
    e8(e, 0x00);
    g_smc_page = (uint32_t) (l + 1) & ~0xfffu;
    g_smc_patch = l + 1;
    g_smc_orig  = 0x00;
    put_le(store + 2, PG_ALIAS_LIN + (((uint32_t) (l + 1)) & 0xfff), 4);
}

static void
group_pg_smc_alias(void)
{
    const char    *name = "pg.smc.alias";
    struct variant v    = { .group = name, .paging = 1, .faults_ok = 1, .emit = emit_smc_alias,
                            .fx = FX_CUSTOM, .defmask = dm_fxarg, .arg3 = 32 };
#ifndef HOSTTEST
    v.before_run = pg_setup;
    v.after_run  = pg_reset; /* before the harness reads the buffer */
#endif
    if (!pg_begin(name, 0))
        return;
    for (int k = 0; k < 2; k++) {
        static const struct {
            uint8_t len, b[3];
        } sigs[2] = { { SEQ(0x88, 0xeb, 0xb0) }, { SEQ(0x88, 0xb0) } };
        memcpy(v.target, sigs[k].b, sigs[k].len);
        v.target_len = sigs[k].len;
        v.arg2       = k;
        v.fx_arg     = 1;
        v.canon      = k ? canon_smc_byte : 0; /* without the jump: old or new byte */
        run_plain(&v, 2);
    }
    pg_end(name);
}

/*
 * Paging: page tables, #PF, the accessed and dirty bits, the TLB, 4 MB
 * pages. Paging is on only inside these groups, with an identity map of
 * the first 256 MB in 4 KB pages, plus an alias area at 1 GB whose page
 * table the tests point wherever they need. Before each run the tables
 * are put back and the TLB flushed, so every run starts the same. The
 * Linux build skips these groups. Included into groups.inc after
 * groups_cpu.inc; append only.
 */

#define PG_DIR    0x200000u  /* the page directory */
#define PG_TABLES 0x201000u  /* 64 page tables: 0-256 MB */
#define PG_ALIAS  0x241000u  /* the page table for 1 GB up */
#define PG_ALIAS_LIN 0x40000000u
#define PTE_P  0x001u
#define PTE_W  0x002u
#define PTE_A  0x020u
#define PTE_D  0x040u
#define PDE_PS 0x080u
#define CR0_PG (1u << 31)
#define CR0_WP (1u << 16)
#define CR4_PSE (1u << 4)
#define CPUID_PSE (1u << 3)

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
        pd[i] = (PG_TABLES + i * 4096) | PTE_P | PTE_W;
    pd[PG_ALIAS_LIN >> 22] = PG_ALIAS | PTE_P | PTE_W;
    uint32_t *pt = (uint32_t *) PG_TABLES;
    for (uint32_t i = 0; i < 64 * 1024; i++)
        pt[i] = (i << 12) | PTE_P | PTE_W;
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
        *pte_of(a) = a | PTE_P | PTE_W;
    uint32_t *al = (uint32_t *) PG_ALIAS;
    for (int i = 0; i < 16; i++)
        al[i] = 0;
    ((uint32_t *) PG_DIR)[1] = (PG_TABLES + 4096) | PTE_P | PTE_W; /* 4-8 MB, a PSE test may take it */
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
#endif
    if (!pg_begin(name, 0))
        return;
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        run_plain(&v, 2);
    }
    pg_end(name);
}

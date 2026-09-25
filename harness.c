/*
 * CPU accuracy test: the harness.
 *
 * Every test is a few bytes of code generated here at run time: load the
 * flags and seven registers from an input record, optionally run an
 * instruction that leaves the flags in a known lazy state, run the
 * instruction under test, and store the flags and registers to an output
 * record. Each generated copy goes to an address never used for code
 * before and is run four times with the same input. 86Box interprets a
 * block the first time it sees it, interprets it again while recompiling
 * it the second time, and runs the compiled code from the third time on,
 * so runs 1-2 and 3-4 are the interpreter and the recompiler on identical
 * input. Any difference between the four is a recompiler bug.
 *
 * Every result also goes into two CRCs per group, one over the interpreted
 * runs and one over the compiled runs, so different builds, an
 * interpreter-only configuration and real hardware can be compared by
 * group.
 *
 * Output is text on COM1 at 115200 baud, and progress on port 80h.
 */

#include "harness.h"

/* ---- port I/O and the serial console ---------------------------------- */

#ifdef HOSTTEST
/* The same harness as an ordinary 32-bit Linux program (host.c): the tests
   run on the host CPU, output goes to stdout. It checks the harness, not
   86Box. */
#    include "host.h"
static int vga_quiet __attribute__((unused));
#else
static inline void
outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t
inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

#define COM1 0x3f8

static void
serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01); /* 115200 */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xc7);
    outb(COM1 + 4, 0x03);
}

/* The screen too, for a real machine with nothing on its serial port: the
   80x25 colour text buffer, scrolling. Dump lines go to the serial port
   only (see vga_quiet). */
static int      vga_row = 24, vga_col;
static int      vga_quiet;
#define VGA ((volatile uint16_t *) 0xb8000)

static void
vga_putch(char c)
{
    if (c == '\r')
        return;
    if (c == '\n' || vga_col == 80) {
        vga_col = 0;
        if (++vga_row == 25) {
            for (int i = 0; i < 24 * 80; i++)
                VGA[i] = VGA[i + 80];
            for (int i = 24 * 80; i < 25 * 80; i++)
                VGA[i] = 0x0720;
            vga_row = 24;
        }
        if (c == '\n')
            return;
    }
    VGA[vga_row * 80 + vga_col++] = 0x0700 | (uint8_t) c;
}

static void
putch(char c)
{
    int spin = 100000;
    if (c == '\n')
        putch('\r');
    while (!(inb(COM1 + 5) & 0x20) && --spin)
        ;
    outb(COM1, c);
    if (!vga_quiet)
        vga_putch(c);
}

static void
puts_(const char *s)
{
    while (*s)
        putch(*s++);
}

static void
puthex(uint32_t v, int digits)
{
    for (int i = digits - 1; i >= 0; i--)
        putch("0123456789abcdef"[(v >> (i * 4)) & 15]);
}

static void
putdec(uint32_t v)
{
    char buf[12];
    int  n = 0;
    do {
        buf[n++] = '0' + (v % 10);
        v /= 10;
    } while (v);
    while (n)
        putch(buf[--n]);
}

static void
post(uint8_t code)
{
    outb(0x80, code);
}

void *
memcpy(void *d, const void *s, unsigned n)
{
    uint8_t       *dp = d;
    const uint8_t *sp = s;
    while (n--)
        *dp++ = *sp++;
    return d;
}

void *
memset(void *d, int c, unsigned n)
{
    uint8_t *dp = d;
    while (n--)
        *dp++ = c;
    return d;
}
#endif

static int
memcmp_(const void *a, const void *b, unsigned n)
{
    const uint8_t *ap = a, *bp = b;
    for (unsigned i = 0; i < n; i++)
        if (ap[i] != bp[i])
            return 1;
    return 0;
}

/* ---- CRC-32 and the pseudo-random inputs -------------------------------- */

static uint32_t crc_table[256];

static void
crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320 ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t
crc_add(uint32_t crc, const void *data, unsigned len)
{
    const uint8_t *p = data;
    crc              = ~crc;
    while (len--)
        crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static uint32_t rng_state;

static uint32_t
rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Operand values: mostly random, with the edges that break arithmetic. */
static uint32_t
rnd_operand(void)
{
    static const uint32_t edges[] = {
        0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff,
        0x10000, 0x7fffffff, 0x80000000, 0x80000001, 0xfffffffe, 0xffffffff
    };
    uint32_t r = rnd();
    if ((r & 3) == 0)
        return edges[(r >> 2) % (sizeof(edges) / sizeof(edges[0]))];
    return rnd();
}

/* ---- the machine: CPU identity, RAM, descriptor tables ----------------- */

struct cpu_info g_cpu;

static uint32_t
eflags_toggles(uint32_t bit)
{
    uint32_t before, after;
    __asm__ volatile("pushfl\n\t"
                     "popl %%eax\n\t"
                     "movl %%eax, %0\n\t"
                     "xorl %2, %%eax\n\t"
                     "pushl %%eax\n\t"
                     "popfl\n\t"
                     "pushfl\n\t"
                     "popl %%eax\n\t"
                     "movl %%eax, %1\n\t"
                     "pushl %0\n\t"
                     "popfl"
                     : "=&r"(before), "=&r"(after)
                     : "r"(bit)
                     : "eax", "cc");
    return (before ^ after) & bit;
}

static void
cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static void
cpu_detect(void)
{
    memset(&g_cpu, 0, sizeof(g_cpu));
    g_cpu.is486 = eflags_toggles(1u << 18) != 0; /* AC exists from the 486 on */
    g_cpu.has_cpuid = eflags_toggles(1u << 21) != 0;
    /* The Cyrix 5/2 test: 5 / 2 with the flags cleared; a Cyrix leaves
       them as they were (a 6x86 may have CPUID turned off). */
    {
        uint8_t ah;
        __asm__ volatile("xorw %%ax, %%ax\n\t"
                         "sahf\n\t"
                         "movw $5, %%ax\n\t"
                         "movb $2, %%bl\n\t"
                         "divb %%bl\n\t"
                         "lahf\n\t"
                         "movb %%ah, %0"
                         : "=r"(ah)
                         :
                         : "eax", "ebx", "cc");
        g_cpu.cyrix = ah == 0x02;
    }
    if (g_cpu.has_cpuid) {
        uint32_t a, b, c, d;
        cpuid(0, &a, &b, &c, &d);
        memcpy(g_cpu.vendor + 0, &b, 4);
        memcpy(g_cpu.vendor + 4, &d, 4);
        memcpy(g_cpu.vendor + 8, &c, 4);
        g_cpu.vendor[12] = 0;
        /* With CPUID, the vendor decides. */
        g_cpu.cyrix = !memcmp_(g_cpu.vendor, "CyrixInstead", 12);
        if (a >= 1) {
            cpuid(1, &a, &b, &c, &d);
            g_cpu.signature = a;
            g_cpu.features  = d;
        }
        /* Extended leaves (3DNow!). Older Intel CPUs answer an unknown
           leaf with a basic one, which has bit 31 of EAX clear. */
        cpuid(0x80000000, &a, &b, &c, &d);
        if ((a & 0x80000000u) && a >= 0x80000001u && a < 0x80000100u) {
            cpuid(0x80000001, &a, &b, &c, &d);
            g_cpu.ext_features = d;
        }
    }
}

/* The BIOS memory map the loader collected (stub.asm): at E820_AT a
   status word, an entry count, then 24-byte entries. Whether the call
   exists and gives a sane table is the BIOS's business, so a failure is a
   result, not a stop: e820_read() checks the table and only the tests
   that need it are left out when it's missing or bad. */
#define E820_AT  0x5000u
#define E820_MAX 64
enum { E820_NOT_RUN, E820_OK, E820_NO_CALL, E820_NOT_SMAP, E820_TOO_MANY, E820_SHORT, E820_CORRUPT, E820_NO_RAM };

struct e820_entry {
    uint64_t base, len;
    uint32_t type, attr;
} __attribute__((packed));

static struct e820_entry g_e820[E820_MAX];
static unsigned          g_e820_n, g_e820_status;
static uint32_t          g_e820_mb_high; /* usable RAM at or above 4 GB, in MB */
static uint64_t          g_hi_page;      /* a 4 MB-aligned usable 4 KB at or above 4 GB, below 64 GB; 0 if none */

static void
e820_read(void)
{
#ifndef HOSTTEST
    volatile uint16_t *h = (volatile uint16_t *) E820_AT;
    g_e820_status        = h[0];
    g_e820_n             = h[1];
    if (g_e820_n > E820_MAX) {
        g_e820_status = E820_CORRUPT;
        g_e820_n      = 0;
    }
    memcpy(g_e820, (const void *) (E820_AT + 8), g_e820_n * sizeof(g_e820[0]));
    if (g_e820_status != E820_OK)
        return;
    int usable = 0;
    for (unsigned i = 0; i < g_e820_n; i++) {
        const struct e820_entry *e = &g_e820[i];
        uint64_t                 end = e->base + e->len;
        if (e->type == 0 || end < e->base) { /* no such type; wraps past 2^64 */
            g_e820_status = E820_CORRUPT;
            g_hi_page     = 0;
            g_e820_mb_high = 0;
            return;
        }
        if (e->type != 1 || e->len == 0)
            continue;
        usable = 1;
        if (end > 0x100000000ull) {
            uint64_t from = e->base > 0x100000000ull ? e->base : 0x100000000ull;
            uint64_t mb   = g_e820_mb_high + ((end - from) >> 20);
            g_e820_mb_high = mb > 0xffffffffu ? 0xffffffffu : (uint32_t) mb;
            uint64_t page  = (from + 0x3fffff) & ~0x3fffffull;
            if (!g_hi_page && page + 0x1000 <= end && page + 0x1000 <= (1ull << 36))
                g_hi_page = page;
        }
    }
    if (!usable)
        g_e820_status = E820_NO_RAM;
#endif
}

#include "mp.inc"

#ifndef HOSTTEST
static uint8_t
cmos_read(uint8_t reg)
{
    outb(0x70, 0x80 | reg);
    return inb(0x71);
}

static uint32_t g_ram_top;

static void
ram_detect(void)
{
    uint32_t ext = cmos_read(0x30) | (cmos_read(0x31) << 8); /* KB above 1 MB */
    uint32_t alt = cmos_read(0x17) | (cmos_read(0x18) << 8);
    if (alt > ext)
        ext = alt;
    if (ext < 4096)
        ext = 4096;
    g_ram_top = 0x100000 + ext * 1024;
}


struct idt_entry {
    uint16_t off_lo, sel;
    uint8_t  zero, type;
    uint16_t off_hi;
} __attribute__((packed));

static struct idt_entry idt[32] __attribute__((aligned(8)));

extern uint32_t isr_table[32];

static void
idt_init(void)
{
    for (int i = 0; i < 32; i++) {
        idt[i].off_lo = isr_table[i] & 0xffff;
        idt[i].off_hi = isr_table[i] >> 16;
        idt[i].sel    = 0x08;
        idt[i].zero   = 0;
        idt[i].type   = 0x8e;
    }
    struct {
        uint16_t limit;
        uint32_t base;
    } __attribute__((packed)) idtr = { sizeof(idt) - 1, (uint32_t) idt };
    __asm__ volatile("lidt %0" : : "m"(idtr));
}
#endif

/* ---- code arena: every generated copy gets a fresh address -------------- */

#define SLOT_ALIGN  64u
#define PAGE_SIZE   4096u

/* On bare metal these are fixed; the ELF build takes what the OS gives it. */
uint8_t        *g_sandbox = (uint8_t *) 0x00300000;
static uint32_t g_arena_base = 0x00400000;
static uint32_t g_arena_next, g_arena_end, g_arena_wraps;

static void
arena_init(void)
{
#ifdef HOSTTEST
    g_arena_base = host_map(0x00400000, 60u << 20);
    g_arena_end  = g_arena_base + (60u << 20);
    g_sandbox    = (uint8_t *) host_map(0x00300000, SANDBOX_SIZE);
#else
    g_arena_end = g_ram_top - 0x100000;
    if (g_arena_end > 0x10000000) /* the paging groups map the first 256 MB */
        g_arena_end = 0x10000000;
#endif
    g_arena_next = g_arena_base;
}

/* An address never used for code before (until the arena wraps; the
   summary says how often it did). A copy never crosses a page. */
static uint8_t *
arena_alloc(uint32_t len)
{
    uint32_t a = (g_arena_next + SLOT_ALIGN - 1) & ~(SLOT_ALIGN - 1);
    if ((a & (PAGE_SIZE - 1)) + len > PAGE_SIZE)
        a = (a + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (a + len > g_arena_end) {
        g_arena_wraps++;
        a = g_arena_base;
    }
    g_arena_next = a + len;
    return (uint8_t *) a;
}

/* ---- the state a test sees ---------------------------------------------- */

struct kin  g_in;
struct kout g_out;

/* The low buffer is below 64 KB so 16-bit address forms can reach it. */
#ifdef HOSTTEST
static uint8_t g_lowbuf_host[LOWBUF_SIZE];
#    define LOWBUF g_lowbuf_host
#else
#    define LOWBUF ((uint8_t *) 0x9000)
#endif

/* Set by an input fixer when the variant can't run here (host build). */
static int g_skip;

/* Live status: each group is first counted (g_counting), then run; the
   status line shows how far through it is. */
static int      g_counting;
static uint32_t g_expected;
static int      g_group_no, g_group_count;
static uint32_t g_pass;
static const char *g_group_name;
static void status(void);

/* ---- the emitter --------------------------------------------------------- */

struct emit {
    uint8_t *p;
};

static void
e8(struct emit *e, uint8_t b)
{
    *e->p++ = b;
}

static void
e32(struct emit *e, uint32_t v)
{
    memcpy(e->p, &v, 4);
    e->p += 4;
}

static void
put32_at(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static void
ebytes(struct emit *e, const uint8_t *b, int n)
{
    while (n--)
        e8(e, *b++);
}

/* Register numbers in ModRM order. */
enum { R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI };

static uint32_t *
in_reg(int r)
{
    static uint32_t *const map[8] = { &g_in.eax, &g_in.ecx, &g_in.edx, &g_in.ebx, 0, &g_in.ebp, &g_in.esi, &g_in.edi };
    return map[r];
}

static uint32_t *
out_reg(int r)
{
    static uint32_t *const map[8] = { &g_out.eax, &g_out.ecx, &g_out.edx, &g_out.ebx, 0, &g_out.ebp, &g_out.esi, &g_out.edi };
    return map[r];
}

static void
emit_prologue(struct emit *e)
{
    e8(e, 0xff); /* push dword [flags] */
    e8(e, 0x35);
    e32(e, (uint32_t) &g_in.flags);
    e8(e, 0x9d); /* popfd */
    static const int order[] = { R_ECX, R_EDX, R_EBX, R_EBP, R_ESI, R_EDI, R_EAX };
    for (unsigned i = 0; i < 7; i++) {
        e8(e, 0x8b); /* mov r32, [abs] */
        e8(e, 0x05 | (order[i] << 3));
        e32(e, (uint32_t) in_reg(order[i]));
    }
}

static void
emit_epilogue(struct emit *e, int reset_flags)
{
    e8(e, 0x9c); /* pushfd */
    e8(e, 0x8f); /* pop dword [abs] */
    e8(e, 0x05);
    e32(e, (uint32_t) &g_out.flags);
    static const int order[] = { R_EAX, R_ECX, R_EDX, R_EBX, R_EBP, R_ESI, R_EDI };
    for (unsigned i = 0; i < 7; i++) {
        e8(e, 0x89); /* mov [abs], r32 */
        e8(e, 0x05 | (order[i] << 3));
        e32(e, (uint32_t) out_reg(order[i]));
    }
    if (reset_flags) {
        e8(e, 0x6a); /* push 2; popfd: no NT, AC, ID or IOPL left for the harness */
        e8(e, 0x02);
        e8(e, 0x9d);
    }
    e8(e, 0xc3);
}

/* Instructions run before the one under test, to leave the flags in each
   of the lazy states the recompiler tracks. They use ECX and EDX only. */
/* Flag bits. */
#define F_CF 0x0001
#define F_PF 0x0004
#define F_AF 0x0010
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_OF 0x0800
#define F_ARITH (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

struct producer {
    const char   *name;
    uint8_t       len;
    uint8_t       bytes[6];
    uint16_t      undef; /* flags it leaves architecturally undefined */
};

static const struct producer producers[] = {
    { "none", 0, { 0 }, 0 },
    { "cmp", 2, { 0x39, 0xd1 }, 0 },
    { "sub", 2, { 0x29, 0xd1 }, 0 },
    { "add", 2, { 0x01, 0xd1 }, 0 },
    { "adc", 2, { 0x11, 0xd1 }, 0 },
    { "sbb", 2, { 0x19, 0xd1 }, 0 },
    { "and", 2, { 0x21, 0xd1 }, F_AF },
    { "or", 2, { 0x09, 0xd1 }, F_AF },
    { "xor", 2, { 0x31, 0xd1 }, F_AF },
    { "test", 2, { 0x85, 0xd1 }, F_AF },
    { "inc", 1, { 0x41 }, 0 },
    { "dec", 1, { 0x49 }, 0 },
    { "neg", 2, { 0xf7, 0xd9 }, 0 },
    { "shl1", 2, { 0xd1, 0xe1 }, F_AF },
    { "shr1", 2, { 0xd1, 0xe9 }, F_AF },
    { "sarcl", 2, { 0xd3, 0xf9 }, F_AF | F_OF },
    { "rol1", 2, { 0xd1, 0xc1 }, 0 },
    { "add8", 2, { 0x00, 0xd1 }, 0 },
    { "sub16", 3, { 0x66, 0x29, 0xd1 }, 0 },
    { "imul", 3, { 0x0f, 0xaf, 0xca }, F_SF | F_ZF | F_AF | F_PF },
    { "bt", 3, { 0x0f, 0xa3, 0xd1 }, F_OF | F_SF | F_AF | F_PF },
    { "xadd", 3, { 0x0f, 0xc1, 0xd1 }, 0 },
    { "subshl0", 5, { 0x29, 0xd1, 0xc1, 0xe2, 0x00 }, 0 }, /* a shift by 0 leaves SUB's flags */
    { "rcl1", 2, { 0xd1, 0xd1 }, 0 },
    { "neg8", 2, { 0xf6, 0xd9 }, 0 },
    { "inc8", 2, { 0xfe, 0xc1 }, 0 },
    { "dec16", 2, { 0x66, 0x49 }, 0 },
    { "clc", 1, { 0xf8 }, 0 },
    { "cmc", 1, { 0xf5 }, 0 },
};
#define N_PRODUCERS (sizeof(producers) / sizeof(producers[0]))

/* ---- running one test ---------------------------------------------------- */

extern int           run_kernel(void *code);
extern struct fault  g_fault;

/* What a test variant is: the bytes under test, what runs before them, and
   whether a jump puts them at the start of a block of their own. */
/* What the instruction under test does to the flags, for the defined-result
   CRC: which of its results are architecturally defined, given what the
   producer before it left undefined. */
enum {
    FX_NONE,  /* flags untouched, results always defined */
    FX_SETCC, /* fx_arg = condition; the result is undefined if it reads an undefined flag */
    FX_LAHF_SAHF, /* fx_arg = the sequence, see lahf_sahf_effect() */
    FX_BT,    /* CF defined, the others undefined */
    FX_ALU,   /* fx_arg = the ALU operation, 0-7 */
    FX_UNDEF, /* the result itself is undefined (BSWAP r16) */
    FX_CUSTOM /* the variant's defmask() decides, from the input */
};

struct emit;
struct variant;
/* Given the input and the producer's undefined flags in *undef, leave in
   *undef what is undefined after the test and clear any undefined
   registers in *d. Returns 0 if nothing of the result is defined. */
typedef int (*defmask_fn)(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef);

struct variant {
    const char   *group;
    uint8_t       target[48];
    uint8_t       target_len;
    uint8_t       producer;
    uint8_t       boundary;
    void        (*fix_input)(const struct variant *v);
    uint32_t      arg; /* for fix_input */
    uint8_t       fx, fx_arg;
    uint8_t       no_ref; /* depends on this machine (ESP, low buffer): not in the defined CRC */
    /* The rest are zero in the first six groups. */
    uint32_t      arg2, arg3;    /* for fix_input and defmask */
    uint32_t      flags_mask;    /* EFLAGS bits compared; 0 = the arithmetic flags and DF */
    uint8_t       stack;         /* runs on a private stack in the sandbox */
    uint8_t       faults_ok;     /* a fault is a result like any other */
    uint8_t       norm;          /* registers (1 << ModRM number) holding sandbox addresses */
    uint8_t       reg_fix;       /* fix_input sets registers only: no sandbox refill */
    uint8_t       mmx;           /* MM0-MM7 loaded from g_mmx_in, stored to g_mmx_out */
    uint8_t       code16;        /* producer and target run in a 16-bit code segment */
    uint16_t      code16_ip;     /* ... starting at this IP (the segment based that much before the code) */
    uint8_t       paging;        /* #PF: fault_err = error code | (CR2 - buffer) << 8 */
    uint8_t       ring;          /* 1-3: run at that CPL; 4: in V86 mode (see ring_env_on()) */
    uint8_t       debug;         /* fault_err = DR6 after the run, fault or not */
    uint32_t      ring_flags;    /* EFLAGS bits the entry IRET adds: IOPL, AC */
    void        (*before_run)(const struct variant *v); /* before each of the four runs */
    void        (*after_run)(const struct variant *v);  /* after each, before the harness looks at memory */
    /* Where a real CPU may legitimately give one of several results (code
       it just overwrote without a jump, say), this rewrites any allowed
       result to one canonical one before the runs are compared; anything
       outside the allowed set is left alone, and stops the test. */
    void        (*canon)(const struct variant *v, const struct kin *in, struct kout *o);
    void        (*emit)(struct emit *e, const struct variant *v); /* instead of target[] */
    defmask_fn    defmask;
};

struct group_stats {
    uint32_t tests, mismatches, faults;
    uint32_t crc_interp, crc_comp;
    uint32_t defined, crc_defined;
    uint32_t crc_raw; /* the same results unmasked: one CPU model against another */
};

static void fix_mem_ebx(const struct variant *v);
static void fix_bt_mem_reg(const struct variant *v);

/* A register of an input or a result by ModRM number (not ESP). */
static uint32_t *
kout_reg(struct kout *o, int r)
{
    switch (r) {
        case R_EAX: return &o->eax;
        case R_ECX: return &o->ecx;
        case R_EDX: return &o->edx;
        case R_EBX: return &o->ebx;
        case R_EBP: return &o->ebp;
        case R_ESI: return &o->esi;
        default:    return &o->edi;
    }
}

static uint32_t
kin_reg(const struct kin *in, int r)
{
    switch (r) {
        case R_EAX: return in->eax;
        case R_ECX: return in->ecx;
        case R_EDX: return in->edx;
        case R_EBX: return in->ebx;
        case R_EBP: return in->ebp;
        case R_ESI: return in->esi;
        default:    return in->edi;
    }
}

/* Byte registers in ModRM order: AL CL DL BL AH CH DH BH. */
static uint32_t
kin_reg8(const struct kin *in, int r)
{
    return r < 4 ? kin_reg(in, r) & 0xff : (kin_reg(in, r - 4) >> 8) & 0xff;
}

/* A result as any machine would give it: EBX holds the buffer's address in
   the memory forms, and the buffer is wherever this machine put it. */
static void
normalize(const struct variant *v, struct kout *d)
{
    if (v->fix_input == fix_mem_ebx || v->fix_input == fix_bt_mem_reg)
        d->ebx -= (uint32_t) g_buf;
    for (int r = 0; r < 8; r++)
        if ((v->norm & (1 << r)) && r != R_ESP)
            *kout_reg(d, r) -= (uint32_t) g_buf;
}

/* The flags each SETcc condition reads. */
static const uint16_t cc_reads[8] = {
    F_OF, F_CF, F_ZF, F_CF | F_ZF, F_SF, F_PF, F_SF | F_OF, F_ZF | F_SF | F_OF
};

/* Fold one result into the defined-result CRC, masking what the
   architecture leaves undefined. Returns 0 if nothing of it is defined. */
static int
defined_result(const struct variant *v, const struct kin *in, const struct kout *o, struct kout *d)
{
    uint16_t undef    = producers[v->producer].undef;
    uint32_t eax_mask = 0xffffffff;

    *d = *o;
    normalize(v, d);
    switch (v->fx) {
        case FX_NONE:
            break;
        case FX_SETCC:
            if (cc_reads[v->fx_arg >> 1] & undef)
                return 0;
            break;
        case FX_LAHF_SAHF: {
            /* The sequence, one byte at a time: LAHF copies SF ZF AF PF CF
               into AH (undefined ones too); SAHF copies them back. */
            uint16_t ah_undef = 0;
            for (int i = 0; i < v->target_len; i++) {
                if (v->target[i] == 0x9f)
                    ah_undef = undef & (F_SF | F_ZF | F_AF | F_PF | F_CF);
                else
                    undef = (undef & ~(F_SF | F_ZF | F_AF | F_PF | F_CF)) | ah_undef;
            }
            eax_mask = ~((uint32_t) ah_undef << 8);
            break;
        }
        case FX_BT:
            undef = (undef | F_ARITH) & ~F_CF;
            break;
        case FX_ALU:
            /* add or adc sbb and sub xor cmp: all six written; AF undefined
               after the logical ones. */
            undef &= ~F_ARITH;
            if (v->fx_arg == 1 || v->fx_arg == 4 || v->fx_arg == 6)
                undef |= F_AF;
            break;
        case FX_CUSTOM:
            if (!v->defmask(v, in, d, &undef))
                return 0;
            break;
        default:
            return 0;
    }
    d->flags &= ~undef;
    d->eax &= eax_mask;
    return 1;
}

static struct group_stats g_stats;
static uint32_t           g_total_mismatches, g_total_tests;

#define RUNS 4

static void
fill_input(void)
{
    g_in.flags = (rnd() & 0x0cd5) | 0x0002; /* arithmetic flags and DF; IF, TF off */
    g_in.eax   = rnd_operand();
    g_in.ecx   = rnd_operand();
    g_in.edx   = rnd_operand();
    g_in.ebx   = rnd_operand();
    g_in.ebp   = rnd_operand();
    g_in.esi   = rnd_operand();
    g_in.edi   = rnd_operand();
    for (int i = 0; i < BUF_SIZE; i++)
        g_in.buf[i] = rnd();
    for (int i = 0; i < LOWBUF_SIZE; i++)
        g_in.lowbuf[i] = rnd();
}

/* The stack a variant with v->stack runs on: in the sandbox, well above the
   buffer. ESP is saved around it; its final value, relative to the buffer,
   is returned in fault_err when nothing faulted. */
#define STACK_TOP (g_buf + 0x800)
uint32_t g_stack_save, g_stack_out;

/* MMX variants: MM0-MM7 in and out (as 32-bit halves, low first), and
   where FNSTENV goes (outside the sandbox: it holds absolute addresses). */
uint32_t g_mmx_in[16], g_mmx_out[16];

/* 16-bit code: the far call goes through GDT selector 18h (16-bit code,
   64 KB), whose base is moved to each variant's 16-bit code before it
   runs, so that code too is at a fresh address every time. */
#define SEL_CODE16 0x18
#ifdef HOSTTEST
static uint8_t gdt[0x28];
#else
extern uint8_t gdt[];
#endif

/* ---- rings 1-3 and V86 ------------------------------------------------------ */

/* The ring groups run with their own GDT, IDT and TSS (ring_env_on/off),
   so every other group sees the small GDT and IDT of rt.asm unchanged.
   Selectors in the ring GDT: */
#define SEL_CODE1   0x28
#define SEL_DATA1   0x30
#define SEL_CODE2   0x38
#define SEL_DATA2   0x40
#define SEL_CODE3   0x48
#define SEL_DATA3   0x50
#define SEL_TSS     0x58
#define SEL_CONF0   0x60 /* conforming code, DPL 0 */
#define SEL_GATE3   0x68 /* call gate, DPL 3, to ring 0 */
#define SEL_GATE1   0x70 /* call gate, DPL 1, to ring 0 */
#define SEL_GATE0   0x78 /* call gate, DPL 0, to ring 0 */
#define SEL_GATE31  0x80 /* call gate, DPL 3, to ring 1 */
#define SEL_GATE3P  0x88 /* call gate, DPL 3, to ring 0, two parameters */
#define SEL_SCRATCH 0x90 /* a data descriptor tests rewrite */
#define SEL_GATE21  0x98 /* call gate, DPL 2, to ring 1 */
#define SEL_CONF1   0xa0 /* conforming code, DPL 1 */
#define SEL_CONF3   0xa8 /* conforming code, DPL 3 */
#define SEL_GATE33  0xb0 /* call gate, DPL 3, to ring 3 */
#define SEL_GATE3P1 0xb8 /* call gate, DPL 3, to ring 1, three parameters */
#define SEL_C16R3   0xc0 /* 16-bit code, DPL 3, based at the test's code */
#define SEL_C16R1   0xc8 /* 16-bit code, DPL 1, the same */
#define SEL_NP3     0xd0 /* data, DPL 3, not present */
#define SEL_XO3     0xd8 /* code, DPL 3, execute-only */
#define SEL_RO3     0xe0 /* data, DPL 3, read-only */
#define SEL_C16R2   0xe8 /* 16-bit code, DPL 2 */
#define SEL_GATE13  0xf0 /* call gate, DPL 1, to ring 3 code (outward: #GP) */
#define SEL_LDT     0xf8  /* the LDT (g_ldt) */
#define SEL_TSS2    0x100 /* a second task (g_tss2) */
#define SEL_TGATE   0x108 /* task gate, DPL 3, to SEL_TSS2 */
#define SEL_TSS3    0x110 /* a TSS too short to switch to (#TS) */
#define RING_GDT_N  35

#define V86_BLOCK   0x8000u /* the V86 test's inputs, below 64 KB */
#define V86_STACK   0x7f00u
#define V86_ARENA   0x20000u
#define V86_ARENA_END 0x80000u

#ifdef HOSTTEST
#    define RING_UNUSED __attribute__((unused))
#else
#    define RING_UNUSED
#endif
static uint32_t g_gdt2[RING_GDT_N * 2] __attribute__((aligned(8))) RING_UNUSED;
static uint32_t g_idt2[64 * 2] __attribute__((aligned(8))) RING_UNUSED;
/* The TSS: 104 bytes, the VME interrupt redirection bitmap (32 bytes),
   the I/O permission bitmap (8 KB) and its terminating FFh. */
#define TSS_IOMAP 136
static uint8_t  g_tss[TSS_IOMAP + 8192 + 1] __attribute__((aligned(16))) RING_UNUSED;
static uint8_t  g_rstack[4][1024] __attribute__((aligned(16))); /* rings 0 (faults) to 3 */
/* The LDT: DPL-3 data (sel 07h at RPL 3), DPL-3 code (0Fh), DPL-0 data
   (14h), a DPL-3 call gate to ring 0 (1Fh), a missing DPL-3 data segment
   (27h), DPL-1 data (2Dh). */
static uint32_t g_ldt[6 * 2] __attribute__((aligned(8))) RING_UNUSED;
/* A second task, and the stack it runs on. */
static uint8_t  g_tss2[104] __attribute__((aligned(16))) RING_UNUSED;
static uint8_t  g_task_stack[1024] __attribute__((aligned(16))) RING_UNUSED;
static int      g_tr_loaded RING_UNUSED;

/* Where the call gates go: MOV ESI, CS (the CPL they run at, in its RPL)
   and back; the two-parameter one also takes the first parameter into
   EDI and drops both. */
static uint8_t g_gate_ret[] RING_UNUSED = { 0x8c, 0xce, 0xcb };                               /* mov esi, cs; retf */
/* For the gates into ring 1: also ESP and SS there, to show the stack
   switch (or its absence). */
static uint8_t g_gate_retsp[] RING_UNUSED = { 0x8c, 0xce, 0x89, 0xe7, 0x8c, 0xd5, 0xcb }; /* mov esi, cs; mov edi, esp; mov ebp, ss; retf */
/* The three-parameter gate into ring 1: CS, and the first and last
   parameters (as copied), then RETF 12. */
static uint8_t g_gate_retp3[] RING_UNUSED = { 0x8c, 0xce, 0x8b, 0x7c, 0x24, 0x08, 0x8b, 0x6c, 0x24, 0x10, 0xca, 0x0c, 0x00 };
/* An interrupt or trap gate's handler in ring 1, 2 or 3: CS, ESP, SS,
   and IRETD back. */
static uint8_t g_ring_isr[] RING_UNUSED = { 0x8c, 0xce, 0x89, 0xe7, 0x8c, 0xd5, 0xcf };
static uint8_t g_gate_retp[] RING_UNUSED = { 0x8c, 0xce, 0x8b, 0x7c, 0x24, 0x0c, 0xca, 0x08, 0x00 }; /* ...; mov edi, [esp+12]; retf 8 */

static void seg_desc(int sel, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags);

/* The scratch descriptor (SEL_SCRATCH) a limits or stack test loads,
   based 4 KB into the sandbox: (access, flags, limit) by config. */
static const struct {
    uint8_t  access, flags;
    uint32_t limit;
} g_scratch_cfg[] RING_UNUSED = {
    { 0xf2, 0x4, 0x0fff }, /* 0: data, read/write, DPL 3, 4 KB */
    { 0xf0, 0x4, 0x0fff }, /* 1: data, read-only */
    { 0xf6, 0x4, 0x0fff }, /* 2: data, expand-down: 1000h up to 4 GB */
    { 0xf6, 0x0, 0x0fff }, /* 3: expand-down, 16-bit: 1000h up to 64 KB */
    { 0xfa, 0x4, 0x0fff }, /* 4: code, execute/read */
    { 0xf8, 0x4, 0x0fff }, /* 5: code, execute-only */
    { 0xf2, 0xc, 0x00000 }, /* 6: data, one 4 KB page (granular) */
    { 0xb2, 0x4, 0x0fff }, /* 7: data, DPL 1 */
    { 0x72, 0x4, 0x0fff }, /* 8: data, not present */
};
#define N_SCRATCH (sizeof(g_scratch_cfg) / sizeof(g_scratch_cfg[0]))

/* arg: the config in the low byte, the DPL in bits 9-10 (config 7 keeps
   its own DPL 1). With bit 11 set instead: the access byte itself in bits
   12-19 and the flags nibble in 20-23, limit 0FFFh. */
static void
scratch_set(unsigned arg)
{
    if (arg & 0x800) {
        seg_desc(SEL_SCRATCH, (uint32_t) SANDBOX + 0x1000, 0x0fff, (arg >> 12) & 0xff, (arg >> 20) & 0xf);
        return;
    }
    unsigned cfg = arg & 0xff, dpl = (arg >> 9) & 3;
    uint8_t  acc = g_scratch_cfg[cfg].access;
    if (cfg != 7)
        acc = (acc & ~0x60) | (dpl << 5);
    seg_desc(SEL_SCRATCH, (uint32_t) SANDBOX + 0x1000, g_scratch_cfg[cfg].limit, acc, g_scratch_cfg[cfg].flags);
}

static void
seg_desc(int sel, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    uint32_t *d = g_gdt2 + (sel >> 3) * 2;
    d[0]        = (limit & 0xffff) | (base << 16);
    d[1]        = ((base >> 16) & 0xff) | ((uint32_t) access << 8) | (limit & 0xf0000) | ((uint32_t) flags << 20) | (base & 0xff000000);
}

static void
ldt_desc(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    uint32_t *d = g_ldt + i * 2;
    d[0]        = (limit & 0xffff) | (base << 16);
    d[1]        = ((base >> 16) & 0xff) | ((uint32_t) access << 8) | (limit & 0xf0000) | ((uint32_t) flags << 20) | (base & 0xff000000);
}

static void
gate_desc(uint32_t *d, uint16_t sel, uint32_t off, uint8_t type, uint8_t params)
{
    d[0] = (off & 0xffff) | ((uint32_t) sel << 16);
    d[1] = (off & 0xffff0000) | ((uint32_t) type << 8) | params;
}

#ifndef HOSTTEST
extern uint32_t isr_table[32];
extern void     isr_48(void);
static struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) g_saved_gdtr, g_saved_idtr;

static void
ring_build(void)
{
    memset(g_gdt2, 0, sizeof(g_gdt2));
    memcpy(g_gdt2, gdt, 0x28); /* null, the flat and 16-bit segments as before */
    seg_desc(SEL_CODE1, 0, 0xfffff, 0xba, 0xc);
    seg_desc(SEL_DATA1, 0, 0xfffff, 0xb2, 0xc);
    seg_desc(SEL_CODE2, 0, 0xfffff, 0xda, 0xc);
    seg_desc(SEL_DATA2, 0, 0xfffff, 0xd2, 0xc);
    seg_desc(SEL_CODE3, 0, 0xfffff, 0xfa, 0xc);
    seg_desc(SEL_DATA3, 0, 0xfffff, 0xf2, 0xc);
    seg_desc(SEL_TSS, (uint32_t) g_tss, sizeof(g_tss) - 1, 0x89, 0);
    seg_desc(SEL_CONF0, 0, 0xfffff, 0x9e, 0xc);
    gate_desc(g_gdt2 + (SEL_GATE3 >> 3) * 2, 0x08, (uint32_t) g_gate_ret, 0xec, 0);
    gate_desc(g_gdt2 + (SEL_GATE1 >> 3) * 2, 0x08, (uint32_t) g_gate_ret, 0xac, 0);
    gate_desc(g_gdt2 + (SEL_GATE0 >> 3) * 2, 0x08, (uint32_t) g_gate_ret, 0x8c, 0);
    gate_desc(g_gdt2 + (SEL_GATE31 >> 3) * 2, SEL_CODE1, (uint32_t) g_gate_retsp, 0xec, 0);
    gate_desc(g_gdt2 + (SEL_GATE3P >> 3) * 2, 0x08, (uint32_t) g_gate_retp, 0xec, 2);
    seg_desc(SEL_SCRATCH, 0, 0xfffff, 0xf2, 0xc);
    gate_desc(g_gdt2 + (SEL_GATE21 >> 3) * 2, SEL_CODE1, (uint32_t) g_gate_retsp, 0xcc, 0);
    seg_desc(SEL_CONF1, 0, 0xfffff, 0xbe, 0xc);
    seg_desc(SEL_CONF3, 0, 0xfffff, 0xfe, 0xc);
    gate_desc(g_gdt2 + (SEL_GATE33 >> 3) * 2, SEL_CODE3, (uint32_t) g_gate_ret, 0xec, 0);
    gate_desc(g_gdt2 + (SEL_GATE3P1 >> 3) * 2, SEL_CODE1, (uint32_t) g_gate_retp3, 0xec, 3);
    seg_desc(SEL_C16R3, 0, 0xffff, 0xfa, 0);
    seg_desc(SEL_C16R1, 0, 0xffff, 0xba, 0);
    seg_desc(SEL_NP3, 0, 0xfffff, 0x72, 0xc);
    seg_desc(SEL_XO3, 0, 0xfffff, 0xf8, 0xc);
    seg_desc(SEL_RO3, 0, 0xfffff, 0xf0, 0xc);
    seg_desc(SEL_C16R2, 0, 0xffff, 0xda, 0);
    gate_desc(g_gdt2 + (SEL_GATE13 >> 3) * 2, SEL_CODE3, (uint32_t) g_gate_ret, 0xac, 0);
    seg_desc(SEL_LDT, (uint32_t) g_ldt, sizeof(g_ldt) - 1, 0x82, 0);
    seg_desc(SEL_TSS2, (uint32_t) g_tss2, sizeof(g_tss2) - 1, 0x89, 0);
    gate_desc(g_gdt2 + (SEL_TGATE >> 3) * 2, SEL_TSS2, 0, 0xe5, 0);
    seg_desc(SEL_TSS3, (uint32_t) g_tss2, 0x60, 0x89, 0);
    memset(g_ldt, 0, sizeof(g_ldt));
    ldt_desc(0, 0, 0xfffff, 0xf2, 0xc);
    ldt_desc(1, 0, 0xfffff, 0xfa, 0xc);
    ldt_desc(2, 0, 0xfffff, 0x92, 0xc);
    gate_desc(g_ldt + 3 * 2, 0x08, (uint32_t) g_gate_ret, 0xec, 0);
    ldt_desc(4, 0, 0xfffff, 0x72, 0xc);
    ldt_desc(5, 0, 0xfffff, 0xb2, 0xc);

    memset(g_idt2, 0, sizeof(g_idt2));
    for (int i = 0; i < 32; i++)
        gate_desc(g_idt2 + i * 2, 0x08, isr_table[i], 0x8e, 0);
    gate_desc(g_idt2 + 0x30 * 2, 0x08, (uint32_t) isr_48, 0xee, 0); /* DPL 3: the way back */
    gate_desc(g_idt2 + 0x31 * 2, 0x08, (uint32_t) isr_48, 0x8e, 0); /* DPL 0 */
    gate_desc(g_idt2 + 0x32 * 2, 0x08, (uint32_t) isr_48, 0xae, 0); /* DPL 1 */
    gate_desc(g_idt2 + 0x33 * 2, SEL_CODE1, (uint32_t) g_ring_isr, 0xee, 0); /* interrupt gate into ring 1 */
    gate_desc(g_idt2 + 0x34 * 2, SEL_CODE1, (uint32_t) g_ring_isr, 0xef, 0); /* trap gate into ring 1 */
    gate_desc(g_idt2 + 0x35 * 2, SEL_CODE2, (uint32_t) g_ring_isr, 0xee, 0); /* into ring 2 */
    gate_desc(g_idt2 + 0x36 * 2, SEL_CODE3, (uint32_t) g_ring_isr, 0xee, 0); /* into ring 3 */
    gate_desc(g_idt2 + 0x37 * 2, SEL_CODE1, (uint32_t) g_ring_isr, 0xae, 0); /* into ring 1, DPL 1 */
    gate_desc(g_idt2 + 0x38 * 2, SEL_TSS2, 0, 0xe5, 0);                     /* a task gate, DPL 3 */
    gate_desc(g_idt2 + 0x39 * 2, 0x08, (uint32_t) isr_48, 0x6e, 0);          /* not present: #NP */
    gate_desc(g_idt2 + 0x3a * 2, 0x08, (uint32_t) isr_48, 0xe0, 0);          /* type 0, invalid: #GP */
    gate_desc(g_idt2 + 0x3b * 2, 0x08, (uint32_t) isr_48, 0xef, 0);          /* a 386 trap gate, DPL 3 */
    gate_desc(g_idt2 + 0x3c * 2, 0x10, (uint32_t) isr_48, 0xee, 0);          /* to a data segment: #GP */
    gate_desc(g_idt2 + 0x3d * 2, 0x00, (uint32_t) isr_48, 0xee, 0);          /* to the null selector: #GP */

    memset(g_tss, 0, sizeof(g_tss));
    *(uint32_t *) (g_tss + 4)   = (uint32_t) g_rstack[0] + sizeof(g_rstack[0]);
    *(uint32_t *) (g_tss + 8)   = 0x10;
    *(uint32_t *) (g_tss + 12)  = (uint32_t) g_rstack[1] + sizeof(g_rstack[1]);
    *(uint32_t *) (g_tss + 16)  = SEL_DATA1 | 1;
    *(uint32_t *) (g_tss + 20)  = (uint32_t) g_rstack[2] + sizeof(g_rstack[2]);
    *(uint32_t *) (g_tss + 24)  = SEL_DATA2 | 2;
    *(uint16_t *) (g_tss + 102) = TSS_IOMAP;
    memset(g_tss + 104, 0xff, 32); /* VME: every INT n through the IDT unless a test says */
    g_tss[sizeof(g_tss) - 1]    = 0xff;
}

/* Into the ring GDT and IDT (and the TSS, loaded once); CR4 and CR0 bits
   as a group needs them. */
static void
ring_env_on(uint32_t cr4_set, uint32_t cr0_set)
{
    struct {
        uint16_t limit;
        uint32_t base;
    } __attribute__((packed)) gdtr = { sizeof(g_gdt2) - 1, (uint32_t) g_gdt2 }, idtr = { sizeof(g_idt2) - 1, (uint32_t) g_idt2 };
    ring_build();
    __asm__ volatile("sgdt %0\n\tsidt %1" : "=m"(g_saved_gdtr), "=m"(g_saved_idtr));
    __asm__ volatile("lgdt %0\n\tlidt %1" : : "m"(gdtr), "m"(idtr) : "memory");
    if (!g_tr_loaded) {
        __asm__ volatile("ltr %w0" : : "r"(SEL_TSS));
        g_tr_loaded = 1;
    } else {
        /* LTR marked the descriptor busy; the rebuilt one is available:
           keep it busy, as the loaded TR expects. */
        g_gdt2[(SEL_TSS >> 3) * 2 + 1] |= 0x200;
    }
    if (cr4_set) {
        uint32_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 | cr4_set));
    }
    if (cr0_set) {
        uint32_t cr0;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 | cr0_set));
    }
}

static void
ring_env_off(uint32_t cr4_clear, uint32_t cr0_clear)
{
    if (cr4_clear) {
        uint32_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 & ~cr4_clear));
    }
    if (cr0_clear) {
        uint32_t cr0;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 & ~cr0_clear));
    }
    __asm__ volatile("lgdt %0\n\tlidt %1" : : "m"(g_saved_gdtr), "m"(g_saved_idtr) : "memory");
}
#endif

/* V86 code goes in conventional memory, 16-byte aligned so IP starts at
   0 in each slot. */
static uint32_t g_v86_next = V86_ARENA;

static uint8_t *
v86_alloc(uint32_t len)
{
    uint32_t a = (g_v86_next + 15) & ~15u;
    if (a + len > V86_ARENA_END) {
        g_arena_wraps++;
        a = V86_ARENA;
    }
    g_v86_next = a + len;
    return (uint8_t *) a;
}

static void
code16_base(uint32_t base)
{
    uint8_t *d = gdt + SEL_CODE16;
    d[2]       = base;
    d[3]       = base >> 8;
    d[4]       = base >> 16;
    d[7]       = base >> 24;
}
uint32_t g_fenv[7];
static uint32_t g_mmx_runs[4][16];

static void
capture(const struct variant *v, struct kout *o, uint8_t *slot, int vec, int mem)
{
    uint32_t fmask = v->flags_mask ? v->flags_mask : 0x0cd5;
    *o          = g_out;
    o->flags   &= fmask;
    if (v->stack)
        o->fault_err = g_stack_out - (uint32_t) g_buf;
    o->fault    = vec;
    memcpy(o->buf, g_buf, BUF_SIZE);
    memcpy(o->lowbuf, LOWBUF, LOWBUF_SIZE);
    o->sandbox_crc = mem ? crc_add(0, SANDBOX, SANDBOX_SIZE) : 0;
    if (v->mmx) /* the MMX registers count as memory */
        o->sandbox_crc = crc_add(o->sandbox_crc, g_mmx_out, sizeof(g_mmx_out));
    if (vec != 0xff) {
        /* The registers at the fault, not the (unwritten) output record. */
        o->eax      = g_fault.eax;
        o->ecx      = g_fault.ecx;
        o->edx      = g_fault.edx;
        o->ebx      = g_fault.ebx;
        o->ebp      = g_fault.ebp;
        o->esi      = g_fault.esi;
        o->edi      = g_fault.edi;
        o->flags    = g_fault.eflags & fmask;
        /* In 16-bit code the IP is already relative to the code; a fault
           at an address in the sandbox is given relative to the buffer
           (bit 31 set), so it compares across machines. */
        o->fault_ip = v->code16 || v->ring == 4 ? g_fault.eip : g_fault.eip - (uint32_t) slot;
        if (!v->code16 && g_fault.eip >= (uint32_t) SANDBOX && g_fault.eip < (uint32_t) SANDBOX + SANDBOX_SIZE)
            o->fault_ip = 0x80000000u | (g_fault.eip - (uint32_t) g_buf);
        o->fault_err = g_fault.err;
#ifndef HOSTTEST
        if (v->debug) {
            uint32_t dr6;
            __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));
            o->fault_err = dr6;
        }
        if (v->paging && vec == 14) {
            uint32_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            /* CR2 relative to the buffer, or (bit 31) to the alias area at
               1 GB, so it doesn't move with the build */
            o->fault_err = cr2 >= 0x40000000u ? 0x80000000u | (g_fault.err & 0xff) | ((cr2 - 0x40000000u) << 8)
                                              : (g_fault.err & 0xff) | ((cr2 - (uint32_t) g_buf) << 8);
        }
#endif
    }
}

/* ---- stopping: a mismatch, or a fault in the harness ------------------- */

/* What is running, for the reports. */
static const struct variant *g_cur_v;
static uint8_t              *g_cur_slot, *g_cur_low;
static uint32_t              g_cur_len, g_cur_low_len;

static void
halt_forever(void)
{
#ifdef HOSTTEST
    fflush(stdout);
    exit(1);
#else
    for (;;)
        __asm__ volatile("cli\n\thlt");
#endif
}

/* The top line of the screen in red. */
static void
stop_banner(const char *what)
{
#ifndef HOSTTEST
    char line[80];
    int  n = 0;
    for (const char *p = "STOPPED: "; *p; p++)
        line[n++] = *p;
    for (const char *p = what; *p && n < 60; p++)
        line[n++] = *p;
    for (const char *p = " - details on COM1"; *p && n < 80; p++)
        line[n++] = *p;
    while (n < 80)
        line[n++] = ' ';
    for (int i = 0; i < 80; i++)
        VGA[i] = 0x4f00 | (uint8_t) line[i]; /* white on red */
#else
    (void) what;
#endif
}

static void
hexdump(const char *label, const uint8_t *p, uint32_t n, int addr)
{
    puts_("  ");
    puts_(label);
    puts_(" (");
    putdec(n);
    puts_(" bytes):\n");
    for (uint32_t i = 0; i < n; i += 16) {
        puts_("    ");
        if (addr) {
            puthex((uint32_t) p + i, 8);
            puts_(": ");
        } else {
            puthex(i, 3);
            puts_(": ");
        }
        for (uint32_t k = i; k < i + 16 && k < n; k++) {
            puthex(p[k], 2);
            putch(' ');
        }
        putch('\n');
    }
}

static void
put_kv(const char *k, uint32_t v, int digits)
{
    putch(' ');
    puts_(k);
    putch('=');
    puthex(v, digits);
}

static void
where_line(void)
{
    puts_("  group ");
    puts_(g_group_name ? g_group_name : "(none)");
    puts_(" (");
    putdec(g_group_no);
    putch('/');
    putdec(g_group_count);
    puts_("), test ");
    putdec(g_stats.tests + 1);
    puts_(" of ");
    putdec(g_expected);
    puts_(", pass ");
    putdec(g_pass);
    puts_("\n  cpu ");
    if (g_cpu.has_cpuid) {
        puts_(g_cpu.vendor);
        put_kv("sig", g_cpu.signature, 4);
        put_kv("features", g_cpu.features, 8);
        put_kv("ext", g_cpu.ext_features, 8);
    } else
        puts_(g_cpu.is486 ? "486-no-cpuid" : "386");
    if (g_cpu.cyrix)
        puts_(" cyrix");
#ifndef HOSTTEST
    uint32_t cr0, cr2, cr3, cr4 = 0;
    __asm__ volatile("mov %%cr0, %0\n\tmov %%cr2, %1\n\tmov %%cr3, %2" : "=r"(cr0), "=r"(cr2), "=r"(cr3));
    if (g_cpu.has_cpuid)
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    puts_("\n ");
    put_kv("cr0", cr0, 8);
    put_kv("cr2", cr2, 8);
    put_kv("cr3", cr3, 8);
    put_kv("cr4", cr4, 8);
#endif
    putch('\n');
}

static void
variant_lines(const struct variant *v)
{
    if (!v) {
        puts_("  (no variant running)\n");
        return;
    }
    puts_("  variant: bytes");
    for (int i = 0; i < v->target_len; i++) {
        putch(' ');
        puthex(v->target[i], 2);
    }
    puts_("\n   producer ");
    puts_(producers[v->producer].name);
    puts_(v->boundary ? " +jmp (a new block)" : " (same block)");
    puts_("\n  ");
    put_kv("fx", v->fx, 1);
    put_kv("fx_arg", v->fx_arg, 2);
    put_kv("arg", v->arg, 8);
    put_kv("arg2", v->arg2, 8);
    put_kv("arg3", v->arg3, 8);
    put_kv("norm", v->norm, 2);
    put_kv("flags_mask", v->flags_mask, 8);
    puts_("\n  ");
    put_kv("stack", v->stack, 1);
    put_kv("mmx", v->mmx, 1);
    put_kv("code16", v->code16, 1);
    put_kv("paging", v->paging, 1);
    put_kv("ring", v->ring, 1);
    put_kv("ring_flags", v->ring_flags, 8);
    put_kv("faults_ok", v->faults_ok, 1);
    put_kv("no_ref", v->no_ref, 1);
    put_kv("emitted", v->emit != 0, 1);
    putch('\n');
}

static void
regs_line(const char *label, uint32_t fl, const uint32_t *r7)
{
    static const char *const names[7] = { "eax", "ecx", "edx", "ebx", "ebp", "esi", "edi" };
    puts_("  ");
    puts_(label);
    put_kv("fl", fl, 8);
    for (int i = 0; i < 7; i++)
        put_kv(names[i], r7[i], 8);
    putch('\n');
}

/* Runs 1-4 disagreed: everything there is to know about this test, then
   stop. */
static void
stop_mismatch(const struct variant *v, const struct kin *in, const struct kout out[RUNS])
{
    static const char *const labels[RUNS] = { "run 1 (interpreted)", "run 2 (interpreted, compiling)", "run 3 (compiled)", "run 4 (compiled again)" };
    stop_banner("runs disagree");
    vga_quiet = 0;
    puts_("\n==== STOP: THE FOUR RUNS OF ONE TEST DISAGREE ====\n");
    where_line();
    variant_lines(v);
    puts_("  INPUT\n");
    regs_line("in ", in->flags, &in->eax);
    hexdump("in buffer [g_buf]", in->buf, BUF_SIZE, 0);
    hexdump("in low buffer", in->lowbuf, LOWBUF_SIZE, 0);
    if (v->mmx)
        hexdump("in mm0-mm7", (const uint8_t *) g_mmx_in, sizeof(g_mmx_in), 0);
    puts_("  CODE\n");
    hexdump("slot", g_cur_slot, g_cur_len, 1);
    if (g_cur_low)
        hexdump("V86 code", g_cur_low, g_cur_low_len, 1);
    puts_("  RESULTS\n");
    for (int r = 0; r < RUNS; r++) {
        puts_("  ");
        puts_(labels[r]);
        putch('\n');
        regs_line("   ", out[r].flags, &out[r].eax);
        puts_("    ");
        put_kv("fault", out[r].fault, 2);
        put_kv("fault_ip", out[r].fault_ip, 8);
        put_kv("fault_err", out[r].fault_err, 8);
        put_kv("sandbox_crc", out[r].sandbox_crc, 8);
        putch('\n');
        hexdump("buffer", out[r].buf, BUF_SIZE, 0);
        hexdump("low buffer", out[r].lowbuf, LOWBUF_SIZE, 0);
        if (v->mmx)
            hexdump("mm0-mm7", (const uint8_t *) g_mmx_runs[r], sizeof(g_mmx_runs[r]), 0);
    }
    puts_("  DIFFERENCES from run 1\n");
    static const char *const fields[] = { "flags", "eax", "ecx", "edx", "ebx", "ebp", "esi", "edi", "fault", "fault_ip", "fault_err", "sandbox_crc" };
    for (int r = 1; r < RUNS; r++) {
        const uint32_t *a = &out[0].flags, *b = &out[r].flags;
        puts_("    run ");
        putdec(r + 1);
        puts_(":");
        int any = 0;
        for (int f = 0; f < 12; f++) {
            if (a[f] != b[f]) {
                putch(' ');
                puts_(fields[f]);
                putch('(');
                puthex(a[f], 8);
                puts_("->");
                puthex(b[f], 8);
                putch(')');
                any = 1;
            }
        }
        for (int i = 0; i < BUF_SIZE; i++)
            if (out[0].buf[i] != out[r].buf[i]) {
                puts_(" buf[");
                putdec(i);
                puts_("](");
                puthex(out[0].buf[i], 2);
                puts_("->");
                puthex(out[r].buf[i], 2);
                putch(')');
                any = 1;
            }
        for (int i = 0; i < LOWBUF_SIZE; i++)
            if (out[0].lowbuf[i] != out[r].lowbuf[i]) {
                puts_(" lowbuf[");
                putdec(i);
                puts_("](");
                puthex(out[0].lowbuf[i], 2);
                puts_("->");
                puthex(out[r].lowbuf[i], 2);
                putch(')');
                any = 1;
            }
        if (v->mmx)
            for (int i = 0; i < 16; i++)
                if (g_mmx_runs[0][i] != g_mmx_runs[r][i]) {
                    puts_(" mm");
                    putdec(i / 2);
                    puts_(i & 1 ? ".hi" : ".lo");
                    any = 1;
                }
        if (!any)
            puts_(" (the same: the difference is in the sandbox outside the buffer)");
        putch('\n');
    }
    puts_("==== STOPPED. Nothing more runs; reset the machine to start again. ====\n");
    halt_forever();
}

#ifndef HOSTTEST
/* The fault handler found no test running: the harness itself faulted.
   Called by isr_common on a stack of its own, with g_fault filled in. */
void harness_fault(void);
void
harness_fault(void)
{
    uint32_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    stop_banner("fault in the harness");
    vga_quiet = 0;
    puts_("\n==== STOP: A FAULT IN THE HARNESS ITSELF, NOT IN A TEST ====\n ");
    put_kv("vector", g_fault.vec, 2);
    put_kv("error", g_fault.err, 8);
    put_kv("eip", g_fault.eip, 8);
    put_kv("eflags", g_fault.eflags, 8);
    put_kv("cr2", cr2, 8);
    putch('\n');
    regs_line("at the fault", g_fault.eflags, &g_fault.eax);
    puts_("  while (the last test started, or the one after it):\n");
    where_line();
    variant_lines(g_cur_v);
    if (g_cur_slot)
        hexdump("its slot", g_cur_slot, g_cur_len, 1);
    puts_("==== STOPPED. Nothing more runs; reset the machine to start again. ====\n");
    halt_forever();
}
#endif

/* The producer, the block boundary if any, and the code under test. */
static void
emit_body(struct emit *e, const struct variant *v)
{
    ebytes(e, producers[v->producer].bytes, producers[v->producer].len);
    if (v->boundary) {
        e8(e, 0xeb); /* jmp $+2: the test starts a new block */
        e8(e, 0x00);
    }
    if (v->emit)
        v->emit(e, v);
    else
        ebytes(e, v->target, v->target_len);
}

#ifdef DUMP
/* build.py --dump PREFIX: dump lines only for the groups whose name starts
   with PREFIX (all of them without one). */
#    ifndef DUMP_ONLY
#        define DUMP_ONLY ""
#    endif
static int
dump_this(const struct variant *v)
{
    const char *p = DUMP_ONLY, *g = v->group;
    while (*p)
        if (*p++ != *g++)
            return 0;
    return 1;
}
#endif

/* The seven registers from the input: in V86 through DS = 0 from the copy
   at V86_BLOCK. */
static void
emit_ring_regs(struct emit *e, int v86)
{
    static const int order[] = { R_ECX, R_EDX, R_EBX, R_EBP, R_ESI, R_EDI, R_EAX };
    for (unsigned i = 0; i < 7; i++) {
        uint32_t *r = in_reg(order[i]);
        if (v86 == 2) { /* 16-bit protected-mode code: mov r32, [abs32] */
            e8(e, 0x66);
            e8(e, 0x67);
            e8(e, 0x8b);
            e8(e, 0x05 | (order[i] << 3));
            e32(e, (uint32_t) r);
        } else if (v86) {
            uint32_t disp = V86_BLOCK + (uint32_t) ((uint8_t *) r - (uint8_t *) &g_in);
            e8(e, 0x66); /* mov r32, [disp16] */
            e8(e, 0x8b);
            e8(e, 0x06 | (order[i] << 3));
            e8(e, disp);
            e8(e, disp >> 8);
        } else {
            e8(e, 0x8b); /* mov r32, [abs] */
            e8(e, 0x05 | (order[i] << 3));
            e32(e, (uint32_t) r);
        }
    }
}

/* A test at CPL 1-3 or in V86: IRETD there with the input flags (IOPL and
   AC from ring_flags), load the registers, run the producer and the
   target, and INT 30h back; the registers come back through the fault
   path, and a fault on the way is the result instead. */
static uint8_t *g_ring_iret; /* the harness's IRETD into the ring, to tell its faults from the test's */

static void
emit_ring(struct emit *e, const struct variant *v)
{
    static const uint8_t code_sel[4]   = { 0, SEL_CODE1 | 1, SEL_CODE2 | 2, SEL_CODE3 | 3 };
    static const uint8_t code16_sel[4] = { 0, SEL_C16R1 | 1, SEL_C16R2 | 2, SEL_C16R3 | 3 };
    static const uint8_t data_sel[4] = { 0, SEL_DATA1 | 1, SEL_DATA2 | 2, SEL_DATA3 | 3 };
    int                  v86          = v->ring == 4;
    uint8_t             *low          = 0, *push_l = 0;

    if (v86) {
        low = v86_alloc(96 + v->target_len);
        for (int i = 0; i < 5; i++) { /* GS FS DS ES SS: all 0 */
            e8(e, 0x6a);
            e8(e, 0x00);
        }
        e8(e, 0x68); /* ESP */
        e32(e, V86_STACK);
    } else {
        e8(e, 0x68); /* SS: a dword, as push imm8 would sign-extend C3h to FFC3h */
        e32(e, data_sel[v->ring]);
        e8(e, 0x68); /* ESP */
        e32(e, (uint32_t) g_rstack[v->ring] + sizeof(g_rstack[0]));
    }
    e8(e, 0xff); /* push dword [flags] */
    e8(e, 0x35);
    e32(e, (uint32_t) &g_in.flags);
    e8(e, 0x81); /* and dword [esp], ~(TF IF IOPL AC VM) */
    e8(e, 0x24);
    e8(e, 0x24);
    e32(e, ~(0x0100u | 0x0200u | 0x3000u | 0x40000u | 0x20000u));
    e8(e, 0x81); /* or dword [esp], ring_flags (+ VM) */
    e8(e, 0x0c);
    e8(e, 0x24);
    e32(e, v->ring_flags | (v86 ? 0x20000u : 0));
    if (v86) {
        e8(e, 0x68); /* CS */
        e32(e, (uint32_t) low >> 4);
        e8(e, 0x68); /* IP */
        e32(e, 0);
    } else {
        e8(e, 0x68); /* CS */
        e32(e, v->code16 ? code16_sel[v->ring] : code_sel[v->ring]);
        e8(e, 0x68); /* EIP: just after the IRETD (0 in a 16-bit segment based there) */
        push_l = e->p;
        e32(e, 0);
    }
    g_ring_iret = e->p;
    e8(e, 0xcf); /* iretd */
    if (v86) {
        struct emit c = { low };
        emit_ring_regs(&c, 1);
        emit_body(&c, v);
        e8(&c, 0xcd); /* int 30h */
        e8(&c, 0x30);
        g_cur_low     = low;
        g_cur_low_len = c.p - low;
    } else if (v->code16) {
#ifndef HOSTTEST
        uint32_t base = (uint32_t) e->p;
        int      sel  = code16_sel[v->ring] & ~3;
        g_gdt2[(sel >> 3) * 2]     = (g_gdt2[(sel >> 3) * 2] & 0xffff) | (base << 16);
        g_gdt2[(sel >> 3) * 2 + 1] = (g_gdt2[(sel >> 3) * 2 + 1] & 0x00ffff00) | ((base >> 16) & 0xff) | (base & 0xff000000);
#endif
        e8(e, 0xb8); /* mov ax, data selector; mov ds, ax; mov es, ax (16-bit code) */
        e8(e, data_sel[v->ring]);
        e8(e, 0x00);
        e8(e, 0x8e);
        e8(e, 0xd8);
        e8(e, 0x8e);
        e8(e, 0xc0);
        emit_ring_regs(e, 2);
        emit_body(e, v);
        e8(e, 0xcd); /* int 30h */
        e8(e, 0x30);
    } else {
        put32_at(push_l, (uint32_t) e->p);
        e8(e, 0x66); /* mov ax, data selector; mov ds, ax; mov es, ax */
        e8(e, 0xb8);
        e8(e, data_sel[v->ring]);
        e8(e, 0x00);
        e8(e, 0x8e);
        e8(e, 0xd8);
        e8(e, 0x8e);
        e8(e, 0xc0);
        emit_ring_regs(e, 0);
        emit_body(e, v);
        e8(e, 0xcd); /* int 30h */
        e8(e, 0x30);
    }
}

/* A fingerprint of a group's definition: every variant's bytes and
   settings, as the counting pass meets them. A stored reference is only
   compared when its fingerprint matches, so changing a group can never
   make an old reference stop the test. */
static uint32_t g_defhash, g_group_def;

static void
def_hash(const struct variant *v)
{
    uint32_t f[16] = { v->target_len, v->producer, v->boundary, v->fx, v->fx_arg, v->arg, v->arg2, v->arg3,
                       v->flags_mask, v->stack | (v->mmx << 1) | (v->code16 << 2) | (v->paging << 3) | (v->faults_ok << 4) | (v->no_ref << 5) | (v->reg_fix << 6) | (v->debug << 7),
                       v->ring | (v->code16_ip << 8), v->ring_flags, v->norm, v->emit != 0, v->defmask != 0, (v->fix_input != 0) | ((v->canon != 0) << 1) };
    g_defhash = crc_add(g_defhash, v->target, v->target_len);
    g_defhash = crc_add(g_defhash, f, sizeof(f));
}

/* One variant with one input, run four times from a fresh address. */
static void
run_variant(const struct variant *v)
{
    struct kout out[RUNS];
    struct kin  in;

    fill_input();
    if (v->mmx)
        for (int i = 0; i < 16; i++)
            g_mmx_in[i] = rnd_operand();
    g_skip = 0;
    if (v->fix_input)
        v->fix_input(v);
#ifdef HOSTTEST
    if (v->code16 || v->ring)
        g_skip = 1; /* no far calls into our own GDT, no rings or V86, in a Linux process */
#endif
    if (g_skip)
        return;
    if (g_counting) {
        g_stats.tests++;
        def_hash(v);
        return;
    }
    in = g_in;

    int         big  = v->emit || v->stack;
    uint32_t    len  = v->emit ? 512 : v->mmx || v->code16 ? 384 : big ? 256 : 160;
    if (v->target_len > 16) /* room for a long target on top */
        len += v->target_len;
    uint8_t    *slot = arena_alloc(len);
    struct emit e    = { slot };
    g_cur_low        = 0;
    if (v->ring)
        emit_ring(&e, v);
    else {
        emit_prologue(&e);
        if (v->stack) {
            e8(&e, 0x89); /* mov [g_stack_save], esp */
            e8(&e, 0x25);
            e32(&e, (uint32_t) &g_stack_save);
            e8(&e, 0xbc); /* mov esp, STACK_TOP */
            e32(&e, (uint32_t) STACK_TOP);
        }
        if (v->mmx) {
            e8(&e, 0xdb); /* fninit: the same x87 state every time */
            e8(&e, 0xe3);
            for (int i = 0; i < 8; i++) {
                e8(&e, 0x0f); /* movq mmI, [g_mmx_in + 8*I] */
                e8(&e, 0x6f);
                e8(&e, 0x05 | (i << 3));
                e32(&e, (uint32_t) &g_mmx_in[2 * i]);
            }
        }
        if (v->code16) {
            e8(&e, 0x9a); /* call far 18h:IP; the 16-bit part follows the epilogue */
            e32(&e, v->code16_ip);
            e8(&e, SEL_CODE16);
            e8(&e, 0x00);
            if (v->stack) {
                /* wipe the return address the call left on the private stack */
                static const uint8_t scrub[2][8] = { { 0xc7, 0x44, 0x24, 0xf8, 0, 0, 0, 0 },
                                                     { 0xc7, 0x44, 0x24, 0xfc, 0, 0, 0, 0 } };
                ebytes(&e, scrub[0], 8);
                ebytes(&e, scrub[1], 8);
            }
        } else
            emit_body(&e, v);
        if (v->mmx) {
            for (int i = 0; i < 8; i++) {
                e8(&e, 0x0f); /* movq [g_mmx_out + 8*I], mmI */
                e8(&e, 0x7f);
                e8(&e, 0x05 | (i << 3));
                e32(&e, (uint32_t) &g_mmx_out[2 * i]);
            }
            e8(&e, 0x0f); /* emms */
            e8(&e, 0x77);
        }
        if (v->stack) {
            e8(&e, 0x89); /* mov [g_stack_out], esp */
            e8(&e, 0x25);
            e32(&e, (uint32_t) &g_stack_out);
            e8(&e, 0x8b); /* mov esp, [g_stack_save] */
            e8(&e, 0x25);
            e32(&e, (uint32_t) &g_stack_save);
        }
        emit_epilogue(&e, v->stack || v->flags_mask);
        if (v->code16) {
            code16_base((uint32_t) e.p - v->code16_ip);
            emit_body(&e, v);
            e8(&e, 0x66); /* o32 retf: back to the 32-bit caller */
            e8(&e, 0xcb);
        }

    }

    /* Only the memory forms (those with an input fixer) and the stack
       variants touch memory: the sandbox is refilled and checked for them
       alone. */
    g_cur_v    = v;
    g_cur_slot = slot;
    g_cur_len  = e.p - slot;
    int mem = (v->fix_input != 0 && !v->reg_fix) || v->stack;
#ifndef HOSTTEST
    if (v->ring == 4)
        memcpy((void *) V86_BLOCK, &g_in, 32); /* the flags and registers, for V86 */
#endif
    for (int r = 0; r < RUNS; r++) {
        if (mem)
            memset(SANDBOX, 0xa5, SANDBOX_SIZE);
        memcpy(g_buf, in.buf, BUF_SIZE);
        memcpy(LOWBUF, in.lowbuf, LOWBUF_SIZE);
        memset(&g_out, 0, sizeof(g_out));
        memset(g_mmx_out, 0, sizeof(g_mmx_out));
        if (v->before_run)
            v->before_run(v);
        int vec = run_kernel(slot);
#ifndef HOSTTEST
        /* a test that loads FS or GS and doesn't fault leaves them loaded */
        __asm__ volatile("mov %0, %%fs\n\tmov %0, %%gs" : : "r"(0x10));
#endif
        if (v->after_run)
            v->after_run(v);
        capture(v, &out[r], slot, vec, mem);
#ifndef HOSTTEST
        if (v->ring && vec != 0xff && g_fault.eip == (uint32_t) g_ring_iret)
            harness_fault(); /* the way in failed: no test ran */
#endif
#ifndef HOSTTEST
        if (v->debug && vec == 0xff) {
            uint32_t dr6;
            __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));
            out[r].fault_err = dr6;
        }
#endif
        if (v->canon && out[r].fault == 0xff)
            v->canon(v, &in, &out[r]);
        if (v->mmx)
            memcpy(g_mmx_runs[r], g_mmx_out, sizeof(g_mmx_out));
    }

    g_stats.tests++;
    if ((g_stats.tests & 15) == 0 || g_stats.tests == g_expected)
        status();
    if (out[0].fault != 0xff)
        g_stats.faults++;
    int bad = 0;
    for (int r = 1; r < RUNS; r++)
        if (memcmp_(&out[r], &out[0], sizeof(struct kout)))
            bad = 1;
    if (bad) {
        g_stats.mismatches++;
        stop_mismatch(v, &in, out);
    }
    g_stats.crc_interp = crc_add(g_stats.crc_interp, &out[0], sizeof(struct kout));
    g_stats.crc_comp   = crc_add(g_stats.crc_comp, &out[RUNS - 1], sizeof(struct kout));
    if (!v->no_ref && (out[0].fault == 0xff || v->faults_ok)) {
        struct kout raw = out[0];
        normalize(v, &raw);
        g_stats.crc_raw = crc_add(g_stats.crc_raw, &raw, sizeof(raw));
        struct kout d;
        if (defined_result(v, &in, &out[0], &d)) {
            g_stats.defined++;
            g_stats.crc_defined = crc_add(g_stats.crc_defined, &d, sizeof(d));
#ifdef DUMP
            if (dump_this(v)) {
                /* One line per defined result, to diff against the host build. */
                vga_quiet = 1;
                puts_("D ");
                puts_(v->group);
                putch(' ');
                for (int i = 0; i < v->target_len; i++)
                    puthex(v->target[i], 2);
                putch(' ');
                puts_(producers[v->producer].name);
                putch(v->boundary ? 'j' : '-');
                puts_(" in=");
                puthex(in.flags, 4);
                putch(',');
                puthex(in.ecx, 8);
                putch(',');
                puthex(in.edx, 8);
                puts_(" fl=");
                puthex(d.flags, 4);
                for (int r = 0; r < 7; r++) { /* eax ecx edx ebx ebp esi edi */
                    putch(' ');
                    puthex((&d.eax)[r], 8);
                }
                puts_(" m=");
                puthex(crc_add(d.sandbox_crc, d.buf, BUF_SIZE + LOWBUF_SIZE), 8);
                putch('\n');
                vga_quiet = 0;
            }
#endif
        }
#ifdef DUMP
        /* The same result unmasked, undefined flags included: for comparing
           an emulated CPU with the real one. */
        if (dump_this(v)) {
            struct kout r = out[0];
            normalize(v, &r);
            vga_quiet = 1;
            puts_("R ");
            puts_(v->group);
            putch(' ');
            for (int i = 0; i < v->target_len; i++)
                puthex(v->target[i], 2);
            putch(' ');
            puts_(producers[v->producer].name);
            putch(v->boundary ? 'j' : '-');
            puts_(" in=");
            puthex(in.flags, 4);
            putch(',');
            puthex(in.ecx, 8);
            putch(',');
            puthex(in.edx, 8);
            puts_(" fl=");
            puthex(r.flags, 4);
            for (int i = 0; i < 7; i++) {
                putch(' ');
                puthex((&r.eax)[i], 8);
            }
            puts_(" m=");
            puthex(crc_add(r.sandbox_crc, r.buf, BUF_SIZE + LOWBUF_SIZE), 8);
            putch('\n');
            vga_quiet = 0;
        }
#endif
    }
}

static uint8_t g_group_post;

/* Every 3 seconds by the CMOS clock's seconds register, read with
   interrupts off (no timer interrupt runs, and RDTSC is missing on the
   486s this also tests). Called every 16 tests. */
static int
progress_due(void)
{
#ifdef HOSTTEST
    return 0;
#else
    static uint8_t last = 0xff;
    static int     seconds;
    uint8_t        now = cmos_read(0x00);
    if (last == 0xff)
        last = now;
    if (now != last) {
        last = now;
        if (++seconds >= 3) {
            seconds = 0;
            return 1;
        }
    }
    return 0;
#endif
}

#ifndef HOSTTEST
/* A number into the status line. */
static int
line_num(char *line, int n, uint32_t v)
{
    char d[12];
    int  k = 0;
    do {
        d[k++] = '0' + v % 10;
        v /= 10;
    } while (v);
    while (k)
        line[n++] = d[--k];
    return n;
}
#endif

/* The top line of the screen, redrawn in place every 16 tests; COM1 gets a
   PROGRESS line every 3 seconds. */
static void
status(void)
{
#ifndef HOSTTEST
    char     line[80];
    int      n    = 0;
    uint32_t done = g_stats.tests, left = g_expected - g_stats.tests;
    const char *p;
    for (p = "PASS "; *p; p++)
        line[n++] = *p;
    {
        char     pn[12];
        int      pk = 0;
        uint32_t pv = g_pass;
        do { pn[pk++] = '0' + pv % 10; pv /= 10; } while (pv);
        while (pk) line[n++] = pn[--pk];
    }
    for (p = "  group "; *p; p++)
        line[n++] = *p;
    n = line_num(line, n, g_group_no);
    line[n++] = '/';
    n = line_num(line, n, g_group_count);
    line[n++] = ' ';
    for (p = g_group_name; *p && n < 40; p++)
        line[n++] = *p;
    for (p = ": done "; *p; p++)
        line[n++] = *p;
    char num[12];
    int  k;
    k = 0;
    do { num[k++] = '0' + done % 10; done /= 10; } while (done);
    while (k) line[n++] = num[--k];
    for (p = ", left "; *p; p++)
        line[n++] = *p;
    k = 0;
    do { num[k++] = '0' + left % 10; left /= 10; } while (left);
    while (k) line[n++] = num[--k];
    while (n < 80)
        line[n++] = ' ';
    for (int i = 0; i < 80; i++)
        VGA[i] = 0x1f00 | (uint8_t) line[i]; /* white on blue */
#endif
    if (progress_due() || g_stats.tests == g_expected) {
        vga_quiet = 1;
        puts_("PROGRESS ");
        puts_(g_group_name);
        putch(' ');
        putdec(g_stats.tests);
        putch('/');
        putdec(g_expected);
        putch('\n');
        vga_quiet = 0;
    }
}

/* ---- references: what real CPUs gave ---------------------------------------- */

/* One group's results on one real CPU (refs.inc, made by refs.py from
   its serial log). skipped: the group didn't run there. */
struct cpu_ref {
    const char *vendor; /* CPUID vendor, or "386" / "486-no-cpuid" */
    uint32_t    sig;
    const char *group;
    uint8_t     skipped;
    uint32_t    tests, def, crc_defined, crc_raw;
};

static const struct cpu_ref cpu_refs[] = {
#include "refs.inc"
    { 0, 0, 0, 0, 0, 0, 0, 0 }
};

static int
str_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static const char *
cpu_key(void)
{
    return g_cpu.has_cpuid ? g_cpu.vendor : g_cpu.is486 ? "486-no-cpuid" : "386";
}

static const struct cpu_ref *
ref_find(const char *name)
{
    for (const struct cpu_ref *r = cpu_refs; r->group; r++)
        if (r->sig == (g_cpu.has_cpuid ? g_cpu.signature : 0) && str_eq(r->vendor, cpu_key()) && str_eq(r->group, name))
            return r;
    return 0;
}

static void stop_banner(const char *what);
static void where_line(void);
static void halt_forever(void);

/* The end of a group (or its skip): if a real CPU of this model has a
   reference for it, with the same definition, the results must match. */
static void
ref_check(const char *name, int skipped)
{
    const struct cpu_ref *r = ref_find(name);
    if (!r)
        return;
    if (!skipped && !r->skipped && r->def != g_group_def) {
        vga_quiet = 1;
        puts_("REF ");
        puts_(name);
        puts_(" stale: the group has changed since the reference was taken\n");
        vga_quiet = 0;
        return;
    }
    if (skipped == r->skipped && (skipped || (r->tests == g_stats.tests && r->crc_raw == g_stats.crc_raw && r->crc_defined == g_stats.crc_defined))) {
        vga_quiet = 1;
        puts_("REF ");
        puts_(name);
        puts_(" matches\n");
        vga_quiet = 0;
        return;
    }
    stop_banner("differs from the real CPU");
    vga_quiet = 0;
    puts_("\n==== STOP: A GROUP DIFFERS FROM WHAT THE REAL CPU GAVE ====\n");
    where_line();
    puts_("  reference (the same CPU model, real hardware):\n    ");
    if (r->skipped)
        puts_("skipped the group");
    else {
        puts_("tests=");
        putdec(r->tests);
        puts_(" crc_defined=");
        puthex(r->crc_defined, 8);
        puts_(" crc_raw=");
        puthex(r->crc_raw, 8);
        puts_(" def=");
        puthex(r->def, 8);
    }
    puts_("\n  this run:\n    ");
    if (skipped)
        puts_("skipped the group (a CPUID feature the reference had is missing)");
    else {
        puts_("tests=");
        putdec(g_stats.tests);
        puts_(" crc_defined=");
        puthex(g_stats.crc_defined, 8);
        puts_(" crc_raw=");
        puthex(g_stats.crc_raw, 8);
        puts_(" def=");
        puthex(g_group_def, 8);
        puts_(" mismatches=");
        putdec(g_stats.mismatches);
        puts_(" faults=");
        putdec(g_stats.faults);
        puts_(" crc_interp=");
        puthex(g_stats.crc_interp, 8);
        puts_(" crc_comp=");
        puthex(g_stats.crc_comp, 8);
    }
    puts_("\n  crc_defined differs: an architectural result differs from the real CPU.\n"
          "  only crc_raw differs: an undefined flag or result differs (the model's own behaviour).\n"
          "  To find the test: build with --dump ");
    puts_(name);
    puts_(" and run it here and on the real CPU; diffdump.py names every line that differs.\n");
    puts_("==== STOPPED. Nothing more runs; reset the machine to start again. ====\n");
    halt_forever();
}

static void
group_begin(const char *name)
{
    memset(&g_stats, 0, sizeof(g_stats));
    /* Each group's inputs depend only on its name, so a group gives the
       same CRCs whatever ran before it. */
    uint32_t seed = 0x9e3779b9;
    for (const char *p = name; *p; p++)
        seed = (seed ^ (uint8_t) *p) * 0x01000193;
    rng_state = seed | 1;
    if (g_counting) {
        g_defhash = 0;
        return;
    }
    g_group_name = name;
    post(++g_group_post);
    puts_("START ");
    puts_(name);
    puts_(" tests=");
    putdec(g_expected);
    puts_("\n");
}

static void
group_end(const char *name)
{
    if (g_counting) {
        g_expected  = g_stats.tests;
        g_group_def = g_defhash;
        return;
    }
    puts_("GROUP ");
    puts_(name);
    puts_(" tests=");
    putdec(g_stats.tests);
    puts_(" mismatches=");
    putdec(g_stats.mismatches);
    puts_(" faults=");
    putdec(g_stats.faults);
    puts_(" crc_interp=");
    puthex(g_stats.crc_interp, 8);
    puts_(" crc_comp=");
    puthex(g_stats.crc_comp, 8);
    puts_(" defined=");
    putdec(g_stats.defined);
    puts_(" crc_defined=");
    puthex(g_stats.crc_defined, 8);
    puts_(" crc_raw=");
    puthex(g_stats.crc_raw, 8);
    puts_(" def=");
    puthex(g_group_def, 8);
    puts_("\n");
    g_total_tests += g_stats.tests;
    g_total_mismatches += g_stats.mismatches;
    ref_check(name, 0);
}

/* A group this CPU can't run: said once, counted as nothing. */
static void
group_skip(const char *name, const char *why)
{
    if (g_counting)
        return;
    post(++g_group_post);
    puts_("GROUP ");
    puts_(name);
    puts_(" skipped: ");
    puts_(why);
    puts_("\n");
    ref_check(name, 1);
}

#include "groups.inc"

/* ---- entry --------------------------------------------------------------- */

void
cmain(uint32_t rom_base)
{
    post(0x01);
    serial_init();
    /* Each start-up step says it's done, so a board that stops early shows
       where. */
    puts_("CPUTEST: harness c");
    crc_init();
    putch('i');
    idt_init();
    putch('p');
    cpu_detect();
    putch('e');
    e820_read();
    putch('m');
    mp_read();
    putch('r');
    ram_detect();
    putch('a');
    arena_init();
    putch('\n');
#ifndef HOSTTEST
    /* Every IRQ masked: SYSRET, and STI in the rings, can leave IF set in
       a test, and nothing here wants an interrupt. */
    outb(0x21, 0xff);
    outb(0xa1, 0xff);
    /* The cache may be off (CR0.CD), as a BIOS leaves it for option ROMs,
       and 86Box interprets everything while it is: turn it on for the
       tests. WBINVD exists from the 486 on. */
    uint32_t cr0_boot, cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0_boot));
    if (g_cpu.is486)
        __asm__ volatile("wbinvd" ::: "memory");
    cr0 = cr0_boot & ~((1u << 30) | (1u << 29)); /* CD, NW */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
#endif

    puts_("\nCPUTEST 3 load=");
    puthex(rom_base, 5);
    puts_(" ram=");
    putdec(g_ram_top >> 20);
    puts_("M e820=");
    putdec(g_e820_status);
    puts_("/");
    putdec(g_e820_n);
    if (g_e820_mb_high) {
        puts_(" above4g=");
        putdec(g_e820_mb_high);
        puts_("M");
    }
    puts_(" mp=");
    putdec(g_mp.status);
    puts_("/");
    putdec(g_mp.n_cpus);
    puts_("/");
    putdec(g_mp.n_ioapics);
    puts_(" cpu=");
    if (g_cpu.has_cpuid) {
        puts_(g_cpu.vendor);
        puts_(" sig=");
        puthex(g_cpu.signature, 4);
        puts_(" features=");
        puthex(g_cpu.features, 8);
    } else
        puts_(g_cpu.is486 ? "486-no-cpuid" : "386");
#ifndef HOSTTEST
    puts_(" cr0=");
    puthex(cr0_boot, 8);
    puts_("->");
    puthex(cr0, 8);
#endif
    puts_("\n");

    /* The test goes round again when it finishes, for
       soaking a machine; every pass must print the same CRCs. The host
       build runs once. */
    for (g_pass = 1;; g_pass++) {
        g_total_tests = g_total_mismatches = 0;
        g_group_post = 0;
        run_groups();

        puts_("DONE pass=");
        putdec(g_pass);
        puts_(" tests=");
        putdec(g_total_tests);
        puts_(" mismatches=");
        putdec(g_total_mismatches);
        puts_(" arena_wraps=");
        putdec(g_arena_wraps);
        puts_(g_total_mismatches ? " FAIL\n" : " PASS\n");
        post(g_total_mismatches ? 0xee : 0xaa);
#ifdef HOSTTEST
        break;
#endif
    }
}

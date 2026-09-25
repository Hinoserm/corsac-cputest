/*
 * CPU accuracy test ROM: the harness.
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

static void
putch(char c)
{
    int spin = 100000;
    if (c == '\n')
        putch('\r');
    while (!(inb(COM1 + 5) & 0x20) && --spin)
        ;
    outb(COM1, c);
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
    if (g_cpu.has_cpuid) {
        uint32_t a, b, c, d;
        cpuid(0, &a, &b, &c, &d);
        memcpy(g_cpu.vendor + 0, &b, 4);
        memcpy(g_cpu.vendor + 4, &d, 4);
        memcpy(g_cpu.vendor + 8, &c, 4);
        g_cpu.vendor[12] = 0;
        if (a >= 1) {
            cpuid(1, &a, &b, &c, &d);
            g_cpu.signature = a;
            g_cpu.features  = d;
        }
    }
}

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

#define ARENA_BASE  0x00400000u
#define SLOT_ALIGN  64u
#define PAGE_SIZE   4096u

static uint32_t g_arena_next, g_arena_end, g_arena_wraps;

static void
arena_init(void)
{
    g_arena_next = ARENA_BASE;
    g_arena_end  = g_ram_top - 0x100000;
#ifdef HOSTTEST
    host_arena(ARENA_BASE, g_arena_end - ARENA_BASE);
    host_arena((uint32_t) SANDBOX, SANDBOX_SIZE);
#endif
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
        a = ARENA_BASE;
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
emit_epilogue(struct emit *e)
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
    e8(e, 0xc3);
}

/* Instructions run before the one under test, to leave the flags in each
   of the lazy states the recompiler tracks. They use ECX and EDX only. */
struct producer {
    const char   *name;
    uint8_t       len;
    uint8_t       bytes[4];
};

static const struct producer producers[] = {
    { "none", 0, { 0 } },
    { "cmp", 2, { 0x39, 0xd1 } },
    { "sub", 2, { 0x29, 0xd1 } },
    { "add", 2, { 0x01, 0xd1 } },
    { "adc", 2, { 0x11, 0xd1 } },
    { "sbb", 2, { 0x19, 0xd1 } },
    { "and", 2, { 0x21, 0xd1 } },
    { "or", 2, { 0x09, 0xd1 } },
    { "xor", 2, { 0x31, 0xd1 } },
    { "test", 2, { 0x85, 0xd1 } },
    { "inc", 1, { 0x41 } },
    { "dec", 1, { 0x49 } },
    { "neg", 2, { 0xf7, 0xd9 } },
    { "shl1", 2, { 0xd1, 0xe1 } },
    { "shr1", 2, { 0xd1, 0xe9 } },
    { "sarcl", 2, { 0xd3, 0xf9 } },
    { "rol1", 2, { 0xd1, 0xc1 } },
    { "add8", 2, { 0x00, 0xd1 } },
    { "sub16", 3, { 0x66, 0x29, 0xd1 } },
};
#define N_PRODUCERS (sizeof(producers) / sizeof(producers[0]))

/* ---- running one test ---------------------------------------------------- */

extern int           run_kernel(void *code);
extern struct fault  g_fault;

/* What a test variant is: the bytes under test, what runs before them, and
   whether a jump puts them at the start of a block of their own. */
struct variant {
    const char   *group;
    uint8_t       target[16];
    uint8_t       target_len;
    uint8_t       producer;
    uint8_t       boundary;
    void        (*fix_input)(const struct variant *v);
    uint32_t      arg; /* for fix_input */
};

struct group_stats {
    uint32_t tests, mismatches, faults;
    uint32_t crc_interp, crc_comp;
};

static struct group_stats g_stats;
static uint32_t           g_total_mismatches, g_total_tests;
static uint32_t           g_printed;

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

static void
capture(struct kout *o, uint8_t *slot, int vec)
{
    *o          = g_out;
    o->flags   &= 0x0cd5;
    o->fault    = vec;
    memcpy(o->buf, g_buf, BUF_SIZE);
    memcpy(o->lowbuf, LOWBUF, LOWBUF_SIZE);
    o->sandbox_crc = crc_add(0, SANDBOX, SANDBOX_SIZE);
    if (vec != 0xff) {
        /* The registers at the fault, not the (unwritten) output record. */
        o->eax      = g_fault.eax;
        o->ecx      = g_fault.ecx;
        o->edx      = g_fault.edx;
        o->ebx      = g_fault.ebx;
        o->ebp      = g_fault.ebp;
        o->esi      = g_fault.esi;
        o->edi      = g_fault.edi;
        o->flags    = g_fault.eflags & 0x0cd5;
        o->fault_ip = g_fault.eip - (uint32_t) slot;
        o->fault_err = g_fault.err;
    }
}

static void
print_out(const char *label, const struct kout *o)
{
    puts_("    ");
    puts_(label);
    puts_(" fl=");
    puthex(o->flags, 4);
    puts_(" a=");
    puthex(o->eax, 8);
    puts_(" c=");
    puthex(o->ecx, 8);
    puts_(" d=");
    puthex(o->edx, 8);
    puts_(" b=");
    puthex(o->ebx, 8);
    puts_(" bp=");
    puthex(o->ebp, 8);
    puts_(" si=");
    puthex(o->esi, 8);
    puts_(" di=");
    puthex(o->edi, 8);
    if (o->fault != 0xff) {
        puts_(" FAULT ");
        putdec(o->fault);
        puts_(" ip+");
        puthex(o->fault_ip, 4);
    }
    puts_("\n");
}

static void
report_mismatch(const struct variant *v, const struct kin *in, const struct kout out[RUNS])
{
    g_printed++;
    if (g_printed > 40)
        return;
    puts_("MISMATCH ");
    puts_(v->group);
    puts_(" bytes=");
    for (int i = 0; i < v->target_len; i++) {
        puthex(v->target[i], 2);
        putch(' ');
    }
    puts_("after=");
    puts_(producers[v->producer].name);
    if (v->boundary)
        puts_(" +jmp");
    puts_("\n    in fl=");
    puthex(in->flags, 4);
    puts_(" a=");
    puthex(in->eax, 8);
    puts_(" c=");
    puthex(in->ecx, 8);
    puts_(" d=");
    puthex(in->edx, 8);
    puts_(" b=");
    puthex(in->ebx, 8);
    puts_(" bp=");
    puthex(in->ebp, 8);
    puts_(" si=");
    puthex(in->esi, 8);
    puts_(" di=");
    puthex(in->edi, 8);
    puts_("\n");
    static const char *const labels[RUNS] = { "interp1", "interp2", "comp1  ", "comp2  " };
    for (int r = 0; r < RUNS; r++)
        print_out(labels[r], &out[r]);
    for (int r = 1; r < RUNS; r++) {
        if (memcmp_(out[r].buf, out[0].buf, BUF_SIZE) || memcmp_(out[r].lowbuf, out[0].lowbuf, LOWBUF_SIZE)) {
            puts_("    memory differs in run ");
            putdec(r + 1);
            puts_("\n");
        }
    }
    for (int r = 0; r < RUNS; r++) {
        if (out[r].sandbox_crc != out[0].sandbox_crc || r == 0) {
            puts_("    run ");
            putdec(r + 1);
            puts_(" sandbox crc ");
            puthex(out[r].sandbox_crc, 8);
            puts_("\n");
        }
    }
}

/* One variant with one input, run four times from a fresh address. */
static void
run_variant(const struct variant *v)
{
    struct kout out[RUNS];
    struct kin  in;

    fill_input();
    g_skip = 0;
    if (v->fix_input)
        v->fix_input(v);
    if (g_skip)
        return;
    in = g_in;

    uint8_t    *slot = arena_alloc(160);
    struct emit e    = { slot };
    emit_prologue(&e);
    ebytes(&e, producers[v->producer].bytes, producers[v->producer].len);
    if (v->boundary) {
        e8(&e, 0xeb); /* jmp $+2: the test starts a new block */
        e8(&e, 0x00);
    }
    ebytes(&e, v->target, v->target_len);
    emit_epilogue(&e);

    for (int r = 0; r < RUNS; r++) {
        memset(SANDBOX, 0xa5, SANDBOX_SIZE);
        memcpy(g_buf, in.buf, BUF_SIZE);
        memcpy(LOWBUF, in.lowbuf, LOWBUF_SIZE);
        memset(&g_out, 0, sizeof(g_out));
        int vec = run_kernel(slot);
        capture(&out[r], slot, vec);
    }

    g_stats.tests++;
    if (out[0].fault != 0xff)
        g_stats.faults++;
    int bad = 0;
    for (int r = 1; r < RUNS; r++)
        if (memcmp_(&out[r], &out[0], sizeof(struct kout)))
            bad = 1;
    if (bad) {
        g_stats.mismatches++;
        report_mismatch(v, &in, out);
    }
    g_stats.crc_interp = crc_add(g_stats.crc_interp, &out[0], sizeof(struct kout));
    g_stats.crc_comp   = crc_add(g_stats.crc_comp, &out[RUNS - 1], sizeof(struct kout));
}

static uint8_t g_group_post;

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
    post(++g_group_post);
}

static void
group_end(const char *name)
{
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
    puts_("\n");
    g_total_tests += g_stats.tests;
    g_total_mismatches += g_stats.mismatches;
}


#include "groups.inc"

/* ---- entry --------------------------------------------------------------- */

void
cmain(uint32_t rom_base)
{
    post(0x01);
    serial_init();
    crc_init();
    idt_init();
    cpu_detect();
    ram_detect();
    arena_init();

    puts_("\nCPUTEST 1 rom=");
    puthex(rom_base, 5);
    puts_(" ram=");
    putdec(g_ram_top >> 20);
    puts_("M cpu=");
    if (g_cpu.has_cpuid) {
        puts_(g_cpu.vendor);
        puts_(" sig=");
        puthex(g_cpu.signature, 4);
        puts_(" features=");
        puthex(g_cpu.features, 8);
    } else
        puts_(g_cpu.is486 ? "486-no-cpuid" : "386");
    puts_("\n");

    run_groups();

    puts_("DONE tests=");
    putdec(g_total_tests);
    puts_(" mismatches=");
    putdec(g_total_mismatches);
    puts_(" arena_wraps=");
    putdec(g_arena_wraps);
    puts_(g_total_mismatches ? " FAIL\n" : " PASS\n");
    post(g_total_mismatches ? 0xee : 0xaa);
}

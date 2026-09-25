/*
 * Multiprocessor machines: the BIOS's MP and ACPI tables, the local APIC,
 * the I/O APIC, and the other processors. Everything here is for bare
 * metal; the Linux build skips it. Included into groups.inc last.
 */

/* ---- the tables ------------------------------------------------------------ */

/* The processors and I/O APICs as the BIOS lists them (mp.inc): the MP
   floating pointer and configuration table, an MP default configuration,
   or the ACPI MADT. The board's answer, not the CPU's: no reference and
   never a stop; a missing or bad table only turns off the tests that need
   it. EAX the status (1 MP table, 2 default configuration, 3 MADT only, 0
   none; 4-10 a bad table: floating pointer, signature, checksum, length,
   entry, extended checksum, MADT), ECX the processors listed, EDX the I/O
   APICs, ESI the MP spec revision, EDI the IMCR flag. The entries are
   printed too. */
static void
group_mp_table(void)
{
    const char *name = "mp.table";
#ifdef HOSTTEST
    group_skip(name, "BIOS (host build)");
#else
    struct variant v = { .group = name, .no_ref = 1 };
    group_begin(name);
    if (!g_counting) {
        puts_("MP fps=");
        puthex(g_mp.fps, 8);
        puts_(" table=");
        puthex(g_mp.table, 8);
        puts_(" lapic=");
        puthex(g_mp.lapic, 8);
        puts_(" default=");
        putdec(g_mp.deftype);
        putch('\n');
        for (unsigned i = 0; i < g_mp.n_cpus; i++) {
            puts_("MP cpu apic=");
            puthex(g_mp.cpu[i].apic_id, 2);
            puts_(" ver=");
            puthex(g_mp.cpu[i].apic_ver, 2);
            puts_(" flags=");
            puthex(g_mp.cpu[i].flags, 2);
            puts_(" sig=");
            puthex(g_mp.cpu[i].sig, 8);
            putch('\n');
        }
        for (unsigned i = 0; i < g_mp.n_ioapics; i++) {
            puts_("MP ioapic id=");
            puthex(g_mp.io[i].id, 2);
            puts_(" ver=");
            puthex(g_mp.io[i].ver, 2);
            puts_(" en=");
            putdec(g_mp.io[i].en);
            puts_(" addr=");
            puthex(g_mp.io[i].addr, 8);
            putch('\n');
        }
    }
    static const uint8_t op[5] = { 0xb8, 0xb9, 0xba, 0xbe, 0xbf }; /* mov eax/ecx/edx/esi/edi, imm32 */
    uint32_t val[5] = { g_mp.status, g_mp.cpus_listed, g_mp.n_ioapics, g_mp.spec, g_mp.imcrp };
    int      n      = 0;
    for (int i = 0; i < 5; i++) {
        v.target[n++] = op[i];
        put_le(v.target + n, val[i], 4);
        n += 4;
    }
    v.target_len = n;
    run_plain(&v, 1);
    group_end(name);
#endif
}

/* ---- the local APIC --------------------------------------------------------- */

#define LAPIC_ID    0x020
#define LAPIC_VER   0x030
#define LAPIC_TPR   0x080
#define LAPIC_PPR   0x0a0
#define LAPIC_EOI   0x0b0
#define LAPIC_LDR   0x0d0
#define LAPIC_DFR   0x0e0
#define LAPIC_SVR   0x0f0
#define LAPIC_ESR   0x280
#define LAPIC_ICRLO 0x300
#define LAPIC_ICRHI 0x310
#define LAPIC_TIMER 0x320
#define LAPIC_LINT0 0x350
#define LAPIC_LINT1 0x360
#define LAPIC_ERROR 0x370
#define LAPIC_TICR  0x380
#define LAPIC_TCCR  0x390
#define LAPIC_TDCR  0x3e0

/* This processor's local APIC: at APIC_BASE (MSR 1Bh) on a P6, where a
   BIOS that cleared its enable bit has turned it off until reset (0);
   at FEE00000h on a Pentium, where CPUID says whether it was strapped
   on. Global: the interrupt handlers in rt.asm EOI through it. */
uint32_t g_lapic;

static int
lapic_find(void)
{
    g_lapic = 0;
#ifndef HOSTTEST
    if (!(g_cpu.features & CPUID_APIC))
        return 0;
    if (((g_cpu.signature >> 8) & 0xf) >= 6 && (g_cpu.features & CPUID_MSR)) {
        uint32_t base = rdmsr_lo(0x1b);
        if (!(base & 0x800))
            return 0;
        g_lapic = base & 0xfffff000u;
    } else
        g_lapic = 0xfee00000u;
#endif
    return g_lapic != 0;
}

static uint32_t
lapic_rd(uint32_t reg)
{
    return *(volatile uint32_t *) (g_lapic + reg);
}

/* Writes by XCHG: the P54C's erratum 11AP loses a plain write that
   follows another APIC access; XCHG is harmless everywhere. */
static void
lapic_wr(uint32_t reg, uint32_t val)
{
    volatile uint32_t *p = (volatile uint32_t *) (g_lapic + reg);
    __asm__ volatile("xchgl %0, %1" : "+r"(val), "+m"(*p) : : "memory");
}

/* The registers a test may change, saved and put back as the BIOS left
   them (virtual wire and all). */
static const uint16_t lapic_saved_regs[] = { LAPIC_TPR, LAPIC_LDR, LAPIC_DFR, LAPIC_TIMER, LAPIC_LINT0,
                                             LAPIC_LINT1, LAPIC_ERROR, LAPIC_TDCR, LAPIC_TICR, LAPIC_SVR };
static uint32_t       g_lapic_saved[sizeof(lapic_saved_regs) / sizeof(lapic_saved_regs[0])];

static void
lapic_save(void)
{
    for (unsigned i = 0; i < sizeof(lapic_saved_regs) / sizeof(lapic_saved_regs[0]); i++)
        g_lapic_saved[i] = lapic_rd(lapic_saved_regs[i]);
}

static void
lapic_restore(void)
{
    /* SVR first (enabled, so the LVT masks can be written), then the rest,
       then SVR as it was. */
    lapic_wr(LAPIC_SVR, lapic_rd(LAPIC_SVR) | 0x100);
    for (unsigned i = 0; i < sizeof(lapic_saved_regs) / sizeof(lapic_saved_regs[0]); i++)
        lapic_wr(lapic_saved_regs[i], g_lapic_saved[i]);
}

/* ESR as the model reads it after an error: a P6 latches errors into the
   readable ESR only on a write, a Pentium's read returns and clears them. */
static uint32_t
lapic_esr(void)
{
    if (((g_cpu.signature >> 8) & 0xf) >= 6)
        lapic_wr(LAPIC_ESR, 0);
    return lapic_rd(LAPIC_ESR);
}

/* Facts gathered in C, run as MOV reg, imm32 into EAX ECX EDX ESI EDI so
   they land in the CRCs: zeros while counting (the definition is the
   code, not the values). */
static void
run_facts(struct variant *v, const uint32_t val[5])
{
    static const uint8_t op[5] = { 0xb8, 0xb9, 0xba, 0xbe, 0xbf };
    int                  n     = 0;
    for (int i = 0; i < 5; i++) {
        v->target[n++] = op[i];
        put_le(v->target + n, g_counting ? 0 : val[i], 4);
        n += 4;
    }
    v->target_len = n;
    run_plain(v, 1);
}

/* The boot processor's local APIC, registers only (no interrupt is
   taken; the BIOS's settings are put back after):
   1. version, ID's reserved bits, TPR/LDR/DFR written all ones then
      zeros (their writable bits);
   2. SVR written 1FFh and 100h (its low bits are hard-wired on P5/P6),
      the timer divider's writable bits, LINT0/LINT1/error/timer LVTs
      written all ones (masked) and read back;
   3. SVR bit 8 cleared (software disable): the LVT masks set and an
      unmask ignored, then enabled again with the masks still set;
   4. the timer, masked, one-shot from FFFFFFFFh: the count falls across
      a 1 ms PIT wait, and an initial count of 0 stops it;
   5. an IPI to self with vector 3 (illegal: the error register's send
      and receive illegal-vector bits), read the model's way. */
static void
group_apic_regs(void)
{
    const char *name = "apic.regs";
#ifdef HOSTTEST
    group_skip(name, "the local APIC (host build)");
#else
    struct variant v = { .group = name };
    if (!lapic_find()) {
        group_skip(name, "no local APIC, or disabled");
        return;
    }
    group_begin(name);
    uint32_t f[5][5] = { { 0 } };
    if (!g_counting) {
        lapic_save();
        lapic_wr(LAPIC_SVR, g_lapic_saved[9] | 0x100);
        f[0][0] = lapic_rd(LAPIC_VER);
        f[0][1] = lapic_rd(LAPIC_ID) & 0x00ffffffu;
        lapic_wr(LAPIC_TPR, 0xffffffffu);
        f[0][2] = lapic_rd(LAPIC_TPR);
        lapic_wr(LAPIC_LDR, 0xffffffffu);
        f[0][3] = lapic_rd(LAPIC_LDR);
        lapic_wr(LAPIC_DFR, 0);
        f[0][4] = lapic_rd(LAPIC_DFR);
        lapic_wr(LAPIC_TPR, 0);
        lapic_wr(LAPIC_LDR, 0);
        lapic_wr(LAPIC_DFR, 0xffffffffu);

        lapic_wr(LAPIC_SVR, 0x1ff);
        f[1][0] = lapic_rd(LAPIC_SVR);
        lapic_wr(LAPIC_SVR, 0x100);
        f[1][1] = lapic_rd(LAPIC_SVR);
        lapic_wr(LAPIC_TDCR, 0xffffffffu);
        f[1][2] = lapic_rd(LAPIC_TDCR);
        lapic_wr(LAPIC_TDCR, 0);
        lapic_wr(LAPIC_LINT0, 0xffffffffu);
        lapic_wr(LAPIC_LINT1, 0xffffffffu);
        lapic_wr(LAPIC_ERROR, 0xffffffffu);
        lapic_wr(LAPIC_TIMER, 0xffffffffu);
        f[1][3] = lapic_rd(LAPIC_LINT0) ^ (lapic_rd(LAPIC_LINT1) << 1);
        f[1][4] = lapic_rd(LAPIC_ERROR) ^ (lapic_rd(LAPIC_TIMER) << 1);

        lapic_wr(LAPIC_TIMER, 0x000000f0); /* unmasked, vector F0h */
        lapic_wr(LAPIC_SVR, 0x0ff);        /* software disable */
        f[2][0] = lapic_rd(LAPIC_TIMER);
        lapic_wr(LAPIC_LINT0, 0x00000700); /* unmask while disabled */
        f[2][1] = lapic_rd(LAPIC_LINT0);
        lapic_wr(LAPIC_SVR, 0x1ff);
        f[2][2] = lapic_rd(LAPIC_TIMER);
        f[2][3] = lapic_rd(LAPIC_LINT0);
        f[2][4] = lapic_rd(LAPIC_SVR);

        lapic_wr(LAPIC_TIMER, 0x000100f0); /* masked, one-shot */
        lapic_wr(LAPIC_TDCR, 0x0b);        /* divide by 1 */
        lapic_wr(LAPIC_TICR, 0xffffffffu);
        uint32_t c0 = lapic_rd(LAPIC_TCCR);
        int      ok = delay_us(1000);
        uint32_t c1 = lapic_rd(LAPIC_TCCR);
        f[3][0] = lapic_rd(LAPIC_TICR);
        f[3][1] = c1 < c0;
        f[3][2] = ok;
        lapic_wr(LAPIC_TICR, 0);
        f[3][3] = lapic_rd(LAPIC_TCCR);
        f[3][4] = lapic_rd(LAPIC_TIMER);

        lapic_wr(LAPIC_ERROR, 0x000100fe); /* masked: nothing is taken */
        lapic_esr();                       /* clear what's there */
        lapic_wr(LAPIC_ESR, 0);
        f[4][0] = lapic_rd(LAPIC_ESR);
        lapic_wr(LAPIC_ICRHI, 0);
        lapic_wr(LAPIC_ICRLO, 0x00044003); /* fixed, self, vector 3 */
        for (int spin = 0; spin < 100000 && (lapic_rd(LAPIC_ICRLO) & 0x1000); spin++)
            ;
        f[4][1] = lapic_rd(LAPIC_ICRLO) & 0x1000;
        f[4][2] = lapic_rd(LAPIC_ESR);     /* before any write */
        f[4][3] = lapic_esr();             /* the model's way */
        f[4][4] = lapic_rd(LAPIC_ESR);     /* and again */
        lapic_restore();
    }
    for (int i = 0; i < 5; i++)
        run_facts(&v, f[i]);
    group_end(name);
#endif
}

/* ---- interrupts through the local APIC -------------------------------------- */

#ifndef HOSTTEST
extern uint32_t          irq_table[256], isr_table[32];
extern volatile uint32_t g_irq_hits[256], g_irq_log[16], g_irq_nlog, g_irq_eip, g_irq_eoi;

struct dtr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* The APIC groups' own IDT: the harness's exception handlers for 0-31
   (a fault in a group is a harness fault), rt.asm's counting handlers for
   NMI and 20h-FFh. */
static struct idt_entry g_idt_apic[256] __attribute__((aligned(8)));
static struct dtr       g_idt_saved;

static void
apic_idt_on(void)
{
    for (int i = 0; i < 256; i++) {
        uint32_t a            = irq_table[i] ? irq_table[i] : isr_table[i];
        g_idt_apic[i].off_lo = a & 0xffff;
        g_idt_apic[i].off_hi = a >> 16;
        g_idt_apic[i].sel    = 0x08;
        g_idt_apic[i].zero   = 0;
        g_idt_apic[i].type   = 0x8e;
    }
    struct dtr d = { sizeof(g_idt_apic) - 1, (uint32_t) g_idt_apic };
    __asm__ volatile("sidt %0\n\tlidt %1" : "=m"(g_idt_saved) : "m"(d) : "memory");
    for (int i = 0; i < 256; i++)
        g_irq_hits[i] = 0;
    g_irq_nlog = 0;
    g_irq_eoi  = 1;
}

static void
apic_idt_off(void)
{
    __asm__ volatile("cli\n\tlidt %0" : : "m"(g_idt_saved) : "memory");
}

/* A moment with interrupts on: what is pending is taken. */
static void
irq_window(void)
{
    __asm__ volatile("sti\n\tnop\n\tnop\n\tnop\n\tcli" : : : "memory");
}

static int
irr_bit(unsigned vec)
{
    return (lapic_rd(0x200 + (vec >> 5) * 0x10) >> (vec & 31)) & 1;
}

static int
icr_idle(void)
{
    for (int spin = 0; spin < 1000000; spin++)
        if (!(lapic_rd(LAPIC_ICRLO) & 0x1000))
            return 1;
    return 0;
}

static void
self_ipi(unsigned vec)
{
    lapic_wr(LAPIC_ICRHI, 0);
    lapic_wr(LAPIC_ICRLO, 0x00044000u | vec); /* fixed, edge, the self shorthand */
    icr_idle();
}
#endif

/* Interrupts on the boot processor, through its local APIC (IPIs to
   itself and its timer), with the APIC groups' IDT:
   1. a self-IPI taken at once; two held with IF clear (both in IRR),
      then taken highest class first;
   2. TPR 50h holding a class-4 vector pending (PPR 50h), released when
      TPR drops; no spurious vector (FFh) taken;
   3. the timer: one-shot delivers once, periodic more than once in the
      same 2 ms; then HLT woken by it returns past the HLT;
   4. the STI shadow: a pending IPI waits for the instruction after STI
      (the handler sees EIP past the first of two INCs); an NMI sent to its
      own APIC ID is taken, and still with port 70h's NMI-disable bit set
      (that gates only the board's NMI pin). */
static void
group_apic_irq(void)
{
    const char *name = "apic.irq";
#ifdef HOSTTEST
    group_skip(name, "the local APIC (host build)");
#else
    struct variant v = { .group = name };
    if (!lapic_find()) {
        group_skip(name, "no local APIC, or disabled");
        return;
    }
    group_begin(name);
    uint32_t f[4][5] = { { 0 } };
    if (!g_counting) {
        uint32_t id = lapic_rd(LAPIC_ID) >> 24;
        lapic_save();
        apic_idt_on();
        lapic_wr(LAPIC_SVR, 0x1ff);
        lapic_wr(LAPIC_TPR, 0);

        self_ipi(0x41);
        irq_window();
        f[0][0] = g_irq_hits[0x41];
        g_irq_nlog = 0;
        self_ipi(0x41);
        self_ipi(0x81);
        f[0][1] = irr_bit(0x41);
        f[0][2] = irr_bit(0x81);
        irq_window();
        f[0][3] = g_irq_log[0];
        f[0][4] = g_irq_log[1];

        g_irq_hits[0x41] = 0;
        lapic_wr(LAPIC_TPR, 0x50);
        self_ipi(0x41);
        irq_window();
        f[1][0] = g_irq_hits[0x41];
        f[1][1] = irr_bit(0x41);
        f[1][3] = lapic_rd(LAPIC_PPR) & 0xff;
        lapic_wr(LAPIC_TPR, 0);
        irq_window();
        f[1][2] = g_irq_hits[0x41];

        lapic_wr(LAPIC_TDCR, 0x0b); /* divide by 1 */
        lapic_wr(LAPIC_TIMER, 0x60);
        lapic_wr(LAPIC_TICR, 20000);
        __asm__ volatile("sti" : : : "memory");
        int ok = delay_us(2000);
        __asm__ volatile("cli" : : : "memory");
        f[2][0] = g_irq_hits[0x60];
        lapic_wr(LAPIC_TIMER, 0x20060); /* periodic */
        lapic_wr(LAPIC_TICR, 20000);
        g_irq_hits[0x60] = 0;
        __asm__ volatile("sti" : : : "memory");
        ok &= delay_us(2000);
        __asm__ volatile("cli" : : : "memory");
        lapic_wr(LAPIC_TICR, 0);
        f[2][1] = g_irq_hits[0x60] >= 2;
        f[2][3] = ok;
        if (f[2][0] == 1) { /* only with a timer that fires: nothing else wakes the HLT */
            uint32_t label;
            lapic_wr(LAPIC_TIMER, 0x60);
            lapic_wr(LAPIC_TICR, 20000);
            __asm__ volatile("movl $1f, %0\n\tsti\n\thlt\n1:\tcli" : "=r"(label) : : "memory");
            f[2][2] = g_irq_eip == label;
            lapic_wr(LAPIC_TICR, 0);
        } else
            f[2][2] = 2;

        uint32_t label, n = 0;
        self_ipi(0x42);
        __asm__ volatile("sti\n\tincl %0\n1:\tincl %0\n\tcli\n\tmovl $1b, %1" : "+r"(n), "=&r"(label) : : "memory");
        f[3][0] = g_irq_eip == label;
        f[3][1] = n;
        lapic_wr(LAPIC_ICRHI, id << 24);
        lapic_wr(LAPIC_ICRLO, 0x00004400); /* NMI to its own ID */
        icr_idle();
        delay_us(100);
        f[3][2] = g_irq_hits[2];
        outb(0x70, 0x8d);                  /* the board's NMI off */
        lapic_wr(LAPIC_ICRHI, id << 24);
        lapic_wr(LAPIC_ICRLO, 0x00004400);
        icr_idle();
        delay_us(100);
        outb(0x70, 0x0d);
        f[3][3] = g_irq_hits[2];
        f[1][4] = g_irq_hits[0xff];

        apic_idt_off();
        lapic_restore();
    }
    for (int i = 0; i < 4; i++)
        run_facts(&v, f[i]);
    group_end(name);
#endif
}

/* ---- the other processors ------------------------------------------------------ */

/* Started by INIT and STARTUP IPIs into rt.asm's ap_tramp, each takes a
   ticket (1 up; the BSP is 0) and runs ap_main on its own stack with its
   own IDT: an exception there stops that processor and is reported, never
   reaching the harness's handlers. Then it waits in a mailbox loop, with
   interrupts on, for the BSP's commands; it touches nothing of the
   harness's but its own slot and the counters a command names. Only the
   BSP prints, times, or touches the harness. */
#define AP_MAX    15
#define AP_TRAMP  0x3000u
#define T_DATA    0x800u
#define T_GDTR    (AP_TRAMP + T_DATA + 0)
#define T_TICKET  (AP_TRAMP + T_DATA + 8)
#define T_ARRIVED (AP_TRAMP + T_DATA + 12)
#define T_SLOTS   (AP_TRAMP + T_DATA + 16)

enum { AP_CMD_NONE, AP_CMD_PING, AP_CMD_LOCKS, AP_CMD_LDR };

struct percpu {
    volatile uint32_t cmd_seq, ack_seq, cmd, arg, state, apic_id, sig, result;
} __attribute__((aligned(64)));

struct percpu     g_pc[AP_MAX + 1];
uint32_t          g_ap_fault[(AP_MAX + 1) * 2]; /* by ticket: EIP or error code, and a flag */
static uint32_t   g_ap_cr0 RING_UNUSED; /* the BSP's CR0, which the APs take */
static int        g_ap_started RING_UNUSED, g_ap_count RING_UNUSED, g_ap_expected RING_UNUSED;
static uint32_t   g_ap_entry[AP_MAX + 1][3] RING_UNUSED; /* EDX, CR0, EFLAGS as each arrived */

#ifndef HOSTTEST
extern uint8_t    ap_tramp[], ap_tramp_end[], ap_fault_stub[];
extern volatile uint32_t g_irq_by_id[16 * 256];

static struct idt_entry g_idt_ap[256] __attribute__((aligned(8)));

/* Counters the lock test shares, each on a line of its own; c4 straddles
   two 64-byte lines. */
static volatile uint32_t g_lk[64] __attribute__((aligned(64)));
#define LK_INC   (g_lk + 0)
#define LK_XADD  (g_lk + 16)
#define LK_CAS   (g_lk + 32)
#define LK_SPIN  (g_lk + 48)
#define LK_PLAIN (g_lk + 50)
#define LK_SPLIT ((volatile uint32_t *) ((uint8_t *) (g_lk + 16) - 2)) /* across the line at +64 */

static void
ap_locks(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        __asm__ volatile("lock incl %0" : "+m"(*LK_INC));
        uint32_t one = 1;
        __asm__ volatile("lock xaddl %0, %1" : "+r"(one), "+m"(LK_XADD[1]));
        uint32_t old = *LK_CAS, want;
        do {
            want = old + 1;
            __asm__ volatile("lock cmpxchgl %2, %1" : "+a"(old), "+m"(*LK_CAS) : "r"(want) : "cc");
        } while (old != want - 1);
        uint32_t lock = 1;
        do
            __asm__ volatile("xchgl %0, %1" : "+r"(lock), "+m"(*LK_SPIN) : : "memory");
        while (lock);
        LK_PLAIN[0] = LK_PLAIN[0] + 1; /* only under the spinlock */
        __asm__ volatile("movl $0, %0" : "=m"(*LK_SPIN) : : "memory");
        __asm__ volatile("lock addl $1, %0" : "+m"(*LK_SPLIT));
    }
}

void ap_main(uint32_t ticket);
void
ap_main(uint32_t ticket)
{
    struct percpu *pc = &g_pc[ticket];
    struct dtr     d  = { sizeof(g_idt_ap) - 1, (uint32_t) g_idt_ap };
    uint32_t       a, b, c, dx;
    __asm__ volatile("lidt %0" : : "m"(d));
    __asm__ volatile("wbinvd\n\tmov %0, %%cr0\n\tfninit" : : "r"(g_ap_cr0) : "memory");
    cpuid(1, &a, &b, &c, &dx);
    pc->sig     = a;
    pc->apic_id = lapic_rd(LAPIC_ID) >> 24;
    lapic_wr(LAPIC_SVR, 0x1ff);
    lapic_wr(LAPIC_TPR, 0);
    pc->state = 1;
    __asm__ volatile("sti");
    for (;;) {
        while (pc->cmd_seq == pc->ack_seq)
            __asm__ volatile("pause" : : : "memory");
        switch (pc->cmd) {
            case AP_CMD_PING: pc->result++; break;
            case AP_CMD_LOCKS: ap_locks(pc->arg); break;
            case AP_CMD_LDR: /* logical flat: DFR all ones, LDR = arg */
                lapic_wr(LAPIC_DFR, 0xffffffffu);
                lapic_wr(LAPIC_LDR, pc->arg);
                pc->result = lapic_rd(LAPIC_LDR);
                break;
            default: break;
        }
        pc->ack_seq = pc->cmd_seq;
    }
}

/* Give AP t a command and wait up to 2 s for it to be done. */
static int
ap_do(int t, uint32_t cmd, uint32_t arg)
{
    struct percpu *pc = &g_pc[t];
    pc->cmd           = cmd;
    pc->arg           = arg;
    pc->cmd_seq       = pc->ack_seq + 1;
    for (int ms = 0; ms < 2000; ms++) {
        if (pc->ack_seq == pc->cmd_seq)
            return 1;
        delay_us(1000);
    }
    return 0;
}

/* One IPI; 0 if the ICR never goes idle (1 ms). */
static int
ipi(uint32_t dest, uint32_t icrlo)
{
    lapic_wr(LAPIC_ICRHI, dest << 24);
    lapic_wr(LAPIC_ICRLO, icrlo);
    for (int spin = 0; spin < 1000; spin++) {
        if (!(lapic_rd(LAPIC_ICRLO) & 0x1000))
            return 1;
        delay_us(1);
    }
    return 0;
}

/* Start every processor the tables list as enabled and not this one: INIT
   (level assert, then de-assert), two STARTUPs at AP_TRAMP (the second is
   ignored by one that started), each within the MP spec's delays; then up
   to 200 ms for it to arrive and 200 ms more to report ready. An 82489DX
   (APIC version below 10h) takes no STARTUP: not started, reported. */
static void
ap_start_all(void)
{
    uint32_t own = lapic_rd(LAPIC_ID) >> 24, gdtr[2];
    for (int i = 0; i < 256; i++) {
        uint32_t a = i >= 32 ? irq_table[i] : i == 2 ? irq_table[2] : (uint32_t) ap_fault_stub;
        g_idt_ap[i].off_lo = a & 0xffff;
        g_idt_ap[i].off_hi = a >> 16;
        g_idt_ap[i].sel    = 0x08;
        g_idt_ap[i].zero   = 0;
        g_idt_ap[i].type   = 0x8e;
    }
    __asm__ volatile("mov %%cr0, %0" : "=r"(g_ap_cr0));
    memcpy((void *) AP_TRAMP, ap_tramp, ap_tramp_end - ap_tramp);
    __asm__ volatile("sgdt %0" : "=m"(*(uint8_t (*)[6]) gdtr));
    memcpy((void *) T_GDTR, gdtr, 6);
    *(volatile uint32_t *) T_TICKET  = 1;
    *(volatile uint32_t *) T_ARRIVED = 0;
    for (unsigned i = 0; i < g_mp.n_cpus && g_ap_count < AP_MAX; i++) {
        const struct mp_cpu *c = &g_mp.cpu[i];
        if (!(c->flags & 1) || c->apic_id == own)
            continue;
        g_ap_expected++;
        if (c->apic_ver && c->apic_ver < 0x10)
            continue; /* an 82489DX: not started this way */
        uint32_t before = *(volatile uint32_t *) T_ARRIVED;
        lapic_esr();
        ipi(c->apic_id, 0x0000c500); /* INIT, level, assert */
        delay_us(10000);
        ipi(c->apic_id, 0x00008500); /* INIT, level, de-assert */
        delay_us(10000);
        for (int k = 0; k < 2; k++) {
            ipi(c->apic_id, 0x00000600 | (AP_TRAMP >> 12)); /* STARTUP */
            delay_us(200);
        }
        for (int ms = 0; ms < 200 && *(volatile uint32_t *) T_ARRIVED == before; ms++)
            delay_us(1000);
        if (*(volatile uint32_t *) T_ARRIVED == before)
            continue;
        int t = g_ap_count + 1;
        for (int ms = 0; ms < 200 && !g_pc[t].state && !g_ap_fault[t * 2 + 1]; ms++)
            delay_us(1000);
        if (g_pc[t].state) {
            memcpy(g_ap_entry[t], (const void *) (T_SLOTS + 16 * t), 12);
            g_ap_count++;
        }
    }
}
#endif

/* Starting the other processors (once, on the first pass; the result is
   kept): how many the tables list besides this one, how many started and
   reported, whether each came in with its own signature in EDX and the
   CR0 INIT leaves, and its APIC ID the table's. The machine's answer (how
   many processors), so no reference; the entry facts are compared among
   the processors in the result (1 = all the same as the first). */
static void
group_smp_start(void)
{
    const char *name = "smp.start";
#ifdef HOSTTEST
    group_skip(name, "processors (host build)");
#else
    struct variant v = { .group = name, .no_ref = 1 };
    if (!lapic_find() || !mp_usable()) {
        group_skip(name, "no local APIC or no MP/ACPI table");
        return;
    }
    group_begin(name);
    uint32_t f[5] = { 0 };
    if (!g_counting) {
        if (!g_ap_started) {
            g_ap_started = 1;
            ap_start_all();
            for (int t = 1; t <= g_ap_count; t++) {
                puts_("SMP cpu ticket=");
                putdec(t);
                puts_(" apic=");
                puthex(g_pc[t].apic_id, 2);
                puts_(" sig=");
                puthex(g_pc[t].sig, 8);
                puts_(" entry edx=");
                puthex(g_ap_entry[t][0], 8);
                puts_(" cr0=");
                puthex(g_ap_entry[t][1], 8);
                puts_(" eflags=");
                puthex(g_ap_entry[t][2], 8);
                putch('\n');
            }
        }
        f[0]     = g_ap_expected;
        f[1]     = g_ap_count;
        f[2]     = 1;
        f[3]     = 1;
        for (int t = 1; t <= g_ap_count; t++) {
            if (g_ap_entry[t][0] != g_ap_entry[1][0] || g_ap_entry[t][1] != g_ap_entry[1][1])
                f[2] = 0;
            if ((g_ap_entry[t][0] & 0xfff) != (g_pc[t].sig & 0xfff))
                f[3] = 0;
        }
        f[4] = g_ap_count ? g_ap_entry[1][1] : 0;
    }
    run_facts(&v, f);
    group_end(name);
#endif
}

/* IPIs from the BSP to each started processor (physical, fixed vector
   50h; then NMI) and to all but itself (51h); logical flat, each AP its
   own LDR bit, all of them in one IPI (52h). EAX: every physical IPI
   taken exactly once; ECX: the NMIs; EDX: the broadcast (each AP once,
   the BSP not); ESI: the logical one; EDI: every AP answered. */
static void
group_smp_ipi(void)
{
    const char *name = "smp.ipi";
#ifdef HOSTTEST
    group_skip(name, "processors (host build)");
#else
    struct variant v = { .group = name, .no_ref = 1 };
    if (!g_ap_count && !g_counting) {
        group_skip(name, "no other processor started");
        return;
    }
    if (!lapic_find()) {
        group_skip(name, "no local APIC");
        return;
    }
    group_begin(name);
    uint32_t f[5] = { 0 };
    if (!g_counting) {
        uint32_t own = lapic_rd(LAPIC_ID) >> 24, mask = 0;
        f[0] = f[1] = f[2] = f[3] = f[4] = 1;
        for (int t = 1; t <= g_ap_count; t++) {
            uint32_t id = g_pc[t].apic_id & 15;
            g_irq_by_id[id * 256 + 0x50] = g_irq_by_id[id * 256 + 2] = 0;
            g_irq_by_id[id * 256 + 0x51] = g_irq_by_id[id * 256 + 0x52] = 0;
        }
        g_irq_by_id[(own & 15) * 256 + 0x51] = 0;
        for (int t = 1; t <= g_ap_count; t++) {
            uint32_t id = g_pc[t].apic_id;
            ipi(id, 0x00004050); /* fixed, physical, vector 50h */
            ipi(id, 0x00004400); /* NMI */
        }
        ipi(0, 0x000c4051); /* all excluding self, vector 51h */
        for (int t = 1; t <= g_ap_count; t++) {
            f[4] &= ap_do(t, AP_CMD_LDR, 1u << (24 + t));
            mask |= 1u << t;
        }
        uint32_t ldr = lapic_rd(LAPIC_LDR); /* the BSP out of it: nothing it can't take */
        lapic_wr(LAPIC_LDR, 0);
        ipi(mask, 0x00004852); /* fixed, logical, vector 52h */
        delay_us(10000);
        lapic_wr(LAPIC_LDR, ldr);
        for (int t = 1; t <= g_ap_count; t++) {
            uint32_t id = g_pc[t].apic_id & 15;
            f[0] &= g_irq_by_id[id * 256 + 0x50] == 1;
            f[1] &= g_irq_by_id[id * 256 + 2] == 1;
            f[2] &= g_irq_by_id[id * 256 + 0x51] == 1;
            f[3] &= g_irq_by_id[id * 256 + 0x52] == 1;
            f[4] &= ap_do(t, AP_CMD_PING, 0);
        }
        f[2] &= g_irq_by_id[(own & 15) * 256 + 0x51] == 0;
        for (int t = 1; t <= g_ap_count; t++) /* back to what INIT left */
            ap_do(t, AP_CMD_LDR, 0);
    }
    run_facts(&v, f);
    group_end(name);
#endif
}

/* Every processor at once, 100000 times each: LOCK INC, LOCK XADD, a LOCK
   CMPXCHG loop, a plain increment under an XCHG spinlock, and LOCK ADD on
   a dword straddling two cache lines. Each total must be exact (EAX-EDI
   hold 1 for each that is; the fifth, EDI, the split one). The BSP runs
   its share while the others run theirs. */
static void
group_smp_lock(void)
{
    const char *name = "smp.lock";
#ifdef HOSTTEST
    group_skip(name, "processors (host build)");
#else
    struct variant v = { .group = name, .no_ref = 1 };
    if (!g_ap_count && !g_counting) {
        group_skip(name, "no other processor started");
        return;
    }
    group_begin(name);
    uint32_t f[5] = { 0 };
    if (!g_counting) {
        const uint32_t n = 100000;
        for (int i = 0; i < 64; i++)
            g_lk[i] = 0;
        for (int t = 1; t <= g_ap_count; t++) { /* all start before any is waited for */
            g_pc[t].cmd     = AP_CMD_LOCKS;
            g_pc[t].arg     = n;
            g_pc[t].cmd_seq = g_pc[t].ack_seq + 1;
        }
        ap_locks(n);
        int done = 1;
        for (int t = 1; t <= g_ap_count; t++) {
            int ms = 0;
            while (g_pc[t].ack_seq != g_pc[t].cmd_seq && ms++ < 20000)
                delay_us(1000);
            done &= g_pc[t].ack_seq == g_pc[t].cmd_seq;
        }
        uint32_t total = n * (g_ap_count + 1);
        f[0] = done && *LK_INC == total;
        f[1] = done && LK_XADD[1] == total;
        f[2] = done && *LK_CAS == total;
        f[3] = done && LK_PLAIN[0] == total;
        f[4] = done && *LK_SPLIT == total;
    }
    run_facts(&v, f);
    group_end(name);
#endif
}

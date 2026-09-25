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
   on. */
static uint32_t g_lapic;

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

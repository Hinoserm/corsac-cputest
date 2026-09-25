/*
 * CPU-specific groups: undocumented aliases, CPUID, the MSRs a model has,
 * the TSC, and the Cyrix configuration registers. Most results are per
 * model, so they go into the raw CRC only. Included into groups.inc after
 * groups_map.inc.
 */

#define CPUID_MSR (1u << 5)

/* Undocumented encodings that alias documented ones, and sub-opcodes that
   must raise #UD. */
static void
group_aliases(void)
{
    static const struct {
        uint8_t mem, len, b[8];
    } seqs[] = {
        { 0, SEQ(0x82, 0xc0, 0x7f) },                   /* 82 /0: add al, 7Fh */
        { 0, SEQ(0x82, 0xc9, 0x80) },                   /* 82 /1: or cl, 80h */
        { 0, SEQ(0x82, 0xd2, 0x01) },                   /* 82 /2: adc dl, 1 */
        { 0, SEQ(0x82, 0xdb, 0xff) },                   /* 82 /3: sbb bl, 0FFh */
        { 0, SEQ(0x82, 0xe4, 0x0f) },                   /* 82 /4: and ah, 0Fh */
        { 0, SEQ(0x82, 0xed, 0x10) },                   /* 82 /5: sub ch, 10h */
        { 0, SEQ(0x82, 0xf6, 0x55) },                   /* 82 /6: xor dh, 55h */
        { 0, SEQ(0x82, 0xff, 0x00) },                   /* 82 /7: cmp bh, 0 */
        { 1, SEQ(0xf0, 0x82, 0x03, 0x01) },             /* lock 82 /0: add byte [ebx], 1 */
        { 0, SEQ(0xf6, 0xc8, 0x55) },                   /* F6 /1: test al, 55h */
        { 0, SEQ(0xf7, 0xc9, 0x00, 0x80, 0x00, 0x80) }, /* F7 /1: test ecx, 80008000h */
        { 0, SEQ(0xd0, 0xf0) },                         /* D0 /6: sal al, 1 */
        { 0, SEQ(0xd1, 0xf1) },                         /* D1 /6: sal ecx, 1 */
        { 0, SEQ(0xd2, 0xf2) },                         /* D2 /6: sal dl, cl */
        { 0, SEQ(0xd3, 0xf3) },                         /* D3 /6: sal ebx, cl */
        { 0, SEQ(0xc0, 0xf4, 0x03) },                   /* C0 /6: sal ah, 3 */
        { 0, SEQ(0xc1, 0xf6, 0x11) },                   /* C1 /6: sal esi, 17 */
        { 1, SEQ(0xc1, 0x33, 0x05) },                   /* C1 /6: sal dword [ebx], 5 */
        { 0, SEQ(0x8f, 0xc8) },                         /* 8F /1: #UD */
        { 0, SEQ(0xc6, 0xc8, 0x00) },                   /* C6 /1: #UD */
        { 0, SEQ(0xc7, 0xc8, 0x00, 0x00, 0x00, 0x00) }, /* C7 /1: #UD */
        { 0, SEQ(0xfe, 0xd0) },                         /* FE /2: #UD */
        { 0, SEQ(0xfe, 0xf8) },                         /* FE /7: #UD */
        { 0, SEQ(0xff, 0xf8) },                         /* FF /7: #UD */
        { 0, SEQ(0xff, 0xd8) },                         /* FF /3 with a register: #UD */
        { 0, SEQ(0xff, 0xe8) },                         /* FF /5 with a register: #UD */
        { 1, SEQ(0x0f, 0x0d, 0x13) },                   /* 0F 0D /2 [ebx]: a PREFETCH alias on AMD */
    };
    const char    *name = "aliases";
    struct variant v    = { .group = name, .faults_ok = 1, .fx = FX_CUSTOM, .defmask = dm_arg3, .arg = MEM_OFF };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.fix_input  = seqs[i].mem ? fix_mem_ebx : 0;
        run_plain(&v, 1);
    }
    group_end(name);
}

/* mov eax, leaf; xor ecx, ecx; cpuid */
static void
group_cpuid(void)
{
    static const uint32_t leaves[] = { 0, 1, 2, 3, 0x80000000, 0x80000001, 0x80000002, 0x80000003,
                                       0x80000004, 0x80000005, 0x80000006, 0xc0000000, 0x8fffffff };
    const char           *name     = "cpuid";
    struct variant        v        = { .group = name, .faults_ok = 1, .fx = FX_CUSTOM, .defmask = dm_arg3 };
    if (!g_cpu.has_cpuid) {
        group_skip(name, "no CPUID");
        return;
    }
    group_begin(name);
    for (unsigned i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
        v.target[0] = 0xb8;
        put_le(v.target + 1, leaves[i], 4);
        v.target[5]  = 0x31;
        v.target[6]  = 0xc9;
        v.target[7]  = 0x0f;
        v.target[8]  = 0xa2;
        v.target_len = 9;
        run_plain(&v, 1);
    }
    /* The local APIC's version register, where CPUID says there is an
       APIC: through APIC_BASE (1Bh) if it's enabled, 0 if not. */
    if ((g_cpu.features & CPUID_APIC) && (g_cpu.features & CPUID_MSR)) {
        static const uint8_t apic[] = {
            0xb9, 0x1b, 0x00, 0x00, 0x00, /* mov ecx, 1bh */
            0x0f, 0x32,                   /* rdmsr */
            0x0f, 0xba, 0xe0, 0x0b,       /* bt eax, 11: enabled */
            0x73, 0x0a,                   /* jnc 1f */
            0x25, 0x00, 0xf0, 0xff, 0xff, /* and eax, 0fffff000h */
            0x8b, 0x40, 0x30,             /* mov eax, [eax+30h] */
            0xeb, 0x02,                   /* jmp 2f */
            0x31, 0xc0,                   /* 1: xor eax, eax */
            0x31, 0xd2,                   /* 2: xor edx, edx */
            0x31, 0xc9,                   /* xor ecx, ecx */
        };
        memcpy(v.target, apic, sizeof(apic));
        v.target_len = sizeof(apic);
        run_plain(&v, 1);
    }
    group_end(name);
}

/* Which MSRs a model has: RDMSR of each, then EDX:EAX cleared, so only
   whether it faulted (#GP) remains (most values are volatile or set by
   the BIOS). */
static void
group_msr_map(void)
{
    static const uint32_t msrs[] = {
        0x00, 0x01, 0x02, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, /* P5 MCA, test registers */
        0x10, 0x11, 0x12, 0x13, 0x14, 0x17, 0x1b, 0x2a, 0x79, 0x8b, 0xc1, 0xfe,              /* TSC, CESR, counters, P6 */
        0x82, 0x83, 0x85, 0x86, 0x87, 0x88, 0x89,                                            /* AMD K5 */
        0x107, 0x108, 0x109, 0x10a, 0x147,                                                   /* IDT WinChip */
        0x179, 0x17b, 0x186, 0x200, 0x250, 0x2ff,                                            /* P6 MCA, MTRRs */
        0x88, 0x116, 0x11e, 0x174, 0x175, 0x176, 0x17a, 0x187, 0xc2, 0x1d9, 0x1db, 0x1dc,    /* P6: L2, SYSENTER, counters, LBR */
        0x201, 0x20f, 0x258, 0x259, 0x268, 0x26f, 0x277, 0x400, 0x401, 0x402, 0x403,       /* P6: MTRRs, PAT, MC bank 0 */
        0x110, 0x120, 0x1107, 0x1108,                                                        /* WinChip MCR, Cyrix III FCR */
        0x40000010, 0x80000010,                                                              /* the TSC with high ECX bits */
        0xc0000080, 0xc0000081, 0xc0000082, 0xc0000083, 0xc0000084, 0xc0000085, 0xc0000086, 0xc0000087, /* AMD K6 */
        0xc0010000, 0x40000000, 0xffffffff,
    };
    const char    *name = "msr.map";
    struct variant v    = { .group = name, .faults_ok = 1, .fx = FX_CUSTOM, .defmask = dm_arg3 };
    if (!(g_cpu.features & CPUID_MSR)) {
        group_skip(name, "no MSRs");
        return;
    }
    group_begin(name);
    for (unsigned i = 0; i < sizeof(msrs) / sizeof(msrs[0]); i++) {
        v.target[0] = 0xb9; /* mov ecx, msr */
        put_le(v.target + 1, msrs[i], 4);
        v.target[5]  = 0x0f; /* rdmsr */
        v.target[6]  = 0x32;
        v.target[7]  = 0x31; /* xor eax, eax */
        v.target[8]  = 0xc0;
        v.target[9]  = 0x31; /* xor edx, edx */
        v.target[10] = 0xd2;
        v.target_len = 11;
        run_plain(&v, 1);
    }
    group_end(name);
}

/* RDMSR of the TSC (10h) against RDTSC just after: the two agree to within
   a little, so EAX becomes 1; EDX is the upper halves' difference (0). */
static void
tsc_rdmsr(const char *name)
{
    static const uint8_t seq[] = {
        0xb9, 0x10, 0x00, 0x00, 0x00, /* mov ecx, 10h */
        0x0f, 0x32,                   /* rdmsr */
        0x89, 0xc6,                   /* mov esi, eax */
        0x89, 0xd7,                   /* mov edi, edx */
        0x0f, 0x31,                   /* rdtsc */
        0x29, 0xf0,                   /* sub eax, esi */
        0x19, 0xfa,                   /* sbb edx, edi */
        0x3d, 0x00, 0x00, 0x01, 0x00, /* cmp eax, 10000h */
        0x0f, 0x92, 0xc0,             /* setb al */
        0x0f, 0xb6, 0xc0,             /* movzx eax, al */
        0x31, 0xf6,                   /* xor esi, esi */
        0x31, 0xff,                   /* xor edi, edi */
        0x31, 0xc9,                   /* xor ecx, ecx */
    };
    struct variant v = { .group = name, .faults_ok = 1, .fx = FX_CUSTOM, .defmask = dm_fxarg, .fx_arg = 1 };
    memcpy(v.target, seq, sizeof(seq));
    v.target_len = sizeof(seq);
    run_plain(&v, 2);
}

/* On a P6 (Intel family 6): WRMSR to the read-only MTRRcap (FEh) and
   MCG_CAP (179h) and to the LBR MSR 1DBh is #GP, and so is a variable
   MTRR with a reserved memory type (2); PerfCtr0 (C1h) takes the low 32
   bits of what is written and sign-extends them to 40; where CPUID
   reports PAT (Pentium II Deschutes), PAT with a reserved memory type (2)
   is #GP. MTRR 0, PerfCtr0, EvtSel0 (stopped, so the counter holds
   still) and PAT are saved before each run and put back after. */
#define CPUID_PAT (1u << 16)
static uint32_t g_msrw_saved[8] RING_UNUSED;

static void
msrw_before(const struct variant *v)
{
#ifndef HOSTTEST
    static const uint32_t msrs[3] = { 0x200, 0xc1, 0x186 };
    for (int i = 0; i < 3; i++)
        __asm__ volatile("rdmsr" : "=a"(g_msrw_saved[2 * i]), "=d"(g_msrw_saved[2 * i + 1]) : "c"(msrs[i]));
    if (g_cpu.features & CPUID_PAT)
        __asm__ volatile("rdmsr" : "=a"(g_msrw_saved[6]), "=d"(g_msrw_saved[7]) : "c"(0x277));
    wrmsr_(0x186, 0, 0);
#endif
}

static void
msrw_after(const struct variant *v)
{
#ifndef HOSTTEST
    static const uint32_t msrs[3] = { 0x200, 0xc1, 0x186 };
    for (int i = 0; i < 3; i++)
        wrmsr_(msrs[i], g_msrw_saved[2 * i], g_msrw_saved[2 * i + 1]);
    if (g_cpu.features & CPUID_PAT)
        wrmsr_(0x277, g_msrw_saved[6], g_msrw_saved[7]);
#endif
}

static void
msr_write_p6(const char *name)
{
    static const struct {
        uint8_t len, b[24];
    } seqs[] = {
        { SEQ(0xb9, 0xfe, 0x00, 0x00, 0x00, 0x31, 0xc0, 0x31, 0xd2, 0x0f, 0x30) },       /* MTRRcap */
        { SEQ(0xb9, 0x79, 0x01, 0x00, 0x00, 0x31, 0xc0, 0x31, 0xd2, 0x0f, 0x30) },       /* MCG_CAP */
        { SEQ(0xb9, 0xdb, 0x01, 0x00, 0x00, 0x31, 0xc0, 0x31, 0xd2, 0x0f, 0x30) },       /* LastBranchFromIP */
        { SEQ(0xb9, 0x00, 0x02, 0x00, 0x00, 0xb8, 0x02, 0x00, 0x00, 0x00, 0x31, 0xd2,
              0x0f, 0x30) },                                                             /* MTRRphysBase0, type 2 */
        { SEQ(0xb9, 0xc1, 0x00, 0x00, 0x00, 0xb8, 0x00, 0x00, 0x00, 0x80, 0x31, 0xd2,
              0x0f, 0x30, 0x0f, 0x32) },                                                 /* PerfCtr0 = 80000000h */
        { SEQ(0xb9, 0xc1, 0x00, 0x00, 0x00, 0xb8, 0x34, 0x12, 0x00, 0x00, 0xba, 0xab,
              0x00, 0x00, 0x00, 0x0f, 0x30, 0x0f, 0x32) },                               /* EDX ignored */
    };
    struct variant v = { .group = name, .faults_ok = 1, .fx = FX_CUSTOM, .defmask = dm_fxarg, .fx_arg = 1,
                         .before_run = msrw_before, .after_run = msrw_after };
    if (!is_vendor("GenuineIntel") || ((g_cpu.signature >> 8) & 0xf) != 6)
        return;
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        run_plain(&v, 1);
    }
    if (g_cpu.features & CPUID_PAT) {
        static const uint8_t pat[] = { 0xb9, 0x77, 0x02, 0x00, 0x00, 0xb8, 0x02, 0x00, 0x00, 0x00, 0x31, 0xd2,
                                       0x0f, 0x30 }; /* PAT entry 0 = 2: reserved */
        memcpy(v.target, pat, sizeof(pat));
        v.target_len = sizeof(pat);
        run_plain(&v, 1);
    }
}

/* WRMSR to the TSC with the upper half set, then RDTSC: the upper half
   reads back as written, or zero on models that write only 32 bits, and
   the lower half is a little past what was written. EAX becomes that
   comparison, EDX stays. */
static void
group_tsc_write(void)
{
    static const uint8_t seq[] = {
        0xb9, 0x10, 0x00, 0x00, 0x00, /* mov ecx, 10h */
        0xb8, 0x00, 0x10, 0x00, 0x00, /* mov eax, 1000h */
        0xba, 0x01, 0x00, 0x00, 0x00, /* mov edx, 1 */
        0x0f, 0x30,                   /* wrmsr */
        0x0f, 0x31,                   /* rdtsc */
        0x2d, 0x00, 0x10, 0x00, 0x00, /* sub eax, 1000h */
        0x3d, 0x00, 0x00, 0x00, 0x01, /* cmp eax, 1000000h */
        0x0f, 0x92, 0xc0,             /* setb al */
        0x0f, 0xb6, 0xc0,             /* movzx eax, al */
    };
    const char    *name = "msr.write";
    struct variant v    = { .group = name, .faults_ok = 1, .fx = FX_CUSTOM, .defmask = dm_arg3 };
    if (!(g_cpu.features & CPUID_MSR) || !(g_cpu.features & CPUID_TSC)) {
        group_skip(name, "no MSRs or TSC");
        return;
    }
    group_begin(name);
    memcpy(v.target, seq, sizeof(seq));
    v.target_len = sizeof(seq);
    run_plain(&v, 1);
    tsc_rdmsr(name);
    msr_write_p6(name);
    group_end(name);
}

/* The Cyrix device identification and configuration registers through
   ports 22h/23h: DIR0 DIR1 (the model and stepping), CCR0-CCR3. Only on a
   Cyrix: elsewhere those ports can belong to the chipset. */
static void
group_cyrix(void)
{
    static const uint8_t regs[] = { 0xfe, 0xff, 0xc0, 0xc1, 0xc2, 0xc3 };
    const char          *name   = "cyrix.dir";
    struct variant       v      = { .group = name, .faults_ok = 1, .fx = FX_CUSTOM, .defmask = dm_arg3 };
    if (!g_cpu.cyrix) {
        group_skip(name, "not a Cyrix");
        return;
    }
    group_begin(name);
    for (unsigned i = 0; i < sizeof(regs); i++) {
        static const uint8_t seq[] = {
            0xb0, 0x00, /* mov al, index */
            0xe6, 0x22, /* out 22h, al */
            0xe4, 0x23, /* in al, 23h */
            0x0f, 0xb6, 0xc0, /* movzx eax, al */
        };
        memcpy(v.target, seq, sizeof(seq));
        v.target[1]  = regs[i];
        v.target_len = sizeof(seq);
        run_plain(&v, 1);
    }
    group_end(name);
}

/* The BIOS memory map (INT 15h E820) the loader collected, as the machine
   reports it: the answer is the board's, not the CPU's, so there is no
   reference and no outcome stops the test; a missing or bad table only
   turns off what needs it (PSE-36 above 4 GB). EAX the status (1 read,
   2 no such call, 3 not "SMAP" back, 4 more than 64 entries, 5 an entry
   under 20 bytes, 6 corrupt: type 0 or wrapping past 2^64, 7 no usable
   RAM), ECX the entries, EDX the usable MB above 4 GB, ESI 1 when there
   is a page for PSE-36 there. The entries are printed too. */
static void
group_e820(void)
{
    const char    *name = "e820";
    struct variant v    = { .group = name, .no_ref = 1 };
#ifdef HOSTTEST
    group_skip(name, "BIOS (host build)");
    return;
#endif
    group_begin(name);
    if (!g_counting) {
        for (unsigned i = 0; i < g_e820_n; i++) {
            puts_("E820 ");
            puthex((uint32_t) (g_e820[i].base >> 32), 8);
            puthex((uint32_t) g_e820[i].base, 8);
            putch(' ');
            puthex((uint32_t) (g_e820[i].len >> 32), 8);
            puthex((uint32_t) g_e820[i].len, 8);
            putch(' ');
            putdec(g_e820[i].type);
            putch('\n');
        }
    }
    int n = 0;
    v.target[n++] = 0xb8; /* mov eax, status */
    put_le(v.target + n, g_e820_status, 4);
    n += 4;
    v.target[n++] = 0xb9; /* mov ecx, entries */
    put_le(v.target + n, g_e820_n, 4);
    n += 4;
    v.target[n++] = 0xba; /* mov edx, MB above 4 GB */
    put_le(v.target + n, g_e820_mb_high, 4);
    n += 4;
    v.target[n++] = 0xbe; /* mov esi, a PSE-36 page */
    put_le(v.target + n, g_hi_page != 0, 4);
    n += 4;
    v.target_len = n;
    run_plain(&v, 1);
    group_end(name);
}

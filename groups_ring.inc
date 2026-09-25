/*
 * Privilege levels: code at CPL 1, 2 and 3, and in virtual-8086 mode.
 * Each test enters its ring by IRETD with the IOPL (and AC) it needs, runs
 * the producer and the code under test there, and comes back to ring 0 by
 * INT 30h; the registers come back through the fault path, and a fault on
 * the way (a #GP from a privileged instruction, say) is the result
 * instead. These groups run with the ring GDT, IDT and TSS of
 * ring_env_on(); the others never see them. Ring 1 gets the most: it is
 * where CORSAC's sub-kernels run, with IOPL 1. The Linux build skips all
 * of it. Included into groups.inc after groups_paging.inc.
 */

#define RING_FMASK 0x001f3fd5u /* arithmetic flags, DF, IF, TF, IOPL, NT-less: RF VM AC VIF VIP */
#define CR0_AM     (1u << 18)
#define CR4_VME    (1u << 0)
#define CR4_TSD    (1u << 2)
#define CPUID_VME  (1u << 1)
#define EF_AC      0x40000u
#define IOPL(n)    ((uint32_t) (n) << 12)

/* Before each run: the TSS bitmaps as the variant wants them (arg3).
   1: port 80h denied in the I/O bitmap; 2: INT 60h redirected to the V86
   vector table (VME). */
static void
ring_setup(const struct variant *v)
{
#ifndef HOSTTEST
    if (v->arg & 0x100) /* a scratch descriptor config */
        scratch_set(v->arg);
    g_tss[TSS_IOMAP + 0x80 / 8] = (v->arg3 & 1) ? 0x01 : 0x00;
    if (v->arg3 & 2)
        g_tss[104 + 0x60 / 8] &= ~0x01;
    else
        g_tss[104 + 0x60 / 8] |= 0x01;
    if (v->paging)
        pg_setup(v);
#endif
}

/* Inputs a ring test may need (fix field). */
enum { RF_NONE, RF_MEM, RF_POPF, RF_PORT, RF_V86MEM, RF_V86FAR, RF_SANDBOX /* no input, but writes the sandbox */ };

static void
fix_ring(const struct variant *v)
{
    switch (v->arg2) {
        case RF_MEM: g_in.ebx = (uint32_t) g_buf + MEM_OFF; break;
        case RF_POPF: g_in.edi = rnd() & ~0x300u; break; /* no TF, no IF */
        case RF_PORT:
            g_in.edx = (g_in.edx & 0xffff0000u) | 0x80;
            g_in.esi = (uint32_t) g_buf;
            g_in.ecx = 1;
            break;
        case RF_V86MEM: /* BX a few bytes into the low buffer at 900:0 */
            g_in.ebx = (g_in.ebx & 0xffff0000u) | (rnd() & 7);
            g_in.eax = (g_in.eax & 0xffff0000u) | 0x0900;
            break;
        case RF_V86FAR: /* EDI past 64 KB */
            g_in.edi = 0x00010000u | (rnd() & 0xff);
            break;
        default: break;
    }
}

/* One sequence: its ring (4 = V86), IOPL, extra EFLAGS (AC), the TSS
   bitmap setup, the input it needs, whether its result is defined across
   models, and its bytes. */
struct ring_seq {
    uint8_t  ring, iopl, ac;
    uint16_t setup;
    uint8_t  fix, defined, len, b[24];
    uint16_t scratch; /* 0, or 0x100 | config | DPL << 9: the scratch descriptor to load */
    uint8_t  code16;  /* run as 16-bit code (rings 1-3) */
};

static int
ring_begin(const char *name, uint32_t cr4, uint32_t cr0)
{
#ifdef HOSTTEST
    group_skip(name, "rings (host build)");
    return 0;
#else
    group_begin(name);
    if (!g_counting)
        ring_env_on(cr4, cr0);
    return 1;
#endif
}

static void
ring_end(const char *name, uint32_t cr4, uint32_t cr0)
{
#ifndef HOSTTEST
    if (!g_counting)
        ring_env_off(cr4, cr0);
#endif
    group_end(name);
}

/* MOV AX, CS in V86 gives the segment the test's code happens to be in,
   which moves from pass to pass: made relative to that segment. */
static void
canon_v86_cs(const struct variant *v, const struct kin *in, struct kout *o)
{
    o->eax = (o->eax & 0xffff0000u) | ((o->eax - ((uint32_t) g_cur_low >> 4)) & 0xffff);
}

/* ring_run, then more(name): tests of the same group that don't fit a
   ring_seq, in the same ring environment. */
static void
ring_run_more(const char *name, const struct ring_seq *seqs, unsigned n, uint32_t cr4, uint32_t cr0,
              void (*emit)(struct emit *, const struct variant *), void (*more)(const char *name))
{
    struct variant v = { .group = name, .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
                         .fix_input = fix_ring, .reg_fix = 0, .fx = FX_CUSTOM, .defmask = dm_fxarg, .emit = emit };
    if (!ring_begin(name, cr4, cr0))
        return;
    for (unsigned i = 0; i < n; i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.ring       = seqs[i].ring;
        v.ring_flags = IOPL(seqs[i].iopl) | (seqs[i].ac ? EF_AC : 0);
        v.arg3       = seqs[i].setup;
        v.arg2       = seqs[i].fix;
        v.fx_arg     = seqs[i].defined;
        v.norm       = seqs[i].fix == RF_MEM ? 1 << R_EBX : seqs[i].fix == RF_PORT ? 1 << R_ESI : 0;
        v.canon      = seqs[i].ring == 4 && seqs[i].b[0] == 0x8c && seqs[i].b[1] == 0xc8 ? canon_v86_cs : 0;
        v.arg        = seqs[i].scratch;
        v.code16     = seqs[i].code16;
        run_plain(&v, 1);
    }
    if (more)
        more(name);
    ring_end(name, cr4, cr0);
}

static void
ring_run(const char *name, const struct ring_seq *seqs, unsigned n, uint32_t cr4, uint32_t cr0, void (*emit)(struct emit *, const struct variant *))
{
    ring_run_more(name, seqs, n, cr4, cr0, emit, 0);
}

/* A ring sequence with any EFLAGS on the way in and a 32-bit setup (arg3:
   ring_setup's bitmaps in bits 0-1, pg_setup's pages from 128 up, a
   group's own bits from 100000h). */
struct rx_seq {
    uint8_t  ring, iopl, defined, len, b[24];
    uint32_t flags;
    uint32_t setup;
};

static void
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


/* ---- what the rings share: the same code at CPL 1, 2 and 3 ---------------- */

/* Ordinary work, and what the code can see of where it runs. */
#define BASIC_SEQS(r)                                                              \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x66, 0x8c, 0xce) },       /* mov si, cs */      \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x66, 0x8c, 0xd7) },       /* mov di, ss */      \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x66, 0x8c, 0xdd) },       /* mov bp, ds */      \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x01, 0xd1, 0x11, 0xd0) }, /* add ecx, edx; adc eax, edx */ \
    { r, 0, 0, 0, RF_MEM, 1, SEQ(0x8b, 0x03, 0x89, 0x4b, 0x04) }, /* mov eax, [ebx]; mov [ebx+4], ecx */ \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x52, 0x51, 0x5e, 0x5f) }, /* push edx; push ecx; pop esi; pop edi */ \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x9c, 0x5e) },             /* pushfd; pop esi: IOPL as entered */ \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x01, 0xe0) },       /* smsw eax */        \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x00, 0xc8) },       /* str eax */         \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x00, 0xc0) },       /* sldt eax */        \
    { r, 0, 0, 0, RF_MEM, 0, SEQ(0x0f, 0x01, 0x03) },        /* sgdt [ebx] */      \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0xcd, 0x30) },             /* straight back */

/* Every privileged instruction: #GP(0) anywhere but CPL 0. The I/O-
   sensitive ones (CLI, STI) fault only when CPL > IOPL (IOPL 0 here). */
#define PRIV_SEQS(r)                                                                \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0xf4) },                   /* hlt */            \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0xfa) },                   /* cli */            \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0xfb) },                   /* sti */            \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x06) },             /* clts */           \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x08) },             /* invd */           \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x09) },             /* wbinvd */         \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x20, 0xc0) },       /* mov eax, cr0 */   \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x22, 0xc0) },       /* mov cr0, eax */   \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x20, 0xd8) },       /* mov eax, cr3 */   \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x21, 0xf8) },       /* mov eax, dr7 */   \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x23, 0xf8) },       /* mov dr7, eax */   \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x30) },             /* wrmsr */          \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x32) },             /* rdmsr */          \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x33) },             /* rdpmc (CR4.PCE clear) */ \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x01, 0x38) },       /* invlpg [eax] */   \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x01, 0xf0) },       /* lmsw ax */        \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x00, 0xd0) },       /* lldt ax */        \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x00, 0xd8) },       /* ltr ax */         \
    { r, 0, 0, 0, RF_MEM, 1, SEQ(0x0f, 0x01, 0x13) },        /* lgdt [ebx] */     \
    { r, 0, 0, 0, RF_MEM, 1, SEQ(0x0f, 0x01, 0x1b) },        /* lidt [ebx] */

/* The I/O permission checks: port 80h by OUT imm8, OUT DX and OUTSB, with
   the port allowed and denied in the TSS bitmap, at IOPL below and at
   the CPL. And CLI, which IOPL alone decides. */
#define IO_SEQS(r)                                                                  \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0xe6, 0x80) },             /* out 80h, al: bitmap allows */ \
    { r, 0, 0, 1, RF_NONE, 1, SEQ(0xe6, 0x80) },             /* bitmap denies: #GP */ \
    { r, r, 0, 1, RF_NONE, 1, SEQ(0xe6, 0x80) },             /* IOPL = CPL: the bitmap is not read */ \
    { r, 3, 0, 1, RF_NONE, 1, SEQ(0xe6, 0x80) },             /* IOPL 3 */         \
    { r, 0, 0, 0, RF_PORT, 1, SEQ(0xee) },                   /* out dx, al */     \
    { r, 0, 0, 1, RF_PORT, 1, SEQ(0xee) },                                        \
    { r, 0, 0, 0, RF_PORT, 1, SEQ(0x6e) },                   /* outsb */          \
    { r, 0, 0, 1, RF_PORT, 1, SEQ(0x6e) },                                        \
    { r, r, 0, 0, RF_NONE, 1, SEQ(0xfa) },                   /* cli at IOPL = CPL: allowed */ \
    { r, 3, 0, 0, RF_NONE, 1, SEQ(0xfa, 0x9c, 0x5e) },       /* cli at IOPL 3; pushfd; pop esi */

/* POPF below ring 0: IOPL never changes, IF only when CPL <= IOPL, AC
   always (the value never has IF or TF set). */
#define POPF_SEQS(r)                                                                \
    { r, 0, 0, 0, RF_POPF, 1, SEQ(0x57, 0x9d, 0x9c, 0x5e) }, /* push edi; popfd; pushfd; pop esi */ \
    { r, r, 0, 0, RF_POPF, 1, SEQ(0x57, 0x9d, 0x9c, 0x5e) },                      \
    { r, 3, 0, 0, RF_POPF, 1, SEQ(0x57, 0x9d, 0x9c, 0x5e) },                      \
    { r, 0, 0, 0, RF_POPF, 1, SEQ(0x66, 0x57, 0x66, 0x9d, 0x66, 0x9c, 0x66, 0x5e) }, /* the same, 16-bit */ \
    { r, 0, 0, 0, RF_POPF, 1, SEQ(0x57, 0x9d, 0x0f, 0x92, 0xc0) }, /* popfd; setb al: the flags it took */

/* The interrupt gates' DPL: INT 31h (DPL 0) and INT 32h (DPL 1), INT3
   and INT 3 (the exception gate, DPL 0), ICEBP (no DPL check), INTO. */
#define INT_SEQS(r)                                                                 \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0xcd, 0x31) },                                  \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0xcd, 0x32) },                                  \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0xcc) },                                        \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0xcd, 0x03) },                                  \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0xf1) },                                        \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x05, 0xff, 0xff, 0xff, 0x7f, 0xce) }, /* add eax, 7FFFFFFFh; into */

/* The alignment check: #AC(0) only at CPL 3 with CR0.AM and EFLAGS.AC. */
#define AC_SEQS(r)                                                                  \
    { r, 0, 1, 0, RF_MEM, 1, SEQ(0x8b, 0x43, 0x01) },        /* mov eax, [ebx+1] */ \
    { r, 0, 1, 0, RF_MEM, 1, SEQ(0x8b, 0x43, 0x02) },        /* mov eax, [ebx+2] */ \
    { r, 0, 1, 0, RF_MEM, 1, SEQ(0x66, 0x8b, 0x43, 0x01) },  /* mov ax, [ebx+1] */ \
    { r, 0, 1, 0, RF_MEM, 1, SEQ(0x66, 0x8b, 0x43, 0x02) },  /* mov ax, [ebx+2]: aligned */ \
    { r, 0, 1, 0, RF_MEM, 1, SEQ(0x8a, 0x43, 0x01) },        /* mov al, [ebx+1]: bytes never */ \
    { r, 0, 1, 0, RF_MEM, 1, SEQ(0x89, 0x4b, 0x03) },        /* mov [ebx+3], ecx */ \
    { r, 0, 1, 0, RF_MEM, 1, SEQ(0xff, 0x73, 0x01) },        /* push dword [ebx+1] */ \
    { r, 0, 1, 0, RF_NONE, 1, SEQ(0x83, 0xec, 0x02, 0x50) }, /* sub esp, 2; push eax: the stack */ \
    { r, 0, 0, 0, RF_MEM, 1, SEQ(0x8b, 0x43, 0x01) },        /* AC clear: no check */

/* ---- ring 3 --------------------------------------------------------------- */

static void
group_r3_basic(void)
{
    static const struct ring_seq seqs[] = { BASIC_SEQS(3) };
    ring_run("r3.basic", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

static void
group_r3_priv(void)
{
    static const struct ring_seq seqs[] = { PRIV_SEQS(3) };
    ring_run("r3.priv", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

static void
group_r3_io(void)
{
    static const struct ring_seq seqs[] = { IO_SEQS(3) };
    ring_run("r3.io", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

static void
group_r3_popf(void)
{
    static const struct ring_seq seqs[] = { POPF_SEQS(3) };
    ring_run("r3.popf", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

static void
group_r3_ac(void)
{
    static const struct ring_seq seqs[] = { AC_SEQS(3) };
    if (!g_cpu.is486) {
        group_skip("r3.ac", "386 (no alignment check)");
        return;
    }
    ring_run("r3.ac", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, CR0_AM, 0);
}

/* Segment loads at CPL 3: data needs DPL >= CPL and RPL, SS needs DPL = RPL
   = CPL, system descriptors never; and LAR/VERR of what ring 3 may and
   may not see. */
#define SEGLOAD(r, sel, sreg) { r, 0, 0, 0, RF_NONE, 1, SEQ(0x66, 0xb8, sel, 0x00, 0x8e, 0xc0 | ((sreg) << 3), 0x66, 0x8c, 0xc6 | ((sreg) << 3)) }

static void
group_r3_seg(void)
{
    static const struct ring_seq seqs[] = {
        SEGLOAD(3, 0x10, 3), SEGLOAD(3, 0x53, 3), SEGLOAD(3, 0x31, 3), SEGLOAD(3, 0x4b, 3), SEGLOAD(3, 0x48, 3),
        SEGLOAD(3, 0x00, 4), SEGLOAD(3, 0x58, 3), SEGLOAD(3, 0x68, 3),
        SEGLOAD(3, 0x31, 2), SEGLOAD(3, 0x50, 2), SEGLOAD(3, 0x53, 2), SEGLOAD(3, 0x4b, 2),
        { 3, 0, 0, 0, RF_NONE, 1, SEQ(0x66, 0xb9, 0x10, 0x00, 0x0f, 0x02, 0xc1, 0x0f, 0x94, 0xc3) }, /* lar eax, 10h; setz bl */
        { 3, 0, 0, 0, RF_NONE, 1, SEQ(0x66, 0xb9, 0x53, 0x00, 0x0f, 0x02, 0xc1, 0x0f, 0x94, 0xc3) },
        { 3, 0, 0, 0, RF_NONE, 1, SEQ(0x66, 0xb9, 0x4b, 0x00, 0x0f, 0x00, 0xe1, 0x0f, 0x94, 0xc3) }, /* verr 4Bh */
        { 3, 0, 0, 0, RF_NONE, 1, SEQ(0x66, 0xb9, 0x4b, 0x00, 0x0f, 0x00, 0xe9, 0x0f, 0x94, 0xc3) }, /* verw 4Bh: code, no */
    };
    ring_run("r3.seg", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

/* Far transfers from a ring: call gates of each DPL, a gate with
   parameters, a direct call to more privileged code (#GP), a conforming
   segment (runs at the caller's CPL), a JMP through a call gate. The
   gate's targets report CS (and in ring 1, ESP and SS). */
static void
far_seqs(struct ring_seq *s, int *n, int r)
{
    static const uint16_t gates[] = { SEL_GATE3, SEL_GATE1, SEL_GATE0, SEL_GATE31, SEL_GATE21 };
    for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
        struct ring_seq q = { r, 0, 0, 0, RF_NONE, 1, 7, { 0x9a, 0, 0, 0, 0, gates[i], 0 } };
        s[(*n)++]         = q;
    }
    {
        struct ring_seq q = { r, 0, 0, 0, RF_NONE, 1, 17,
                              { 0x68, 0x11, 0x11, 0x11, 0x11, 0x68, 0x22, 0x22, 0x22, 0x22, 0x9a, 0, 0, 0, 0, SEL_GATE3P, 0 } };
        s[(*n)++] = q;
    }
    for (int k = 0; k < 3; k++) { /* call 08:x (#GP), call conforming 60:x, jmp through a gate */
        static const uint16_t sels[3] = { 0x08, SEL_CONF0, SEL_GATE3 };
        struct ring_seq       q       = { r, 0, 0, 0, RF_NONE, 1, 7, { k == 2 ? 0xea : 0x9a } };
        put_le(q.b + 1, (uint32_t) g_gate_ret, 4);
        q.b[5]    = sels[k];
        q.b[6]    = 0;
        s[(*n)++] = q;
    }
}

static void
group_r3_gates(void)
{
    static struct ring_seq seqs[12];
    int                    n = 0;
    far_seqs(seqs, &n, 3);
    ring_run("r3.gates", seqs, n, 0, 0, 0);
}

static void
group_r3_int(void)
{
    static const struct ring_seq seqs[] = { INT_SEQS(3) };
    ring_run("r3.int", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

/* RDPMC of counters 0, 1 and 2 (#GP: there are two) at CPL 0, and outside
   it with CR4.PCE (setup 100000h) and without. The counts are cleared
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
tsd_rdpmc(const char *name)
{
    static const struct rx_seq seqs[] = {
        PMC(0, 0, 0), PMC(0, 1, 0), PMC(0, 2, 0), PMC(3, 0, 1),
        PMC(3, 1, 1), PMC(3, 2, 1), PMC(1, 0, 0), PMC(1, 0, 1), PMC(2, 1, 1),
    };
    struct variant v      = { .group = name, .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = pce_before,
                              .after_run = pce_after, .fx = FX_CUSTOM, .defmask = dm_fxarg };
    unsigned       family = (g_cpu.signature >> 8) & 0xf;
    if (!is_vendor("GenuineIntel") || !(family == 6 || (family == 5 && (g_cpu.features & CPUID_MMX)))) {
        return;
    }
    rx_run(&v, seqs, sizeof(seqs) / sizeof(seqs[0]));
}

/* CR4.TSD: RDTSC is privileged; RDPMC too without CR4.PCE. */
static void
group_r3_tsd(void)
{
    static const struct ring_seq seqs[] = {
        { 3, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x31) },
        { 3, 3, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x31) }, /* IOPL doesn't matter */
        { 1, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x31) }, /* nor does ring 1 */
        { 2, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x31) },
        { 3, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x33) },
    };
    if (!(g_cpu.features & CPUID_TSC)) {
        group_skip("r3.tsd", "no TSC");
        return;
    }
    ring_run_more("r3.tsd", seqs, sizeof(seqs) / sizeof(seqs[0]), CR4_TSD, 0, 0, tsd_rdpmc);
}

/* Paging from rings: every page user-accessible except the buffer's second
   page (supervisor), which ring 3 can't touch (#PF, U/S in the error code)
   and ring 1 can; a read-only page, which ring 3 can never write and ring
   1 can write only with CR0.WP clear. */
static void
ring_pf_run(const char *name, const struct ring_seq *seqs, unsigned n, uint32_t cr0)
{
    struct variant v = { .group = name, .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
                         .fix_input = fix_ring, .fx = FX_CUSTOM, .defmask = dm_fxarg, .paging = 1 };
#ifndef HOSTTEST
    v.after_run = pg_reset;
#endif
    if (!ring_begin(name, 0, 0))
        return;
#ifndef HOSTTEST
    if (!g_counting) {
        g_pg_user = PTE_U;
        pg_on(cr0);
    }
#endif
    for (unsigned i = 0; i < n; i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.ring       = seqs[i].ring;
        v.ring_flags = IOPL(seqs[i].iopl);
        v.arg3       = seqs[i].setup;
        v.arg2       = seqs[i].fix;
        v.fx_arg     = seqs[i].defined;
        v.norm       = 1 << R_EBX;
        run_plain(&v, 1);
    }
#ifndef HOSTTEST
    if (!g_counting) {
        pg_off();
        g_pg_user = 0;
    }
#endif
    ring_end(name, 0, 0);
}

#define PF_SEQS(r)                                                                  \
    { r, 0, 0, 64, RF_MEM, 1, SEQ(0x8b, 0x43, 0x18) },       /* read the supervisor page */ \
    { r, 0, 0, 64, RF_MEM, 1, SEQ(0x89, 0x4b, 0x18) },       /* write it */        \
    { r, 0, 0, 64, RF_MEM, 1, SEQ(0x8b, 0x43, 0x10) },       /* the user page next to it */ \
    { r, 0, 0, 64, RF_MEM, 1, SEQ(0x8b, 0x43, 0x16) },       /* a dword across both */ \
    { r, 0, 0, 4, RF_MEM, 1, SEQ(0x89, 0x4b, 0x18) },        /* write a read-only page */ \
    { r, 0, 0, 4, RF_MEM, 1, SEQ(0x8b, 0x43, 0x18) },        /* read it */         \
    { r, 0, 0, 1, RF_MEM, 1, SEQ(0x8b, 0x43, 0x18) },        /* a missing page */ \
    { r, 0, 0, 128, RF_SANDBOX, 1, SEQ(RD_ALIAS) },          /* the alias page */ \
    { r, 0, 0, 128, RF_SANDBOX, 1, SEQ(WR_ALIAS) },                               \
    { r, 0, 0, 128 | 1024, RF_SANDBOX, 1, SEQ(RD_ALIAS) },   /* its PDE supervisor-only */ \
    { r, 0, 0, 128 | 2048, RF_SANDBOX, 1, SEQ(RD_ALIAS) },   /* its PTE supervisor-only */ \
    { r, 0, 0, 128 | 256, RF_SANDBOX, 1, SEQ(WR_ALIAS) },    /* its PDE read-only */ \
    { r, 0, 0, 128 | 512, RF_SANDBOX, 1, SEQ(RD_ALIAS) },    /* its PDE missing */

static void
group_r3_pf(void)
{
    static const struct ring_seq seqs[] = { PF_SEQS(3) };
    ring_pf_run("r3.pf", seqs, sizeof(seqs) / sizeof(seqs[0]), 0);
}

/* ---- ring 1: where CORSAC's sub-kernels run ------------------------------- */

static void
group_r1_basic(void)
{
    static const struct ring_seq seqs[] = { BASIC_SEQS(1) };
    ring_run("r1.basic", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

static void
group_r1_priv(void)
{
    static const struct ring_seq seqs[] = { PRIV_SEQS(1) };
    ring_run("r1.priv", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

/* Ring 1 with IOPL 1, as CORSAC runs it: every port without the bitmap,
   CLI, and still no privileged instruction. */
static void
group_r1_iopl1(void)
{
    static const struct ring_seq seqs[] = {
        { 1, 1, 0, 1, RF_NONE, 1, SEQ(0xe6, 0x80) },       /* out 80h with the bitmap denying */
        { 1, 1, 0, 1, RF_PORT, 1, SEQ(0xee) },             /* out dx, al */
        { 1, 1, 0, 1, RF_PORT, 1, SEQ(0x6e) },             /* outsb */
        { 1, 1, 0, 0, RF_NONE, 1, SEQ(0xfa, 0x9c, 0x5e) }, /* cli; pushfd; pop esi */
        { 1, 1, 0, 0, RF_POPF, 1, SEQ(0x57, 0x9d, 0x9c, 0x5e) }, /* popfd: IF may change (never set), IOPL not */
        { 1, 1, 0, 0, RF_NONE, 1, SEQ(0xf4) },             /* hlt: #GP still */
        { 1, 1, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x20, 0xc0) }, /* mov eax, cr0: #GP still */
        { 1, 1, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x32) },       /* rdmsr: #GP still */
        { 1, 1, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x06) },       /* clts: #GP still */
        { 1, 1, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x09) },       /* wbinvd: #GP still */
        { 1, 1, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x01, 0x38) }, /* invlpg: #GP still */
        { 1, 2, 0, 1, RF_NONE, 1, SEQ(0xe6, 0x80) },       /* IOPL 2 */
        { 1, 0, 0, 1, RF_NONE, 1, SEQ(0xe6, 0x80) },       /* IOPL 0: the bitmap decides */
        { 1, 0, 0, 0, RF_NONE, 1, SEQ(0xe6, 0x80) },
    };
    ring_run("r1.iopl1", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

static void
group_r1_io(void)
{
    static const struct ring_seq seqs[] = { IO_SEQS(1) };
    ring_run("r1.io", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

static void
group_r1_popf(void)
{
    static const struct ring_seq seqs[] = { POPF_SEQS(1) };
    ring_run("r1.popf", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

/* Segment loads at CPL 1: data of DPL 1-3 (not 0), SS only of DPL 1, a
   ring-1 code segment as data, the RPL raising the check. */
static void
group_r1_seg(void)
{
    static const struct ring_seq seqs[] = {
        SEGLOAD(1, 0x10, 3), SEGLOAD(1, 0x31, 3), SEGLOAD(1, 0x42, 3), SEGLOAD(1, 0x53, 3), SEGLOAD(1, 0x29, 3),
        SEGLOAD(1, 0x33, 3), SEGLOAD(1, 0x30, 3), SEGLOAD(1, 0x58, 3), SEGLOAD(1, 0x00, 5),
        SEGLOAD(1, 0x31, 2), SEGLOAD(1, 0x30, 2), SEGLOAD(1, 0x42, 2), SEGLOAD(1, 0x53, 2), SEGLOAD(1, 0x10, 2),
        { 1, 0, 0, 0, RF_NONE, 1, SEQ(0xea, 0, 0, 0, 0, 0x4b, 0x00) }, /* jmp to ring-3 code: #GP */
    };
    ring_run("r1.seg", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

static void
group_r1_gates(void)
{
    static struct ring_seq seqs[12];
    int                    n = 0;
    far_seqs(seqs, &n, 1);
    ring_run("r1.gates", seqs, n, 0, 0, 0);
}

static void
group_r1_int(void)
{
    static const struct ring_seq seqs[] = { INT_SEQS(1) };
    ring_run("r1.int", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

/* The alignment check never applies at CPL 1, whatever AM and AC say. */
static void
group_r1_ac(void)
{
    static const struct ring_seq seqs[] = { AC_SEQS(1) };
    if (!g_cpu.is486) {
        group_skip("r1.ac", "386 (no alignment check)");
        return;
    }
    ring_run("r1.ac", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, CR0_AM, 0);
}

/* Paging at CPL 1 is supervisor: the supervisor page is fine, and a
   read-only page is writable unless CR0.WP. */
static void
group_r1_pf(void)
{
    static const struct ring_seq seqs[] = { PF_SEQS(1) };
    ring_pf_run("r1.pf", seqs, sizeof(seqs) / sizeof(seqs[0]), 0);
}

static void
group_r1_pf_wp(void)
{
    static const struct ring_seq seqs[] = { PF_SEQS(1) };
    if (!g_cpu.is486) {
        group_skip("r1.pf.wp", "386 (no CR0.WP)");
        return;
    }
    ring_pf_run("r1.pf.wp", seqs, sizeof(seqs) / sizeof(seqs[0]), CR0_WP);
}

/* Leaving ring 1 for an outer ring by IRETD and RETF: data segments the
   new ring may not use (DPL 1 here) are loaded null, so the first access
   through DS faults. The target reports DS and ES, then reads [ebx]. The
   last one IRETs within ring 1 (no stack change, DS kept). */
static void
emit_outer(struct emit *e, const struct variant *v)
{
    int      to = v->arg3 >> 4, retf = v->arg3 & 8;
    uint8_t  cs = to == 3 ? SEL_CODE3 | 3 : to == 2 ? SEL_CODE2 | 2 : SEL_CODE1 | 1;
    uint8_t  ss = to == 3 ? SEL_DATA3 | 3 : to == 2 ? SEL_DATA2 | 2 : SEL_DATA1 | 1;
    uint8_t *push_l;
    if (to != 1) {
        e8(e, 0x6a); /* push ss */
        e8(e, ss);
        e8(e, 0x68); /* push esp */
        e32(e, (uint32_t) g_rstack[to] + sizeof(g_rstack[0]));
    }
    if (!retf)
        e8(e, 0x9c); /* pushfd */
    e8(e, 0x6a);     /* push cs */
    e8(e, cs);
    e8(e, 0x68);     /* push L */
    push_l = e->p;
    e32(e, 0);
    e8(e, retf ? 0xcb : 0xcf); /* retf / iretd */
    put32_at(push_l, (uint32_t) e->p);
    static const uint8_t after[] = { 0x66, 0x8c, 0xde,  /* mov si, ds */
                                     0x66, 0x8c, 0xc7,  /* mov di, es */
                                     0x8b, 0x0b };      /* mov ecx, [ebx] */
    ebytes(e, after, sizeof(after));
}

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
outer_frames(const char *name)
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
    struct variant v = { .group = name, .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
                         .emit = emit_iretfr, .fx = FX_CUSTOM, .defmask = dm_fxarg, .fx_arg = 1 };
    for (unsigned i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
        v.target[0]  = 0xcf; /* for the reports */
        put_le(v.target + 1, frames[i][0], 2);
        put_le(v.target + 3, frames[i][1], 2);
        v.target_len = 5;
        v.arg3       = (uint32_t) frames[i][1] << 16 | frames[i][0];
        run_plain(&v, 1);
    }
}

static void
group_r1_outer(void)
{
    static const uint8_t cases[] = { 3 << 4, 2 << 4, (3 << 4) | 8, (2 << 4) | 8, 1 << 4 };
    const char          *name    = "r1.outer";
    struct variant       v       = { .group = name, .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
                                     .fix_input = fix_ring, .arg2 = RF_MEM, .fx = FX_CUSTOM, .defmask = dm_fxarg,
                                     .fx_arg = 1, .emit = emit_outer, .ring = 1, .norm = 1 << R_EBX };
    if (!ring_begin(name, 0, 0))
        return;
    for (unsigned i = 0; i < sizeof(cases); i++) {
        v.target[0]  = cases[i] & 8 ? 0xcb : 0xcf; /* for the reports */
        v.target[1]  = cases[i] >> 4;
        v.target_len = 2;
        v.arg3       = cases[i];
        run_plain(&v, 1);
    }
    outer_frames(name);
    ring_end(name, 0, 0);
}

/* The stack switch into ring 1: through a DPL-3 gate from ring 3 and a
   DPL-2 gate from ring 2 the target runs on SS1:ESP1 from the TSS (ESP
   and SS reported); from ring 1 itself it stays on its own stack. */
static void
group_r1_stack(void)
{
    static const struct ring_seq seqs[] = {
        { 3, 0, 0, 0, RF_NONE, 1, SEQ(0x9a, 0, 0, 0, 0, SEL_GATE31, 0x00) },
        { 2, 0, 0, 0, RF_NONE, 1, SEQ(0x9a, 0, 0, 0, 0, SEL_GATE21, 0x00) },
        { 2, 0, 0, 0, RF_NONE, 1, SEQ(0x9a, 0, 0, 0, 0, SEL_GATE31, 0x00) },
        { 1, 0, 0, 0, RF_NONE, 1, SEQ(0x9a, 0, 0, 0, 0, SEL_GATE31, 0x00) },
        { 1, 0, 0, 0, RF_NONE, 1, SEQ(0x9a, 0, 0, 0, 0, SEL_GATE21, 0x00) },
        { 3, 0, 0, 0, RF_NONE, 1, SEQ(0x9a, 0, 0, 0, 0, SEL_GATE21, 0x00) }, /* DPL 2 from ring 3: #GP */
    };
    ring_run("r1.stack", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

/* A raw scratch descriptor (ring_setup, arg bit 800h): its access byte and
   flags as given. */
#define SCRATCH_RAW(access, flags) (0x900 | ((uint32_t) (access) << 12) | ((uint32_t) (flags) << 20))

/* The same checks at CPL 0 on the scratch descriptor as each of the 16
   system types (S = 0): which ones LAR and LSL accept. */
static void
lar_systypes(const char *name)
{
    static const uint8_t code[] = {
        0x66, 0xb9, SEL_SCRATCH, 0x00, /* mov cx, sel */
        0x0f, 0x02, 0xc1,              /* lar eax, ecx */
        0x0f, 0x94, 0xc3,              /* setz bl */
        0x0f, 0x03, 0xd1,              /* lsl edx, ecx */
        0x0f, 0x94, 0xc7,              /* setz bh */
        0x0f, 0x00, 0xe1,              /* verr cx */
        0x0f, 0x94, 0xc5,              /* setz ch */
    };
    struct variant v = { .group = name, .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
                         .fix_input = fix_ring, .arg2 = RF_NONE, .fx = FX_CUSTOM, .defmask = dm_fxarg };
    memcpy(v.target, code, sizeof(code));
    v.target_len = sizeof(code);
    for (int t = 0; t < 16; t++) {
        v.arg = SCRATCH_RAW(0x80 | t, 0x4);
        run_plain(&v, 1);
    }
}

/* LAR, LSL and VERR at CPL 1 of every descriptor in the ring GDT, with RPL
   0 and 3: which ones ring 1 can see, their access rights and limits.
   ZF lands in BL after LAR, BH after LSL and CH after VERR. LAR's result
   has bits the manuals leave undefined, so raw only. */
static void
group_r1_lar(void)
{
    static struct ring_seq seqs[RING_GDT_N * 2];
    static const uint8_t   body[] = {
        0x66, 0xb9, 0x00, 0x00, /* mov cx, sel */
        0x0f, 0x02, 0xc1,       /* lar eax, ecx */
        0x0f, 0x94, 0xc3,       /* setz bl */
        0x0f, 0x03, 0xd1,       /* lsl edx, ecx */
        0x0f, 0x94, 0xc7,       /* setz bh */
        0x0f, 0x00, 0xe1,       /* verr cx */
        0x0f, 0x94, 0xc5,       /* setz ch */
    };
    int n = 0;
    for (int d = 0; d < RING_GDT_N; d++) {
        for (int rpl = 0; rpl < 4; rpl += 3) {
            struct ring_seq q = { 1, 0, 0, 0, RF_NONE, 0, sizeof(body), { 0 } };
            memcpy(q.b, body, sizeof(body));
            q.b[2]    = (d << 3) | rpl;
            seqs[n++] = q;
        }
    }
    ring_run_more("r1.lar", seqs, n, 0, 0, 0, lar_systypes);
}

/* ---- ring 2 --------------------------------------------------------------- */

static void
group_r2_basic(void)
{
    static const struct ring_seq seqs[] = { BASIC_SEQS(2) IO_SEQS(2) };
    ring_run("r2.basic", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

static void
group_r2_gates(void)
{
    static struct ring_seq seqs[12];
    int                    n = 0;
    far_seqs(seqs, &n, 2);
    ring_run("r2.gates", seqs, n, 0, 0, 0);
}

/* ---- virtual-8086 mode ------------------------------------------------------ */

/* Real-mode work in V86 at IOPL 3: 16-bit arithmetic, segment arithmetic
   (DS = 900h reaches the low buffer), the stack, PUSHF, CLI; and what
   stays privileged. */
static void
group_v86_basic(void)
{
    static const struct ring_seq seqs[] = {
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0x01, 0xd1) },                         /* add cx, dx */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0x66, 0x01, 0xd1) },                   /* add ecx, edx */
        { 4, 3, 0, 0, RF_V86MEM, 1, SEQ(0x8e, 0xd8, 0x8b, 0x07, 0x8b, 0x4f, 0x02) }, /* mov ds, ax; mov ax, [bx]; mov cx, [bx+2] */
        { 4, 3, 0, 0, RF_V86MEM, 1, SEQ(0x8e, 0xc0, 0x26, 0x89, 0x17) },     /* mov es, ax; mov es:[bx], dx */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0x52, 0x51, 0x5e, 0x5f) },             /* push dx; push cx; pop si; pop di */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0x9c, 0x5e) },                         /* pushf; pop si */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0x66, 0x9c, 0x66, 0x5e) },             /* pushfd; pop esi */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0xfa, 0x9c, 0x5e) },                   /* cli */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0x8c, 0xc8) },                         /* mov ax, cs: relative, see canon_v86_cs */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0xcd, 0x32) },                         /* INT through a DPL-1 gate: #GP */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0xf4) },                               /* hlt: #GP */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x20, 0xc0) },                   /* mov eax, cr0: #GP */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x01, 0xe0) },                   /* smsw ax: allowed */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0xe6, 0x80) },                         /* out 80h, al */
    };
    ring_run("v86.basic", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

/* IOPL 0 in V86 (no VME): CLI STI PUSHF POPF INT IRET all #GP; ports
   still go by the bitmap alone. */
static void
group_v86_iopl0(void)
{
    static const struct ring_seq seqs[] = {
        { 4, 0, 0, 0, RF_NONE, 1, SEQ(0xfa) },
        { 4, 0, 0, 0, RF_NONE, 1, SEQ(0xfb) },
        { 4, 0, 0, 0, RF_NONE, 1, SEQ(0x9c) },
        { 4, 0, 0, 0, RF_NONE, 1, SEQ(0x6a, 0x00, 0x9d) }, /* push 0; popf */
        { 4, 0, 0, 0, RF_NONE, 1, SEQ(0xcd, 0x60) },
        { 4, 0, 0, 0, RF_NONE, 1, SEQ(0xcf) },
        { 4, 0, 0, 0, RF_NONE, 1, SEQ(0xe6, 0x80) },
        { 4, 0, 0, 1, RF_NONE, 1, SEQ(0xe6, 0x80) },
        { 4, 0, 0, 0, RF_NONE, 1, SEQ(0x01, 0xd1) }, /* then INT 30h itself: #GP, still a way back */
    };
    ring_run("v86.iopl0", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

/* The I/O bitmap in V86 applies even at IOPL 3. */
static void
group_v86_io(void)
{
    static const struct ring_seq seqs[] = {
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0xe6, 0x80) }, { 4, 3, 0, 1, RF_NONE, 1, SEQ(0xe6, 0x80) },
        { 4, 3, 0, 0, RF_PORT, 1, SEQ(0xee) },       { 4, 3, 0, 1, RF_PORT, 1, SEQ(0xee) },
        { 4, 0, 0, 1, RF_PORT, 1, SEQ(0xee) },
    };
    ring_run("v86.io", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

/* What V86 mode does not have: the protected-mode-only instructions #UD;
   an address past 64 KB #GP. */
static void
group_v86_ud(void)
{
    static const struct ring_seq seqs[] = {
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0x63, 0xc1) },       /* arpl */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x02, 0xc1) }, /* lar */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x03, 0xc1) }, /* lsl */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x00, 0xe1) }, /* verr */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x00, 0xc0) }, /* sldt */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x00, 0xc8) }, /* str */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x00, 0xd0) }, /* lldt */
        { 4, 3, 0, 0, RF_V86FAR, 1, SEQ(0x67, 0x8b, 0x07) },       /* mov ax, [edi] past 64 KB: #GP */
        { 4, 3, 0, 0, RF_NONE, 1, SEQ(0x67, 0x66, 0x8d, 0x04, 0x8b) }, /* lea eax, [ebx+ecx*4]: no access, fine */
    };
    ring_run("v86.ud", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

/* VME (CR4.VME) at IOPL 0: CLI and STI change VIF instead of faulting,
   PUSHF/POPF show and take VIF in IF's place, INT 60h goes through the
   V86 vector table when the redirection bitmap says so (a handler at
   0:7F80 that increments DI) and faults when it doesn't. */
#define V86_HANDLER 0x7f80u
/* INT 60h's entry in the real-mode vector table. Through a variable, so
   the compiler doesn't take the low address for a null pointer. */
static uint32_t *volatile g_ivt60 = (uint32_t *) (0x60 * 4);

/* CR4.PVI, protected-mode virtual interrupts: at CPL 3 with IOPL < 3, CLI and STI
   change VIF instead of faulting, and STI with VIP set is #GP; POPFD
   changes neither IF nor VIF; at IOPL 3 CLI is the real one; outside
   ring 3 nothing changes (#GP). */
static void
vme_pvi(const char *name)
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
    struct variant v = { .group = name, .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
                         .fx = FX_CUSTOM, .defmask = dm_fxarg };
    rx_run(&v, seqs, sizeof(seqs) / sizeof(seqs[0]));
}

static void
group_v86_vme(void)
{
    static const struct ring_seq seqs[] = {
        { 4, 0, 0, 0, RF_NONE, 1, SEQ(0xfa) },                         /* cli: VIF clear */
        { 4, 0, 0, 0, RF_NONE, 1, SEQ(0xfb) },                         /* sti: VIF set */
        { 4, 0, 0, 0, RF_NONE, 0, SEQ(0xfb, 0x9c, 0x5e) },             /* sti; pushf; pop si */
        { 4, 0, 0, 0, RF_NONE, 0, SEQ(0x68, 0x00, 0x02, 0x9d) },       /* push 200h; popf */
        { 4, 0, 0, 0, RF_NONE, 1, SEQ(0x66, 0x9c) },                   /* pushfd: #GP still */
        { 4, 0, 0, 2, RF_NONE, 1, SEQ(0xcd, 0x60) },                   /* INT 60h redirected */
        { 4, 0, 0, 0, RF_NONE, 1, SEQ(0xcd, 0x60) },                   /* not redirected: #GP */
        { 4, 3, 0, 2, RF_NONE, 1, SEQ(0xcd, 0x60) },                   /* IOPL 3 redirected */
    };
#ifndef HOSTTEST
    uint32_t saved = 0;
#endif
    if (!(g_cpu.features & CPUID_VME)) {
        group_skip("v86.vme", "no VME");
        return;
    }
#ifndef HOSTTEST
    if (!g_counting) {
        uint8_t *h = (uint8_t *) V86_HANDLER;
        h[0]       = 0x47; /* inc di */
        h[1]       = 0xcf; /* iret */
        saved      = *g_ivt60;
        *g_ivt60   = V86_HANDLER; /* 0000:7F80 */
    }
#endif
    ring_run_more("v86.vme", seqs, sizeof(seqs) / sizeof(seqs[0]), CR4_VME | CR4_PVI, 0, 0, vme_pvi);
#ifndef HOSTTEST
    if (!g_counting)
        *g_ivt60 = saved;
#endif
}

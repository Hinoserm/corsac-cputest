/*
 * More privilege-level groups: segment limits and types, stack faults,
 * conforming code, interrupt gates into rings 1 and 2, returns to inner
 * rings, 16-bit code in rings 1 and 3, POP of segment registers, and call
 * gates that copy parameters. Included into groups.inc after
 * groups_ring.inc.
 */

/* The scratch descriptor for a ring: config, and DPL the ring's own. */
#define SCRATCH(cfg, ring) (0x100 | (cfg) | ((ring) << 9))

static const uint8_t ring_data_sel[4] = { 0x10, SEL_DATA1 | 1, SEL_DATA2 | 2, SEL_DATA3 | 3 };
static const uint8_t ring_code_sel[4] = { 0x08, SEL_CODE1 | 1, SEL_CODE2 | 2, SEL_CODE3 | 3 };

/* Each of the 16 code and data types (S = 1), at DPL 0 from ring 0 and
   DPL 3 from ring 3: VERR and VERW, and a load into FS with a read and a
   write through it. */
static void
seg_types(const char *name)
{
    struct variant v = { .group = name, .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
                         .fix_input = fix_ring, .fx = FX_CUSTOM, .defmask = dm_fxarg, .fx_arg = 1 };
    static const uint8_t checks[] = {
        0x66, 0xb9, 0x00, 0x00, /* mov cx, sel */
        0x0f, 0x00, 0xe1,       /* verr cx */
        0x0f, 0x94, 0xc3,       /* setz bl */
        0x0f, 0x00, 0xe9,       /* verw cx */
        0x0f, 0x94, 0xc7,       /* setz bh */
    };
    static const uint8_t use[] = {
        0x66, 0xb8, 0x00, 0x00, /* mov ax, sel; mov fs, ax */
        0x8e, 0xe0,
        0x64, 0x8b, 0x35, 0x10, 0x00, 0x00, 0x00, /* mov esi, fs:[10h] */
        0x64, 0x89, 0x0d, 0x20, 0x00, 0x00, 0x00, /* mov fs:[20h], ecx */
    };
    for (int r = 0; r <= 3; r += 3) {
        for (int t = 0; t < 16; t++) {
            for (int k = 0; k < 2; k++) {
                const uint8_t *b = k ? use : checks;
                int            n = k ? (int) sizeof(use) : (int) sizeof(checks);
                memcpy(v.target, b, n);
                v.target[2]  = SEL_SCRATCH | r;
                v.target_len = n;
                v.ring       = r;
                v.arg        = SCRATCH_RAW(0x90 | (r << 5) | t, 0x4);
                v.arg2       = RF_SANDBOX;
                run_plain(&v, 1);
            }
        }
    }
}

/* Loads and stores through FS on every kind of scratch descriptor, at
   offsets either side of its limit: 0, the last dword, a dword across the
   limit, the first byte past it, and one well inside an expand-down
   segment's range; and a byte at the limit. The descriptor lies 4 KB into
   the sandbox, so every access that works lands in it. */
static void
limits_run(const char *name, int ring, void (*more)(const char *name))
{
    static struct ring_seq seqs[N_SCRATCH * 11];
    static const uint32_t  offs[5] = { 0x0000, 0x0ffc, 0x0ffe, 0x1000, 0x2ffc };
    int                    n       = 0;
    for (unsigned c = 0; c < N_SCRATCH; c++) {
        for (int k = 0; k < 11; k++) {
            struct ring_seq q = { ring, 0, 0, 0, RF_SANDBOX, 1, 0, { 0 } };
            int             m = 0;
            q.b[m++]          = 0x66; /* mov ax, scratch; mov fs, ax */
            q.b[m++]          = 0xb8;
            q.b[m++]          = SEL_SCRATCH | ring;
            q.b[m++]          = 0x00;
            q.b[m++]          = 0x8e;
            q.b[m++]          = 0xe0;
            q.b[m++]          = 0xbb; /* mov ebx, offset */
            put_le(q.b + m, k < 10 ? offs[k >> 1] : 0x0fff, 4);
            m += 4;
            q.b[m++] = 0x64;
            if (k == 10) { /* mov al, fs:[ebx] */
                q.b[m++] = 0x8a;
                q.b[m++] = 0x03;
            } else if (k & 1) { /* mov fs:[ebx], ecx */
                q.b[m++] = 0x89;
                q.b[m++] = 0x0b;
            } else { /* mov eax, fs:[ebx] */
                q.b[m++] = 0x8b;
                q.b[m++] = 0x03;
            }
            q.len     = m;
            q.scratch = SCRATCH(c, ring);
            seqs[n++] = q;
        }
    }
    ring_run_more(name, seqs, n, 0, 0, 0, more);
}

static void
group_r3_limits(void)
{
    limits_run("r3.limits", 3, seg_types);
}

static void
group_r2_limits(void)
{
    limits_run("r2.limits", 2, 0);
}

/* Operands that straddle a segment limit must #GP whatever the
   instruction does with them; 86Box checked only some of them. FS the
   scratch descriptor as byte-granular read/write data with limit FFFh
   (DPL 1: these run at CPL 1), each operand from FFAh-FFEh on:
   rotates and shifts by 0 (count 0 still checks), by CL=0 and by 1; BT
   with an immediate, BT and BTS with a register bit offset past the
   limit; MOVQ in and out, MOVD, PADDB (MMX); CMPXCHG8B (CX8); and a MOV
   just inside the limit. */
static void
limits_ops(const char *name)
{
    static const struct {
        uint8_t need, len, b[20];
    } seqs[] = {
        { 0, SEQ(0xbb, 0xfc, 0x0f, 0x00, 0x00, 0x64, 0x8b, 0x03) },                               /* mov eax, fs:[ffc]: inside */
        { 0, SEQ(0xbb, 0xfd, 0x0f, 0x00, 0x00, 0x64, 0xc1, 0x03, 0x00) },                         /* rol dword fs:[ffd], 0 */
        { 0, SEQ(0xbb, 0xfd, 0x0f, 0x00, 0x00, 0x31, 0xc9, 0x64, 0xd3, 0x23) },                   /* shl dword fs:[ffd], cl=0 */
        { 0, SEQ(0xbb, 0xfd, 0x0f, 0x00, 0x00, 0x64, 0xd1, 0x03) },                               /* rol dword fs:[ffd], 1 */
        { 0, SEQ(0xbb, 0xfe, 0x0f, 0x00, 0x00, 0x64, 0x0f, 0xba, 0x23, 0x1f) },                   /* bt dword fs:[ffe], 31 */
        { 0, SEQ(0xbb, 0xfc, 0x0f, 0x00, 0x00, 0xba, 0x20, 0x00, 0x00, 0x00, 0x64, 0x0f, 0xa3, 0x13) }, /* bt fs:[ffc], edx=32 */
        { 0, SEQ(0xbb, 0xfc, 0x0f, 0x00, 0x00, 0xba, 0x20, 0x00, 0x00, 0x00, 0x64, 0x0f, 0xab, 0x13) }, /* bts: nothing written */
        { 1, SEQ(0xbb, 0xfa, 0x0f, 0x00, 0x00, 0x64, 0x0f, 0x6f, 0x03, 0x0f, 0x77) },             /* movq mm0, fs:[ffa]; emms */
        { 1, SEQ(0xbb, 0xfa, 0x0f, 0x00, 0x00, 0x64, 0x0f, 0x7f, 0x03, 0x0f, 0x77) },             /* movq fs:[ffa], mm0 */
        { 1, SEQ(0xbb, 0xfd, 0x0f, 0x00, 0x00, 0x64, 0x0f, 0x6e, 0x03, 0x0f, 0x77) },             /* movd mm0, fs:[ffd] */
        { 1, SEQ(0xbb, 0xfa, 0x0f, 0x00, 0x00, 0x64, 0x0f, 0xfc, 0x03, 0x0f, 0x77) },             /* paddb mm0, fs:[ffa] */
        { 2, SEQ(0xbb, 0xfa, 0x0f, 0x00, 0x00, 0x64, 0x0f, 0xc7, 0x0b) },                         /* cmpxchg8b fs:[ffa] */
    };
    struct variant v = { .group = name, .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
                         .fix_input = fix_ring, .arg2 = RF_SANDBOX, .fx = FX_CUSTOM, .defmask = dm_fxarg,
                         .fx_arg = 1, .ring = 1, .arg = SCRATCH_RAW(0xb2, 0x4) };
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        if ((seqs[i].need == 1 && !(g_cpu.features & CPUID_MMX)) || (seqs[i].need == 2 && !(g_cpu.features & (1u << 8))))
            continue;
        int n = 0;
        static const uint8_t fs[] = { 0x66, 0xb8, SEL_SCRATCH | 1, 0x00, 0x8e, 0xe0 }; /* mov fs, scratch */
        memcpy(v.target, fs, sizeof(fs));
        n = sizeof(fs);
        memcpy(v.target + n, seqs[i].b, seqs[i].len);
        v.target_len = n + seqs[i].len;
        run_plain(&v, 1);
    }
}

static void
group_r1_limits(void)
{
    limits_run("r1.limits", 1, limits_ops);
}

/* SS on the scratch descriptors, then a push and a pop at stack pointers
   either side of the limit: #SS when the stack leaves the segment, #GP
   (or #SS for a missing one) when SS can't be loaded at all; expand-down
   and 16-bit stacks. */
static void
ss_run(const char *name, int ring)
{
    static const uint8_t  cfgs[] = { 0, 1, 2, 3, 4, 7, 8 };
    static const uint32_t esps[] = { 0x10, 0x02, 0x1000, 0x1004, 0x2000, 0x3000 };
    static struct ring_seq seqs[sizeof(cfgs) * sizeof(esps) / sizeof(esps[0])];
    int                    n = 0;
    for (unsigned c = 0; c < sizeof(cfgs); c++) {
        for (unsigned k = 0; k < sizeof(esps) / sizeof(esps[0]); k++) {
            struct ring_seq q = { ring, 0, 0, 0, RF_SANDBOX, 1, 0, { 0 } };
            int             m = 0;
            q.b[m++]          = 0x66; /* mov ax, scratch; mov ss, ax */
            q.b[m++]          = 0xb8;
            q.b[m++]          = SEL_SCRATCH | ring;
            q.b[m++]          = 0x00;
            q.b[m++]          = 0x8e;
            q.b[m++]          = 0xd0;
            q.b[m++]          = 0xbc; /* mov esp, value */
            put_le(q.b + m, esps[k], 4);
            m += 4;
            q.b[m++]  = 0x51; /* push ecx */
            q.b[m++]  = 0x5e; /* pop esi */
            q.len     = m;
            q.scratch = SCRATCH(cfgs[c], ring);
            seqs[n++] = q;
        }
    }
    ring_run(name, seqs, n, 0, 0, 0);
}

static void
group_r3_ss(void)
{
    ss_run("r3.ss", 3);
}

static void
group_r1_ss(void)
{
    ss_run("r1.ss", 1);
}

/* A far call into conforming code of DPL 0, 1 and 3 from rings 1, 2 and
   3: it runs at the caller's CPL (CS keeps the caller's RPL), and a DPL
   above the CPL is #GP. */
static void
group_conforming(void)
{
    static const uint8_t   sels[3] = { SEL_CONF0, SEL_CONF1, SEL_CONF3 };
    static struct ring_seq seqs[9];
    int                    n = 0;
    for (int ring = 1; ring <= 3; ring++) {
        for (int k = 0; k < 3; k++) {
            struct ring_seq q = { ring, 0, 0, 0, RF_NONE, 1, 7, { 0x9a } };
            put_le(q.b + 1, (uint32_t) g_gate_ret, 4);
            q.b[5]    = sels[k] | ring;
            q.b[6]    = 0;
            seqs[n++] = q;
        }
    }
    ring_run("conforming", seqs, n, 0, 0, 0);
}

/* Gates that must fail: INT through a missing gate (#NP), a gate of an
   invalid type, one whose selector is data or null (#GP), and a 386 trap
   gate, from rings 0 and 3. Then the IDT's limit: an INT past it is #GP
   (with the IDT bit in the error code), and a gate only partly inside it
   counts as outside. Those variants set the limit before the run (arg3)
   and put it back after. */
static void
idt_limit_before(const struct variant *v)
{
    ring_setup(v);
#ifndef HOSTTEST
    struct {
        uint16_t limit;
        uint32_t base;
    } __attribute__((packed)) idtr = { (uint16_t) v->arg3, (uint32_t) g_idt2 };
    __asm__ volatile("lidt %0" : : "m"(idtr));
#endif
}

static void
idt_limit_after(const struct variant *v)
{
#ifndef HOSTTEST
    struct {
        uint16_t limit;
        uint32_t base;
    } __attribute__((packed)) idtr = { sizeof(g_idt2) - 1, (uint32_t) g_idt2 };
    __asm__ volatile("lidt %0" : : "m"(idtr));
#endif
}

static void
intgate_idt(const char *name)
{
    static const struct {
        uint8_t  ring, vec;
        uint16_t limit;
    } limits[] = {
        { 0, 0x31, 0x31 * 8 - 1 }, /* 31h just past the limit */
        { 0, 0x30, 0x31 * 8 - 1 }, /* 30h the last one inside */
        { 0, 0x30, 0x30 * 8 + 3 }, /* 30h half inside: #GP */
        { 0, 0x03, 0x31 * 8 - 1 },
        { 3, 0x31, 0x31 * 8 - 1 }, /* from ring 3 */
        { 3, 0x30, 0x31 * 8 - 1 },
    };
    struct variant v = { .group = name, .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
                         .fix_input = fix_ring, .arg2 = RF_NONE, .fx = FX_CUSTOM, .defmask = dm_fxarg, .fx_arg = 1 };
    for (int r = 0; r <= 3; r += 3) {
        for (int vec = 0x39; vec <= 0x3d; vec++) {
            v.target[0]  = 0xcd;
            v.target[1]  = vec;
            v.target_len = 2;
            v.ring       = r;
            run_plain(&v, 1);
        }
    }
    v.before_run = idt_limit_before;
    v.after_run  = idt_limit_after;
    for (unsigned i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
        v.target[0]  = 0xcd;
        v.target[1]  = limits[i].vec;
        v.target_len = 2;
        v.ring       = limits[i].ring;
        v.arg3       = limits[i].limit;
        run_plain(&v, 1);
    }
}

/* Interrupt and trap gates whose handlers run in ring 1, 2 or 3 (they
   report CS, ESP, SS and IRET back): from each ring. A gate to an outer
   ring is #GP, as is a DPL-1 gate from ring 2 or 3. */
static void
group_intgate(void)
{
    static struct ring_seq seqs[15];
    int                    n = 0;
    for (int ring = 1; ring <= 3; ring++) {
        for (int vec = 0x33; vec <= 0x37; vec++) {
            struct ring_seq q = { ring, 0, 0, 0, RF_NONE, 1, 2, { 0xcd, (uint8_t) vec } };
            seqs[n++]         = q;
        }
    }
    ring_run_more("intgate", seqs, n, 0, 0, 0, intgate_idt);
}

/* From ring 1, returns that may not happen: IRETD and RETF to ring 0,
   IRETD to a CS whose RPL doesn't match its DPL. And one that may, but
   can't change IOPL: IRETD within ring 1 with IOPL 3 in the image. */
static void
emit_inner(struct emit *e, const struct variant *v)
{
    uint8_t *push_l = 0;
    switch (v->arg3) {
        case 0: /* iretd to ring 0 */
            e8(e, 0x9c);
            e8(e, 0x6a);
            e8(e, 0x08);
            break;
        case 1: /* retf to ring 0 */
            e8(e, 0x6a);
            e8(e, 0x08);
            break;
        case 2: /* iretd within ring 1, IOPL 3 in the flags pushed */
            e8(e, 0x68);
            e32(e, 0x3002);
            e8(e, 0x6a);
            e8(e, SEL_CODE1 | 1);
            break;
        case 3: /* iretd to ring-1 code with RPL 2: out a ring, onto the wrong DPL */
            e8(e, 0x6a);
            e8(e, SEL_DATA2 | 2);
            e8(e, 0x68);
            e32(e, (uint32_t) g_rstack[2] + sizeof(g_rstack[0]));
            e8(e, 0x9c);
            e8(e, 0x6a);
            e8(e, SEL_CODE1 | 2);
            break;
        default: /* retf 8 within ring 1 */
            e8(e, 0x6a);
            e8(e, 0x00);
            e8(e, 0x6a);
            e8(e, 0x00);
            e8(e, 0x6a);
            e8(e, SEL_CODE1 | 1);
            break;
    }
    e8(e, 0x68); /* push L */
    push_l = e->p;
    e32(e, 0);
    if (v->arg3 == 1) {
        e8(e, 0xcb);
    } else if (v->arg3 == 4) {
        e8(e, 0xca);
        e8(e, 0x08);
        e8(e, 0x00);
    } else
        e8(e, 0xcf);
    put32_at(push_l, (uint32_t) e->p);
    static const uint8_t after[] = { 0x9c, 0x5e, 0x66, 0x8c, 0xcf }; /* pushfd; pop esi; mov di, cs */
    ebytes(e, after, sizeof(after));
}

static void
group_r1_inner(void)
{
    const char    *name = "r1.inner";
    struct variant v    = { .group = name, .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
                            .fix_input = fix_ring, .fx = FX_CUSTOM, .defmask = dm_fxarg, .fx_arg = 1,
                            .emit = emit_inner, .ring = 1 };
    if (!ring_begin(name, 0, 0))
        return;
    for (int k = 0; k < 5; k++) {
        static const uint8_t sig[5] = { 0xcf, 0xcb, 0xcf, 0xcf, 0xca };
        v.target[0]  = sig[k];
        v.target[1]  = k;
        v.target_len = 2;
        v.arg3       = k;
        run_plain(&v, 1);
    }
    ring_end(name, 0, 0);
}

/* 16-bit code in ring 3 (Win16 applications) and ring 1: arithmetic at 16
   and 32 bits, SETcc, memory through a 32-bit address, the stack, PUSHF,
   CS, near CALL/RET, a far call through a 32-bit call gate, and the
   gates and I/O the ring may or may not use. */
#define C16_SEQS(r)                                                                               \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x01, 0xd1), 0, 1 },                   /* add cx, dx */       \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x66, 0x01, 0xd1), 0, 1 },             /* add ecx, edx */     \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x0f, 0x94, 0xc0), 0, 1 },             /* setz al */          \
    { r, 0, 0, 0, RF_MEM, 1, SEQ(0x67, 0x8b, 0x03, 0x67, 0x66, 0x89, 0x4b, 0x04), 0, 1 }, /* mov ax, [ebx]; mov [ebx+4], ecx */ \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x52, 0x51, 0x5e, 0x5f), 0, 1 },       /* push dx; push cx; pop si; pop di */ \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x9c, 0x5e), 0, 1 },                   /* pushf; pop si */    \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x8c, 0xce), 0, 1 },                   /* mov si, cs */       \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0xe8, 0x02, 0x00, 0xeb, 0x02, 0x46, 0xc3), 0, 1 }, /* call L; jmp over; L: inc si; ret */ \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x9a, 0x00, 0x00, SEL_GATE3, 0x00), 0, 1 },   /* call far through a 32-bit gate */ \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0x9a, 0x00, 0x00, SEL_GATE31, 0x00), 0, 1 },  /* into ring 1 */  \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0xcd, 0x32), 0, 1 },                   /* INT through the DPL-1 gate */ \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0xfa), 0, 1 },                         /* cli at IOPL 0 */    \
    { r, r, 0, 0, RF_NONE, 1, SEQ(0xe6, 0x80), 0, 1 },                   /* out 80h at IOPL = CPL */ \
    { r, 0, 0, 0, RF_NONE, 1, SEQ(0xf4), 0, 1 },                         /* hlt */

static void
group_r3_code16(void)
{
    static const struct ring_seq seqs[] = { C16_SEQS(3) };
    ring_run("r3.code16", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

static void
group_r1_code16(void)
{
    static const struct ring_seq seqs[] = { C16_SEQS(1) };
    ring_run("r1.code16", seqs, sizeof(seqs) / sizeof(seqs[0]), 0, 0, 0);
}

/* POP into DS, ES, FS, GS and SS of a pushed selector: the ring's own
   data, another ring's, DPL 0, null, the TSS, readable and execute-only
   code, read-only data, a missing segment. */
static void
popseg_run(const char *name, int ring)
{
    static const uint8_t pops[5][2] = { { 0x1f }, { 0x07 }, { 0x0f, 0xa1 }, { 0x0f, 0xa9 }, { 0x17 } };
    static struct ring_seq seqs[9 * 5];
    uint8_t                sels[9] = { ring_data_sel[ring], SEL_DATA3 | 3, 0x10, 0x00, SEL_TSS,
                                       ring_code_sel[ring], SEL_XO3 | 3, SEL_RO3 | 3, SEL_NP3 | 3 };
    int                    n       = 0;
    for (int s = 0; s < 9; s++) {
        for (int p = 0; p < 5; p++) {
            struct ring_seq q = { ring, 0, 0, 0, RF_NONE, 1, 0, { 0x68 } }; /* push dword sel */
            int             m = 1;
            put_le(q.b + m, sels[s], 4);
            m += 4;
            q.b[m++] = pops[p][0];
            if (pops[p][1])
                q.b[m++] = pops[p][1];
            q.len     = m;
            seqs[n++] = q;
        }
    }
    ring_run(name, seqs, n, 0, 0, 0);
}

static void
group_r3_popseg(void)
{
    popseg_run("r3.popseg", 3);
}

static void
group_r1_popseg(void)
{
    popseg_run("r1.popseg", 1);
}

/* Call gates that copy three parameters into ring 1, from rings 3, 2 and
   1 (no copy: the same stack); gates to ring-3 code, which only ring 3
   may use; and a DPL-1 gate outward. */
static void
group_gateparams(void)
{
    static struct ring_seq seqs[9];
    int                    n = 0;
    for (int ring = 1; ring <= 3; ring++) {
        struct ring_seq q = { ring, 0, 0, 0, RF_NONE, 1, 22,
                              { 0x68, 0x11, 0x11, 0x11, 0x11, 0x68, 0x22, 0x22, 0x22, 0x22, 0x68, 0x33, 0x33, 0x33, 0x33,
                                0x9a, 0, 0, 0, 0, SEL_GATE3P1, 0x00 } };
        seqs[n++] = q;
        struct ring_seq g = { ring, 0, 0, 0, RF_NONE, 1, 7, { 0x9a, 0, 0, 0, 0, SEL_GATE33, 0x00 } };
        seqs[n++]         = g;
        struct ring_seq o = { ring, 0, 0, 0, RF_NONE, 1, 7, { 0x9a, 0, 0, 0, 0, SEL_GATE13, 0x00 } };
        seqs[n++]         = o;
    }
    ring_run("gateparams", seqs, n, 0, 0, 0);
}

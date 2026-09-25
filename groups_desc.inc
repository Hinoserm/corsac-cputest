/*
 * Descriptor tables and tasks: the accessed bit, the LDT (LLDT, loads and
 * gates through TI=1 selectors), and task switches by CALL, JMP, task
 * gates in the GDT and the IDT, IRET with NT, and the errors they raise.
 * All with the ring tables of ring_env_on(). Included into groups.inc
 * after groups_sys.inc; append only.
 */

/* ---- the accessed bit ------------------------------------------------------ */

/* After each operation the test reads the scratch descriptor's upper half
   straight from the GDT (into ECX): a segment load sets the accessed bit
   (bit 8), LAR, LSL and VERR leave it alone. The descriptor starts with
   it clear on every run. */
static void
group_desc_accessed(void)
{
    static struct ring_seq seqs[16];
    int                    n  = 0;
    uint32_t               hi = (uint32_t) &g_gdt2[(SEL_SCRATCH >> 3) * 2 + 1];
    static const struct {
        uint8_t len, b[12];
    } ops[] = {
        { 0, { 0 } },                                                  /* just read it */
        { SEQ(0x66, 0xb8, 0x90, 0x00, 0x8e, 0xe0) },                   /* mov fs, scratch */
        { SEQ(0x66, 0xb8, 0x90, 0x00, 0x0f, 0x02, 0xc0) },             /* lar eax, ax */
        { SEQ(0x66, 0xb8, 0x90, 0x00, 0x0f, 0x03, 0xc0) },             /* lsl eax, ax */
        { SEQ(0x66, 0xb8, 0x90, 0x00, 0x0f, 0x00, 0xe0) },             /* verr ax */
        { SEQ(0x66, 0xb8, 0x90, 0x00, 0x0f, 0x00, 0xe8) },             /* verw ax */
        { SEQ(0x66, 0xb8, 0x90, 0x00, 0x8e, 0xe0, 0x0f, 0xa0, 0x0f, 0xa9) }, /* mov fs; push fs; pop gs */
    };
    for (int r = 0; r <= 3; r += 3) {
        for (unsigned k = 0; k < sizeof(ops) / sizeof(ops[0]); k++) {
            struct ring_seq q = { r, 0, 0, 0, RF_NONE, 1, 0, { 0 } };
            memcpy(q.b, ops[k].b, ops[k].len);
            int m = ops[k].len;
            if (m >= 3)
                q.b[2] = SEL_SCRATCH | r; /* the selector's RPL: the ring's */
            q.b[m++] = 0x8b;              /* mov ecx, [descriptor high] */
            q.b[m++] = 0x0d;
            put_le(q.b + m, hi, 4);
            m += 4;
            q.len     = m;
            q.scratch = SCRATCH(0, r);
            seqs[n++] = q;
        }
    }
    ring_run("desc.accessed", seqs, n, 0, 0, 0);
}

/* ---- the LDT ----------------------------------------------------------------- */

static void
ldt_before(const struct variant *v)
{
    ring_setup(v);
#ifndef HOSTTEST
    __asm__ volatile("lldt %w0" : : "r"(SEL_LDT));
#endif
}

static void
ldt_after(const struct variant *v)
{
#ifndef HOSTTEST
    __asm__ volatile("lldt %w0" : : "r"(0));
#endif
}

/* ring_run with the LDT loaded before each run and dropped after. */
static void
ldt_run(const char *name, const struct ring_seq *seqs, unsigned n)
{
    struct variant v = { .group = name, .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ldt_before,
                         .after_run = ldt_after, .fix_input = fix_ring, .fx = FX_CUSTOM, .defmask = dm_fxarg };
    if (!ring_begin(name, 0, 0))
        return;
    for (unsigned i = 0; i < n; i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.ring       = seqs[i].ring;
        v.ring_flags = IOPL(seqs[i].iopl);
        v.arg2       = seqs[i].fix;
        v.fx_arg     = seqs[i].defined;
        v.norm       = seqs[i].fix == RF_MEM ? 1 << R_EBX : 0;
        v.arg        = 0;
        run_plain(&v, 1);
    }
    ring_end(name, 0, 0);
}

/* LLDT at CPL 0: the LDT, null, a TSS, a data segment, an LDT-relative
   selector, from memory; SLDT reads back. A null LDTR makes any TI=1
   selector #GP. */
static void
group_ldt_load(void)
{
    static const struct ring_seq seqs[] = {
        { 0, 0, 0, 0, RF_NONE, 1, SEQ(0x66, 0xb8, SEL_LDT, 0x00, 0x0f, 0x00, 0xd0, 0x0f, 0x00, 0xc1) },       /* lldt LDT; sldt ecx */
        { 0, 0, 0, 0, RF_NONE, 1, SEQ(0x31, 0xc0, 0x0f, 0x00, 0xd0, 0x0f, 0x00, 0xc1) },                   /* lldt 0 */
        { 0, 0, 0, 0, RF_NONE, 1, SEQ(0x31, 0xc0, 0x0f, 0x00, 0xd0, 0x66, 0xb8, 0x07, 0x00, 0x8e, 0xe0) },   /* lldt 0; mov fs, 07h: #GP */
        { 0, 0, 0, 0, RF_NONE, 1, SEQ(0x66, 0xb8, SEL_TSS, 0x00, 0x0f, 0x00, 0xd0) },                       /* lldt TSS: #GP */
        { 0, 0, 0, 0, RF_NONE, 1, SEQ(0x66, 0xb8, 0x10, 0x00, 0x0f, 0x00, 0xd0) },                          /* lldt data: #GP */
        { 0, 0, 0, 0, RF_NONE, 1, SEQ(0x66, 0xb8, 0x04, 0x00, 0x0f, 0x00, 0xd0) },                          /* lldt TI=1: #GP */
        { 0, 0, 0, 0, RF_NONE, 1, SEQ(0x66, 0xb8, SEL_LDT | 3, 0x00, 0x0f, 0x00, 0xd0, 0x0f, 0x00, 0xc1) },   /* RPL 3 */
        { 0, 0, 0, 0, RF_MEM, 1, SEQ(0x66, 0xc7, 0x03, SEL_LDT, 0x00, 0x0f, 0x00, 0x13, 0x0f, 0x00, 0x03) },  /* lldt [ebx]; sldt [ebx] */
    };
    ldt_run("ldt.load", seqs, sizeof(seqs) / sizeof(seqs[0]));
}

/* Segment loads, LAR, LSL and VERR through the LDT at CPL 0, 1 and 3:
   DPL, not present, past the LDT's limit, SS from the LDT. */
static void
group_ldt_seg(void)
{
    static const uint8_t   sels[] = { 0x07, 0x0f, 0x14, 0x1f, 0x27, 0x2d, 0x34 };
    static struct ring_seq seqs[3 * 7 * 3];
    int                    n = 0;
    for (int r = 0; r <= 3; r++) {
        if (r == 2)
            continue;
        for (unsigned s = 0; s < sizeof(sels); s++) {
            struct ring_seq q = { r, 0, 0, 0, RF_MEM, 1, 0, { 0 } };
            int             m = 0;
            q.b[m++] = 0x66; /* mov cx, sel; lar eax, ecx; setz bl; lsl edx, ecx; setz bh */
            q.b[m++] = 0xb9;
            q.b[m++] = sels[s];
            q.b[m++] = 0x00;
            static const uint8_t larlsl[] = { 0x0f, 0x02, 0xc1, 0x0f, 0x94, 0xc3, 0x0f, 0x03, 0xd1, 0x0f, 0x94, 0xc7 };
            memcpy(q.b + m, larlsl, sizeof(larlsl));
            m += sizeof(larlsl);
            q.len     = m;
            q.defined = 0; /* LAR's undefined bits */
            seqs[n++] = q;
            struct ring_seq f = { r, 0, 0, 0, RF_MEM, 1, 0, { 0 } };
            m                 = 0;
            f.b[m++]          = 0x66; /* mov ax, sel; mov fs, ax; mov ecx, fs:[ebx] */
            f.b[m++]          = 0xb8;
            f.b[m++]          = sels[s];
            f.b[m++]          = 0x00;
            f.b[m++]          = 0x8e;
            f.b[m++]          = 0xe0;
            f.b[m++]          = 0x64;
            f.b[m++]          = 0x8b;
            f.b[m++]          = 0x0b;
            f.len             = m;
            seqs[n++]         = f;
            struct ring_seq t = { r, 0, 0, 0, RF_NONE, 1, 0, { 0 } };
            m                 = 0;
            t.b[m++]          = 0x66; /* mov ax, sel; mov ss, ax; push ecx; pop esi */
            t.b[m++]          = 0xb8;
            t.b[m++]          = sels[s];
            t.b[m++]          = 0x00;
            t.b[m++]          = 0x8e;
            t.b[m++]          = 0xd0;
            t.b[m++]          = 0x51;
            t.b[m++]          = 0x5e;
            t.len             = m;
            seqs[n++]         = t;
        }
    }
    ldt_run("ldt.seg", seqs, n);
}

/* The LDT's call gate (to ring 0) and code segment (DPL 3), called from
   rings 3, 1 and 0: calling less privileged code is #GP. */
static void
group_ldt_gate(void)
{
    static struct ring_seq seqs[6];
    int                    n = 0;
    for (int r = 0; r <= 3; r++) {
        if (r == 2)
            continue;
        struct ring_seq g = { r, 0, 0, 0, RF_NONE, 1, 7, { 0x9a, 0, 0, 0, 0, 0x1f, 0x00 } };
        seqs[n++]         = g;
        struct ring_seq c = { r, 0, 0, 0, RF_NONE, 1, 7, { 0x9a } };
        put_le(c.b + 1, (uint32_t) g_gate_ret, 4);
        c.b[5]    = 0x0f;
        c.b[6]    = 0x00;
        seqs[n++] = c;
    }
    ldt_run("ldt.gate", seqs, n);
}

/* ---- tasks --------------------------------------------------------------------- */

/* Where the second task starts, set when its code is emitted. */
static uint32_t g_task_eip;

/* Before each run: the second task's TSS as it starts (known registers,
   its entry point, its own stack), both TSS descriptors in the state the
   test expects, no back link in ours. arg3 bit 0: the second TSS marked
   busy already. */
static void
task_before(const struct variant *v)
{
    ring_setup(v);
#ifndef HOSTTEST
    uint32_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    memset(g_tss2, 0, sizeof(g_tss2));
    static const uint32_t regs[8] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444, 0, 0x55555555, 0x66666666, 0x77777777 };
    *(uint32_t *) (g_tss2 + 0x1c) = cr3;
    *(uint32_t *) (g_tss2 + 0x20) = g_task_eip;
    *(uint32_t *) (g_tss2 + 0x24) = 0x2;
    for (int i = 0; i < 8; i++)
        *(uint32_t *) (g_tss2 + 0x28 + i * 4) = regs[i];
    *(uint32_t *) (g_tss2 + 0x38) = (uint32_t) g_task_stack + sizeof(g_task_stack);
    *(uint32_t *) (g_tss2 + 0x48) = 0x10; /* ES */
    *(uint32_t *) (g_tss2 + 0x4c) = 0x08; /* CS */
    *(uint32_t *) (g_tss2 + 0x50) = 0x10; /* SS */
    *(uint32_t *) (g_tss2 + 0x54) = 0x10; /* DS */
    *(uint32_t *) (g_tss2 + 0x58) = 0x10; /* FS */
    *(uint32_t *) (g_tss2 + 0x5c) = 0x10; /* GS */
    *(uint16_t *) (g_tss2 + 0x66) = sizeof(g_tss2);
    *(uint32_t *) g_tss            = 0;   /* no back link in ours */
    *(uint32_t *) (g_tss + 0x1c)   = cr3; /* a switch back loads CR3 from it */
    uint32_t *d2 = &g_gdt2[(SEL_TSS2 >> 3) * 2 + 1];
    *d2          = (*d2 & ~0xf00u) | ((v->arg3 & 1 ? 0xb : 0x9) << 8);
#endif
}

/* After: if the test ended in the other task, or loaded TR, come home. */
static void
task_after(const struct variant *v)
{
#ifndef HOSTTEST
    uint16_t tr;
    __asm__ volatile("str %w0" : "=r"(tr));
    if (tr != SEL_TSS) {
        g_gdt2[(SEL_TSS >> 3) * 2 + 1] &= ~0x200u;  /* available again, then LTR */
        g_gdt2[(SEL_TSS2 >> 3) * 2 + 1] &= ~0x200u;
        __asm__ volatile("ltr %w0" : : "r"(SEL_TSS));
    }
#endif
}

/* The second task's code: it stores what it sees into the buffer (EAX
   and EDI from its TSS, TR, EFLAGS with NT, the back link), then leaves:
   IRETD (arg2 bit 4 clear: it was called) or JMP back to our TSS. */
static void
emit_task_code(struct emit *e, int jmp_back)
{
    uint32_t b = (uint32_t) g_buf;
    g_task_eip = (uint32_t) e->p;
    e8(e, 0xa3); /* mov [buf], eax */
    e32(e, b);
    e8(e, 0x89); /* mov [buf+4], edi */
    e8(e, 0x3d);
    e32(e, b + 4);
    e8(e, 0x0f); /* str eax */
    e8(e, 0x00);
    e8(e, 0xc8);
    e8(e, 0xa3); /* mov [buf+8], eax */
    e32(e, b + 8);
    e8(e, 0x9c); /* pushfd; pop eax; mov [buf+12], eax */
    e8(e, 0x58);
    e8(e, 0xa3);
    e32(e, b + 12);
    e8(e, 0x66); /* mov ax, [back link]; mov [buf+16], ax */
    e8(e, 0xa1);
    e32(e, (uint32_t) g_tss2);
    e8(e, 0x66);
    e8(e, 0xa3);
    e32(e, b + 16);
    if (jmp_back) {
        e8(e, 0xea); /* jmp far our TSS */
        e32(e, 0);
        e8(e, SEL_TSS);
        e8(e, 0x00);
    } else
        e8(e, 0xcf); /* iretd */
}

/* The switch (arg2): 0 CALL TSS, 1 CALL task gate, 2 INT 38h, 3 JMP TSS,
   4 JMP task gate. Then the task code behind a jump, then what the caller
   sees afterwards: TR, the second TSS's descriptor (busy bit), EFLAGS. */
static void
emit_task(struct emit *e, const struct variant *v)
{
    int k = v->arg2;
    switch (k) {
        case 0:
        case 1:
        case 3:
        case 4:
            e8(e, k >= 3 ? 0xea : 0x9a);
            e32(e, 0);
            e8(e, (k == 1 || k == 4) ? (SEL_TGATE & 0xff) : (SEL_TSS2 & 0xff));
            e8(e, SEL_TSS2 >> 8);
            break;
        default:
            e8(e, 0xcd);
            e8(e, 0x38);
            break;
    }
    uint8_t *jmp = e->p;
    e8(e, 0xeb); /* over the task's code */
    e8(e, 0x00);
    emit_task_code(e, k >= 3);
    jmp[1] = e->p - (jmp + 2);
    e8(e, 0x0f); /* str ecx */
    e8(e, 0x00);
    e8(e, 0xc9);
    e8(e, 0x8b); /* mov edx, [second TSS descriptor high] */
    e8(e, 0x15);
    e32(e, (uint32_t) &g_gdt2[(SEL_TSS2 >> 3) * 2 + 1]);
    e8(e, 0x9c); /* pushfd; pop esi */
    e8(e, 0x5e);
}

static void
task_run(const char *name, const uint8_t (*cases)[2], unsigned n)
{
    struct variant v = { .group = name, .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = task_before,
                         .after_run = task_after, .fx = FX_CUSTOM, .defmask = dm_fxarg, .fx_arg = 1,
                         .emit = emit_task }; /* arg2 is the switch, not an input: no fixer */
    if (!ring_begin(name, 0, 0))
        return;
    for (unsigned i = 0; i < n; i++) {
        v.ring       = cases[i][1];
        v.arg2       = cases[i][0];
        v.arg3       = 0;
        v.target[0]  = 0x7a; /* for the reports */
        v.target[1]  = cases[i][0];
        v.target[2]  = cases[i][1];
        v.target_len = 3;
        run_plain(&v, 1);
    }
    ring_end(name, 0, 0);
}

/* CALL to a TSS, through a task gate, and INT through the IDT's task gate
   from rings 0 and 3: the new task sees its own registers, TR, NT set and
   its back link; back home, TR is ours again and the other TSS
   available. */
static void
group_task_call(void)
{
    static const uint8_t cases[][2] = { { 0, 0 }, { 1, 0 }, { 2, 0 }, { 2, 3 }, { 1, 3 } };
    task_run("task.call", cases, sizeof(cases) / sizeof(cases[0]));
}

/* JMP to a TSS and through a task gate, and back by JMP: no nesting, no
   back link, NT clear. */
static void
group_task_jmp(void)
{
    static const uint8_t cases[][2] = { { 3, 0 }, { 4, 0 }, { 4, 3 } };
    task_run("task.jmp", cases, sizeof(cases) / sizeof(cases[0]));
}

/* What a task switch must refuse: a CALL to the busy TSS we run on, a
   CALL to a TSS marked busy, a TSS too short (#TS), IRET with NT and no
   back link, a DPL-0 TSS from ring 3, LTR of a busy TSS. */
static void
group_task_errors(void)
{
    static const struct ring_seq seqs[] = {
        { 0, 0, 0, 0, RF_NONE, 1, SEQ(0x9a, 0, 0, 0, 0, SEL_TSS, 0x00) },                         /* call our own TSS */
        { 0, 0, 0, 1, RF_NONE, 1, SEQ(0x9a, 0, 0, 0, 0, SEL_TSS2 & 0xff, SEL_TSS2 >> 8) },         /* call a busy TSS */
        { 0, 0, 0, 0, RF_NONE, 1, SEQ(0x9a, 0, 0, 0, 0, SEL_TSS3 & 0xff, SEL_TSS3 >> 8) },         /* too short: #TS */
        { 0, 0, 0, 0, RF_NONE, 1, SEQ(0x9c, 0x81, 0x0c, 0x24, 0x00, 0x40, 0x00, 0x00, 0x9d, 0xcf) }, /* NT set, IRETD: no back link */
        { 3, 0, 0, 0, RF_NONE, 1, SEQ(0x9a, 0, 0, 0, 0, SEL_TSS2 & 0xff, SEL_TSS2 >> 8) },         /* a DPL-0 TSS from ring 3 */
        { 0, 0, 0, 0, RF_NONE, 1, SEQ(0x66, 0xb8, SEL_TSS, 0x00, 0x0f, 0x00, 0xd8) },              /* ltr of the busy TSS */
        { 0, 0, 0, 0, RF_NONE, 1, SEQ(0x66, 0xb8, 0x10, 0x00, 0x0f, 0x00, 0xd8) },                 /* ltr of a data segment */
    };
    struct variant v = { .group = "task.errors", .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = task_before,
                         .after_run = task_after, .fix_input = fix_ring, .fx = FX_CUSTOM, .defmask = dm_fxarg };
    if (!ring_begin(v.group, 0, 0))
        return;
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.ring       = seqs[i].ring;
        v.arg3       = seqs[i].setup;
        v.arg2       = RF_NONE;
        v.fx_arg     = seqs[i].defined;
        run_plain(&v, 1);
    }
    ring_end(v.group, 0, 0);
}

/*
 * Protection and instruction corners: which instructions LOCK may
 * prefix, segment overrides, far-pointer loads, 16-bit stacks, string
 * copies that overlap or run long, what a segment register looks like in
 * a 32-bit register or on the stack, LAR/VERR/segment loads over every
 * descriptor type, IDT gates that must fail, the end of a 16-bit code
 * segment and operand-size prefixes inside one, SGDT and friends, the
 * IDT's limit, EFLAGS through IRET, XLAT. Included into groups.inc after
 * groups_desc.inc; append only.
 */

/* ---- LOCK ------------------------------------------------------------------ */

/* The lockable instructions with a memory destination run; everything
   else with LOCK, and those with a register destination, is #UD. The
   logic ops leave AF undefined, the bit-test ones everything but CF:
   those parts are raw only. */
static int
dm_lock(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    if (!v->fx_arg)
        return 0;
    if (d->fault == 0xff)
        *undef |= F_AF;
    return 1;
}

static void
group_lock_legal(void)
{
    static const struct {
        uint8_t defined, len, b[7];
    } seqs[] = {
        { 1, SEQ(0xf0, 0x00, 0x03) },             /* add [ebx], al */
        { 1, SEQ(0xf0, 0x01, 0x03) },             /* add [ebx], eax */
        { 1, SEQ(0xf0, 0x66, 0x01, 0x03) },       /* add [ebx], ax */
        { 1, SEQ(0xf0, 0x09, 0x03) },             /* or */
        { 1, SEQ(0xf0, 0x11, 0x03) },             /* adc */
        { 1, SEQ(0xf0, 0x19, 0x03) },             /* sbb */
        { 1, SEQ(0xf0, 0x21, 0x03) },             /* and */
        { 1, SEQ(0xf0, 0x29, 0x03) },             /* sub */
        { 1, SEQ(0xf0, 0x31, 0x03) },             /* xor */
        { 1, SEQ(0xf0, 0x80, 0x03, 0x01) },       /* add byte [ebx], 1 */
        { 1, SEQ(0xf0, 0x83, 0x33, 0x01) },       /* xor dword [ebx], 1 */
        { 1, SEQ(0xf0, 0x86, 0x03) },             /* xchg [ebx], al */
        { 1, SEQ(0xf0, 0x87, 0x03) },             /* xchg [ebx], eax */
        { 1, SEQ(0xf0, 0xfe, 0x03) },             /* inc byte [ebx] */
        { 1, SEQ(0xf0, 0xff, 0x0b) },             /* dec dword [ebx] */
        { 1, SEQ(0xf0, 0xf6, 0x13) },             /* not byte [ebx] */
        { 1, SEQ(0xf0, 0xf7, 0x1b) },             /* neg dword [ebx] */
        { 0, SEQ(0x83, 0xe0, 0x1f, 0xf0, 0x0f, 0xab, 0x03) }, /* and eax, 31; bts [ebx], eax */
        { 0, SEQ(0x83, 0xe0, 0x1f, 0xf0, 0x0f, 0xb3, 0x03) }, /* btr */
        { 0, SEQ(0x83, 0xe0, 0x1f, 0xf0, 0x0f, 0xbb, 0x03) }, /* btc */
        { 0, SEQ(0xf0, 0x0f, 0xba, 0x2b, 0x05) }, /* bts [ebx], 5 */
        { 0, SEQ(0xf0, 0x0f, 0xba, 0x33, 0x05) }, /* btr [ebx], 5 */
        { 0, SEQ(0xf0, 0x0f, 0xba, 0x3b, 0x05) }, /* btc [ebx], 5 */
        { 1, SEQ(0xf0, 0x0f, 0xb0, 0x0b) },       /* cmpxchg [ebx], cl */
        { 1, SEQ(0xf0, 0x0f, 0xb1, 0x0b) },       /* cmpxchg [ebx], ecx */
        { 1, SEQ(0xf0, 0x0f, 0xc0, 0x03) },       /* xadd [ebx], al */
        { 1, SEQ(0xf0, 0x0f, 0xc1, 0x03) },       /* xadd [ebx], eax */
        { 1, SEQ(0xf0, 0x0f, 0xc7, 0x0b) },       /* cmpxchg8b [ebx] */
        { 1, SEQ(0xf0, 0x39, 0x03) },             /* cmp: #UD */
        { 1, SEQ(0xf0, 0x03, 0x03) },             /* add eax, [ebx]: destination a register: #UD */
        { 1, SEQ(0xf0, 0x85, 0x03) },             /* test: #UD */
        { 1, SEQ(0xf0, 0xf6, 0x03, 0x01) },       /* test byte [ebx], 1: #UD */
        { 1, SEQ(0xf0, 0x88, 0x03) },             /* mov [ebx], al: #UD */
        { 1, SEQ(0xf0, 0x8b, 0x03) },             /* mov eax, [ebx]: #UD */
        { 1, SEQ(0xf0, 0x8d, 0x03) },             /* lea: #UD */
        { 1, SEQ(0x83, 0xe0, 0x1f, 0xf0, 0x0f, 0xa3, 0x03) }, /* bt [ebx], eax: #UD */
        { 1, SEQ(0xf0, 0x0f, 0xba, 0x23, 0x05) }, /* bt [ebx], 5: #UD */
        { 1, SEQ(0xf0, 0xff, 0x33) },             /* push [ebx]: #UD */
        { 1, SEQ(0xf0, 0x8f, 0x03) },             /* pop [ebx]: #UD */
        { 1, SEQ(0xf0, 0xf7, 0x23) },             /* mul dword [ebx]: #UD */
        { 1, SEQ(0xf0, 0x0f, 0xb6, 0x03) },       /* movzx: #UD */
        { 1, SEQ(0xf0, 0x0f, 0xbc, 0x03) },       /* bsf: #UD */
        { 1, SEQ(0xf0, 0x01, 0xc8) },             /* add eax, ecx: #UD */
        { 1, SEQ(0xf0, 0x87, 0xc8) },             /* xchg eax, ecx: #UD */
        { 1, SEQ(0xf0, 0xff, 0xc0) },             /* inc eax: #UD */
        { 1, SEQ(0xf0, 0x40) },                   /* inc eax (one byte): #UD */
    };
    const char    *name = "lock.legal";
    struct variant v    = { .group = name, .faults_ok = 1, .fx = FX_CUSTOM, .defmask = dm_lock,
                            .fix_input = fix_mem_ebx, .arg = MEM_OFF };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.fx_arg     = seqs[i].defined;
        run_plain(&v, 1);
    }
    group_end(name);
}

/* ---- segment overrides -------------------------------------------------------- */

/* CS: reads (a readable code segment) but can't write; SS: and ES: are
   data; a null FS faults on use; a 64 KB segment ends before the buffer;
   with two overrides the last counts. */
static void
group_segovr(void)
{
    static const struct {
        uint8_t len, b[12];
    } seqs[] = {
        { SEQ(0x2e, 0x89, 0x03) },                               /* mov cs:[ebx], eax: #GP */
        { SEQ(0x2e, 0x8b, 0x03) },                               /* mov eax, cs:[ebx] */
        { SEQ(0x36, 0x8b, 0x03) },                               /* ss: */
        { SEQ(0x26, 0x89, 0x0b) },                               /* es: write */
        { SEQ(0x66, 0xb8, 0x00, 0x00, 0x8e, 0xe0, 0x64, 0x8b, 0x03) }, /* fs = 0; fs: read: #GP */
        { SEQ(0x66, 0xb8, 0x00, 0x00, 0x8e, 0xe0, 0x64, 0x89, 0x0b) }, /* fs: write */
        { SEQ(0x66, 0xb8, 0x20, 0x00, 0x8e, 0xe0, 0x64, 0x8b, 0x03) }, /* fs = 64 KB data: past its limit */
        { SEQ(0x89, 0xde, 0x2e, 0xad) },                         /* lodsd from cs: */
        { SEQ(0x3e, 0x2e, 0x89, 0x03) },                         /* ds: cs: write: CS counts */
        { SEQ(0x2e, 0x3e, 0x89, 0x03) },                         /* cs: ds: write: DS counts */
        { SEQ(0x64, 0x65, 0x8b, 0x03) },                         /* fs: gs: read */
    };
    const char    *name = "segovr";
    struct variant v    = { .group = name, .faults_ok = 1, .fix_input = fix_mem_ebx, .arg = MEM_OFF };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        run_plain(&v, 1);
    }
    group_end(name);
}

/* ---- far pointer loads ----------------------------------------------------------- */

/* LDS LES LFS LGS LSS from a far pointer in the buffer, 32- and 16-bit
   offsets, with a data, null, code, 64 KB and out-of-GDT selector; what
   was loaded is read back with MOV CX, Sreg. DS and ES are put back
   straight after; LSS's SS:ESP too, ESP kept in EDI meanwhile (which then
   holds the loaded offset), SS through AX. */
static void
group_lds_forms(void)
{
    static const struct {
        uint8_t n, op[2], sreg;
    } ops[] = {
        { 1, { 0xc5 }, 3 },       /* lds */
        { 1, { 0xc4 }, 0 },       /* les */
        { 2, { 0x0f, 0xb4 }, 4 }, /* lfs */
        { 2, { 0x0f, 0xb5 }, 5 }, /* lgs */
        { 2, { 0x0f, 0xb2 }, 2 }, /* lss */
    };
    static const uint8_t sels[] = { 0x10, 0x00, 0x08, 0x20, 0x28 };
    const char          *name   = "lds.forms";
    struct variant       v      = { .group = name, .faults_ok = 1, .fix_input = fix_mem_ebx, .arg = MEM_OFF };
#ifdef HOSTTEST
    group_skip(name, "segment loads (host build)");
    return;
#endif
    group_begin(name);
    for (unsigned o = 0; o < sizeof(ops) / sizeof(ops[0]); o++) {
        int sr = ops[o].sreg;
        for (int w = 0; w < 2; w++) {
            for (unsigned s = 0; s < sizeof(sels); s++) {
                int      n = 0;
                uint8_t *t = v.target;
                t[n++]     = 0x66; /* mov word [ebx+4 or +2], sel */
                t[n++]     = 0xc7;
                t[n++]     = 0x43;
                t[n++]     = w ? 2 : 4;
                t[n++]     = sels[s];
                t[n++]     = 0;
                if (sr == 2) {
                    t[n++] = 0x89; /* mov edi, esp */
                    t[n++] = 0xe7;
                }
                if (w)
                    t[n++] = 0x66;
                memcpy(t + n, ops[o].op, ops[o].n);
                n += ops[o].n;
                t[n++] = 0x33; /* esi, [ebx] */
                t[n++] = 0x66; /* mov cx, sreg */
                t[n++] = 0x8c;
                t[n++] = 0xc1 | (sr << 3);
                if (sr == 2) {
                    t[n++] = 0x87; /* xchg edi, esp */
                    t[n++] = 0xe7;
                }
                if (sr != 4 && sr != 5) {
                    t[n++] = 0x66; /* mov ax, 10h; mov sreg, ax */
                    t[n++] = 0xb8;
                    t[n++] = 0x10;
                    t[n++] = 0x00;
                    t[n++] = 0x8e;
                    t[n++] = 0xc0 | (sr << 3);
                }
                v.target_len = n;
                run_plain(&v, 1);
            }
        }
    }
    group_end(name);
}

/* ---- a 16-bit stack --------------------------------------------------------------- */

/* SS on the 64 KB data segment (B = 0): pushes and pops move SP only (the
   upper half of ESP stays as it was), into free memory at 7800h; CALL,
   ENTER, PUSHA and PUSHFD on it; and SP wrapping at 0. ESP and SS are
   put back after. */
static void
canon_stack16(const struct variant *v, const struct kin *in, struct kout *o)
{
    if (v->arg2 == 2) /* call: the pushed return address, relative to the code */
        o->esi -= (uint32_t) g_cur_slot;
}

static void
emit_stack16(struct emit *e, const struct variant *v)
{
    static const struct {
        uint8_t len, b[12];
    } body[] = {
        { SEQ(0x50, 0x59) },                         /* push eax; pop ecx */
        { SEQ(0x66, 0x50, 0x66, 0x5b) },             /* push ax; pop bx */
        { SEQ(0xe8, 0x00, 0x00, 0x00, 0x00, 0x5e) }, /* call $+5; pop esi */
        { SEQ(0xc8, 0x04, 0x00, 0x01) },             /* enter 4, 1 */
        { SEQ(0x60, 0x61) },                         /* pushad; popad */
        { SEQ(0x9c, 0x5e) },                         /* pushfd; pop esi */
        { SEQ(0x6a, 0x7f, 0x8f, 0xc6) },             /* push 7Fh; pop esi */
    };
    int k = v->arg2;
    e8(e, 0x89); /* mov [g_stack_save], esp */
    e8(e, 0x25);
    e32(e, (uint32_t) &g_stack_save);
    e8(e, 0x66); /* mov ax, 20h; mov ss, ax */
    e8(e, 0xb8);
    e8(e, 0x20);
    e8(e, 0x00);
    e8(e, 0x8e);
    e8(e, 0xd0);
    e8(e, 0xbc); /* mov esp, value */
    e32(e, v->arg3);
    ebytes(e, body[k].b, body[k].len);
    e8(e, 0x89); /* mov edx, esp */
    e8(e, 0xe2);
    e8(e, 0x66); /* mov ax, 10h; mov ss, ax; mov esp, [g_stack_save] */
    e8(e, 0xb8);
    e8(e, 0x10);
    e8(e, 0x00);
    e8(e, 0x8e);
    e8(e, 0xd0);
    e8(e, 0x8b);
    e8(e, 0x25);
    e32(e, (uint32_t) &g_stack_save);
}

static void
group_stack16(void)
{
    static const uint32_t esps[] = { 0xabcd7810, 0x00007810, 0x12340002 };
    const char           *name   = "stack16";
    struct variant        v      = { .group = name, .faults_ok = 1, .emit = emit_stack16, .canon = canon_stack16,
                                     .fx = FX_CUSTOM, .defmask = dm_fxarg, .fx_arg = 1 };
#ifdef HOSTTEST
    group_skip(name, "a 16-bit stack segment (host build)");
    return;
#endif
    group_begin(name);
    for (int k = 0; k < 7; k++) {
        for (int s = 0; s < 3; s++) {
            if (s == 2 && k != 0)
                continue; /* SP wrapping at 0: once */
            v.target[0]  = 0x5a; /* for the reports */
            v.target[1]  = k;
            v.target[2]  = s;
            v.target_len = 3;
            v.arg2       = k;
            v.arg3       = esps[s];
            v.fx_arg     = k != 5; /* PUSHFD holds the producer's undefined flags */
            run_plain(&v, 1);
        }
    }
    group_end(name);
}

/* ---- string copies that overlap, and long ones --------------------------------------- */

/* The cases (edge order): MOVSB onto itself one byte on (a byte repeated
   down the buffer), one byte back, DF set; MOVSW and MOVSD overlapping
   by one or two bytes; a count of 0. */
static void
fix_overlap(const struct variant *v)
{
    static const struct {
        int8_t  delta;
        uint8_t count, df;
    } cases[] = { { 1, 16, 0 }, { -1, 16, 0 }, { 1, 16, 1 }, { -1, 16, 1 }, { 2, 8, 0 }, { 3, 8, 0 }, { 1, 0, 0 } };
    int e = edge(7);
    if (e < 0)
        e = rnd() % 7;
    g_in.flags = (g_in.flags & ~0x400u) | (cases[e].df ? 0x400 : 0);
    g_in.esi   = (uint32_t) g_buf + 24;
    g_in.edi   = g_in.esi + cases[e].delta;
    g_in.ecx   = cases[e].count;
}

static void
group_string_overlap(void)
{
    static const struct {
        uint8_t len, b[3];
    } seqs[] = { { SEQ(0xf3, 0xa4) }, { SEQ(0xf3, 0xa5) }, { SEQ(0xf3, 0x66, 0xa5) } };
    const char    *name = "string.overlap";
    struct variant v    = { .group = name, .fix_input = fix_overlap, .norm = (1 << R_ESI) | (1 << R_EDI) };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        run_keep(&v, 8);
    }
    group_end(name);
}

/* Long REP runs through the sandbox, across its page boundaries: STOSD,
   MOVSD, unaligned STOSB, MOVSB backwards, CMPSD over equal memory (it
   runs out), SCASB for a byte that isn't there. arg2: the case. */
static void
fix_long(const struct variant *v)
{
    uint32_t s = (uint32_t) SANDBOX;
    g_in.flags &= ~0x400u;
    switch (v->arg2) {
        case 0: g_in.edi = s + 0x0400; g_in.ecx = 1000; break;
        case 1: g_in.esi = s + 0x0100; g_in.edi = s + 0x2100; g_in.ecx = 1024; break;
        case 2: g_in.edi = s + 0x0401; g_in.ecx = 3001; break;
        case 3: g_in.flags |= 0x400u; g_in.esi = s + 0x1f00; g_in.edi = s + 0x3f00; g_in.ecx = 2500; break;
        case 4: g_in.esi = s + 0x0000; g_in.edi = s + 0x2400; g_in.ecx = 1000; break;
        default: g_in.edi = s + 0x2400; g_in.ecx = 3000; g_in.eax &= ~0xffu; break;
    }
}

static void
group_string_long(void)
{
    static const struct {
        uint8_t len, b[2];
    } seqs[] = { { SEQ(0xf3, 0xab) }, { SEQ(0xf3, 0xa5) }, { SEQ(0xf3, 0xaa) }, { SEQ(0xf3, 0xa4) }, { SEQ(0xf3, 0xa7) }, { SEQ(0xf2, 0xae) } };
    const char    *name = "string.long";
    struct variant v    = { .group = name, .fix_input = fix_long, .norm = (1 << R_ESI) | (1 << R_EDI) };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.arg2       = i;
        run_keep(&v, 1);
    }
    group_end(name);
}

/* ---- segment registers in 32-bit registers and on the stack ---------------------------- */

/* MOV r32, Sreg (the upper half: zero on a P6, the model's own before),
   MOV r16, Sreg, MOV [m], Sreg (16 bits), and PUSH Sreg with a 32-bit
   operand onto a slot holding FFFFFFFFh: a 16-bit write or a zero-
   extended one. Raw. */
static void
group_sreg_upper(void)
{
    static const uint8_t pushes[6][2] = { { 0x06 }, { 0x0e }, { 0x16 }, { 0x1e }, { 0x0f, 0xa0 }, { 0x0f, 0xa8 } };
    const char          *name         = "sreg.upper";
    struct variant       v            = { .group = name, .fx = FX_CUSTOM, .defmask = dm_fxarg, .fx_arg = 0,
                                          .fix_input = fix_mem_ebx, .arg = MEM_OFF };
    group_begin(name);
    for (int s = 0; s < 6; s++) {
        uint8_t mod = 0xc0 | (s << 3); /* ModRM for Sreg s */
        v.target[0] = 0x8c;             /* mov eax, sreg (EAX random going in) */
        v.target[1] = mod;
        v.target_len = 2;
        run_plain(&v, 1);
        v.target[0] = 0x66; /* mov ax, sreg */
        v.target[1] = 0x8c;
        v.target[2] = mod;
        v.target_len = 3;
        run_plain(&v, 1);
        v.target[0] = 0x8c; /* mov [ebx], sreg */
        v.target[1] = 0x03 | (s << 3);
        v.target_len = 2;
        run_plain(&v, 1);
        int n = 0; /* mov dword [esp-4], -1; push sreg; pop eax */
        v.target[n++] = 0xc7;
        v.target[n++] = 0x44;
        v.target[n++] = 0x24;
        v.target[n++] = 0xfc;
        put_le(v.target + n, 0xffffffff, 4);
        n += 4;
        v.target[n++] = pushes[s][0];
        if (pushes[s][1])
            v.target[n++] = pushes[s][1];
        v.target[n++] = 0x58;
        v.target_len  = n;
        run_plain(&v, 1);
    }
    group_end(name);
}

/* ---- every descriptor type --------------------------------------------------------------- */

#define SCRATCH_RAW(access, flags) (0x900 | ((uint32_t) (access) << 12) | ((uint32_t) (flags) << 20))

/* LAR, LSL and VERR of the scratch descriptor as each of the 16 system
   types (S = 0): which ones LAR and LSL accept. Raw (LAR's undefined
   bits). */
static void
group_lar_systypes(void)
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
    struct variant v = { .group = "lar.systypes", .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
                         .fix_input = fix_ring, .arg2 = RF_NONE, .fx = FX_CUSTOM, .defmask = dm_fxarg };
    if (!ring_begin(v.group, 0, 0))
        return;
    memcpy(v.target, code, sizeof(code));
    v.target_len = sizeof(code);
    for (int t = 0; t < 16; t++) {
        v.arg = SCRATCH_RAW(0x80 | t, 0x4);
        run_plain(&v, 1);
    }
    ring_end(v.group, 0, 0);
}

/* The 16 code and data types (S = 1), at DPL 0 from ring 0 and DPL 3
   from ring 3: VERR, VERW, LAR, and a load into FS, a read and a write
   through it. */
static void
group_seg_types(void)
{
    struct variant v = { .group = "seg.types", .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
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
    if (!ring_begin(v.group, 0, 0))
        return;
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
    ring_end(v.group, 0, 0);
}

/* ---- IDT gates that must fail, and the IDT's limit ---------------------------------------- */

/* INT through a missing gate (#NP), a gate of an invalid type, one whose
   selector is data or null (#GP), and a 386 trap gate, from rings 0 and
   3. */
static void
group_idt_types(void)
{
    static struct ring_seq seqs[10];
    int                    n = 0;
    for (int r = 0; r <= 3; r += 3)
        for (int vec = 0x39; vec <= 0x3d; vec++) {
            struct ring_seq q = { r, 0, 0, 0, RF_NONE, 1, 2, { 0xcd, (uint8_t) vec } };
            seqs[n++]         = q;
        }
    ring_run("idt.types", seqs, n, 0, 0, 0);
}

/* The IDT's limit: an INT past it is #GP (with the IDT bit in the error
   code), and a gate only partly inside it counts as outside. Variants set
   the limit before the run (arg3: the limit) and put it back after. */
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
group_idt_limit(void)
{
    static const struct {
        uint8_t  ring, vec;
        uint16_t limit;
    } cases[] = {
        { 0, 0x31, 0x31 * 8 - 1 }, /* 31h just past the limit */
        { 0, 0x30, 0x31 * 8 - 1 }, /* 30h the last one inside */
        { 0, 0x30, 0x30 * 8 + 3 }, /* 30h half inside: #GP */
        { 0, 0x03, 0x31 * 8 - 1 },
        { 3, 0x31, 0x31 * 8 - 1 }, /* from ring 3 */
        { 3, 0x30, 0x31 * 8 - 1 },
    };
    struct variant v = { .group = "idt.limit", .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = idt_limit_before,
                         .after_run = idt_limit_after, .fix_input = fix_ring, .fx = FX_CUSTOM, .defmask = dm_fxarg,
                         .fx_arg = 1 };
    if (!ring_begin(v.group, 0, 0))
        return;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        v.target[0]  = 0xcd;
        v.target[1]  = cases[i].vec;
        v.target_len = 2;
        v.ring       = cases[i].ring;
        v.arg3       = cases[i].limit;
        v.arg2       = RF_NONE;
        run_plain(&v, 1);
    }
    ring_end(v.group, 0, 0);
}

/* ---- the end of a 16-bit code segment, and prefixes inside one ----------------------------- */

/* 16-bit code placed so its last byte is the segment's last (FFFFh), or
   one byte past it: running off the end is #GP. */
static void
group_code16_limit(void)
{
    static const struct {
        uint8_t len, b[4];
    } bodies[] = { { SEQ(0x01, 0xd1) }, { SEQ(0x90, 0x90, 0x90) }, { SEQ(0x66, 0x01, 0xd1) } };
    const char    *name = "code16.limit";
    struct variant v    = { .group = name, .code16 = 1, .faults_ok = 1 };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        for (int over = 0; over < 3; over++) {
            memcpy(v.target, bodies[i].b, bodies[i].len);
            v.target_len = bodies[i].len;
            /* the body plus the final o32 RETF (2 bytes), and no producer */
            v.code16_ip = (uint16_t) (0x10000 - (bodies[i].len + 2) + over);
            v.producer  = 0;
            v.boundary  = 0;
            run_inputs(&v, 1);
        }
    }
    v.code16_ip = 0;
    group_end(name);
}

/* Operand- and address-size prefixes in 16-bit code: CALL rel32 and RET
   with 66 (32-bit return addresses), PUSH imm with and without, LOOP by
   ECX (67) and by CX, PUSHFD. */
static void
fix_c16loop(const struct variant *v)
{
    g_in.ecx = 1 + (rnd() & 3);
    if (v->arg2) /* LOOP by CX: the upper half must stay out of it */
        g_in.ecx |= rnd() & 0xffff0000u;
}

static void
group_code16_opsize(void)
{
    static const struct {
        uint8_t fix, len, b[12];
    } seqs[] = {
        { 0, SEQ(0x66, 0xe8, 0x01, 0x00, 0x00, 0x00, 0x90, 0x66, 0x5e) },       /* call rel32 over a nop; pop esi */
        { 0, SEQ(0xe8, 0x01, 0x00, 0x90, 0x5e) },                               /* call rel16; pop si */
        { 0, SEQ(0x66, 0xe8, 0x02, 0x00, 0x00, 0x00, 0xeb, 0x02, 0x66, 0xc3) }, /* call; jmp over; L: ret with 66 */
        { 0, SEQ(0x66, 0x6a, 0xff, 0x66, 0x58) },                               /* push dword -1; pop eax */
        { 0, SEQ(0x6a, 0xff, 0x58) },                                           /* push word -1; pop ax */
        { 1, SEQ(0x46, 0x67, 0xe2, 0xfc) },                                     /* L: inc si; loop L by ECX */
        { 2, SEQ(0x46, 0xe2, 0xfd) },                                           /* L: inc si; loop L by CX */
        { 0, SEQ(0x66, 0x9c, 0x66, 0x5e) },                                     /* pushfd; pop esi */
    };
    const char    *name = "code16.opsize";
    struct variant v    = { .group = name, .code16 = 1, .faults_ok = 1, .fx = FX_CUSTOM, .defmask = dm_fxarg, .fx_arg = 1 };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.fix_input  = seqs[i].fix ? fix_c16loop : 0;
        v.arg2       = seqs[i].fix == 2;
        v.reg_fix    = 1;
        v.fx_arg     = i != 7; /* PUSHFD holds the producer's undefined flags */
        run_plain(&v, 2);
    }
    group_end(name);
}

/* ---- SGDT and friends --------------------------------------------------------------------- */

/* SGDT and SIDT with 32- and 16-bit operand size: the limit stays in
   memory, the base is read back into ESI and cleared there (it is where
   this build put the table), and made relative to the real one; the byte
   after the six is the buffer's own. SLDT and STR to a 32-bit register
   (the upper half), a 16-bit one, and memory. */
static void
canon_sgdt(const struct variant *v, const struct kin *in, struct kout *o)
{
#ifndef HOSTTEST
    struct {
        uint16_t limit;
        uint32_t base;
    } __attribute__((packed)) t;
    if (v->arg2)
        __asm__ volatile("sidt %0" : "=m"(t));
    else
        __asm__ volatile("sgdt %0" : "=m"(t));
    o->esi -= t.base;
#endif
}

static void
fix_sgdt(const struct variant *v)
{
#ifdef HOSTTEST
    g_skip = 1; /* UMIP, and a table of Linux's */
#endif
    fix_mem_ebx(v);
}

static void
group_sgdt_forms(void)
{
    static const struct {
        uint8_t len, b[16];
    } seqs[] = {
        { SEQ(0x0f, 0x01, 0x03, 0x8b, 0x73, 0x02, 0xc7, 0x43, 0x02, 0, 0, 0, 0) },       /* sgdt */
        { SEQ(0x66, 0x0f, 0x01, 0x03, 0x8b, 0x73, 0x02, 0xc7, 0x43, 0x02, 0, 0, 0, 0) }, /* o16 sgdt */
        { SEQ(0x0f, 0x01, 0x0b, 0x8b, 0x73, 0x02, 0xc7, 0x43, 0x02, 0, 0, 0, 0) },       /* sidt */
        { SEQ(0x66, 0x0f, 0x01, 0x0b, 0x8b, 0x73, 0x02, 0xc7, 0x43, 0x02, 0, 0, 0, 0) }, /* o16 sidt */
        { SEQ(0x0f, 0x00, 0xc1) },       /* sldt ecx */
        { SEQ(0x66, 0x0f, 0x00, 0xc1) }, /* sldt cx */
        { SEQ(0x0f, 0x00, 0x03) },       /* sldt [ebx] */
        { SEQ(0x0f, 0x00, 0xc9) },       /* str ecx */
        { SEQ(0x66, 0x0f, 0x00, 0xc9) }, /* str cx */
        { SEQ(0x0f, 0x00, 0x0b) },       /* str [ebx] */
    };
    const char    *name = "sgdt.forms";
    struct variant v    = { .group = name, .faults_ok = 1, .fx = FX_CUSTOM, .defmask = dm_fxarg,
                            .fix_input = fix_sgdt, .arg = MEM_OFF };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.canon      = i < 4 ? canon_sgdt : 0;
        v.arg2       = i >= 2 && i < 4;
        v.fx_arg     = i != 4 && i != 7; /* a 32-bit register's upper half is the model's own */
        run_plain(&v, 1);
    }
    group_end(name);
}

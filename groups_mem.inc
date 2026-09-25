/*
 * Addressing and memory forms. Included into groups.inc after
 * groups_exhaust.inc; append only.
 */

/* ---- LEA: every ModRM and SIB addressing form ------------------------------ */

static const uint8_t  lea_d8[]  = { 0x00, 0x01, 0x7f, 0x80, 0xff };
static const uint32_t lea_d32[] = { 0x00000000, 0x7fffffff, 0x80000000, 0xffffffff, 0x12345678 };
static const uint16_t lea_d16[] = { 0x0000, 0x7fff, 0x8000, 0xffff };

/* The displacement for a variant: the lists above in turn, so every size
   of displacement meets each of its edges across the group. */
static int
put_disp(uint8_t *t, int n, int bytes, unsigned k)
{
    if (bytes == 1)
        t[n++] = lea_d8[k % sizeof(lea_d8)];
    else if (bytes == 2) {
        put_le(t + n, lea_d16[k % (sizeof(lea_d16) / sizeof(lea_d16[0]))], 2);
        n += 2;
    } else if (bytes == 4) {
        put_le(t + n, lea_d32[k % (sizeof(lea_d32) / sizeof(lea_d32[0]))], 4);
        n += 4;
    }
    return n;
}

/* LEA computes the address and nothing else, so every form runs with the
   registers as they come. A base of ESP runs on the private stack, and the
   result is taken relative to the buffer like ESP itself. */
static void
group_lea(void)
{
    const char    *name = "lea";
    struct variant v    = { .group = name };
    unsigned       k    = 0;
    group_begin(name);

    /* 32-bit addressing without SIB: mod 0-2, rm 0-7 but 4. */
    for (int mod = 0; mod < 3; mod++) {
        for (int rm = 0; rm < 8; rm++) {
            if (rm == 4)
                continue;
            int dsize = mod == 1 ? 1 : mod == 2 || rm == 5 ? 4 : 0;
            int n     = 0;
            v.target[n++] = 0x8d;
            v.target[n++] = (mod << 6) | (R_EAX << 3) | rm;
            n             = put_disp(v.target, n, dsize, k++);
            v.target_len  = n;
            v.stack       = 0;
            v.norm        = 0;
            run_plain(&v, 2);
        }
    }
    /* 32-bit with SIB: every base (index ECX*4), and every index and scale
       (base EBX), at each mod. Base 5 with mod 0 is disp32 and no base;
       index 4 is no index. */
    for (int mod = 0; mod < 3; mod++) {
        for (int form = 0; form < 8 + 32; form++) {
            int base  = form < 8 ? form : R_EBX;
            int index = form < 8 ? R_ECX : (form - 8) & 7;
            int scale = form < 8 ? 2 : (form - 8) >> 3;
            int dsize = mod == 1 ? 1 : mod == 2 || (mod == 0 && base == 5) ? 4 : 0;
            int n     = 0;
            v.target[n++] = 0x8d;
            v.target[n++] = (mod << 6) | (R_EAX << 3) | 4;
            v.target[n++] = (scale << 6) | (index << 3) | base;
            n             = put_disp(v.target, n, dsize, k++);
            v.target_len  = n;
            v.stack       = base == R_ESP;
            v.norm        = base == R_ESP ? 1 << R_EAX : 0;
            run_plain(&v, 2);
        }
    }
    v.stack = 0;
    v.norm  = 0;
    /* The destination is the base register itself. */
    {
        static const struct {
            uint8_t len, b[4];
        } seqs[] = {
            { SEQ(0x8d, 0x1c, 0x9b) },       /* lea ebx, [ebx+ebx*4] */
            { SEQ(0x8d, 0x76, 0x80) },       /* lea esi, [esi-128] */
            { SEQ(0x8d, 0x3c, 0x3f) },       /* lea edi, [edi+edi] */
            { SEQ(0x8d, 0x6c, 0x2d, 0x7f) }, /* lea ebp, [ebp+ebp+127] */
        };
        for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
            memcpy(v.target, seqs[i].b, seqs[i].len);
            v.target_len = seqs[i].len;
            run_plain(&v, 2);
        }
    }
    /* 16-bit addressing: mod 0-2, rm 0-7 (mod 0 rm 6 is disp16), into a
       32-bit register (the address zero-extended) and a 16-bit one. */
    for (int size16 = 0; size16 < 2; size16++) {
        for (int mod = 0; mod < 3; mod++) {
            for (int rm = 0; rm < 8; rm++) {
                int dsize = mod == 1 ? 1 : mod == 2 || (mod == 0 && rm == 6) ? 2 : 0;
                int n     = 0;
                v.target[n++] = 0x67;
                if (size16)
                    v.target[n++] = 0x66;
                v.target[n++] = 0x8d;
                v.target[n++] = (mod << 6) | (R_EAX << 3) | rm;
                n             = put_disp(v.target, n, dsize, k++);
                v.target_len  = n;
                run_plain(&v, 2);
            }
        }
    }
    /* A 16-bit destination with 32-bit addressing: the low half of the
       address. */
    {
        static const struct {
            uint8_t len, b[8];
        } seqs[] = {
            { SEQ(0x66, 0x8d, 0x84, 0x8b, 0xff, 0xff, 0x00, 0x00) }, /* lea ax, [ebx+ecx*4+0FFFFh] */
            { SEQ(0x66, 0x8d, 0x46, 0x80) },                         /* lea ax, [esi-128] */
        };
        for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
            memcpy(v.target, seqs[i].b, seqs[i].len);
            v.target_len = seqs[i].len;
            run_plain(&v, 2);
        }
    }
    group_end(name);
}

/* ---- ALU operations on memory ---------------------------------------------- */

static const uint8_t  imm8_edges[]  = { 0x00, 0x01, 0x7f, 0x80, 0xff };
static const uint32_t imm32_edges[] = { 0x00000000, 0x7fffffff, 0x80000000, 0xffffffff, 12345 };

/* ADD OR ADC SBB AND SUB XOR CMP with memory: r/m,r and r,r/m, 80/81/83
   with immediates at their edges (rotating), every size, LOCK on each
   memory destination (#UD for CMP), and a misaligned dword. ADC and SBB
   read CF: every producer, unlocked. */
static void
group_alu_mem(void)
{
    const char    *name = "alu.mem";
    struct variant v    = { .group = name, .fx = FX_ALU, .fix_input = fix_mem_ebx, .arg = MEM_OFF };
    unsigned       k    = 0;
    group_begin(name);
    for (int op = 0; op < 8; op++) {
        int reads_cf = op == 2 || op == 3;
        for (int form = 0; form < 4; form++) { /* op r/m,r; op r,r/m; 80/81 imm; 83 imm8 */
            for (int size = 8; size <= 32; size *= 2) {
                if (form == 3 && size == 8)
                    continue;
                for (int lock = 0; lock < (form == 1 ? 1 : 2); lock++) {
                    int n = 0;
                    if (lock)
                        v.target[n++] = 0xf0;
                    if (size == 16)
                        v.target[n++] = 0x66;
                    int w = size != 8;
                    switch (form) {
                        case 0:
                        case 1:
                            v.target[n++] = (op << 3) | (form << 1) | w;
                            v.target[n++] = 0x03 | ((size == 8 ? 6 : R_EDX) << 3); /* DH / EDX, [ebx] */
                            break;
                        case 2:
                            v.target[n++] = 0x80 | w;
                            v.target[n++] = 0x03 | (op << 3);
                            if (size == 8)
                                v.target[n++] = imm8_edges[k % sizeof(imm8_edges)];
                            else {
                                put_le(v.target + n, imm32_edges[k % 5], size / 8);
                                n += size / 8;
                            }
                            break;
                        default:
                            v.target[n++] = 0x83;
                            v.target[n++] = 0x03 | (op << 3);
                            v.target[n++] = imm8_edges[k % sizeof(imm8_edges)];
                            break;
                    }
                    k++;
                    v.target_len = n;
                    v.fx_arg     = op;
                    v.faults_ok  = lock && op == 7;
                    if (reads_cf && !lock)
                        run_flags(&v, 1);
                    else
                        run_plain(&v, 2);
                }
            }
        }
        /* op [ebx+31], edx: misaligned */
        v.target[0]  = (op << 3) | 1;
        v.target[1]  = 0x43 | (R_EDX << 3);
        v.target[2]  = 0x1f;
        v.target_len = 3;
        v.faults_ok  = 0;
        run_plain(&v, 2);
    }
    group_end(name);
}

/* ---- INC DEC NEG NOT TEST ------------------------------------------------- */

/* fx_arg: 0 INC/DEC (every flag but CF, which passes through), 1 NEG (all),
   2 NOT (none), 3 TEST (all but AF). */
static int
dm_unary(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    if (d->fault != 0xff)
        return 1;
    switch (v->fx_arg) {
        case 0: *undef &= F_CF; break;
        case 1: *undef = 0; break;
        case 3: *undef = F_AF; break;
        default: break;
    }
    return 1;
}

static void
group_unary_mem(void)
{
    /* (opcode byte for 8-bit, ModRM reg field, kind): kind as fx_arg; reg
       0xff for TEST r/m, r (84/85). */
    static const struct {
        uint8_t op, reg, kind, imm;
    } ops[] = {
        { 0xfe, 0, 0, 0 }, /* INC */
        { 0xfe, 1, 0, 0 }, /* DEC */
        { 0xf6, 3, 1, 0 }, /* NEG */
        { 0xf6, 2, 2, 0 }, /* NOT */
        { 0xf6, 0, 3, 1 }, /* TEST r/m, imm */
        { 0x84, 0xff, 3, 0 }, /* TEST r/m, r */
    };
    const char    *name = "unary.mem";
    struct variant v    = { .group = name, .fx = FX_CUSTOM, .defmask = dm_unary };
    unsigned       k    = 0;
    group_begin(name);
    for (unsigned o = 0; o < sizeof(ops) / sizeof(ops[0]); o++) {
        for (int size = 8; size <= 32; size *= 2) {
            /* register (AH / ECX), memory, locked memory */
            for (int form = 0; form < 3; form++) {
                if (form == 2 && ops[o].kind == 3)
                    continue; /* no LOCK TEST */
                int n = 0;
                if (form == 2)
                    v.target[n++] = 0xf0;
                n   = put_op(v.target, n, size, ops[o].op);
                int reg = ops[o].reg == 0xff ? (size == 8 ? 2 : R_EDX) : ops[o].reg;
                if (form == 0)
                    v.target[n++] = MODRM_RR(reg, size == 8 ? 4 : R_ECX);
                else
                    v.target[n++] = 0x03 | (reg << 3);
                if (ops[o].imm) {
                    if (size == 8)
                        v.target[n++] = imm8_edges[k % sizeof(imm8_edges)];
                    else {
                        put_le(v.target + n, imm32_edges[k % 5], size / 8);
                        n += size / 8;
                    }
                }
                k++;
                v.target_len = n;
                v.fx_arg     = ops[o].kind;
                v.fix_input  = form ? fix_mem_ebx : 0;
                v.arg        = MEM_OFF;
                if (ops[o].kind == 0)
                    run_flags(&v, 1); /* CF passes through */
                else
                    run_plain(&v, 2);
            }
        }
    }
    /* The one-byte INC and DEC (40-4F), 32- and 16-bit. */
    for (int size16 = 0; size16 < 2; size16++) {
        for (int r = 0; r < 16; r++) {
            if ((r & 7) == R_ESP)
                continue;
            int n = 0;
            if (size16)
                v.target[n++] = 0x66;
            v.target[n++] = 0x40 | r;
            v.target_len  = n;
            v.fx_arg      = 0;
            v.fix_input   = 0;
            run_plain(&v, 1);
        }
    }
    group_end(name);
}

/* ---- Jcc, JCXZ, LOOP ------------------------------------------------------ */

/* ECX for JECXZ/JCXZ: 0, 1, 10000h (CX zero, ECX not) and FFFFh, then
   random. */
static void
fix_jcxz(const struct variant *v)
{
    static const uint32_t edges[] = { 0, 1, 0x10000, 0xffff };
    int                   e       = edge(4);
    g_in.ecx                      = e >= 0 ? edges[e] : rnd_operand();
}

/* The count for LOOP: small edges (and for CX, 0: 65536 times round), then
   a random small one. EDI a few steps ahead of ESI, where LOOPNE's
   comparison comes true. arg3 bit 0: 16-bit addressing (CX). */
static void
fix_loop(const struct variant *v)
{
    static const uint16_t edges32[] = { 1, 2, 3, 7 }, edges16[] = { 1, 2, 0 };
    int                   a16       = v->arg3 & 1, e = edge(a16 ? 3 : 4);
    uint32_t              c         = e >= 0 ? (a16 ? edges16[e] : edges32[e]) : 1 + rnd() % 9;
    g_in.ecx                        = a16 ? (rnd() & 0xffff0000u) | c : c;
    g_in.edi                        = g_in.esi + 1 + rnd() % 4;
}

/* LOOP's body ends in INC (CF passes through) or CMP (every flag). */
static int
dm_loop(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    if (v->fx_arg)
        *undef = 0;
    else
        *undef &= F_CF;
    return 1;
}

static void
group_jcc_loop(void)
{
    const char    *name = "jcc.loop";
    struct variant v    = { .group = name, .fx = FX_SETCC };
    group_begin(name);
    /* jcc +2; mov al, 1: AL is 1 only if the branch was not taken. */
    for (int cc = 0; cc < 16; cc++) {
        for (int near = 0; near < 2; near++) {
            int n = 0;
            if (near) {
                v.target[n++] = 0x0f;
                v.target[n++] = 0x80 | cc;
                put_le(v.target + n, 2, 4);
                n += 4;
            } else {
                v.target[n++] = 0x70 | cc;
                v.target[n++] = 0x02;
            }
            v.target[n++] = 0xb0;
            v.target[n++] = 0x01;
            v.target_len  = n;
            v.fx_arg      = cc;
            run_flags(&v, 1);
        }
    }
    /* JECXZ and JCXZ */
    {
        static const struct {
            uint8_t len, b[5];
        } seqs[] = { { SEQ(0xe3, 0x02, 0xb0, 0x01) }, { SEQ(0x67, 0xe3, 0x02, 0xb0, 0x01) } };
        v.fx        = FX_NONE;
        v.fix_input = fix_jcxz;
        v.reg_fix   = 1;
        for (int i = 0; i < 2; i++) {
            memcpy(v.target, seqs[i].b, seqs[i].len);
            v.target_len = seqs[i].len;
            run_keep(&v, 5);
        }
    }
    /* LOOP, LOOPE, LOOPNE, backwards over a real body, 32- and 16-bit
       counts. */
    {
        static const struct {
            uint8_t cmp, len, b[6];
        } seqs[] = {
            { 0, SEQ(0x46, 0xe2, 0xfd) },             /* L: inc esi; loop L */
            { 1, SEQ(0x46, 0x39, 0xc0, 0xe1, 0xfb) }, /* L: inc esi; cmp eax, eax; loope L (runs out) */
            { 1, SEQ(0x46, 0x39, 0xfe, 0xe1, 0xfb) }, /* L: inc esi; cmp esi, edi; loope L (leaves at once) */
            { 1, SEQ(0x46, 0x39, 0xfe, 0xe0, 0xfb) }, /* L: inc esi; cmp esi, edi; loopne L (leaves when equal) */
        };
        v.fx        = FX_CUSTOM;
        v.defmask   = dm_loop;
        v.fix_input = fix_loop;
        for (int a16 = 0; a16 < 2; a16++) {
            for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
                int n = 0;
                if (a16) { /* the prefix goes on the LOOP instruction itself */
                    memcpy(v.target, seqs[i].b, seqs[i].len - 2);
                    n             = seqs[i].len - 2;
                    v.target[n++] = 0x67;
                    v.target[n++] = seqs[i].b[seqs[i].len - 2];
                    v.target[n++] = seqs[i].b[seqs[i].len - 1] - 1; /* one byte further back */
                } else {
                    memcpy(v.target, seqs[i].b, seqs[i].len);
                    n = seqs[i].len;
                }
                v.target_len = n;
                v.fx_arg     = seqs[i].cmp;
                v.arg3       = a16;
                run_keep(&v, a16 ? 4 : 5);
            }
        }
    }
    group_end(name);
}

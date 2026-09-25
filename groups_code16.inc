/* 16-bit code placed so its last byte is the segment's last (FFFFh), or
   one byte past it: running off the end is #GP. */
static void
code16_limit(const char *name)
{
    static const struct {
        uint8_t len, b[4];
    } bodies[] = { { SEQ(0x01, 0xd1) }, { SEQ(0x90, 0x90, 0x90) }, { SEQ(0x66, 0x01, 0xd1) },
                   /* two-byte forms an interpreter's length guess got wrong at the limit */
                   { SEQ(0x04, 0x05) }, { SEQ(0x3c, 0x00) }, { SEQ(0x0f, 0xa2) } };
    struct variant v = { .group = name, .code16 = 1, .faults_ok = 1 };
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
code16_opsize(const char *name)
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
    struct variant v = { .group = name, .code16 = 1, .faults_ok = 1, .fx = FX_CUSTOM, .defmask = dm_fxarg, .fx_arg = 1 };
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.fix_input  = seqs[i].fix ? fix_c16loop : 0;
        v.arg2       = seqs[i].fix == 2;
        v.reg_fix    = 1;
        v.fx_arg     = i != 7; /* PUSHFD holds the producer's undefined flags */
        run_plain(&v, 2);
    }
}

/*
 * Code in a 16-bit protected-mode segment: what Windows 3.x and 9x and
 * DOS extenders run. The harness far-calls the producer and the target
 * through a 16-bit code descriptor (see code16_base()), so operand and
 * address size default to 16 bits and the 66/67 prefixes select 32.
 * Included into groups.inc after groups_mem.inc. Not run by
 * the Linux build (no far calls into our own GDT there).
 */

static void
group_code16(void)
{
    const char    *name = "code16";
    struct variant v    = { .group = name, .code16 = 1 };
    group_begin(name);

    /* ALU, register forms, 16-bit by default and 32-bit with 66. ADC and
       SBB after every producer. */
    for (int op = 0; op < 8; op++) {
        for (int o32 = 0; o32 < 2; o32++) {
            int n = 0;
            if (o32)
                v.target[n++] = 0x66;
            v.target[n++] = (op << 3) | 1;
            v.target[n++] = MODRM_RR(R_EDX, R_EAX); /* op ax, dx */
            v.target_len  = n;
            v.fx          = FX_ALU;
            v.fx_arg      = op;
            if (op == 2 || op == 3)
                run_flags(&v, 1);
            else
                run_plain(&v, 2);
        }
    }
    /* SETcc and Jcc (short, and near with a 16-bit displacement): every
       condition after every producer. */
    for (int cc = 0; cc < 16; cc++) {
        static const struct {
            uint8_t len, b[6];
        } forms[3] = { { SEQ(0x0f, 0x90, 0xc0) }, { SEQ(0x70, 0x02, 0xb0, 0x01) }, { SEQ(0x0f, 0x80, 0x02, 0x00, 0xb0, 0x01) } };
        for (int f = 0; f < 3; f++) {
            memcpy(v.target, forms[f].b, forms[f].len);
            v.target[f == 1 ? 0 : 1] |= cc;
            v.target_len = forms[f].len;
            v.fx         = FX_SETCC;
            v.fx_arg     = cc;
            run_flags(&v, 1);
        }
    }
    v.fx = FX_NONE;
    /* Every 16-bit addressing form through LEA, and a 32-bit one with 67. */
    unsigned k = 0;
    for (int mod = 0; mod < 3; mod++) {
        for (int rm = 0; rm < 8; rm++) {
            int dsize = mod == 1 ? 1 : mod == 2 || (mod == 0 && rm == 6) ? 2 : 0;
            int n     = 0;
            v.target[n++] = 0x8d;
            v.target[n++] = (mod << 6) | (R_EAX << 3) | rm;
            n             = put_disp(v.target, n, dsize, k++);
            v.target_len  = n;
            run_plain(&v, 1);
        }
    }
    {
        static const struct {
            uint8_t len, b[7];
        } seqs[] = {
            { SEQ(0x67, 0x8d, 0x04, 0x8b) },       /* lea ax, [ebx+ecx*4] */
            { SEQ(0x66, 0x67, 0x8d, 0x44, 0x4b, 0x80) }, /* lea eax, [ebx+ecx*2-128] */
            { SEQ(0x0f, 0xb6, 0xc1) },             /* movzx ax, cl */
            { SEQ(0x66, 0x0f, 0xb6, 0xc5) },       /* movzx eax, ch */
            { SEQ(0x0f, 0xbe, 0xc1) },             /* movsx ax, cl */
            { SEQ(0x66, 0x0f, 0xbf, 0xc1) },       /* movsx eax, cx */
            { SEQ(0x50, 0x5b) },                   /* push ax; pop bx */
            { SEQ(0x66, 0x50, 0x66, 0x5b) },       /* push eax; pop ebx */
            { SEQ(0x6a, 0x80, 0x5e) },             /* push -128; pop si */
            { SEQ(0x98) },                         /* cbw */
            { SEQ(0x66, 0x98) },                   /* cwde */
            { SEQ(0x99) },                         /* cwd */
            { SEQ(0xe8, 0x02, 0x00, 0xeb, 0x02, 0x46, 0xc3) }, /* call L; jmp over; L: inc si; ret; over: */
        };
        for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
            memcpy(v.target, seqs[i].b, seqs[i].len);
            v.target_len = seqs[i].len;
            /* call/ret: INC SI keeps CF from the producer, writes the rest */
            v.fx      = seqs[i].b[0] == 0xe8 ? FX_CUSTOM : FX_NONE;
            v.defmask = dm_loop;
            v.fx_arg  = 0;
            run_plain(&v, 2);
        }
    }
    /* PUSHF of every lazy state: the 16-bit flags into SI. */
    v.target[0]  = 0x9c; /* pushf */
    v.target[1]  = 0x5e; /* pop si */
    v.target_len = 2;
    v.fx         = FX_CUSTOM;
    v.defmask    = dm_popf;
    v.arg2       = 1;
    run_flags(&v, 1);
    /* MUL and DIV by CX, with the division edges. */
    for (int op = 4; op < 8; op++) {
        v.target[0]  = 0xf7;
        v.target[1]  = MODRM_RR(op, R_ECX);
        v.target_len = 2;
        set_fix_rm(&v, R_ECX, op >= 6 ? fix_div : fix_mul);
        v.arg3      = 16 | ((op == 5 || op == 7) << 8);
        v.defmask   = op >= 6 ? dm_div : dm_mul;
        v.faults_ok = op >= 6;
        run_keep(&v, op >= 6 ? 13 : 7);
    }
    v.faults_ok = 0;
    /* String instructions over SI and DI in the low buffer, REP by CX. */
    {
        static const struct {
            uint8_t rep, len, b[2];
        } seqs[] = {
            { 0, SEQ(0xa4) }, { 1, SEQ(0xf3, 0xa5) }, { 1, SEQ(0xf3, 0xa6) }, { 1, SEQ(0xf2, 0xae) },
            { 1, SEQ(0xf3, 0xab) }, { 0, SEQ(0xad) },
        };
        v.fx        = FX_CUSTOM;
        v.defmask   = dm_str;
        v.fix_input = fix_str;
        v.reg_fix   = 0;
        v.norm      = 0;
        v.arg3      = 1; /* 16-bit addressing */
        for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
            memcpy(v.target, seqs[i].b, seqs[i].len);
            v.target_len = seqs[i].len;
            uint8_t op   = seqs[i].b[seqs[i].len - 1];
            v.arg        = op & 1 ? 16 : 8;
            v.arg2       = seqs[i].rep;
            v.fx_arg     = (op & 0xfe) == 0xa6 || (op & 0xfe) == 0xae;
            run_keep(&v, 6);
        }
    }
    /* LOOP by CX over a real body (CX = 0: 65536 times). */
    {
        static const struct {
            uint8_t cmp, len, b[5];
        } seqs[] = { { 0, SEQ(0x46, 0xe2, 0xfd) }, { 1, SEQ(0x46, 0x39, 0xfe, 0xe0, 0xfb) } };
        v.defmask   = dm_loop;
        v.fix_input = fix_loop;
        v.arg3      = 1;
        for (int i = 0; i < 2; i++) {
            memcpy(v.target, seqs[i].b, seqs[i].len);
            v.target_len = seqs[i].len;
            v.fx_arg     = seqs[i].cmp;
            run_keep(&v, 4);
        }
    }
    code16_limit(name);
    code16_opsize(name);
    group_end(name);
}

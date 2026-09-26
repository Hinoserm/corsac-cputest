/*
 * MMX (P55C, Pentium II, K6 on), the MMX/x87 handover (EMMS, FEMMS), and
 * AMD 3DNow! and its K6-2+/K6-III+ extensions. Included into groups.inc
 * after groups_ops.inc.
 *
 * Every variant loads MM0-MM7 from g_mmx_in after an FNINIT and stores them
 * to g_mmx_out afterwards; the stored registers go into the memory CRC.
 */

#define CPUID_MMX     (1u << 23)
#define CPUID_FXSR    (1u << 24)
#define EXT_3DNOW     (1u << 31)
#define EXT_3DNOWEXT  (1u << 30)

/* The MMX register operand (0-7) or [ebx] (RM_MEM, 8 bytes at the buffer
   offset MEM_OFF). */
static int
put_mm_modrm(uint8_t *t, int n, int reg, int rm)
{
    return put_modrm(t, n, reg, rm);
}

static void
set_mm(int r, uint32_t lo, uint32_t hi)
{
    if (r == RM_MEM) {
        put_le(g_in.buf + MEM_OFF, lo, 4);
        put_le(g_in.buf + MEM_OFF + 4, hi, 4);
    } else {
        g_mmx_in[2 * r]     = lo;
        g_mmx_in[2 * r + 1] = hi;
    }
}

/* Shift counts from an MMX register or memory: every edge for words,
   dwords and the qword in turn, then one with the upper half set (which
   shifts everything out), then one random. */
static const uint32_t mm_counts[] = { 0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 255 };
#define N_MM_COUNTS (sizeof(mm_counts) / sizeof(mm_counts[0]))

static void
fix_mm_count(const struct variant *v)
{
    int      e  = edge(N_MM_COUNTS + 1);
    uint32_t lo = e >= 0 && e < (int) N_MM_COUNTS ? mm_counts[e] : rnd() & 0x3f;
    uint32_t hi = e == (int) N_MM_COUNTS ? 1 + (rnd() & 0xff) : 0;
    if (v->arg2 == RM_MEM)
        g_in.ebx = (uint32_t) g_buf + MEM_OFF;
    set_mm(v->arg2, lo, hi);
}

/* The Cyrix extended MMX instructions (6x86MX, MII) at 0F 50-5E, enabled by CCR7 bit 0 (with
   MAPEN in CCR3 to reach it) and #UD without it. Register forms where
   they exist, memory for all. arg2: enabled. */
#ifndef HOSTTEST
static uint8_t
cx_read(uint8_t i)
{
    outb(0x22, i);
    return inb(0x23);
}

static void
cx_write(uint8_t i, uint8_t val)
{
    outb(0x22, i);
    outb(0x23, val);
}

static void
emmi_set(int on)
{
    uint8_t ccr3 = cx_read(0xc3);
    cx_write(0xc3, (ccr3 & 0x0f) | 0x10); /* MAPEN */
    uint8_t ccr7 = cx_read(0xeb);
    cx_write(0xeb, on ? ccr7 | 1 : ccr7 & ~1);
    cx_write(0xc3, ccr3);
}

static void
emmi_before(const struct variant *v)
{
    emmi_set(v->arg2);
}
#endif

static void
mmx_emmi(const char *name)
{
    static const uint8_t regops[] = { 0x50, 0x51, 0x52, 0x55, 0x59, 0x5d }; /* paveb paddsiw pmagw psubsiw pmulhrw pmulhriw */
    static const uint8_t memops[] = { 0x54, 0x58, 0x5a, 0x5b, 0x5c, 0x5e }; /* pdistib pmvzb pmvnzb pmvlzb pmvgezb pmachriw */
    static const uint8_t pairs[3][2] = { { 0, 1 }, { 3, 5 }, { 6, RM_MEM } };
    struct variant       v           = { .group = name, .mmx = 1, .faults_ok = 1, .arg = MEM_OFF };
    if (!g_cpu.cyrix)
        return;
#ifndef HOSTTEST
    v.before_run = emmi_before;
#endif
    for (int on = 1; on >= 0; on--) {
        v.arg2 = on;
        for (unsigned o = 0; o < sizeof(regops) + sizeof(memops); o++) {
            int mem_only = o >= sizeof(regops);
            for (int p = mem_only ? 2 : 0; p < 3; p++) {
                v.target[0]  = 0x0f;
                v.target[1]  = mem_only ? memops[o - sizeof(regops)] : regops[o];
                v.target_len = put_mm_modrm(v.target, 2, pairs[p][0], pairs[p][1]);
                v.fix_input  = pairs[p][1] == RM_MEM ? fix_mem_ebx : 0;
                run_plain(&v, on ? 2 : 1);
                if (!on)
                    break; /* disabled: one form is enough */
            }
        }
    }
#ifndef HOSTTEST
    if (!g_counting)
        emmi_set(0);
#endif
}

/* ---- x87 ------------------------------------------------------------------ */

/* Every x87 variant starts with FNINIT (and FLDCW for another rounding
   mode), loads 7 or 8 qwords from the buffer onto the stack, runs the
   instruction, takes the tag and status words (FNSTENV: ESI, EDI), stores
   all eight registers back over the buffer with FSTP (empty ones as the
   indefinite) and takes the status word again (AX). An instruction that
   writes memory writes 64 bytes up, [EBX+40h], and the ten bytes there
   come back in ECX, EDX and BP; one that reads memory reads the last
   qword, [EBX+38h], which then isn't loaded onto the stack. Filling all
   eight registers moves every one of them, at every stack position, in
   and out of the compiled code in one block. */

#define CPUID_FPU  (1u << 0)
#define CPUID_CMOV (1u << 15)

enum {
    X_SRC  = 1 << 0,  /* reads [ebx+38h]: seven registers loaded */
    X_DST  = 1 << 1,  /* writes [ebx+40h] */
    X_INT  = 1 << 2,  /* [ebx+38h] holds an integer, not a double */
    X_RC   = 1 << 3,  /* run under all four rounding modes */
    X_P6   = 1 << 4,  /* FCMOVcc, FCOMI: CPUID CMOV */
    X_PUSH = 1 << 5,  /* pushes: seven registers loaded, and eight (overflow) */
    X_CC   = 1 << 6,  /* sets C0, C2 and C3 (compares, FXAM, FPREM); else they're undefined */
    X_APPX = 1 << 7,  /* transcendental: the result differs between models */
};

static const uint16_t x87_cw[4] = { 0x037f, 0x077f, 0x0b7f, 0x0f7f };

/* Doubles for the stack: zeros, small integers and halves (rounding),
   the integer limits (FIST overflow), large and tiny, infinities, a NaN,
   a denormal. */
static const uint32_t x87_doubles[][2] = {
    { 0x00000000, 0x00000000 }, { 0x00000000, 0x80000000 }, { 0x00000000, 0x3ff00000 },
    { 0x00000000, 0xbff00000 }, { 0x00000000, 0x3fe00000 }, { 0x00000000, 0x3ff80000 },
    { 0x00000000, 0x40040000 }, { 0x00000000, 0xc0040000 }, { 0x00000000, 0x40080000 },
    { 0x9999999a, 0x3fb99999 }, { 0x55555555, 0x3fd55555 }, { 0x20000000, 0x4202a05f },
    { 0x00000000, 0x43400000 }, { 0x00000001, 0x43400000 }, { 0x00000000, 0x43e00000 },
    { 0x00000000, 0xc3e00000 }, { 0x00000000, 0x43f00000 }, { 0x00000000, 0x40dfffe0 },
    { 0x00000000, 0xc0e00010 }, { 0xffe00000, 0x41dfffff }, { 0x00000000, 0xc1e00000 },
    { 0xcae1ce65, 0x7e37e43c }, { 0x1bd5f0b6, 0x01a56e1f }, { 0x00000000, 0x7ff00000 },
    { 0x00000000, 0xfff00000 }, { 0x00000000, 0x7ff80000 }, { 0x00000001, 0x00000000 },
    { 0xffffffff, 0x7fefffff }, { 0x54442d18, 0x400921fb }, { 0x00000000, 0x40efffe0 },
};
#define N_X87_DOUBLES (sizeof(x87_doubles) / sizeof(x87_doubles[0]))

/* Integers for FILD and FIADD: the limits of each size, 2^53 + 1 (not a
   double), and mixed patterns. */
static const uint32_t x87_ints[][2] = {
    { 0, 0 },          { 1, 0 },          { 0xffffffff, 0xffffffff }, { 0xffffffff, 0x7fffffff },
    { 0, 0x80000000 }, { 1, 0x00200000 }, { 0xffffffff, 0 },          { 0x9abcdef1, 0x12345678 },
    { 0x7fff, 0 },     { 0x8000, 0 },     { 0x7fffffff, 0 },          { 0x80000000, 0 },
};
#define N_X87_INTS (sizeof(x87_ints) / sizeof(x87_ints[0]))

/* A double between 2^-15 and 2^16, either sign. */
static void
x87_rnd_double(uint8_t *p)
{
    uint32_t hi = (0x3f000000 + (rnd() & 0x01ffffff)) | (rnd() & 0x80000000);
    put_le(p, rnd(), 4);
    put_le(p + 4, hi, 4);
}

/* arg3: the row, to start each one at a different place in the table. */
static void
fix_x87(const struct variant *v)
{
    g_in.ebx = (uint32_t) g_buf;
    for (int i = 0; i < 8; i++) {
        uint8_t *p = g_in.buf + 8 * i;
        if (i == 7 && (v->arg2 & X_INT)) {
            int      e = edge(N_X87_INTS);
            uint32_t k = e >= 0 ? (uint32_t) e : rnd() % (N_X87_INTS + 1);
            if (k < N_X87_INTS) {
                put_le(p, x87_ints[k][0], 4);
                put_le(p + 4, x87_ints[k][1], 4);
            } else {
                put_le(p, rnd(), 4);
                put_le(p + 4, rnd(), 4);
            }
        } else if (g_case < 2) {
            unsigned k = (g_case * 8 + i * 5 + v->arg3 * 7) % N_X87_DOUBLES;
            put_le(p, x87_doubles[k][0], 4);
            put_le(p + 4, x87_doubles[k][1], 4);
        } else
            x87_rnd_double(p);
    }
}

/* fx_arg: the rounding mode; arg: how many registers to load; arg2: the
   X_ flags. */
static void
emit_x87(struct emit *e, const struct variant *v)
{
    e8(e, 0xdb); /* fninit */
    e8(e, 0xe3);
    if (v->fx_arg) {
        e8(e, 0xd9); /* fldcw [x87_cw + 2*rc] */
        e8(e, 0x2d);
        e32(e, (uint32_t) &x87_cw[v->fx_arg]);
    }
    for (uint32_t i = 0; i < v->arg; i++) {
        e8(e, 0xdd); /* fld qword [ebx+8*i] */
        e8(e, 0x43);
        e8(e, 8 * i);
    }
    ebytes(e, v->target, v->target_len);
    ebytes(e, (const uint8_t[]) { 0xd9, 0x35 }, 2); /* fnstenv [g_fenv] */
    e32(e, (uint32_t) &g_fenv[0]);
    ebytes(e, (const uint8_t[]) { 0x0f, 0xb7, 0x35 }, 3); /* movzx esi, word [tag word] */
    e32(e, (uint32_t) &g_fenv[2]);
    ebytes(e, (const uint8_t[]) { 0x0f, 0xb7, 0x3d }, 3); /* movzx edi, word [status word] */
    e32(e, (uint32_t) &g_fenv[1]);
    if (v->arg2 & X_DST)
        ebytes(e, (const uint8_t[]) { 0x8b, 0x4b, 0x40, 0x8b, 0x53, 0x44, 0x0f, 0xb7, 0x6b, 0x48 }, 10); /* mov ecx, [ebx+40h]; mov edx, [ebx+44h]; movzx ebp, word [ebx+48h] */
    for (int i = 0; i < 8; i++) {
        e8(e, 0xdd); /* fstp qword [ebx+8*i] */
        e8(e, 0x5b);
        e8(e, 8 * i);
    }
    e8(e, 0xdf); /* fnstsw ax */
    e8(e, 0xe0);
}

/* C0, C2 and C3 are undefined after most instructions; C1 is not. The
   transcendentals round differently from model to model. */
static int
dm_x87(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    if (v->arg2 & X_APPX)
        return 0;
    if (!(v->arg2 & X_CC)) {
        d->edi &= ~0x4500u;
        d->eax &= ~0x4500u;
    }
    return 1;
}

/* The same memory forms inside a loop run 64 times, so the recompiler
   compiles the body while the test is running, with every general
   register live: ECX counts, EBX points at the buffer, ESI and EDI sum
   the integer FISTP writes, EDX the double FST writes, EBP the status
   words; the x87 stack holds arg registers from the buffer besides the
   two the body uses. One body per iteration:

     fild qword [ebx]          m64int in
     fadd qword [ebx+8]        m64fp arithmetic, each kind
     fmul qword [ebx+10h]
     fsub qword [ebx+18h]
     fdiv qword [ebx+20h]
     fld qword [ebx+28h]
     fcomp qword [ebx+30h]     compare, status into EBP
     fnstsw ax; xor ebp, eax
     fst qword [ebx+28h]       m64fp out, fed to the next FLD
     fistp qword [ebx]         m64int out, fed to the next FILD
     (jmp $+2 when arg3 is 1: a block boundary inside the loop)
     add esi, [ebx]; adc edi, [ebx+4]; xor edx, [ebx+28h]; rol edx, 1
     dec ecx; jnz

   After the loop the final status word goes to AX and the stack to the
   buffer as in the other x87 variants. */
static void
emit_x87_loop(struct emit *e, const struct variant *v)
{
    static const uint8_t body[] = {
        0xdf, 0x2b,             /* fild qword [ebx] */
        0xdc, 0x43, 0x08,       /* fadd qword [ebx+8] */
        0xdc, 0x4b, 0x10,       /* fmul qword [ebx+10h] */
        0xdc, 0x63, 0x18,       /* fsub qword [ebx+18h] */
        0xdc, 0x73, 0x20,       /* fdiv qword [ebx+20h] */
        0xdd, 0x43, 0x28,       /* fld qword [ebx+28h] */
        0xdc, 0x5b, 0x30,       /* fcomp qword [ebx+30h] */
        0xdf, 0xe0,             /* fnstsw ax */
        0x31, 0xc5,             /* xor ebp, eax */
        0xdd, 0x53, 0x28,       /* fst qword [ebx+28h] */
        0xdf, 0x3b,             /* fistp qword [ebx] */
    };
    static const uint8_t tail[] = {
        0x03, 0x33,             /* add esi, [ebx] */
        0x13, 0x7b, 0x04,       /* adc edi, [ebx+4] */
        0x33, 0x53, 0x28,       /* xor edx, [ebx+28h] */
        0xd1, 0xc2,             /* rol edx, 1 */
        0x49,                   /* dec ecx */
    };
    e8(e, 0xdb); /* fninit */
    e8(e, 0xe3);
    if (v->fx_arg) {
        e8(e, 0xd9); /* fldcw [x87_cw + 2*rc] */
        e8(e, 0x2d);
        e32(e, (uint32_t) &x87_cw[v->fx_arg]);
    }
    for (uint32_t i = 0; i < v->arg; i++) {
        e8(e, 0xdd); /* fld qword [ebx+38h]: the spare qword, arg deep */
        e8(e, 0x43);
        e8(e, 0x38);
    }
    e8(e, 0xb9); /* mov ecx, 64 */
    e32(e, 64);
    uint8_t *top = e->p;
    ebytes(e, body, sizeof(body));
    if (v->arg3) {
        e8(e, 0xeb); /* jmp $+2 */
        e8(e, 0x00);
    }
    ebytes(e, tail, sizeof(tail));
    e8(e, 0x0f); /* jnz top */
    e8(e, 0x85);
    e32(e, (uint32_t) (top - (e->p + 4)));
    e8(e, 0xdf); /* fnstsw ax */
    e8(e, 0xe0);
    for (int i = 0; i < 8; i++) {
        e8(e, 0xdd); /* fstp qword [ebx+8*i] */
        e8(e, 0x5b);
        e8(e, 8 * i);
    }
}

/* The loop's input: an integer at [ebx], doubles after it. */
static void
fix_x87_loop(const struct variant *v)
{
    g_in.ebx = (uint32_t) g_buf;
    for (int i = 1; i < 8; i++) {
        uint8_t *p = g_in.buf + 8 * i;
        if (g_case < 2) {
            unsigned k = (g_case * 8 + i * 5 + v->fx_arg * 3) % N_X87_DOUBLES;
            put_le(p, x87_doubles[k][0], 4);
            put_le(p + 4, x87_doubles[k][1], 4);
        } else
            x87_rnd_double(p);
    }
    int      e = edge(N_X87_INTS);
    uint32_t k = e >= 0 ? (uint32_t) e : N_X87_INTS;
    put_le(g_in.buf, k < N_X87_INTS ? x87_ints[k][0] : rnd(), 4);
    put_le(g_in.buf + 4, k < N_X87_INTS ? x87_ints[k][1] : rnd() & 0x000fffff, 4);
}

/* EBP sums status words FCOMP has just set, all of them defined; the
   final status (AX) follows the FSTPs, which leave C0, C2 and C3
   undefined. */
static int
dm_x87_loop(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    d->eax &= ~0x4500u;
    return 1;
}

static int
has_fpu(void)
{
    if (g_cpu.has_cpuid)
        return g_cpu.features & CPUID_FPU;
#ifdef HOSTTEST
    return 1;
#else
    uint16_t sw = 0x5a5a;
    __asm__ volatile("fninit\n\tfnstsw %0" : "+m"(sw));
    return sw == 0;
#endif
}

static void
group_x87(void)
{
    /* Register forms: ESC byte, ModRM with ST(i) in the low three bits,
       ST(0)-ST(7) each. The aliases (DC D0, DD C8, DE D0, DF C0-D8, D9 D8)
       are in; DE D8+i, DA E8+i and the rest of the holes are the opmap
       group's. */
    static const struct {
        uint8_t esc, modrm, flags;
    } regops[] = {
        { 0xd8, 0xc0, 0 },             { 0xd8, 0xc8, 0 },     { 0xd8, 0xd0, X_CC },          { 0xd8, 0xd8, X_CC },
        { 0xd8, 0xe0, 0 },             { 0xd8, 0xe8, 0 },     { 0xd8, 0xf0, 0 },             { 0xd8, 0xf8, 0 },
        { 0xd9, 0xc0, X_PUSH },        { 0xd9, 0xc8, 0 },     { 0xd9, 0xd8, 0 },
        { 0xda, 0xc0, X_P6 },          { 0xda, 0xc8, X_P6 },  { 0xda, 0xd0, X_P6 },          { 0xda, 0xd8, X_P6 },
        { 0xdb, 0xc0, X_P6 },          { 0xdb, 0xc8, X_P6 },  { 0xdb, 0xd0, X_P6 },          { 0xdb, 0xd8, X_P6 },
        { 0xdb, 0xe8, X_P6 | X_CC },   { 0xdb, 0xf0, X_P6 | X_CC },
        { 0xdc, 0xc0, 0 },             { 0xdc, 0xc8, 0 },     { 0xdc, 0xd0, X_CC },          { 0xdc, 0xd8, X_CC },
        { 0xdc, 0xe0, 0 },             { 0xdc, 0xe8, 0 },     { 0xdc, 0xf0, 0 },             { 0xdc, 0xf8, 0 },
        { 0xdd, 0xc0, 0 },             { 0xdd, 0xc8, 0 },     { 0xdd, 0xd0, 0 },             { 0xdd, 0xd8, 0 },
        { 0xdd, 0xe0, X_CC },          { 0xdd, 0xe8, X_CC },
        { 0xde, 0xc0, 0 },             { 0xde, 0xc8, 0 },     { 0xde, 0xd0, X_CC },
        { 0xde, 0xe0, 0 },             { 0xde, 0xe8, 0 },     { 0xde, 0xf0, 0 },             { 0xde, 0xf8, 0 },
        { 0xdf, 0xc0, 0 },             { 0xdf, 0xc8, 0 },     { 0xdf, 0xd0, 0 },             { 0xdf, 0xd8, 0 },
        { 0xdf, 0xe8, X_P6 | X_CC },   { 0xdf, 0xf0, X_P6 | X_CC },
    };
    /* No operand. */
    static const struct {
        uint8_t esc, op, flags;
    } ops[] = {
        { 0xd9, 0xd0, 0 },           /* fnop */
        { 0xd9, 0xe0, 0 },           /* fchs */
        { 0xd9, 0xe1, 0 },           /* fabs */
        { 0xd9, 0xe4, X_CC },        /* ftst */
        { 0xd9, 0xe5, X_CC },        /* fxam */
        { 0xd9, 0xe8, X_PUSH },      /* fld1 */
        { 0xd9, 0xe9, X_PUSH | X_RC }, /* fldl2t */
        { 0xd9, 0xea, X_PUSH | X_RC }, /* fldl2e */
        { 0xd9, 0xeb, X_PUSH | X_RC }, /* fldpi */
        { 0xd9, 0xec, X_PUSH | X_RC }, /* fldlg2 */
        { 0xd9, 0xed, X_PUSH | X_RC }, /* fldln2 */
        { 0xd9, 0xee, X_PUSH },      /* fldz */
        { 0xd9, 0xf0, X_APPX },      /* f2xm1 */
        { 0xd9, 0xf1, X_APPX },      /* fyl2x */
        { 0xd9, 0xf2, X_APPX },      /* fptan */
        { 0xd9, 0xf3, X_APPX },      /* fpatan */
        { 0xd9, 0xf4, 0 },           /* fxtract */
        { 0xd9, 0xf5, X_CC },        /* fprem1 */
        { 0xd9, 0xf6, 0 },           /* fdecstp */
        { 0xd9, 0xf7, 0 },           /* fincstp */
        { 0xd9, 0xf8, X_CC },        /* fprem */
        { 0xd9, 0xf9, X_APPX },      /* fyl2xp1 */
        { 0xd9, 0xfa, X_RC },        /* fsqrt */
        { 0xd9, 0xfb, X_APPX },      /* fsincos */
        { 0xd9, 0xfc, X_RC },        /* frndint */
        { 0xd9, 0xfd, X_RC },        /* fscale */
        { 0xd9, 0xfe, X_APPX },      /* fsin */
        { 0xd9, 0xff, X_APPX },      /* fcos */
        { 0xde, 0xd9, X_CC },        /* fcompp */
        { 0xda, 0xe9, X_CC },        /* fucompp */
        { 0xdb, 0xe0, 0 },           /* feni (8087): a no-op since the 287 */
        { 0xdb, 0xe1, 0 },           /* fdisi (8087) */
        { 0xdb, 0xe2, 0 },           /* fnclex */
        { 0xdb, 0xe3, 0 },           /* fninit, with the stack full */
        { 0xdb, 0xe4, 0 },           /* fnsetpm (287): a no-op since the 387 */
    };
    /* Memory: reading [ebx+38h] or writing [ebx+40h]; ESC byte and /digit. */
    static const struct {
        uint8_t esc, digit, flags;
    } memops[] = {
        { 0xd9, 0, X_SRC | X_PUSH },         /* fld dword */
        { 0xdd, 0, X_SRC | X_PUSH },         /* fld qword */
        { 0xdb, 5, X_SRC | X_PUSH },         /* fld tword (two bytes past the buffer) */
        { 0xdf, 0, X_SRC | X_INT | X_PUSH }, /* fild word */
        { 0xdb, 0, X_SRC | X_INT | X_PUSH }, /* fild dword */
        { 0xdf, 5, X_SRC | X_INT | X_PUSH }, /* fild qword */
        { 0xd8, 0, X_SRC | X_RC },           /* fadd dword */
        { 0xd8, 1, X_SRC | X_RC },           /* fmul dword */
        { 0xd8, 2, X_SRC | X_CC },           /* fcom dword */
        { 0xd8, 3, X_SRC | X_CC },           /* fcomp dword */
        { 0xd8, 4, X_SRC | X_RC },           /* fsub dword */
        { 0xd8, 5, X_SRC | X_RC },           /* fsubr dword */
        { 0xd8, 6, X_SRC | X_RC },           /* fdiv dword */
        { 0xd8, 7, X_SRC | X_RC },           /* fdivr dword */
        { 0xdc, 0, X_SRC | X_RC },           /* fadd qword */
        { 0xdc, 1, X_SRC | X_RC },           /* fmul qword */
        { 0xdc, 2, X_SRC | X_CC },           /* fcom qword */
        { 0xdc, 3, X_SRC | X_CC },           /* fcomp qword */
        { 0xdc, 4, X_SRC | X_RC },           /* fsub qword */
        { 0xdc, 5, X_SRC | X_RC },           /* fsubr qword */
        { 0xdc, 6, X_SRC | X_RC },           /* fdiv qword */
        { 0xdc, 7, X_SRC | X_RC },           /* fdivr qword */
        { 0xda, 0, X_SRC | X_INT | X_RC },   /* fiadd dword */
        { 0xda, 1, X_SRC | X_INT | X_RC },   /* fimul dword */
        { 0xda, 2, X_SRC | X_INT | X_CC },   /* ficom dword */
        { 0xda, 3, X_SRC | X_INT | X_CC },   /* ficomp dword */
        { 0xda, 4, X_SRC | X_INT | X_RC },   /* fisub dword */
        { 0xda, 5, X_SRC | X_INT | X_RC },   /* fisubr dword */
        { 0xda, 6, X_SRC | X_INT | X_RC },   /* fidiv dword */
        { 0xda, 7, X_SRC | X_INT | X_RC },   /* fidivr dword */
        { 0xde, 0, X_SRC | X_INT | X_RC },   /* fiadd word */
        { 0xde, 2, X_SRC | X_INT | X_CC },   /* ficom word */
        { 0xde, 4, X_SRC | X_INT | X_RC },   /* fisub word */
        { 0xde, 7, X_SRC | X_INT | X_RC },   /* fidivr word */
        { 0xd9, 2, X_DST | X_RC },           /* fst dword */
        { 0xd9, 3, X_DST | X_RC },           /* fstp dword */
        { 0xdd, 2, X_DST | X_RC },           /* fst qword */
        { 0xdd, 3, X_DST | X_RC },           /* fstp qword */
        { 0xdb, 7, X_DST },                  /* fstp tword */
        { 0xdf, 2, X_DST | X_RC },           /* fist word */
        { 0xdf, 3, X_DST | X_RC },           /* fistp word */
        { 0xdb, 2, X_DST | X_RC },           /* fist dword */
        { 0xdb, 3, X_DST | X_RC },           /* fistp dword */
        { 0xdf, 7, X_DST | X_RC },           /* fistp qword */
        { 0xdf, 6, X_DST | X_RC },           /* fbstp */
        { 0xd9, 7, X_DST },                  /* fnstcw */
        { 0xdd, 7, X_DST },                  /* fnstsw */
    };
    const char    *name = "x87";
    struct variant v    = { .group = name, .emit = emit_x87, .fix_input = fix_x87, .fx = FX_CUSTOM, .defmask = dm_x87 };
    int            p6   = g_cpu.has_cpuid && (g_cpu.features & CPUID_CMOV);
    unsigned       row  = 0;

    if (!has_fpu()) {
        group_skip(name, "no FPU");
        return;
    }
    group_begin(name);

    /* Loads and stores alone, 0-8 registers deep: every stack position
       (TOP) in turn, the empty ones underflowing on the way out. */
    for (int depth = 0; depth <= 8; depth++) {
        v.target_len = 0;
        v.arg        = depth;
        v.arg2       = 0;
        v.arg3       = row++;
        v.fx_arg     = 0;
        run_plain(&v, 3);
    }
    for (unsigned o = 0; o < sizeof(regops) / sizeof(regops[0]); o++) {
        if ((regops[o].flags & X_P6) && !p6)
            continue;
        for (int i = 0; i < 8; i++) {
            v.target[0]  = regops[o].esc;
            v.target[1]  = regops[o].modrm | i;
            v.target_len = 2;
            v.arg2       = regops[o].flags;
            v.arg        = (v.arg2 & X_PUSH) ? 7 : 8;
            v.arg3       = row++;
            v.fx_arg     = 0;
            run_plain(&v, 2);
        }
    }
    for (unsigned o = 0; o < sizeof(ops) / sizeof(ops[0]); o++) {
        for (int rc = 0; rc < ((ops[o].flags & X_RC) ? 4 : 1); rc++) {
            for (int full = 0; full < ((ops[o].flags & X_PUSH) ? 2 : 1); full++) {
                v.target[0]  = ops[o].esc;
                v.target[1]  = ops[o].op;
                v.target_len = 2;
                v.arg2       = ops[o].flags;
                v.arg        = (v.arg2 & X_PUSH) && !full ? 7 : 8;
                v.arg3       = row++;
                v.fx_arg     = rc;
                run_plain(&v, 3);
            }
        }
    }
    for (unsigned o = 0; o < sizeof(memops) / sizeof(memops[0]); o++) {
        int src = memops[o].flags & X_SRC;
        for (int rc = 0; rc < ((memops[o].flags & X_RC) ? 4 : 1); rc++) {
            v.target[0]  = memops[o].esc;
            v.target[1]  = 0x43 | memops[o].digit << 3; /* [ebx+disp8] */
            v.target[2]  = src ? 0x38 : 0x40;
            v.target_len = 3;
            v.arg2       = memops[o].flags;
            v.arg        = src ? 7 : 8;
            v.arg3       = row++;
            v.fx_arg     = rc;
            run_plain(&v, (v.arg2 & X_INT) ? N_X87_INTS + 1 : 3);
        }
    }

    /* The hot loop: every rounding mode, with nothing else on the stack
       and with it full, with and without a block boundary in the body. */
    v.emit       = emit_x87_loop;
    v.fix_input  = fix_x87_loop;
    v.defmask    = dm_x87_loop;
    v.target_len = 0;
    v.arg2       = 0;
    for (int rc = 0; rc < 4; rc++)
        for (int depth = 0; depth <= 6; depth += 6)
            for (int b = 0; b < 2; b++) {
                v.fx_arg = rc;
                v.arg    = depth;
                v.arg3   = b;
                run_plain(&v, N_X87_INTS + 2);
            }
    group_end(name);
}

/* ---- MMX arithmetic, logic, compares, packs and unpacks --------------- */

static void
group_mmx(void)
{
    /* Two-operand mm, mm/m64 instructions (0F xx). */
    static const uint8_t ops[] = {
        0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, /* unpack low, pack, pcmpgt, unpack high */
        0x74, 0x75, 0x76,                                                       /* pcmpeq */
        0xd5, 0xd8, 0xd9, 0xdb, 0xdc, 0xdd, 0xdf,                               /* pmullw psubus pand paddus pandn */
        0xe5, 0xe8, 0xe9, 0xeb, 0xec, 0xed, 0xef,                               /* pmulhw psubs por padds pxor */
        0xf5, 0xf8, 0xf9, 0xfa, 0xfc, 0xfd, 0xfe,                               /* pmaddwd psub padd */
    };
    /* Shifts by an MMX register or memory. */
    static const uint8_t shifts[] = { 0xd1, 0xd2, 0xd3, 0xe1, 0xe2, 0xf1, 0xf2, 0xf3 };
    /* (destination, source) pairs; 3,3 is the same register twice. */
    static const uint8_t pairs[3][2] = { { 0, 1 }, { 3, 3 }, { 5, RM_MEM } };
    const char          *name        = "mmx";
    struct variant       v           = { .group = name, .mmx = 1 };

    if (!(g_cpu.features & CPUID_MMX)) {
        group_skip(name, "no MMX");
        return;
    }
    group_begin(name);
    for (unsigned o = 0; o < sizeof(ops); o++) {
        for (int p = 0; p < 3; p++) {
            v.target[0]  = 0x0f;
            v.target[1]  = ops[o];
            v.target_len = put_mm_modrm(v.target, 2, pairs[p][0], pairs[p][1]);
            v.fix_input  = pairs[p][1] == RM_MEM ? fix_mem_ebx : 0;
            v.arg        = MEM_OFF;
            run_plain(&v, 2);
        }
    }
    for (unsigned o = 0; o < sizeof(shifts); o++) {
        for (int p = 0; p < 3; p += 2) { /* a register, memory */
            v.target[0]  = 0x0f;
            v.target[1]  = shifts[o];
            v.target_len = put_mm_modrm(v.target, 2, pairs[p][0], pairs[p][1]);
            v.fix_input  = fix_mm_count;
            v.arg2       = pairs[p][1];
            v.reg_fix    = pairs[p][1] != RM_MEM;
            v.norm       = pairs[p][1] == RM_MEM ? 1 << R_EBX : 0;
            run_plain(&v, N_MM_COUNTS + 2);
        }
    }
    v.reg_fix = 0;
    v.norm    = 0;

    /* Shifts by an immediate: 0F 71 (words), 72 (dwords), 73 (qword), with
       /2 right logical, /4 right arithmetic, /6 left. 73 /4 (no PSRAQ) and
       71 /0 are #UD. */
    static const uint8_t imms[] = { 0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 255 };
    for (int op = 0x71; op <= 0x73; op++) {
        for (int sub = 0; sub < 8; sub += 2) {
            int valid = sub != 0 && !(op == 0x73 && sub == 4);
            for (int dst = 0; dst < 8; dst += 6) { /* MM0, MM6 */
                for (unsigned k = 0; k < (valid ? sizeof(imms) : 1); k++) {
                    v.target[0]  = 0x0f;
                    v.target[1]  = op;
                    v.target[2]  = MODRM_RR(sub, dst);
                    v.target[3]  = valid ? imms[k] : 4;
                    v.target_len = 4;
                    v.fix_input  = 0;
                    v.faults_ok  = !valid;
                    run_plain(&v, 1);
                }
            }
        }
    }
    v.faults_ok = 0;

    /* MOVD and MOVQ, both directions, registers and memory (one misaligned). */
    static const struct {
        uint8_t mem, len;
        uint8_t b[5];
    } moves[] = {
        { 0, SEQ(0x0f, 0x6e, 0xc0) },       /* movd mm0, eax */
        { 0, SEQ(0x0f, 0x6e, 0xe6) },       /* movd mm4, esi */
        { 0, SEQ(0x0f, 0x6e, 0xcd) },       /* movd mm1, ebp */
        { 1, SEQ(0x0f, 0x6e, 0x13) },       /* movd mm2, [ebx] */
        { 0, SEQ(0x0f, 0x7e, 0xc8) },       /* movd eax, mm1 */
        { 0, SEQ(0x0f, 0x7e, 0xff) },       /* movd edi, mm7 */
        { 1, SEQ(0x0f, 0x7e, 0x1b) },       /* movd [ebx], mm3 */
        { 0, SEQ(0x0f, 0x6f, 0xea) },       /* movq mm5, mm2 */
        { 1, SEQ(0x0f, 0x6f, 0x33) },       /* movq mm6, [ebx] */
        { 1, SEQ(0x0f, 0x6f, 0x4b, 0x1f) }, /* movq mm1, [ebx+31] */
        { 0, SEQ(0x0f, 0x7f, 0xd5) },       /* movq mm5, mm2 (store form) */
        { 1, SEQ(0x0f, 0x7f, 0x1b) },       /* movq [ebx], mm3 */
        { 1, SEQ(0x0f, 0x7f, 0x63, 0x1f) }, /* movq [ebx+31], mm4 */
    };
    for (unsigned i = 0; i < sizeof(moves) / sizeof(moves[0]); i++) {
        memcpy(v.target, moves[i].b, moves[i].len);
        v.target_len = moves[i].len;
        v.fix_input  = moves[i].mem ? fix_mem_ebx : 0;
        v.arg        = MEM_OFF;
        run_plain(&v, 2);
    }
    mmx_emmi(name);
    group_end(name);
}

/* ---- the MMX/x87 handover ------------------------------------------------- */

/* arg2 bit 0: the tag word is defined (every register empty or holding an
   ordinary x87 value). Otherwise it holds registers MMX wrote, whose tags
   FNSTENV works out from the contents, one way per model. */
static int
dm_fenv(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    if (!(v->arg2 & 1))
        d->esi = 0;
    return 1;
}

/* fnstenv [g_fenv]; movzx esi, word [tag word]; movzx edi, word [status word] */
static int
put_fenv(uint8_t *t, int n)
{
    t[n++] = 0xd9;
    t[n++] = 0x35;
    put_le(t + n, (uint32_t) &g_fenv[0], 4);
    n += 4;
    t[n++] = 0x0f;
    t[n++] = 0xb7;
    t[n++] = 0x35;
    put_le(t + n, (uint32_t) &g_fenv[2], 4);
    n += 4;
    t[n++] = 0x0f;
    t[n++] = 0xb7;
    t[n++] = 0x3d;
    put_le(t + n, (uint32_t) &g_fenv[1], 4);
    return n + 4;
}

/* FXSAVE and FXRSTOR (Pentium II Deschutes on) with the MMX registers
   loaded, into 512 bytes of the sandbox at 3000h (the sandbox CRC takes
   all of it, reserved bytes included; the x87 instruction and operand
   pointers at 8-23 are addresses, so they are cleared after): a save; a
   misaligned one (#GP); a save, a changed ST0/MM0 and a restore (MM0 in
   the MMX result); and 0F AE with a register operand and /7 (#UD). */
static void
fx_save(const char *name)
{
    uint32_t       a = (uint32_t) SANDBOX + 0x3000;
    struct variant v = { .group = name, .mmx = 1, .faults_ok = 1, .fx = FX_CUSTOM, .defmask = dm_fxarg,
                         .fx_arg = 1, .fix_input = fix_mem_ebx, .arg = MEM_OFF };
    if (!(g_cpu.features & CPUID_FXSR))
        return;
    for (int k = 0; k < 5; k++) {
        uint8_t *t = v.target;
        int      n = 0;
        if (k < 3) {
            t[n++] = 0x0f; /* fxsave [a] (misaligned by 8: k 1) */
            t[n++] = 0xae;
            t[n++] = 0x05;
            put_le(t + n, k == 1 ? a + 8 : a, 4);
            n += 4;
            t[n++] = 0x31; /* xor eax, eax */
            t[n++] = 0xc0;
            for (int f = 8; f < 24; f += 4) {
                t[n++] = 0xa3; /* mov [a+f], eax */
                put_le(t + n, a + f, 4);
                n += 4;
            }
        }
        if (k == 2) {
            t[n++] = 0xc7; /* mov dword [a+32], 12345678h: ST0's low mantissa */
            t[n++] = 0x05;
            put_le(t + n, a + 32, 4);
            n += 4;
            put_le(t + n, 0x12345678, 4);
            n += 4;
            t[n++] = 0x0f; /* fxrstor [a] */
            t[n++] = 0xae;
            t[n++] = 0x0d;
            put_le(t + n, a, 4);
            n += 4;
        }
        if (k == 3) {
            t[n++] = 0x0f; /* 0F AE /0 with a register: #UD */
            t[n++] = 0xae;
            t[n++] = 0xc0;
        }
        if (k == 4) {
            t[n++] = 0x0f; /* 0F AE /7 (CLFLUSH later): #UD */
            t[n++] = 0xae;
            t[n++] = 0x3d;
            put_le(t + n, a, 4);
            n += 4;
        }
        v.target_len = n;
        run_plain(&v, 1);
    }
}

static void
group_mmx_x87(void)
{
    static const struct {
        uint8_t tw_defined, len;
        uint8_t b[6];
    } pre[] = {
        { 0, 0, { 0 } },                                /* straight after MMX */
        { 1, SEQ(0x0f, 0x77) },                       /* emms */
        { 0, SEQ(0x0f, 0x77, 0x0f, 0x6f, 0xc1) },     /* emms; movq mm0, mm1 */
        { 0, SEQ(0x0f, 0x77, 0x0f, 0x7e, 0xc0) },     /* emms; movd eax, mm0 */
        { 1, SEQ(0x0f, 0x77, 0xd9, 0xe8) },           /* emms; fld1 */
        { 0, SEQ(0xd9, 0xe8) },                       /* fld1 onto a full stack: masked #IS */
        { 1, SEQ(0x0f, 0x77, 0xd9, 0xe8, 0xd9, 0xee) }, /* emms; fld1; fldz */
    };
    const char    *name = "mmx.x87";
    struct variant v    = { .group = name, .mmx = 1, .fx = FX_CUSTOM, .defmask = dm_fenv };

    if (!(g_cpu.features & CPUID_MMX)) {
        group_skip(name, "no MMX");
        return;
    }
    group_begin(name);
    for (unsigned i = 0; i < sizeof(pre) / sizeof(pre[0]); i++) {
        memcpy(v.target, pre[i].b, pre[i].len);
        v.target_len = put_fenv(v.target, pre[i].len);
        v.arg2       = pre[i].tw_defined;
        run_plain(&v, 1);
    }
    /* The MMX registers are the x87 mantissas: stored and loaded through
       the other side, [ebx] at the buffer. */
    static const struct {
        uint8_t len;
        uint8_t b[30];
    } alias[] = {
        /* after MMX (TOP 0, all valid): fstp tword [ebx+10*i], i 0-5: MMi with exponent FFFFh */
        { SEQ(0xdb, 0x3b, 0xdb, 0x7b, 0x0a, 0xdb, 0x7b, 0x14, 0xdb, 0x7b, 0x1e, 0xdb, 0x7b, 0x28, 0xdb, 0x7b, 0x32) },
        /* after MMX: fstp qword [ebx+8*i], i 0-7: those values narrowed (NaNs, the indefinite) */
        { SEQ(0xdd, 0x1b, 0xdd, 0x5b, 0x08, 0xdd, 0x5b, 0x10, 0xdd, 0x5b, 0x18, 0xdd, 0x5b, 0x20, 0xdd, 0x5b, 0x28,
              0xdd, 0x5b, 0x30, 0xdd, 0x5b, 0x38) },
        /* emms; fld tword [ebx+10*i], i 0-5; fld1; fldz: MM2-MM7 the raw mantissas, MM1 8000..., MM0 0 */
        { SEQ(0x0f, 0x77, 0xdb, 0x2b, 0xdb, 0x6b, 0x0a, 0xdb, 0x6b, 0x14, 0xdb, 0x6b, 0x1e, 0xdb, 0x6b, 0x28, 0xdb,
              0x6b, 0x32, 0xd9, 0xe8, 0xd9, 0xee) },
        /* emms; fild qword [ebx+8*i], i 0-7: MMi the normalised integers, not the integers */
        { SEQ(0x0f, 0x77, 0xdf, 0x2b, 0xdf, 0x6b, 0x08, 0xdf, 0x6b, 0x10, 0xdf, 0x6b, 0x18, 0xdf, 0x6b, 0x20, 0xdf,
              0x6b, 0x28, 0xdf, 0x6b, 0x30, 0xdf, 0x6b, 0x38) },
    };
    struct variant va = { .group = name, .mmx = 1, .fix_input = fix_mem_ebx };
    for (unsigned i = 0; i < sizeof(alias) / sizeof(alias[0]); i++) {
        memcpy(va.target, alias[i].b, alias[i].len);
        va.target_len = alias[i].len;
        run_plain(&va, 3);
    }
    fx_save(name);
    group_end(name);
}

/* ---- 3DNow! --------------------------------------------------------------- */

/* Single-precision inputs: zeros, ones and ordinary numbers, no NaNs,
   infinities or denormals (their handling differs between models). */
static uint32_t
rnd_float(void)
{
    uint32_t r    = rnd();
    uint32_t sign = r & 0x80000000u;
    switch (r & 7) {
        case 0: return sign;              /* +-0 */
        case 1: return sign | 0x3f800000; /* +-1 */
        default: return sign | ((100 + rnd() % 56) << 23) | (rnd() & 0x7fffff);
    }
}

static void
fix_float(const struct variant *v)
{
    for (int i = 0; i < 16; i++)
        g_mmx_in[i] = rnd_float();
    if (v->arg2 == RM_MEM) {
        g_in.ebx = (uint32_t) g_buf + MEM_OFF;
        put_le(g_in.buf + MEM_OFF, rnd_float(), 4);
        put_le(g_in.buf + MEM_OFF + 4, rnd_float(), 4);
    }
}

/* Only exact operations are comparable between models: compares, min and
   max, truncating conversions, integer averages and rounding multiplies,
   swaps. The rest (add, multiply, reciprocal estimates, int to float)
   round one way on the K6-2 and another on the Athlon: raw CRC only. */
static int
dm_3dnow(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    return v->arg3;
}

static void
group_3dnow(void)
{
    static const struct {
        uint8_t op, exact, ext, fl;
    } ops[] = {
        { 0x0d, 0, 0, 0 }, /* PI2FD */
        { 0x1d, 1, 0, 1 }, /* PF2ID */
        { 0x90, 1, 0, 1 }, /* PFCMPGE */
        { 0x94, 1, 0, 1 }, /* PFMIN */
        { 0x96, 0, 0, 1 }, /* PFRCP */
        { 0x97, 0, 0, 1 }, /* PFRSQRT */
        { 0x9a, 0, 0, 1 }, /* PFSUB */
        { 0x9e, 0, 0, 1 }, /* PFADD */
        { 0xa0, 1, 0, 1 }, /* PFCMPGT */
        { 0xa4, 1, 0, 1 }, /* PFMAX */
        { 0xa6, 0, 0, 1 }, /* PFRCPIT1 */
        { 0xa7, 0, 0, 1 }, /* PFRSQIT1 */
        { 0xaa, 0, 0, 1 }, /* PFSUBR */
        { 0xae, 0, 0, 1 }, /* PFACC */
        { 0xb0, 1, 0, 1 }, /* PFCMPEQ */
        { 0xb4, 0, 0, 1 }, /* PFMUL */
        { 0xb6, 0, 0, 1 }, /* PFRCPIT2 */
        { 0xb7, 1, 0, 0 }, /* PMULHRW */
        { 0xbf, 1, 0, 0 }, /* PAVGUSB */
        { 0x0c, 0, 1, 0 }, /* PI2FW */
        { 0x1c, 1, 1, 1 }, /* PF2IW */
        { 0x8a, 0, 1, 1 }, /* PFNACC */
        { 0x8e, 0, 1, 1 }, /* PFPNACC */
        { 0xbb, 1, 1, 0 }, /* PSWAPD */
    };
    static const uint8_t pairs[2][2] = { { 0, 1 }, { 7, RM_MEM } };
    const char          *name        = "3dnow";
    struct variant       v           = { .group = name, .mmx = 1, .fx = FX_CUSTOM, .defmask = dm_3dnow };

    if (!(g_cpu.ext_features & EXT_3DNOW)) {
        group_skip(name, "no 3DNow!");
        return;
    }
    group_begin(name);
    for (unsigned o = 0; o < sizeof(ops) / sizeof(ops[0]); o++) {
        if (ops[o].ext && !(g_cpu.ext_features & EXT_3DNOWEXT))
            continue;
        for (int p = 0; p < 2; p++) {
            int n        = 0;
            v.target[n++] = 0x0f;
            v.target[n++] = 0x0f;
            n             = put_mm_modrm(v.target, n, pairs[p][0], pairs[p][1]);
            v.target[n++] = ops[o].op;
            v.target_len  = n;
            v.arg2        = pairs[p][1];
            v.arg3        = ops[o].exact;
            v.fix_input   = ops[o].fl ? fix_float : pairs[p][1] == RM_MEM ? fix_rm : 0;
            v.reg_fix     = pairs[p][1] != RM_MEM;
            v.norm        = pairs[p][1] == RM_MEM ? 1 << R_EBX : 0;
            run_plain(&v, 2);
        }
    }
    /* FEMMS, and PREFETCH/PREFETCHW (no visible effect but must decode). */
    {
        static const struct {
            uint8_t mem, len;
            uint8_t b[3];
        } seqs[] = {
            { 0, SEQ(0x0f, 0x0e) },       /* femms */
            { 1, SEQ(0x0f, 0x0d, 0x03) }, /* prefetch [ebx] */
            { 1, SEQ(0x0f, 0x0d, 0x0b) }, /* prefetchw [ebx] */
        };
        v.arg3 = 1;
        for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
            int n = seqs[i].len;
            memcpy(v.target, seqs[i].b, n);
            if (i == 0) /* FEMMS: the tag word after it, all empty */
                n = put_fenv(v.target, n);
            v.target_len = n;
            v.fix_input  = seqs[i].mem ? fix_mem_ebx : 0;
            v.arg        = MEM_OFF;
            v.reg_fix    = 0;
            v.norm       = 0;
            run_plain(&v, 1);
        }
    }
    group_end(name);
}

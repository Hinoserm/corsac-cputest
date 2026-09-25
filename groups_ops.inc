/*
 * Groups 7-20: the instructions 86Box's recompiler still hands to the
 * interpreter, then the corners around them. Included into groups.inc.
 * The same rule holds: never reorder, only append.
 */

/* An r/m operand that is memory: [ebx], EBX pointing 8 bytes into the buffer. */
#define RM_MEM   8
#define MEM_OFF  8

static uint32_t
szmask(int size)
{
    return size == 32 ? 0xffffffffu : (1u << size) - 1;
}

static uint32_t
le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void
put_le(uint8_t *p, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; i++)
        p[i] = v >> (8 * i);
}

/* An operand of the pending input by ModRM number and size; RM_MEM is the
   memory operand. Byte registers are AL CL DL BL AH CH DH BH. */
static uint32_t
get_opnd(int r, int size)
{
    if (r == RM_MEM)
        return le32(g_in.buf + MEM_OFF) & szmask(size);
    if (size == 8)
        return r < 4 ? *in_reg(r) & 0xff : (*in_reg(r - 4) >> 8) & 0xff;
    return *in_reg(r) & szmask(size);
}

static void
set_opnd(int r, int size, uint32_t val)
{
    if (r == RM_MEM) {
        put_le(g_in.buf + MEM_OFF, val, size / 8);
        return;
    }
    if (size == 8) {
        if (r < 4)
            *in_reg(r) = (*in_reg(r) & ~0xffu) | (val & 0xff);
        else
            *in_reg(r - 4) = (*in_reg(r - 4) & ~0xff00u) | ((val & 0xff) << 8);
    } else if (size == 16)
        *in_reg(r) = (*in_reg(r) & 0xffff0000u) | (val & 0xffff);
    else
        *in_reg(r) = val;
}

/* The same, from a recorded input. */
static uint32_t
kin_opnd(const struct kin *in, int r, int size)
{
    if (r == RM_MEM)
        return le32(in->buf + MEM_OFF) & szmask(size);
    if (size == 8)
        return kin_reg8(in, r);
    return kin_reg(in, r) & szmask(size);
}

static int
put_modrm(uint8_t *t, int n, int reg, int rm)
{
    t[n++] = rm == RM_MEM ? 0x03 | (reg << 3) : MODRM_RR(reg, rm);
    return n;
}

/* The operand-size prefix and the byte or word/dword opcode. */
static int
put_op(uint8_t *t, int n, int size, uint8_t op8)
{
    if (size == 16)
        t[n++] = 0x66;
    t[n++] = size == 8 ? op8 : op8 | 1;
    return n;
}

/* Memory at [ebx] (if the variant has one) and nothing else. */
static void
fix_rm(const struct variant *v)
{
    if (v->arg2 == RM_MEM)
        g_in.ebx = (uint32_t) g_buf + MEM_OFF;
}

static void
set_fix_rm(struct variant *v, int rm, void (*fix)(const struct variant *))
{
    v->arg2      = rm;
    v->fix_input = fix;
    v->reg_fix   = rm != RM_MEM;
    v->norm      = rm == RM_MEM ? 1 << R_EBX : 0;
}

/* Representative operands (register, register or memory): different, the
   same register twice, memory. */
static const uint8_t opnd32[4][2] = { { R_EAX, R_ECX }, { R_EDX, R_EDX }, { R_ESI, RM_MEM }, { R_EDI, R_EBP } };

/* ---- defined-result masks ------------------------------------------------ */

/* Every arithmetic flag written, and defined. */
static int
dm_alldef(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    if (d->fault == 0xff)
        *undef = 0;
    return 1;
}

/* MUL, IMUL: CF and OF defined, SF ZF AF PF undefined. */
static int
dm_mul(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    if (d->fault == 0xff)
        *undef = F_SF | F_ZF | F_AF | F_PF;
    return 1;
}

/* DIV, IDIV: every flag undefined; #DE leaves everything as it was. */
static int
dm_div(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    if (d->fault == 0xff)
        *undef = F_ARITH;
    return 1;
}

/* ---- IMUL r, r/m and IMUL r, r/m, imm ------------------------------------- */

/* Operands small (no overflow), then random ones (mostly overflowing).
   arg: the size; arg2: the source; arg3: the destination for the
   two-operand form, 0xff for the others. */
static void
fix_imul(const struct variant *v)
{
    int size = v->arg;
    fix_rm(v);
    if (edge(1) == 0) {
        set_opnd(v->arg2, size, (uint32_t) ((int32_t) (rnd() % 200) - 100));
        if (v->arg3 != 0xff)
            set_opnd(v->arg3, size, (uint32_t) ((int32_t) (rnd() % 200) - 100));
    }
}

static void
group_imul(void)
{
    static const uint8_t  imm8s[]  = { 0x00, 0x01, 0xff, 0x7f, 0x80, 0x03 };
    static const uint32_t imm32s[] = { 0x7fffffff, 0x80000000, 0x00010001, 0xfffffffe, 12345 };
    const char           *name     = "imul";
    struct variant        v        = { .group = name, .fx = FX_CUSTOM, .defmask = dm_mul };
    group_begin(name);
    for (int size = 16; size <= 32; size += 16) {
        for (int form = 0; form < 3; form++) { /* 0F AF, 6B ib, 69 iw/id */
            int nimm = form == 0 ? 1 : form == 1 ? 6 : 5;
            for (int p = 0; p < 4; p++) {
                int dst = opnd32[p][0], src = opnd32[p][1];
                for (int k = 0; k < nimm; k++) {
                    int n = 0;
                    if (size == 16)
                        v.target[n++] = 0x66;
                    if (form == 0) {
                        v.target[n++] = 0x0f;
                        v.target[n++] = 0xaf;
                    } else
                        v.target[n++] = form == 1 ? 0x6b : 0x69;
                    n = put_modrm(v.target, n, dst, src);
                    if (form == 1)
                        v.target[n++] = imm8s[k];
                    else if (form == 2) {
                        put_le(v.target + n, imm32s[k], size / 8);
                        n += size / 8;
                    }
                    v.target_len = n;
                    v.arg        = size;
                    v.arg3       = form == 0 ? dst : 0xff;
                    set_fix_rm(&v, src, fix_imul);
                    run_keep(&v, 3);
                }
            }
        }
    }
    group_end(name);
}

/* ---- MUL, IMUL, DIV, IDIV (F6/F7 /4-/7) ----------------------------------- */

/* The dividend's high and low halves and the divisor, per size. */
struct div_case {
    uint32_t high, low, div;
};

/* The known edges, as (high, low, divisor) at each size, for DIV and IDIV
   alike: the most negative dividend over -1 (#DE on every real CPU; some
   emulators compute it in C and crash), its neighbours, the most negative
   quotient that fits, divisors 0 and 1, and a quotient one too big. */
static int
div_edge(int size, int sgn, struct div_case *c)
{
    uint32_t m = szmask(size), top = 1u << (size - 1);
    const struct div_case cases[] = {
        { top, 0, m },          /* MIN / -1: #DE */
        { top - 1, m, m },      /* 7FFF:FFFF / -1 */
        { top, 1, m },          /* 8000:0001 / -1 */
        { m, top, m },          /* -(2^(n-1)) / -1: quotient 2^(n-1), #DE */
        { m, top, 1 },          /* -(2^(n-1)) / 1: fits exactly */
        { m, top, 2 },          /* quotient -(2^(n-2)) */
        { 0, top, 1 },          /* DIV: 2^(n-1) / 1 fits; IDIV: too big */
        { 0, 12345 & m, 0 },    /* divisor 0 */
        { 1, 0, 1 },            /* quotient 2^n: one too big */
        { 0, m, m },            /* quotient 1 */
    };
    int e = edge(sizeof(cases) / sizeof(cases[0]));
    (void) sgn;
    if (e < 0)
        return 0;
    *c = cases[e];
    return 1;
}

/* The edges above, then random dividends that fit (the high half below
   the divisor, or the sign extension of the low half), then one as the
   input came (mostly overflowing into #DE). Where the divisor is part of
   the dividend (AL/AH, EAX/EDX) only the dividend is set. */
static void
fix_div(const struct variant *v)
{
    int             size = v->arg3 & 0xff, sgn = (v->arg3 >> 8) & 1, rm = v->arg2;
    uint32_t        mask = szmask(size);
    struct div_case c;
    int             alias = size == 8 ? (rm == 0 || rm == 4) : (rm == R_EAX || rm == R_EDX);

    fix_rm(v);
    if (!div_edge(size, sgn, &c)) {
        if (edge(12) < 0) /* the last input as it came */
            return;
        c.div  = (rnd() & 1) ? rnd_operand() & mask : rnd() & 0xff;
        c.low  = rnd() & mask;
        if (sgn)
            c.high = (c.low >> (size - 1)) & 1 ? mask : 0;
        else
            c.high = c.div ? rnd() % c.div : rnd() & mask;
    }
    if (size == 8) {
        g_in.eax = (g_in.eax & 0xffff0000u) | ((c.high & 0xff) << 8) | (c.low & 0xff);
    } else {
        set_opnd(R_EAX, size, c.low);
        set_opnd(R_EDX, size, c.high);
    }
    if (!alias)
        set_opnd(rm, size, c.div);
}

/* MUL and IMUL: zero, one, -1, the top bit, then random. */
static void
fix_mul(const struct variant *v)
{
    int      size = v->arg3 & 0xff;
    uint32_t m = szmask(size), edges[] = { 0, 1, m, 1u << (size - 1), m >> 1 };
    int      e = edge(5);
    fix_rm(v);
    if (e >= 0) { /* the accumulator one edge, the source another */
        set_opnd(R_EAX, size, edges[(e + 2) % 5]);
        set_opnd(v->arg2, size, edges[e]);
    }
}

static void
group_muldiv(void)
{
    /* The source: the accumulator itself, another register, a high byte,
       memory. */
    static const uint8_t rms8[]  = { 0, 1, 4, 7, RM_MEM };             /* AL CL AH BH [m] */
    static const uint8_t rms32[] = { R_EAX, R_ECX, R_EDX, R_ESI, RM_MEM };
    const char          *name    = "mul.div";
    struct variant       v       = { .group = name, .fx = FX_CUSTOM };
    group_begin(name);
    for (int size = 8; size <= 32; size *= 2) {
        for (int op = 4; op < 8; op++) {
            for (int r = 0; r < 5; r++) {
                int rm       = size == 8 ? rms8[r] : rms32[r];
                int n        = put_op(v.target, 0, size, 0xf6);
                n            = put_modrm(v.target, n, op, rm);
                v.target_len = n;
                int div      = op >= 6;
                set_fix_rm(&v, rm, div ? fix_div : fix_mul);
                v.arg3      = size | ((op == 5 || op == 7) << 8);
                v.defmask   = div ? dm_div : dm_mul;
                v.faults_ok = div;
                run_keep(&v, div ? 13 : 7);
            }
        }
    }
    group_end(name);
}

/* ---- shifts and rotates: by CL, by 1, by an immediate --------------------- */

/* The counts that matter for 8, 16 and 32 bits alike. */
static const uint8_t shift_counts[] = { 0, 1, 2, 7, 8, 9, 15, 16, 17, 31, 32, 33 };
#define N_SHIFT_COUNTS (sizeof(shift_counts) / sizeof(shift_counts[0]))

/* arg3: the count, or 0x100 for CL: then every count above in turn, and
   one random. */
static void
fix_count(const struct variant *v)
{
    fix_rm(v);
    if (v->arg3 & 0x100) {
        int      e = edge(N_SHIFT_COUNTS);
        uint32_t c = e >= 0 ? shift_counts[e] : rnd() & 0xff;
        g_in.ecx   = (g_in.ecx & ~0xffu) | c;
    }
}

static unsigned
count_of(const struct variant *v, const struct kin *in)
{
    return ((v->arg3 & 0x100) ? in->ecx : v->arg3) & 31;
}

/* fx_arg is the ModRM reg field, arg the operand size. */
static int
dm_shift(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    int      op = v->fx_arg, size = v->arg;
    unsigned c  = count_of(v, in);

    if (d->fault != 0xff || !c)
        return 1; /* a zero count changes nothing */
    if (op < 4) { /* ROL ROR RCL RCR: CF, and OF for a count of 1 */
        *undef &= ~F_CF;
        if (c == 1)
            *undef &= ~F_OF;
        else
            *undef |= F_OF;
    } else { /* SHL SHR SAL SAR */
        *undef = (*undef & ~(F_SF | F_ZF | F_PF | F_CF | F_OF)) | F_AF;
        if (c != 1)
            *undef |= F_OF;
        if (op != 7 && c >= (unsigned) size)
            *undef |= F_CF;
    }
    return 1;
}

static void
group_shift(void)
{
    static const uint8_t rms[] = { 0, 1, RM_MEM }; /* AL/EAX, CL/ECX (the count register), memory */
    const char          *name  = "shift.rotate";
    struct variant       v     = { .group = name, .fx = FX_CUSTOM, .defmask = dm_shift };
    group_begin(name);
    for (int form = 0; form < 3; form++) { /* D2/D3 by CL, D0/D1 by 1, C0/C1 imm */
        for (int size = 8; size <= 32; size *= 2) {
            for (int op = 0; op < 8; op++) {
                for (unsigned r = 0; r < sizeof(rms); r++) {
                    /* By an immediate: the register form, and memory at 32 bits. */
                    if (form == 2 && (rms[r] == 1 || (rms[r] == RM_MEM && size != 32)))
                        continue;
                    int nimm = form == 2 ? (int) N_SHIFT_COUNTS : 1;
                    for (int k = 0; k < nimm; k++) {
                        static const uint8_t base[3] = { 0xd2, 0xd0, 0xc0 };
                        int                  n       = put_op(v.target, 0, size, base[form]);
                        n                            = put_modrm(v.target, n, op, rms[r]);
                        if (form == 2)
                            v.target[n++] = shift_counts[k];
                        v.target_len = n;
                        v.fx_arg     = op;
                        v.arg        = size;
                        v.arg3       = form == 0 ? 0x100 : form == 1 ? 1 : shift_counts[k];
                        set_fix_rm(&v, rms[r], fix_count);
                        if (form == 0)
                            run_keep(&v, N_SHIFT_COUNTS + 1);
                        else if (op == 2 || op == 3)
                            run_flags(&v, 1); /* RCL, RCR read CF */
                        else
                            run_plain(&v, 1);
                    }
                }
            }
        }
    }
    group_end(name);
}

/* ---- SHLD, SHRD ----------------------------------------------------------- */

static int
dm_shd(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    unsigned c = count_of(v, in);

    if (d->fault != 0xff || !c)
        return 1;
    if (v->arg == 16 && c > 16)
        return 0; /* result and flags undefined */
    *undef = (*undef & ~(F_SF | F_ZF | F_PF | F_CF | F_OF)) | F_AF;
    if (c != 1)
        *undef |= F_OF;
    return 1;
}

static void
group_shd(void)
{
    static const uint8_t ops[4]      = { 0xa4, 0xa5, 0xac, 0xad }; /* SHLD imm, SHLD CL, SHRD imm, SHRD CL */
    static const uint8_t pairs[3][2] = { { R_EAX, R_EDX }, { RM_MEM, R_EDI }, { R_ESI, R_ESI } }; /* dst, src */
    const char          *name        = "shld.shrd";
    struct variant       v           = { .group = name, .fx = FX_CUSTOM, .defmask = dm_shd };
    group_begin(name);
    for (int o = 0; o < 4; o++) {
        int by_cl = ops[o] & 1;
        for (int size = 16; size <= 32; size += 16) {
            for (int p = 0; p < 3; p++) {
                int nimm = by_cl ? 1 : (int) N_SHIFT_COUNTS;
                for (int k = 0; k < nimm; k++) {
                    int n = 0;
                    if (size == 16)
                        v.target[n++] = 0x66;
                    v.target[n++] = 0x0f;
                    v.target[n++] = ops[o];
                    n             = put_modrm(v.target, n, pairs[p][1], pairs[p][0]);
                    if (!by_cl)
                        v.target[n++] = shift_counts[k];
                    v.target_len = n;
                    v.arg        = size;
                    v.arg3       = by_cl ? 0x100 : shift_counts[k];
                    set_fix_rm(&v, pairs[p][0], fix_count);
                    if (by_cl)
                        run_keep(&v, N_SHIFT_COUNTS + 1);
                    else
                        run_plain(&v, 1);
                }
            }
        }
    }
    group_end(name);
}

/* ---- BSF, BSR ------------------------------------------------------------- */

/* The source: zero, bit 0, the top bit, one random bit, then random. */
static void
fix_bs(const struct variant *v)
{
    int      size = v->arg;
    uint32_t val;
    fix_rm(v);
    switch (edge(4)) {
        case 0: val = 0; break;
        case 1: val = 1; break;
        case 2: val = 1u << (size - 1); break;
        case 3: val = 1u << (rnd() % size); break;
        default: val = rnd(); break;
    }
    set_opnd(v->arg2, size, val);
}

/* ZF defined; CF OF SF AF PF undefined; the destination undefined for a
   zero source. */
static int
dm_bs(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    if (d->fault != 0xff)
        return 1;
    *undef = F_CF | F_OF | F_SF | F_AF | F_PF;
    if (kin_opnd(in, v->arg2, v->arg) == 0) {
        uint32_t *r = kout_reg(d, v->arg3);
        *r          = v->arg == 32 ? 0 : *r & 0xffff0000u;
    }
    return 1;
}

static void
group_bs(void)
{
    static const uint8_t pairs[3][2] = { { R_EAX, R_ECX }, { R_EDX, R_EDX }, { R_ESI, RM_MEM } }; /* dst, src */
    const char          *name        = "bsf.bsr";
    struct variant       v           = { .group = name, .fx = FX_CUSTOM, .defmask = dm_bs };
    group_begin(name);
    for (int op = 0xbc; op <= 0xbd; op++) {
        for (int size = 16; size <= 32; size += 16) {
            for (int p = 0; p < 3; p++) {
                int n = 0;
                if (size == 16)
                    v.target[n++] = 0x66;
                v.target[n++] = 0x0f;
                v.target[n++] = op;
                n             = put_modrm(v.target, n, pairs[p][0], pairs[p][1]);
                v.target_len  = n;
                v.arg         = size;
                v.arg3        = pairs[p][0];
                set_fix_rm(&v, pairs[p][1], fix_bs);
                run_keep(&v, 6);
            }
        }
    }
    group_end(name);
}

/* ---- CMPXCHG, XADD, CMPXCHG8B --------------------------------------------- */

/* Equal, then not equal, then random: the accumulator against the
   destination. */
static void
fix_cx(const struct variant *v)
{
    fix_rm(v);
    uint32_t dst = get_opnd(v->arg2, v->arg);
    switch (edge(2)) {
        case 0: set_opnd(R_EAX, v->arg, dst); break;
        case 1: set_opnd(R_EAX, v->arg, dst ^ 1); break;
        default: break;
    }
}

/* [esi], ESI 8 bytes into the buffer; EDX:EAX equal, then not, then
   random. */
static void
fix_cx8(const struct variant *v)
{
    g_in.esi = (uint32_t) g_buf + MEM_OFF;
    int e    = edge(2);
    if (e >= 0) {
        g_in.eax = le32(g_in.buf + MEM_OFF) ^ e;
        g_in.edx = le32(g_in.buf + MEM_OFF + 4);
    }
}

/* CMPXCHG8B writes ZF only. */
static int
dm_cx8(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    if (d->fault == 0xff)
        *undef &= ~F_ZF;
    return 1;
}

static void
group_cmpxchg(void)
{
    /* (destination, source); a destination past RM_MEM is locked memory.
       The first has the accumulator as the destination. */
    static const uint8_t pairs8[5][2]  = { { 0, 2 }, { 1, 2 }, { 7, 5 }, { RM_MEM, 2 }, { RM_MEM + 1, 2 } };
    static const uint8_t pairs32[5][2] = { { R_EAX, R_EDX }, { R_ECX, R_EDX }, { R_ESI, R_EDI }, { RM_MEM, R_EDX }, { RM_MEM + 1, R_EDX } };
    const char          *name          = "cmpxchg.xadd";
    struct variant       v             = { .group = name, .fx = FX_CUSTOM, .defmask = dm_alldef };
    if (!g_cpu.is486) {
        group_skip(name, "386");
        return;
    }
    group_begin(name);
    for (int op = 0; op < 2; op++) { /* CMPXCHG 0F B0/B1, XADD 0F C0/C1 */
        uint8_t op8 = op ? 0xc0 : 0xb0;
        for (int size = 8; size <= 32; size *= 2) {
            for (int p = 0; p < 5; p++) {
                const uint8_t *pr  = size == 8 ? pairs8[p] : pairs32[p];
                int            dst = pr[0], rm = dst > RM_MEM ? RM_MEM : dst, n = 0;
                if (dst > RM_MEM)
                    v.target[n++] = 0xf0;
                if (size == 16)
                    v.target[n++] = 0x66;
                v.target[n++] = 0x0f;
                v.target[n++] = size == 8 ? op8 : op8 | 1;
                n             = put_modrm(v.target, n, pr[1], rm);
                v.target_len  = n;
                v.arg         = size;
                v.faults_ok   = 0;
                set_fix_rm(&v, rm, fix_cx);
                run_keep(&v, 3);
            }
            /* LOCK on a register destination: #UD */
            {
                int n = 0;
                v.target[n++] = 0xf0;
                if (size == 16)
                    v.target[n++] = 0x66;
                v.target[n++] = 0x0f;
                v.target[n++] = size == 8 ? op8 : op8 | 1;
                v.target[n++] = MODRM_RR(R_EDX, R_ECX);
                v.target_len  = n;
                v.faults_ok   = 1;
                set_fix_rm(&v, R_ECX, 0);
                run_plain(&v, 1);
            }
        }
    }
    /* CMPXCHG8B. Never with LOCK and a register operand (F0 0F C7 C8+):
       that is the Pentium's F00F erratum and hangs the machine. */
    if (g_cpu.features & (1u << 8)) { /* CX8 */
        static const struct {
            uint8_t len, b[4];
        } forms[] = { { SEQ(0x0f, 0xc7, 0x0e) }, { SEQ(0xf0, 0x0f, 0xc7, 0x0e) }, { SEQ(0x0f, 0xc7, 0xc9) } };
        for (int f = 0; f < 3; f++) {
            memcpy(v.target, forms[f].b, forms[f].len);
            v.target_len = forms[f].len;
            v.defmask    = dm_cx8;
            v.fix_input  = fix_cx8;
            v.reg_fix    = 0;
            v.norm       = 1 << R_ESI;
            v.faults_ok  = f == 2; /* the register form is #UD */
            run_keep(&v, 3);
        }
    }
    group_end(name);
}

/* ---- DAA DAS AAA AAS AAM AAD SALC ----------------------------------------- */

/* fx_arg: 0 DAA/DAS, 1 AAA/AAS, 2 AAM/AAD, 3 SALC. The arithmetic over
   every AL, AF and CF is in exhaust8; here, each after every producer. */
static int
dm_bcd(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    if (d->fault != 0xff)
        return 1;
    switch (v->fx_arg) {
        case 0: *undef = F_OF; break;
        case 1: *undef = F_OF | F_SF | F_ZF | F_PF; break;
        case 2: *undef = F_OF | F_AF | F_CF; break;
        default: break; /* SALC: flags untouched */
    }
    return 1;
}

static void
group_bcd(void)
{
    static const uint8_t imms[] = { 0, 1, 2, 7, 10, 16, 255 };
    const char          *name   = "bcd";
    struct variant       v      = { .group = name, .fx = FX_CUSTOM, .defmask = dm_bcd, .faults_ok = 1 };
    group_begin(name);
    for (int i = 0; i < 4; i++) { /* DAA DAS AAA AAS: read AF and CF */
        static const uint8_t ops[4] = { 0x27, 0x2f, 0x37, 0x3f };
        v.target[0]  = ops[i];
        v.target_len = 1;
        v.fx_arg     = i >> 1;
        run_flags(&v, 1);
    }
    for (int op = 0xd4; op <= 0xd5; op++) { /* AAM, AAD in any base; AAM 0 is #DE */
        for (unsigned k = 0; k < sizeof(imms); k++) {
            v.target[0]  = op;
            v.target[1]  = imms[k];
            v.target_len = 2;
            v.fx_arg     = 2;
            run_plain(&v, 2);
        }
    }
    v.target[0]  = 0xd6; /* SALC: reads CF */
    v.target_len = 1;
    v.fx_arg     = 3;
    run_flags(&v, 1);
    group_end(name);
}

/* ---- MOVS CMPS STOS LODS SCAS, with and without REP ----------------------- */

/* The cases, in order: DF clear and set, each with a count of 0, 1 and 5
   (for REP), and the strings equal (CMPS) or the accumulator present
   (SCAS) half of them; then random. arg: size; arg2: REP prefix; arg3
   bit 0: 16-bit addressing (the low buffer). */
static void
fix_str(const struct variant *v)
{
    static const uint8_t counts[3] = { 0, 1, 5 };
    int                  size = v->arg, rep = v->arg2, e = edge(6);

    if (e >= 0)
        g_in.flags = (g_in.flags & ~0x400u) | ((e & 1) << 10);
    if (v->arg3 & 1) {
#ifdef HOSTTEST
        g_skip = 1;
#endif
        g_in.flags &= ~0x400u; /* stay inside the 16-byte low buffer */
        g_in.esi = (rnd() & 0xffff0000u) | ((uint32_t) LOWBUF + (rnd() & 1));
        g_in.edi = (rnd() & 0xffff0000u) | ((uint32_t) LOWBUF + 6 + (rnd() & 1));
        g_in.ecx = (rnd() & 0xffff0000u) | (e >= 0 ? counts[e >> 1] % 3 : rnd() % 3);
        return;
    }
    int down = (g_in.flags & 0x400) != 0;
    g_in.esi = (uint32_t) g_buf + (down ? 24 : 0) + (rnd() & 7);
    g_in.edi = g_in.esi + 32;
    g_in.ecx = !rep ? rnd_operand() : e >= 0 ? counts[e >> 1] : rnd() % 10;
    /* CMPS: the strings agree up to one flipped bit. */
    if (e >= 0 ? (e >> 1) != 1 : (rnd() & 1)) {
        memcpy(g_in.buf + 32, g_in.buf, 32);
        g_in.buf[32 + (rnd() & 31)] ^= 1 << (rnd() & 7);
    }
    /* SCAS: the accumulator matches an element some way along. */
    if (e >= 0 ? (e >> 1) != 0 : (rnd() & 1)) {
        int k = rnd() % 6;
        int o = (int) (g_in.edi - (uint32_t) g_buf) + (down ? -k : k) * (size / 8);
        if (o >= 0 && o + 4 <= BUF_SIZE)
            set_opnd(R_EAX, size, le32(g_in.buf + o));
    }
}

/* fx_arg: CMPS or SCAS. They write all six flags unless REP ran zero times. */
static int
dm_str(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    uint32_t count = (v->arg3 & 1) ? in->ecx & 0xffff : in->ecx;
    if (d->fault == 0xff && v->fx_arg && (!v->arg2 || count))
        *undef = 0;
    return 1;
}

static void
group_string(void)
{
    static const uint8_t ops[5] = { 0xa4, 0xa6, 0xaa, 0xac, 0xae }; /* MOVS CMPS STOS LODS SCAS */
    const char          *name   = "string";
    struct variant       v      = { .group = name, .fx = FX_CUSTOM, .defmask = dm_str, .fix_input = fix_str };
    group_begin(name);
    for (int a16 = 0; a16 < 2; a16++) {
        for (int o = 0; o < 5; o++) {
            int cmp = o == 1 || o == 4;
            for (int rep = 0; rep < (cmp ? 3 : 2); rep++) {
                for (int size = 8; size <= 32; size *= 2) {
                    int n = 0;
                    if (rep)
                        v.target[n++] = rep == 1 ? 0xf3 : 0xf2;
                    if (a16)
                        v.target[n++] = 0x67;
                    n            = put_op(v.target, n, size, ops[o]);
                    v.target_len = n;
                    v.arg        = size;
                    v.arg2       = rep;
                    v.arg3       = a16;
                    v.fx_arg     = cmp;
                    v.no_ref     = a16;
                    v.norm       = a16 ? 0 : (1 << R_ESI) | (1 << R_EDI);
                    run_keep(&v, rep ? 7 : 3);
                }
            }
        }
    }
    /* A segment override on the source: ES: MOVSD, REP ES: MOVSB. */
    {
        static const struct {
            uint8_t len, b[3];
        } seqs[2] = { { SEQ(0x26, 0xa5) }, { SEQ(0xf3, 0x26, 0xa4) } };
        for (int i = 0; i < 2; i++) {
            memcpy(v.target, seqs[i].b, seqs[i].len);
            v.target_len = seqs[i].len;
            v.arg        = i ? 8 : 32;
            v.arg2       = i;
            v.arg3       = 0;
            v.fx_arg     = 0;
            v.no_ref     = 0;
            v.norm       = (1 << R_ESI) | (1 << R_EDI);
            run_keep(&v, 7);
        }
    }
    group_end(name);
}

/* ---- POPF, PUSHF ---------------------------------------------------------- */

/* EFLAGS as POPF may load them: anything but TF and IF. */
#define POPF_MASK 0x0027ffffu

/* EDI, the value POPF loads: none of the flags, all of them, then
   random. */
static void
fix_popf(const struct variant *v)
{
    switch (edge(2)) {
        case 0: g_in.edi = 0; break;
        case 1: g_in.edi = ~0x300u; break;
        default: g_in.edi = rnd() & ~0x300u; break;
    }
}

/* arg2: 1 = PUSHF of the lazy flags into ESI (the producer's undefined
   flags are undefined there too), 0 = POPF first, everything defined. */
static int
dm_popf(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    if (v->arg2)
        d->esi &= ~(uint32_t) *undef;
    else
        *undef = 0;
    return 1;
}

static void
group_popf(void)
{
    static const struct {
        uint8_t pushf, len;
        uint8_t b[14];
    } seqs[] = {
        { 0, SEQ(0x57, 0x9d) },                                                /* push edi; popfd */
        { 0, SEQ(0x66, 0x57, 0x66, 0x9d) },                                    /* push di; popf */
        { 1, SEQ(0x9c, 0x5e, 0xc7, 0x44, 0x24, 0xfc, 0, 0, 0, 0) },            /* pushfd; pop esi; scrub */
        { 1, SEQ(0x66, 0x9c, 0x66, 0x5e, 0x66, 0xc7, 0x44, 0x24, 0xfe, 0, 0) }, /* pushf; pop si; scrub */
        { 0, SEQ(0x57, 0x9d, 0x9c, 0x5e) },                                    /* popfd; pushfd; pop esi */
        { 0, SEQ(0x57, 0x9d, 0x0f, 0x92, 0xc0, 0x0f, 0x90, 0xc4) },            /* popfd; setb al; seto ah */
        { 0, SEQ(0x57, 0x9d, 0x11, 0xd1) },                                    /* popfd; adc ecx, edx */
        { 0, SEQ(0x57, 0x9d, 0x9f) },                                          /* popfd; lahf */
        { 0, SEQ(0x66, 0x57, 0x66, 0x9d, 0x0f, 0x94, 0xc0) },                  /* popf; setz al */
    };
    const char    *name = "popf.pushf";
    struct variant v    = { .group = name, .fx = FX_CUSTOM, .defmask = dm_popf, .fix_input = fix_popf,
                            .reg_fix = 1, .stack = 1, .flags_mask = POPF_MASK };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.arg2       = seqs[i].pushf;
        if (seqs[i].pushf)
            run_flags(&v, 1); /* PUSHF makes every lazy state concrete */
        else
            run_plain(&v, 4);
    }
    group_end(name);
}

/* ---- the stack: PUSH/POP corners, PUSHA/POPA, ENTER/LEAVE ----------------- */

/* EBP a frame pointer into the buffer, for ENTER's nested copies and
   LEAVE. */
static void
fix_frame(const struct variant *v)
{
    g_in.ebp = (uint32_t) g_buf + 48;
}

static void
group_stack(void)
{
    static const struct {
        uint8_t frame, len;
        uint8_t b[12];
    } seqs[] = {
        { 0, SEQ(0x54, 0x5e, 0xc7, 0x44, 0x24, 0xfc, 0, 0, 0, 0) },          /* push esp; pop esi; scrub */
        { 0, SEQ(0x57, 0x8f, 0x04, 0x24) },                                  /* push edi; pop [esp] */
        { 0, SEQ(0x57, 0x57, 0x8f, 0x44, 0x24, 0x04) },                      /* pop [esp+4] */
        { 0, SEQ(0x57, 0xff, 0x34, 0x24) },                                  /* push [esp] */
        { 0, SEQ(0x60, 0x61, 0xc7, 0x44, 0x24, 0xec, 0, 0, 0, 0) },          /* pushad; popad; scrub ESP slot */
        { 0, SEQ(0x66, 0x60, 0x66, 0x61, 0x66, 0xc7, 0x44, 0x24, 0xf6, 0, 0) }, /* pushaw; popaw */
        { 0, SEQ(0x6a, 0x80, 0x58) },                                        /* push -128; pop eax */
        { 0, SEQ(0x66, 0x6a, 0x80, 0x66, 0x58) },                            /* push word -128; pop ax */
        { 0, SEQ(0x68, 0x78, 0x56, 0x34, 0x12, 0x5b) },                      /* push imm32; pop ebx */
        { 0, SEQ(0x57, 0x5c) },                                              /* push edi; pop esp */
        { 0, SEQ(0x1e, 0x1f, 0xc7, 0x44, 0x24, 0xfc, 0, 0, 0, 0) },          /* push ds; pop ds */
        { 0, SEQ(0x16, 0x17, 0xc7, 0x44, 0x24, 0xfc, 0, 0, 0, 0) },          /* push ss; pop ss */
        { 1, SEQ(0xc8, 0x00, 0x00, 0x00) },                                  /* enter 0, 0 */
        { 1, SEQ(0xc8, 0x08, 0x00, 0x01) },                                  /* enter 8, 1 */
        { 1, SEQ(0xc8, 0x10, 0x00, 0x02) },                                  /* enter 16, 2 */
        { 1, SEQ(0xc8, 0x00, 0x01, 0x05) },                                  /* enter 256, 5 */
        { 1, SEQ(0xc8, 0x04, 0x00, 0x1f) },                                  /* enter 4, 31 */
        { 1, SEQ(0xc8, 0x04, 0x00, 0x20) },                                  /* enter 4, 32 (= 0) */
        { 1, SEQ(0xc8, 0x04, 0x00, 0x21) },                                  /* enter 4, 33 (= 1) */
        { 1, SEQ(0x66, 0xc8, 0x08, 0x00, 0x03) },                            /* enter, 16-bit */
        { 1, SEQ(0xc9) },                                                    /* leave */
        { 1, SEQ(0x66, 0xc9) },                                              /* leave, 16-bit */
        { 1, SEQ(0xc8, 0x10, 0x00, 0x03, 0xc9) },                            /* enter 16, 3; leave */
    };
    const char    *name = "stack";
    struct variant v    = { .group = name, .stack = 1 };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.fix_input  = seqs[i].frame ? fix_frame : 0;
        v.reg_fix    = 1;
        v.norm       = seqs[i].frame ? 1 << R_EBP : (seqs[i].b[0] == 0x54 ? 1 << R_ESI : 0);
        run_plain(&v, 1);
    }
    group_end(name);
}

/* ---- far transfers in protected mode: CALL, JMP, RETF, IRETD --------------- */

/* mov dword [esp+disp8], 0: wipes a stack slot that held an absolute
   address or undefined flags, so the sandbox CRC doesn't see it. */
static void
emit_scrub(struct emit *e, int8_t disp)
{
    static const uint8_t b[4] = { 0xc7, 0x44, 0x24 };
    ebytes(e, b, 3);
    e8(e, (uint8_t) disp);
    e32(e, 0);
}

static void
emit_lea_inc(struct emit *e, int reg) /* lea reg, [reg+1]: no flags */
{
    e8(e, 0x8d);
    e8(e, 0x40 | (reg << 3) | reg);
    e8(e, 0x01);
}

static void
emit_ud2(struct emit *e)
{
    e8(e, 0x0f);
    e8(e, 0x0b);
}

/* arg2 is the kind; the target lands at L1 (ESI + 1) and comes back to the
   code after it (EDI + 1). */
static void
emit_far(struct emit *e, const struct variant *v)
{
    int      kind = v->arg2;
    uint8_t *call = e->p, *jmp = 0, *l1, *ptr = 0, *over;

    switch (kind) {
        case 0: /* call far ptr16:32 ... retf */
        case 1: /* ... retf 8 */
            e8(e, 0x9a);
            e32(e, 0);
            e8(e, 0x08);
            e8(e, 0x00);
            break;
        case 2: /* call far [m16:32] ... retf */
            e8(e, 0xff);
            e8(e, 0x1d);
            e32(e, 0);
            break;
        case 3: /* jmp far ptr16:32 */
            e8(e, 0xea);
            e32(e, 0);
            e8(e, 0x08);
            e8(e, 0x00);
            emit_ud2(e);
            break;
        case 4: /* jmp far [m16:32] */
            e8(e, 0xff);
            e8(e, 0x2d);
            e32(e, 0);
            emit_ud2(e);
            ptr = e->p;
            e32(e, 0);
            e8(e, 0x08);
            e8(e, 0x00);
            break;
        case 5: /* call near ... ret 8 */
            e8(e, 0xe8);
            e32(e, 0);
            break;
        default: /* pushfd; push cs; push L1; iretd */
            e8(e, 0x9c);
            e8(e, 0x0e);
            e8(e, 0x68);
            e32(e, 0);
            e8(e, 0xcf);
            emit_ud2(e);
            break;
    }
    if (kind <= 2 || kind == 5) {
        jmp = e->p;
        e8(e, 0xeb);
        e8(e, 0x00);
    }
    l1 = e->p;
    emit_lea_inc(e, R_ESI);
    if (kind == 0 || kind == 2)
        e8(e, 0xcb);
    else if (kind == 1) {
        e8(e, 0xca);
        e8(e, 0x08);
        e8(e, 0x00);
    } else if (kind == 5) {
        e8(e, 0xc2);
        e8(e, 0x08);
        e8(e, 0x00);
    }
    if (kind == 2) {
        ptr = e->p;
        e32(e, 0);
        e8(e, 0x08);
        e8(e, 0x00);
    }
    over = e->p;
    if (jmp)
        jmp[1] = over - (jmp + 2);
    if (ptr)
        put_le(ptr, (uint32_t) l1, 4);
    switch (kind) {
        case 0:
        case 1:
        case 3: put_le(call + 1, (uint32_t) l1, 4); break;
        case 2:
        case 4: put_le(call + 2, (uint32_t) ptr, 4); break;
        case 5: put_le(call + 1, (uint32_t) (l1 - (call + 5)), 4); break;
        default: put_le(call + 3, (uint32_t) l1, 4); break;
    }
    emit_lea_inc(e, R_EDI);
    switch (kind) {
        case 0:
        case 2:
            emit_scrub(e, -8);
            emit_scrub(e, -4);
            break;
        case 1:
            emit_scrub(e, -16);
            emit_scrub(e, -12);
            break;
        case 5: emit_scrub(e, -12); break;
        case 6:
            emit_scrub(e, -12);
            emit_scrub(e, -8);
            emit_scrub(e, -4);
            break;
        default: break;
    }
}

static void
group_far(void)
{
    static const struct {
        uint8_t len, b[2];
    } sigs[7] = { { SEQ(0x9a, 0xcb) }, { SEQ(0x9a, 0xca) }, { SEQ(0xff, 0x1d) }, { SEQ(0xea) },
                  { SEQ(0xff, 0x2d) }, { SEQ(0xe8, 0xc2) }, { SEQ(0xcf) } };
    const char    *name = "far.call";
    struct variant v    = { .group = name, .stack = 1, .emit = emit_far };
    group_begin(name);
    for (int k = 0; k < 7; k++) {
        memcpy(v.target, sigs[k].b, sigs[k].len); /* for the reports only */
        v.target_len = sigs[k].len;
        v.arg2       = k;
        run_plain(&v, 1);
    }
    group_end(name);
}

/* ---- software interrupts and invalid encodings ---------------------------- */

static void
group_int(void)
{
    static const struct {
        uint8_t reads_flags, len, b[3];
    } seqs[] = {
        { 0, SEQ(0xcc) },       /* INT3 */
        { 0, SEQ(0xcd, 0x03) }, /* INT 3 */
        { 0, SEQ(0xcd, 0x00) }, /* INT 0 */
        { 1, SEQ(0xce) },       /* INTO: traps only with OF */
        { 0, SEQ(0xf1) },       /* ICEBP */
        { 0, SEQ(0xcd, 0x40) }, /* INT 40h, beyond the IDT: #GP(0x202) */
        { 0, SEQ(0x0f, 0x0b) }, /* UD2 */
        { 0, SEQ(0xf0, 0x90) }, /* LOCK NOP: #UD */
        { 0, SEQ(0x8d, 0xc0) }, /* LEA with a register: #UD (not LES/LDS: VEX on newer CPUs) */
        { 0, SEQ(0x8e, 0xc8) }, /* MOV CS, AX: #UD */
    };
    const char    *name = "int.ud";
    struct variant v    = { .group = name, .faults_ok = 1 };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        if (seqs[i].reads_flags)
            run_flags(&v, 1);
        else
            run_plain(&v, 1);
    }
    group_end(name);
}

/* ---- BOUND, ARPL ---------------------------------------------------------- */

/* Bounds at [ebx] and [ebx+size]; the index below, on the low bound,
   inside, on the high bound, above, and with the bounds reversed; then
   random. */
static void
fix_bound(const struct variant *v)
{
    int     size = v->arg, e = edge(6);
    int32_t lo = (int32_t) rnd_operand(), hi = (int32_t) rnd_operand();
    g_in.ebx   = (uint32_t) g_buf + MEM_OFF;
    if (size == 16) {
        lo = (int16_t) lo;
        hi = (int16_t) hi;
    }
    /* Ordered bounds (reversed for case 5), at least one apart. */
    if ((lo > hi) != (e == 5)) {
        int32_t t = lo;
        lo        = hi;
        hi        = t;
    }
    if (lo == hi)
        lo = (int32_t) ((uint32_t) lo - 1);
    if (size == 16)
        lo = (int16_t) lo;
    put_le(g_in.buf + MEM_OFF, (uint32_t) lo, size / 8);
    put_le(g_in.buf + MEM_OFF + size / 8, (uint32_t) hi, size / 8);
    uint32_t x;
    switch (e) {
        case 0: x = (uint32_t) lo - 1; break;
        case 1: x = (uint32_t) lo; break;
        case 2: x = hi > lo ? (uint32_t) lo + rnd() % ((uint32_t) hi - (uint32_t) lo) : (uint32_t) lo; break;
        case 3: x = (uint32_t) hi; break;
        case 4: x = (uint32_t) hi + 1; break;
        case 5: x = (uint32_t) lo; break;
        default: x = rnd(); break;
    }
    set_opnd(v->arg2, size, x);
}

/* ARPL: the destination's RPL below, equal to and above the source's, then
   random. */
static void
fix_arpl(const struct variant *v)
{
    fix_rm(v);
    int e = edge(3);
    if (e >= 0) {
        uint32_t src = get_opnd(v->arg3, 16) & ~3u, dst = get_opnd(v->arg2, 16) & ~3u;
        set_opnd(v->arg3, 16, src | 2);
        set_opnd(v->arg2, 16, dst | (e == 0 ? 1 : e == 1 ? 2 : 3));
    }
}

/* ARPL writes ZF only. */
static int
dm_arpl(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    if (d->fault == 0xff)
        *undef &= ~F_ZF;
    return 1;
}

static void
group_bound_arpl(void)
{
    static const uint8_t regs[] = { R_EAX, R_ECX, R_ESI };
    const char          *name   = "bound.arpl";
    struct variant       v      = { .group = name, .faults_ok = 1 };
    group_begin(name);
    for (int size = 16; size <= 32; size += 16) {
        for (unsigned r = 0; r < sizeof(regs); r++) {
            int n = 0;
            if (size == 16)
                v.target[n++] = 0x66;
            v.target[n++] = 0x62;
            v.target[n++] = 0x03 | (regs[r] << 3);
            v.target_len  = n;
            v.arg         = size;
            v.arg2        = regs[r];
            v.fix_input   = fix_bound;
            v.reg_fix     = 0;
            v.norm        = 1 << R_EBX;
            run_keep(&v, 7);
        }
    }
    /* BOUND with a register operand: #UD. From AVX-512 on, 62 with a
       register ModRM is an EVEX prefix instead; only CPUs without SSE2. */
    if (!(g_cpu.features & (1u << 26))) {
        v.target[0]  = 0x62;
        v.target[1]  = MODRM_RR(R_EAX, R_ECX);
        v.target_len = 2;
        v.fix_input  = 0;
        v.norm       = 0;
        run_plain(&v, 1);
    }

    /* ARPL r/m16, r16 */
    static const uint8_t pairs[3][2] = { { R_EAX, R_ECX }, { RM_MEM, R_EDX }, { R_ESI, R_ESI } }; /* dst, src */
    v.fx      = FX_CUSTOM;
    v.defmask = dm_arpl;
    for (int p = 0; p < 3; p++) {
        v.target[0]  = 0x63;
        v.target_len = put_modrm(v.target, 1, pairs[p][1], pairs[p][0]);
        v.arg3       = pairs[p][1];
        set_fix_rm(&v, pairs[p][0], fix_arpl);
        run_keep(&v, 4);
    }
    group_end(name);
}

/* ---- odds and ends -------------------------------------------------------- */

static void
fix_xlat(const struct variant *v)
{
    g_in.ebx = (uint32_t) g_buf + (rnd() & 31);
    if (rnd() & 1)
        g_in.eax = (g_in.eax & ~0xffu) | (rnd() & 31);
}

static void
group_misc(void)
{
    static const struct {
        uint8_t fix, reads_flags, len, b[3];
    } seqs[] = {
        { 1, 0, SEQ(0xd7) },             /* XLAT */
        { 0, 1, SEQ(0xf5) },             /* CMC */
        { 0, 0, SEQ(0xf8) },             /* CLC */
        { 0, 0, SEQ(0xf9) },             /* STC */
        { 0, 0, SEQ(0xfc) },             /* CLD */
        { 0, 0, SEQ(0xfd) },             /* STD */
        { 0, 0, SEQ(0x9b) },             /* WAIT */
        { 0, 0, SEQ(0x66, 0x90) },       /* xchg ax, ax */
        { 0, 0, SEQ(0xf3, 0x90) },       /* PAUSE */
        { 2, 0, SEQ(0x87, 0x03) },       /* xchg [ebx], eax */
        { 2, 0, SEQ(0x86, 0x23) },       /* xchg [ebx], ah */
        { 2, 0, SEQ(0x66, 0x87, 0x0b) }, /* xchg [ebx], cx */
        { 0, 0, SEQ(0x87, 0xca) },       /* xchg edx, ecx */
        { 0, 0, SEQ(0x86, 0xe0) },       /* xchg al, ah */
    };
    const char    *name = "misc";
    struct variant v    = { .group = name };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.fix_input  = seqs[i].fix == 1 ? fix_xlat : seqs[i].fix == 2 ? fix_mem_ebx : 0;
        v.arg        = MEM_OFF;
        v.norm       = seqs[i].fix == 1 ? 1 << R_EBX : 0;
        if (seqs[i].reads_flags)
            run_flags(&v, 1);
        else
            run_plain(&v, 2);
    }
    group_end(name);
}

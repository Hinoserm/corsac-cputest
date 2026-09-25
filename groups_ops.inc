/*
 * Groups added after the first six: the instructions 86Box's recompiler
 * still hands to the interpreter, then the corners around them. Included
 * into groups.inc. The same rule holds: never reorder, only append.
 */

/* An r/m operand that is memory: [ebx], EBX pointing 8 bytes into the buffer. */
#define RM_MEM   8
#define MEM_OFF  8

/* Producers that leave ECX and EDX alone, for tests whose defined result
   depends on an input the producer would otherwise change (a count in CL,
   a source operand). none, cmp, test: unknown, SUB and ZN lazy states. */
static const uint8_t ctx_keep[] = { 0, 1, 9 };
/* none, add, sub. */
static const uint8_t ctx_some[] = { 0, 3, 2 };

static void
run_ctx(struct variant *v, const uint8_t *set, int n, int inputs)
{
    for (int p = 0; p < n; p++) {
        for (int b = 0; b < 2; b++) {
            v->producer = set[p];
            v->boundary = b;
            run_inputs(v, inputs);
        }
    }
}

/* No producer, then ADD across a block boundary. */
static void
run_two(struct variant *v, int inputs)
{
    v->producer = 0;
    v->boundary = 0;
    run_inputs(v, inputs);
    v->producer = 3;
    v->boundary = 1;
    run_inputs(v, inputs);
}

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
            for (int dst = 0; dst < 8; dst++) {
                if (dst == R_ESP)
                    continue;
                for (int src = 0; src <= RM_MEM; src++) {
                    if (src == R_ESP || (src == RM_MEM && dst == R_EBX))
                        continue;
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
                        set_fix_rm(&v, src, src == RM_MEM ? fix_rm : 0);
                        run_two(&v, 2);
                    }
                }
            }
        }
    }
    group_end(name);
}

/* ---- MUL, IMUL, DIV, IDIV (F6/F7 /4-/7) ----------------------------------- */

/* Mostly dividends that fit: the high half below the divisor (DIV) or the
   sign extension of the low half (IDIV). A quarter as they come, which
   mostly overflow into #DE, and some divisors of zero. */
static void
fix_div(const struct variant *v)
{
    int      size = v->arg3 & 0xff, sgn = (v->arg3 >> 8) & 1, rm = v->arg2;
    uint32_t mask = szmask(size);

    fix_rm(v);
    int mode = rnd() & 3;
    if (mode == 0)
        return;
    /* The divisor is part of the dividend: leave it to chance. */
    if (size == 8 ? (rm == 0 || rm == 4) : (rm == R_EAX || rm == R_EDX))
        return;
    uint32_t div = mode == 1 ? rnd_operand() & mask : rnd() & mask;
    if (mode == 3)
        div &= 0xff;
    set_opnd(rm, size, div);
    uint32_t low = get_opnd(R_EAX, size), high;
    if (sgn)
        high = (low >> (size - 1)) & 1 ? mask : 0;
    else
        high = div ? rnd() % div : rnd() & mask;
    set_opnd(size == 8 ? 4 : R_EDX, size, high);
}

static void
group_muldiv(void)
{
    const char    *name = "mul.div";
    struct variant v    = { .group = name, .fx = FX_CUSTOM };
    group_begin(name);
    for (int size = 8; size <= 32; size *= 2) {
        for (int op = 4; op < 8; op++) {
            for (int rm = 0; rm <= RM_MEM; rm++) {
                if (size != 8 && rm == R_ESP)
                    continue;
                int n        = put_op(v.target, 0, size, 0xf6);
                n            = put_modrm(v.target, n, op, rm);
                v.target_len = n;
                int div      = op >= 6;
                set_fix_rm(&v, rm, div ? fix_div : rm == RM_MEM ? fix_rm : 0);
                v.arg3      = size | ((op == 7) << 8);
                v.defmask   = div ? dm_div : dm_mul;
                v.faults_ok = div;
                run_ctx(&v, ctx_some, 3, 6);
            }
        }
    }
    group_end(name);
}

/* ---- shifts and rotates: by CL, by 1, by an immediate --------------------- */

static const uint8_t shift_counts[] = { 0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 18, 31, 32, 33, 63, 255 };

/* arg3: the count, or 0x100 for CL (then chosen here). */
static void
fix_count(const struct variant *v)
{
    fix_rm(v);
    if (v->arg3 & 0x100) {
        uint32_t c = (rnd() & 1) ? shift_counts[rnd() % sizeof(shift_counts)] : rnd() & 0xff;
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
    static const uint8_t rms[]  = { 0, 1, 3, 5, 7, RM_MEM }; /* AL CL BL CH BH / EAX ECX EBX EBP EDI */
    static const uint8_t imms[] = { 0, 1, 2, 7, 8, 9, 16, 17, 31, 32, 33 };
    const char          *name   = "shift.rotate";
    struct variant       v      = { .group = name, .fx = FX_CUSTOM, .defmask = dm_shift };
    group_begin(name);
    for (int form = 0; form < 3; form++) { /* D2/D3 by CL, D0/D1 by 1, C0/C1 imm */
        for (int size = 8; size <= 32; size *= 2) {
            for (int op = 0; op < 8; op++) {
                for (unsigned r = 0; r < sizeof(rms); r++) {
                    int nimm = form == 2 ? (int) sizeof(imms) : 1;
                    for (int k = 0; k < nimm; k++) {
                        static const uint8_t base[3] = { 0xd2, 0xd0, 0xc0 };
                        int                  n       = put_op(v.target, 0, size, base[form]);
                        n                            = put_modrm(v.target, n, op, rms[r]);
                        if (form == 2)
                            v.target[n++] = imms[k];
                        v.target_len = n;
                        v.fx_arg     = op;
                        v.arg        = size;
                        v.arg3       = form == 0 ? 0x100 : form == 1 ? 1 : imms[k];
                        set_fix_rm(&v, rms[r], fix_count);
                        if (form == 0)
                            run_ctx(&v, ctx_keep, 3, 4);
                        else if (form == 1 && (op == 2 || op == 3))
                            run_all_contexts(&v, 1); /* RCL, RCR read CF from every producer */
                        else if (form == 1)
                            run_ctx(&v, ctx_some, 3, 2);
                        else
                            run_two(&v, 1);
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
    static const uint8_t ops[4]  = { 0xa4, 0xa5, 0xac, 0xad }; /* SHLD imm, SHLD CL, SHRD imm, SHRD CL */
    static const uint8_t dsts[]  = { R_EAX, R_ECX, R_EBX, R_ESI, RM_MEM };
    static const uint8_t srcs[]  = { R_EDX, R_EDI, R_ECX };
    static const uint8_t imms[]  = { 0, 1, 4, 8, 15, 16, 17, 24, 31, 32, 33 };
    const char          *name    = "shld.shrd";
    struct variant       v       = { .group = name, .fx = FX_CUSTOM, .defmask = dm_shd };
    group_begin(name);
    for (int o = 0; o < 4; o++) {
        int by_cl = ops[o] & 1;
        for (int size = 16; size <= 32; size += 16) {
            for (unsigned d = 0; d < sizeof(dsts); d++) {
                for (unsigned s = 0; s < sizeof(srcs); s++) {
                    int nimm = by_cl ? 1 : (int) sizeof(imms);
                    for (int k = 0; k < nimm; k++) {
                        int n = 0;
                        if (size == 16)
                            v.target[n++] = 0x66;
                        v.target[n++] = 0x0f;
                        v.target[n++] = ops[o];
                        n             = put_modrm(v.target, n, srcs[s], dsts[d]);
                        if (!by_cl)
                            v.target[n++] = imms[k];
                        v.target_len = n;
                        v.arg        = size;
                        v.arg3       = by_cl ? 0x100 : imms[k];
                        set_fix_rm(&v, dsts[d], fix_count);
                        if (by_cl)
                            run_ctx(&v, ctx_keep, 3, 4);
                        else
                            run_two(&v, 1);
                    }
                }
            }
        }
    }
    group_end(name);
}

/* ---- BSF, BSR ------------------------------------------------------------- */

/* The source: zero a quarter of the time, a single bit a quarter. */
static void
fix_bs(const struct variant *v)
{
    int      size = v->arg;
    uint32_t val;
    fix_rm(v);
    switch (rnd() & 3) {
        case 0: val = 0; break;
        case 1: val = 1u << (rnd() % size); break;
        case 2: val = rnd(); break;
        default: val = rnd_operand(); break;
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
    const char    *name = "bsf.bsr";
    struct variant v    = { .group = name, .fx = FX_CUSTOM, .defmask = dm_bs };
    group_begin(name);
    for (int op = 0xbc; op <= 0xbd; op++) {
        for (int size = 16; size <= 32; size += 16) {
            for (int dst = 0; dst < 8; dst++) {
                if (dst == R_ESP)
                    continue;
                for (int src = 0; src <= RM_MEM; src++) {
                    if (src == R_ESP || (src == RM_MEM && dst == R_EBX))
                        continue;
                    int n = 0;
                    if (size == 16)
                        v.target[n++] = 0x66;
                    v.target[n++] = 0x0f;
                    v.target[n++] = op;
                    n             = put_modrm(v.target, n, dst, src);
                    v.target_len  = n;
                    v.arg         = size;
                    v.arg3        = dst;
                    set_fix_rm(&v, src, fix_bs);
                    run_ctx(&v, ctx_keep, 3, 3);
                }
            }
        }
    }
    group_end(name);
}

/* ---- CMPXCHG, XADD, CMPXCHG8B --------------------------------------------- */

/* Half the time the accumulator equals the destination, so CMPXCHG
   stores. */
static void
fix_cx(const struct variant *v)
{
    fix_rm(v);
    if (rnd() & 1)
        set_opnd(R_EAX, v->arg, get_opnd(v->arg2, v->arg));
}

/* [esi], ESI 8 bytes into the buffer; half the time EDX:EAX matches. */
static void
fix_cx8(const struct variant *v)
{
    g_in.esi = (uint32_t) g_buf + MEM_OFF;
    if (rnd() & 1) {
        g_in.eax = le32(g_in.buf + MEM_OFF);
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
    static const uint8_t srcs8[]  = { 0, 2, 5, 7 }; /* AL DL CH BH */
    static const uint8_t srcs32[] = { R_EAX, R_EDX, R_ESI, R_EDI };
    const char          *name     = "cmpxchg.xadd";
    struct variant       v        = { .group = name, .fx = FX_CUSTOM, .defmask = dm_alldef };
    if (!g_cpu.is486) {
        group_skip(name, "386");
        return;
    }
    group_begin(name);
    for (int op = 0; op < 2; op++) { /* CMPXCHG 0F B0/B1, XADD 0F C0/C1 */
        uint8_t op8 = op ? 0xc0 : 0xb0;
        for (int size = 8; size <= 32; size *= 2) {
            /* dst 0-7, RM_MEM, and RM_MEM + 1 for a locked memory form */
            for (int dst = 0; dst <= RM_MEM + 1; dst++) {
                if (size != 8 && dst == R_ESP)
                    continue;
                for (int s = 0; s < 4; s++) {
                    int src  = size == 8 ? srcs8[s] : srcs32[s];
                    int rm   = dst > RM_MEM ? RM_MEM : dst;
                    int n    = 0;
                    if (dst > RM_MEM)
                        v.target[n++] = 0xf0;
                    if (size == 16)
                        v.target[n++] = 0x66;
                    v.target[n++] = 0x0f;
                    v.target[n++] = size == 8 ? op8 : op8 | 1;
                    n             = put_modrm(v.target, n, src, rm);
                    v.target_len  = n;
                    v.arg         = size;
                    v.faults_ok   = 0;
                    set_fix_rm(&v, rm, fix_cx);
                    run_ctx(&v, ctx_some, 3, 4);
                }
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
                run_ctx(&v, ctx_some, 3, 2);
            }
        }
    }
    /* CMPXCHG8B. Never with LOCK and a register operand (F0 0F C7 C8+):
       that is the Pentium's F00F erratum and hangs the machine. */
    if (g_cpu.features & (1u << 8)) { /* CX8 */
        static const uint8_t forms[3][4] = { { 0x0f, 0xc7, 0x0e }, { 0xf0, 0x0f, 0xc7, 0x0e }, { 0x0f, 0xc7, 0xc9 } };
        static const uint8_t lens[3]     = { 3, 4, 3 };
        for (int f = 0; f < 3; f++) {
            memcpy(v.target, forms[f], lens[f]);
            v.target_len = lens[f];
            v.defmask    = dm_cx8;
            v.fix_input  = fix_cx8;
            v.reg_fix    = 0;
            v.norm       = 1 << R_ESI;
            v.faults_ok  = f == 2; /* the register form is #UD */
            run_ctx(&v, ctx_keep, 3, 16);
        }
    }
    group_end(name);
}

/* ---- DAA DAS AAA AAS AAM AAD SALC ----------------------------------------- */

/* AL and AH as packed or unpacked BCD half the time. */
static void
fix_bcd(const struct variant *v)
{
    if (rnd() & 1) {
        uint32_t al = ((rnd() % 10) << 4) | (rnd() % 10);
        uint32_t ah = rnd() % 10;
        if (rnd() & 1)
            al = rnd() % 10; /* unpacked */
        g_in.eax = (g_in.eax & 0xffff0000u) | (ah << 8) | al;
    }
}

/* fx_arg: 0 DAA/DAS, 1 AAA/AAS, 2 AAM/AAD, 3 SALC. */
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
    struct variant       v      = { .group = name, .fx = FX_CUSTOM, .defmask = dm_bcd,
                                    .fix_input = fix_bcd, .reg_fix = 1, .faults_ok = 1 };
    group_begin(name);
    for (int i = 0; i < 4; i++) { /* DAA DAS AAA AAS */
        static const uint8_t ops[4] = { 0x27, 0x2f, 0x37, 0x3f };
        v.target[0]  = ops[i];
        v.target_len = 1;
        v.fx_arg     = i >> 1;
        run_all_contexts(&v, 4);
    }
    for (int op = 0xd4; op <= 0xd5; op++) { /* AAM, AAD; AAM 0 is #DE */
        for (unsigned k = 0; k < sizeof(imms); k++) {
            v.target[0]  = op;
            v.target[1]  = imms[k];
            v.target_len = 2;
            v.fx_arg     = 2;
            run_all_contexts(&v, 4);
        }
    }
    v.target[0]  = 0xd6; /* SALC */
    v.target_len = 1;
    v.fx_arg     = 3;
    run_all_contexts(&v, 4);
    group_end(name);
}

/* ---- MOVS CMPS STOS LODS SCAS, with and without REP ----------------------- */

/* ESI in the first half of the buffer, EDI in the second, both walking
   down from near the top when DF is set. arg: size; arg2: REP prefix;
   arg3 bit 0: 16-bit addressing (the low buffer). */
static void
fix_str(const struct variant *v)
{
    int size = v->arg, rep = v->arg2;

    if (v->arg3 & 1) {
#ifdef HOSTTEST
        g_skip = 1;
#endif
        g_in.flags &= ~0x400u; /* stay inside the 16-byte low buffer */
        g_in.esi = (rnd() & 0xffff0000u) | ((uint32_t) LOWBUF + (rnd() & 1));
        g_in.edi = (rnd() & 0xffff0000u) | ((uint32_t) LOWBUF + 6 + (rnd() & 1));
        g_in.ecx = (rnd() & 0xffff0000u) | (rnd() % 3);
        return;
    }
    int down = (g_in.flags & 0x400) != 0;
    g_in.esi = (uint32_t) g_buf + (down ? 24 : 0) + (rnd() & 7);
    g_in.edi = g_in.esi + 32;
    g_in.ecx = rep ? rnd() % 10 : rnd_operand();
    /* CMPS: the strings agree up to one flipped bit, half the time. */
    if (rnd() & 1) {
        memcpy(g_in.buf + 32, g_in.buf, 32);
        g_in.buf[32 + (rnd() & 31)] ^= 1 << (rnd() & 7);
    }
    /* SCAS: the accumulator matches an element some way along. */
    if (rnd() & 1) {
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
                    run_ctx(&v, ctx_keep, 3, 6);
                }
            }
        }
    }
    /* A segment override on the source: ES: MOVSD, REP ES: MOVSB. */
    {
        static const uint8_t seqs[2][3] = { { 0x26, 0xa5 }, { 0xf3, 0x26, 0xa4 } };
        for (int i = 0; i < 2; i++) {
            memcpy(v.target, seqs[i], 3);
            v.target_len = i ? 3 : 2;
            v.arg        = i ? 8 : 32;
            v.arg2       = i;
            v.arg3       = 0;
            v.fx_arg     = 0;
            v.no_ref     = 0;
            v.norm       = (1 << R_ESI) | (1 << R_EDI);
            run_ctx(&v, ctx_keep, 3, 6);
        }
    }
    group_end(name);
}

/* ---- POPF, PUSHF ---------------------------------------------------------- */

/* EFLAGS as POPF may load them: anything but TF and IF. */
#define POPF_MASK 0x0027ffffu

static void
fix_popf(const struct variant *v)
{
    g_in.edi = rnd() & ~0x300u;
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
        uint8_t len, pushf;
        uint8_t b[14];
    } seqs[] = {
        { 2, 0, { 0x57, 0x9d } },                                       /* push edi; popfd */
        { 4, 0, { 0x66, 0x57, 0x66, 0x9d } },                           /* push di; popf */
        { 10, 1, { 0x9c, 0x5e, 0xc7, 0x44, 0x24, 0xfc, 0, 0, 0, 0 } },  /* pushfd; pop esi; scrub */
        { 11, 1, { 0x66, 0x9c, 0x66, 0x5e, 0x66, 0xc7, 0x44, 0x24, 0xfe, 0, 0 } }, /* pushf; pop si; scrub */
        { 4, 0, { 0x57, 0x9d, 0x9c, 0x5e } },                           /* popfd; pushfd; pop esi */
        { 8, 0, { 0x57, 0x9d, 0x0f, 0x92, 0xc0, 0x0f, 0x90, 0xc4 } },   /* popfd; setb al; seto ah */
        { 4, 0, { 0x57, 0x9d, 0x11, 0xd1 } },                           /* popfd; adc ecx, edx */
        { 3, 0, { 0x57, 0x9d, 0x9f } },                                 /* popfd; lahf */
        { 7, 0, { 0x66, 0x57, 0x66, 0x9d, 0x0f, 0x94, 0xc0 } },         /* popf; setz al */
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
            run_all_contexts(&v, 6); /* PUSHF makes every lazy state concrete */
        else
            run_ctx(&v, ctx_some, 3, 16);
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
        uint8_t len, frame;
        uint8_t b[12];
    } seqs[] = {
        { 10, 0, { 0x54, 0x5e, 0xc7, 0x44, 0x24, 0xfc, 0, 0, 0, 0 } },       /* push esp; pop esi; scrub */
        { 4, 0, { 0x57, 0x8f, 0x04, 0x24 } },                                 /* push edi; pop [esp] */
        { 6, 0, { 0x57, 0x57, 0x8f, 0x44, 0x24, 0x04 } },                     /* pop [esp+4] */
        { 4, 0, { 0x57, 0xff, 0x34, 0x24 } },                                 /* push [esp] */
        { 10, 0, { 0x60, 0x61, 0xc7, 0x44, 0x24, 0xec, 0, 0, 0, 0 } },        /* pushad; popad; scrub ESP slot */
        { 11, 0, { 0x66, 0x60, 0x66, 0x61, 0x66, 0xc7, 0x44, 0x24, 0xf6, 0, 0 } }, /* pushaw; popaw */
        { 3, 0, { 0x6a, 0x80, 0x58 } },                                       /* push -128; pop eax */
        { 5, 0, { 0x66, 0x6a, 0x80, 0x66, 0x58 } },                           /* push word -128; pop ax */
        { 6, 0, { 0x68, 0x78, 0x56, 0x34, 0x12, 0x5b } },                     /* push imm32; pop ebx */
        { 2, 0, { 0x57, 0x5c } },                                             /* push edi; pop esp */
        { 10, 0, { 0x1e, 0x1f, 0xc7, 0x44, 0x24, 0xfc, 0, 0, 0, 0 } },        /* push ds; pop ds */
        { 10, 0, { 0x16, 0x17, 0xc7, 0x44, 0x24, 0xfc, 0, 0, 0, 0 } },        /* push ss; pop ss */
        { 4, 1, { 0xc8, 0x00, 0x00, 0x00 } },                                 /* enter 0, 0 */
        { 4, 1, { 0xc8, 0x08, 0x00, 0x01 } },                                 /* enter 8, 1 */
        { 4, 1, { 0xc8, 0x10, 0x00, 0x02 } },                                 /* enter 16, 2 */
        { 4, 1, { 0xc8, 0x00, 0x01, 0x05 } },                                 /* enter 256, 5 */
        { 4, 1, { 0xc8, 0x04, 0x00, 0x1f } },                                 /* enter 4, 31 */
        { 4, 1, { 0xc8, 0x04, 0x00, 0x20 } },                                 /* enter 4, 32 (= 0) */
        { 4, 1, { 0xc8, 0x04, 0x00, 0x21 } },                                 /* enter 4, 33 (= 1) */
        { 5, 1, { 0x66, 0xc8, 0x08, 0x00, 0x03 } },                           /* enter, 16-bit */
        { 1, 1, { 0xc9 } },                                                   /* leave */
        { 2, 1, { 0x66, 0xc9 } },                                             /* leave, 16-bit */
        { 5, 1, { 0xc8, 0x10, 0x00, 0x03, 0xc9 } },                           /* enter 16, 3; leave */
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
        run_ctx(&v, ctx_some, 3, 4);
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
    static const uint8_t sigs[7][2] = { { 0x9a, 0xcb }, { 0x9a, 0xca }, { 0xff, 0x1d }, { 0xea },
                                        { 0xff, 0x2d }, { 0xe8, 0xc2 }, { 0xcf } };
    static const uint8_t lens[7]    = { 2, 2, 2, 1, 2, 2, 1 };
    const char          *name       = "far.call";
    struct variant       v          = { .group = name, .stack = 1, .emit = emit_far };
    group_begin(name);
    for (int k = 0; k < 7; k++) {
        memcpy(v.target, sigs[k], lens[k]); /* for the reports only */
        v.target_len = lens[k];
        v.arg2       = k;
        run_ctx(&v, ctx_some, 3, 4);
    }
    group_end(name);
}

/* ---- software interrupts and invalid encodings ---------------------------- */

static void
group_int(void)
{
    static const struct {
        uint8_t len;
        uint8_t b[3];
    } seqs[] = {
        { 1, { 0xcc } },             /* INT3 */
        { 2, { 0xcd, 0x03 } },       /* INT 3 */
        { 2, { 0xcd, 0x00 } },       /* INT 0 */
        { 1, { 0xce } },             /* INTO: traps only with OF */
        { 1, { 0xf1 } },             /* ICEBP */
        { 2, { 0xcd, 0x40 } },       /* INT 40h, beyond the IDT: #GP(0x202) */
        { 2, { 0x0f, 0x0b } },       /* UD2 */
        { 2, { 0xf0, 0x90 } },       /* LOCK NOP: #UD */
        { 2, { 0x8d, 0xc0 } },       /* LEA with a register: #UD (not LES/LDS: VEX on newer CPUs) */
        { 2, { 0x8e, 0xc8 } },       /* MOV CS, AX: #UD */
    };
    const char    *name = "int.ud";
    struct variant v    = { .group = name, .faults_ok = 1 };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        run_ctx(&v, ctx_some, 3, 4);
    }
    group_end(name);
}

/* ---- BOUND, ARPL ---------------------------------------------------------- */

/* Bounds at [ebx] and [ebx+size]; the index below, inside, on or above. */
static void
fix_bound(const struct variant *v)
{
    int     size = v->arg;
    int32_t lo   = (int32_t) rnd_operand(), hi = (int32_t) rnd_operand(), x;
    g_in.ebx     = (uint32_t) g_buf + MEM_OFF;
    if (size == 16) {
        lo = (int16_t) lo;
        hi = (int16_t) hi;
    }
    if ((rnd() & 3) && lo > hi) {
        int32_t t = lo;
        lo        = hi;
        hi        = t;
    }
    put_le(g_in.buf + MEM_OFF, (uint32_t) lo, size / 8);
    put_le(g_in.buf + MEM_OFF + size / 8, (uint32_t) hi, size / 8);
    switch (rnd() & 3) {
        case 0: x = (int32_t) rnd(); break;
        case 1: x = lo - (rnd() & 1); break;
        case 2: x = hi + (rnd() & 1); break;
        default: x = hi >= lo ? lo + (int32_t) (rnd() % ((uint32_t) (hi - lo) + 1)) : lo; break;
    }
    set_opnd(v->arg2, size, (uint32_t) x);
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
    static const uint8_t regs[] = { R_EAX, R_ECX, R_EDX, R_EBP, R_ESI, R_EDI };
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
            run_ctx(&v, ctx_keep, 3, 8);
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
        run_ctx(&v, ctx_some, 3, 2);
    }

    /* ARPL r/m16, r16 */
    static const uint8_t dsts[] = { R_EAX, R_EBX, R_ESI, RM_MEM };
    static const uint8_t srcs[] = { R_ECX, R_EDX, R_EDI };
    v.fx      = FX_CUSTOM;
    v.defmask = dm_arpl;
    for (unsigned d = 0; d < sizeof(dsts); d++) {
        for (unsigned s = 0; s < sizeof(srcs); s++) {
            v.target[0]  = 0x63;
            v.target_len = put_modrm(v.target, 1, srcs[s], dsts[d]);
            set_fix_rm(&v, dsts[d], dsts[d] == RM_MEM ? fix_rm : 0);
            run_ctx(&v, ctx_some, 3, 4);
        }
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
        uint8_t len, fix;
        uint8_t b[3];
    } seqs[] = {
        { 1, 1, { 0xd7 } },             /* XLAT */
        { 1, 0, { 0xf5 } },             /* CMC */
        { 1, 0, { 0xf8 } },             /* CLC */
        { 1, 0, { 0xf9 } },             /* STC */
        { 1, 0, { 0xfc } },             /* CLD */
        { 1, 0, { 0xfd } },             /* STD */
        { 1, 0, { 0x9b } },             /* WAIT */
        { 2, 0, { 0x66, 0x90 } },       /* xchg ax, ax */
        { 2, 0, { 0xf3, 0x90 } },       /* PAUSE */
        { 2, 2, { 0x87, 0x03 } },       /* xchg [ebx], eax */
        { 2, 2, { 0x86, 0x23 } },       /* xchg [ebx], ah */
        { 3, 2, { 0x66, 0x87, 0x0b } }, /* xchg [ebx], cx */
        { 2, 0, { 0x87, 0xca } },       /* xchg edx, ecx */
        { 2, 0, { 0x86, 0xe0 } },       /* xchg al, ah */
    };
    const char    *name = "misc";
    struct variant v    = { .group = name };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.fix_input  = seqs[i].fix == 1 ? fix_xlat : seqs[i].fix == 2 ? fix_mem_ebx : 0;
        v.arg        = MEM_OFF;
        v.reg_fix    = 0;
        v.norm       = seqs[i].fix == 1 ? 1 << R_EBX : 0;
        run_ctx(&v, ctx_some, 3, 6);
    }
    group_end(name);
}

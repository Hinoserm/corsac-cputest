/*
 * Sampled loops: one instruction per group, over 4096 operand sets that
 * the harness draws beforehand (edge-biased, and constrained where the
 * instruction needs it: divisions that fit, BSF sources that aren't zero).
 * The loop runs inside the test and folds every result register and the
 * flags into two checksums in memory: one of the defined flags (EBP at the
 * end, the defined CRC) and one of all of them (EDI, the raw CRC). The
 * 8-bit forms are walked completely in exhaust8; these are the 16- and
 * 32-bit ones. Included into groups.inc after groups_code16.inc; the table
 * only ever grows at the end.
 */

enum { LK_BIN, LK_UN, LK_SHIFT, LK_SHD, LK_MUL, LK_DIV, LK_IDIV, LK_BS, LK_CMPXCHG, LK_PLAIN };

static const struct loop_def {
    const char *name;
    uint8_t     kind, size;
    uint16_t    mask; /* the defined flags; shifts use a table by count */
    uint8_t     len, b[8];
} loop_defs[] = {
    { "loop.add16", LK_BIN, 16, 0x8d5, SEQ(0x66, 0x01, 0xd8) },
    { "loop.or16", LK_BIN, 16, 0x8c5, SEQ(0x66, 0x09, 0xd8) },
    { "loop.adc16", LK_BIN, 16, 0x8d5, SEQ(0x66, 0x11, 0xd8) },
    { "loop.sbb16", LK_BIN, 16, 0x8d5, SEQ(0x66, 0x19, 0xd8) },
    { "loop.and16", LK_BIN, 16, 0x8c5, SEQ(0x66, 0x21, 0xd8) },
    { "loop.sub16", LK_BIN, 16, 0x8d5, SEQ(0x66, 0x29, 0xd8) },
    { "loop.xor16", LK_BIN, 16, 0x8c5, SEQ(0x66, 0x31, 0xd8) },
    { "loop.cmp16", LK_BIN, 16, 0x8d5, SEQ(0x66, 0x39, 0xd8) },
    { "loop.add32", LK_BIN, 32, 0x8d5, SEQ(0x01, 0xd8) },
    { "loop.or32", LK_BIN, 32, 0x8c5, SEQ(0x09, 0xd8) },
    { "loop.adc32", LK_BIN, 32, 0x8d5, SEQ(0x11, 0xd8) },
    { "loop.sbb32", LK_BIN, 32, 0x8d5, SEQ(0x19, 0xd8) },
    { "loop.and32", LK_BIN, 32, 0x8c5, SEQ(0x21, 0xd8) },
    { "loop.sub32", LK_BIN, 32, 0x8d5, SEQ(0x29, 0xd8) },
    { "loop.xor32", LK_BIN, 32, 0x8c5, SEQ(0x31, 0xd8) },
    { "loop.cmp32", LK_BIN, 32, 0x8d5, SEQ(0x39, 0xd8) },
    { "loop.inc16", LK_UN, 16, 0x8d5, SEQ(0x66, 0xff, 0xc0) },
    { "loop.dec16", LK_UN, 16, 0x8d5, SEQ(0x66, 0xff, 0xc8) },
    { "loop.neg16", LK_UN, 16, 0x8d5, SEQ(0x66, 0xf7, 0xd8) },
    { "loop.not16", LK_UN, 16, 0x0c5, SEQ(0x66, 0xf7, 0xd0) },
    { "loop.inc32", LK_UN, 32, 0x8d5, SEQ(0xff, 0xc0) },
    { "loop.dec32", LK_UN, 32, 0x8d5, SEQ(0xff, 0xc8) },
    { "loop.neg32", LK_UN, 32, 0x8d5, SEQ(0xf7, 0xd8) },
    { "loop.not32", LK_UN, 32, 0x0c5, SEQ(0xf7, 0xd0) },
    { "loop.rol16", LK_SHIFT, 16, 0x000, SEQ(0x66, 0xd3, 0xc0) },
    { "loop.ror16", LK_SHIFT, 16, 0x000, SEQ(0x66, 0xd3, 0xc8) },
    { "loop.rcl16", LK_SHIFT, 16, 0x000, SEQ(0x66, 0xd3, 0xd0) },
    { "loop.rcr16", LK_SHIFT, 16, 0x000, SEQ(0x66, 0xd3, 0xd8) },
    { "loop.shl16", LK_SHIFT, 16, 0x000, SEQ(0x66, 0xd3, 0xe0) },
    { "loop.shr16", LK_SHIFT, 16, 0x000, SEQ(0x66, 0xd3, 0xe8) },
    { "loop.sar16", LK_SHIFT, 16, 0x000, SEQ(0x66, 0xd3, 0xf8) },
    { "loop.rol32", LK_SHIFT, 32, 0x000, SEQ(0xd3, 0xc0) },
    { "loop.ror32", LK_SHIFT, 32, 0x000, SEQ(0xd3, 0xc8) },
    { "loop.rcl32", LK_SHIFT, 32, 0x000, SEQ(0xd3, 0xd0) },
    { "loop.rcr32", LK_SHIFT, 32, 0x000, SEQ(0xd3, 0xd8) },
    { "loop.shl32", LK_SHIFT, 32, 0x000, SEQ(0xd3, 0xe0) },
    { "loop.shr32", LK_SHIFT, 32, 0x000, SEQ(0xd3, 0xe8) },
    { "loop.sar32", LK_SHIFT, 32, 0x000, SEQ(0xd3, 0xf8) },
    { "loop.shld16", LK_SHD, 16, 0x000, SEQ(0x66, 0x0f, 0xa5, 0xd8) },
    { "loop.shrd16", LK_SHD, 16, 0x000, SEQ(0x66, 0x0f, 0xad, 0xd8) },
    { "loop.shld32", LK_SHD, 32, 0x000, SEQ(0x0f, 0xa5, 0xd8) },
    { "loop.shrd32", LK_SHD, 32, 0x000, SEQ(0x0f, 0xad, 0xd8) },
    { "loop.mul16", LK_MUL, 16, 0x801, SEQ(0x66, 0xf7, 0xe3) },
    { "loop.imul16", LK_MUL, 16, 0x801, SEQ(0x66, 0xf7, 0xeb) },
    { "loop.imul2.16", LK_PLAIN, 16, 0x801, SEQ(0x66, 0x0f, 0xaf, 0xc3) },
    { "loop.mul32", LK_MUL, 32, 0x801, SEQ(0xf7, 0xe3) },
    { "loop.imul32", LK_MUL, 32, 0x801, SEQ(0xf7, 0xeb) },
    { "loop.imul2.32", LK_PLAIN, 32, 0x801, SEQ(0x0f, 0xaf, 0xc3) },
    { "loop.imul3.32", LK_PLAIN, 32, 0x801, SEQ(0x69, 0xc3, 0x45, 0x23, 0x01, 0x00) },
    { "loop.div8", LK_DIV, 8, 0x000, SEQ(0xf6, 0xf3) },
    { "loop.idiv8", LK_IDIV, 8, 0x000, SEQ(0xf6, 0xfb) },
    { "loop.div16", LK_DIV, 16, 0x000, SEQ(0x66, 0xf7, 0xf3) },
    { "loop.idiv16", LK_IDIV, 16, 0x000, SEQ(0x66, 0xf7, 0xfb) },
    { "loop.div32", LK_DIV, 32, 0x000, SEQ(0xf7, 0xf3) },
    { "loop.idiv32", LK_IDIV, 32, 0x000, SEQ(0xf7, 0xfb) },
};

#define LOOP_N 4096
static uint32_t g_ltA[LOOP_N], g_ltB[LOOP_N], g_ltC[LOOP_N], g_ltD[LOOP_N];
uint32_t        g_lraw, g_ldef;

/* The operand sets. A: EAX, B: EBX, C: ECX (the count in bits 0-4, the
   carry in bit 8), D: EDX. */
static void
loop_fill(const struct loop_def *d)
{
    uint32_t m = szmask(d->size);
    for (int i = 0; i < LOOP_N; i++) {
        uint32_t a = rnd_operand(), b = rnd_operand(), c = rnd(), x = rnd_operand();
        switch (d->kind) {
            case LK_DIV: {
                uint32_t dv = b & m ? b & m : 1;
                b           = (b & ~m) | dv;
                if (d->size == 8)
                    a = (a & ~0xffffu) | ((rnd() % dv) << 8) | (a & 0xff);
                else
                    x = (x & ~m) | (rnd() % dv);
                break;
            }
            case LK_IDIV: {
                uint32_t dv = b & m ? b & m : 1, lo = a & m, top = 1u << (d->size - 1);
                if (lo == top && dv == m)
                    dv = 1; /* MIN / -1 would be #DE: that is mul.div's */
                b = (b & ~m) | dv;
                if (d->size == 8)
                    a = (a & ~0xffffu) | ((lo & 0x80) ? 0xff00 : 0) | lo;
                else
                    x = (x & ~m) | ((lo & top) ? m : 0);
                break;
            }
            case LK_BS:
                if (!(b & m))
                    b |= 1u << (rnd() % d->size);
                break;
            case LK_SHD:
                if (d->size == 16) /* past 16 the result is undefined */
                    c = (c & ~31u) | (rnd() % 17);
                break;
            case LK_CMPXCHG:
                if (!(rnd() & 3))
                    a = (a & ~m) | (b & m);
                break;
            default: break;
        }
        g_ltA[i] = a;
        g_ltB[i] = b;
        g_ltC[i] = c;
        g_ltD[i] = x;
    }
}

/* The defined flags per count for shifts, rotates and double shifts. The
   setup (SHR ESI, 9) leaves CF SF ZF PF defined for a zero count to pass
   through. */
static void
loop_masks(const struct loop_def *d)
{
    int op = (d->b[d->len - 1] >> 3) & 7;
    for (int c = 0; c < 32; c++) {
        uint32_t mk = 0x0c5;
        if (c == 1)
            mk |= 0x800;
        if (d->kind == LK_SHIFT && c && (op == 4 || op == 5) && c >= d->size)
            mk &= ~1u;
        g_ex_masks[c] = mk;
    }
}

/* mov reg, [table + ebp*4] */
static void
emit_ltload(struct emit *e, int reg, const uint32_t *table)
{
    e8(e, 0x8b);
    e8(e, 0x04 | (reg << 3));
    e8(e, 0xad);
    e32(e, (uint32_t) table);
}

/* rol dword [hash], 5; xor [hash], reg */
static void
emit_fold(struct emit *e, uint32_t *hash, int reg)
{
    e8(e, 0xc1);
    e8(e, 0x05);
    e32(e, (uint32_t) hash);
    e8(e, 0x05);
    e8(e, 0x31);
    e8(e, 0x05 | (reg << 3));
    e32(e, (uint32_t) hash);
}

static void
emit_loop(struct emit *e, const struct variant *v)
{
    const struct loop_def *d = &loop_defs[v->arg2];
    int                    shifts = d->kind == LK_SHIFT || d->kind == LK_SHD;

    emit_seq(e, 2, 0x31, 0xed); /* xor ebp, ebp */
    emit_seq(e, 2, 0xc7, 0x05); /* mov dword [g_lraw], 0 */
    e32(e, (uint32_t) &g_lraw);
    e32(e, 0);
    emit_seq(e, 2, 0xc7, 0x05); /* mov dword [g_ldef], 0 */
    e32(e, (uint32_t) &g_ldef);
    e32(e, 0);
    uint8_t *top = e->p;
    emit_ltload(e, R_EAX, g_ltA);
    emit_ltload(e, R_EBX, g_ltB);
    emit_ltload(e, R_ECX, g_ltC);
    emit_ltload(e, R_EDX, g_ltD);
    if (shifts) {
        emit_seq(e, 3, 0x83, 0xe1, 0x1f); /* and ecx, 31 */
        loop_masks(d);
    }
    emit_ltload(e, R_ESI, g_ltC);
    emit_seq(e, 3, 0xc1, 0xee, 0x09); /* shr esi, 9: CF = bit 8 */
    ebytes(e, d->b, d->len);
    emit_seq(e, 2, 0x9c, 0x5e); /* pushfd; pop esi */
    emit_fold(e, &g_lraw, R_EAX);
    emit_fold(e, &g_lraw, R_EBX);
    emit_fold(e, &g_lraw, R_EDX);
    emit_fold(e, &g_lraw, R_ESI);
    if (shifts) {
        emit_seq(e, 3, 0x23, 0x34, 0x8d); /* and esi, [g_ex_masks + ecx*4] */
        e32(e, (uint32_t) g_ex_masks);
    } else {
        emit_seq(e, 2, 0x81, 0xe6); /* and esi, mask */
        e32(e, d->mask);
    }
    emit_fold(e, &g_ldef, R_EAX);
    emit_fold(e, &g_ldef, R_EBX);
    emit_fold(e, &g_ldef, R_EDX);
    emit_fold(e, &g_ldef, R_ESI);
    e8(e, 0x45);                /* inc ebp */
    emit_seq(e, 2, 0x81, 0xfd); /* cmp ebp, LOOP_N */
    e32(e, LOOP_N);
    emit_seq(e, 2, 0x0f, 0x82); /* jb top */
    e32(e, (uint32_t) (top - (e->p + 4)));
    emit_seq(e, 2, 0x8b, 0x2d); /* mov ebp, [g_ldef] */
    e32(e, (uint32_t) &g_ldef);
    emit_seq(e, 2, 0x8b, 0x3d); /* mov edi, [g_lraw] */
    e32(e, (uint32_t) &g_lraw);
}

static void
group_loop(unsigned i)
{
    const struct loop_def *d = &loop_defs[i];
    struct variant         v = { .group = d->name, .emit = emit_loop, .fx = FX_CUSTOM, .defmask = dm_exhaust, .arg2 = i };
    group_begin(d->name);
    if (!g_counting)
        loop_fill(d);
    memcpy(v.target, d->b, d->len); /* for the reports */
    v.target_len = d->len;
    run_inputs(&v, 1);
    group_end(d->name);
}

#define LOOP_GROUPS \
GA(group_loop, 0), GA(group_loop, 1), GA(group_loop, 2), GA(group_loop, 3), GA(group_loop, 4), GA(group_loop, 5), \
GA(group_loop, 6), GA(group_loop, 7), GA(group_loop, 8), GA(group_loop, 9), GA(group_loop, 10), GA(group_loop, 11), \
GA(group_loop, 12), GA(group_loop, 13), GA(group_loop, 14), GA(group_loop, 15), GA(group_loop, 16), GA(group_loop, 17), \
GA(group_loop, 18), GA(group_loop, 19), GA(group_loop, 20), GA(group_loop, 21), GA(group_loop, 22), GA(group_loop, 23), \
GA(group_loop, 24), GA(group_loop, 25), GA(group_loop, 26), GA(group_loop, 27), GA(group_loop, 28), GA(group_loop, 29), \
GA(group_loop, 30), GA(group_loop, 31), GA(group_loop, 32), GA(group_loop, 33), GA(group_loop, 34), GA(group_loop, 35), \
GA(group_loop, 36), GA(group_loop, 37), GA(group_loop, 38), GA(group_loop, 39), GA(group_loop, 40), GA(group_loop, 41), \
GA(group_loop, 42), GA(group_loop, 43), GA(group_loop, 44), GA(group_loop, 45), GA(group_loop, 46), GA(group_loop, 47), \
GA(group_loop, 48), GA(group_loop, 49), GA(group_loop, 50), GA(group_loop, 51), GA(group_loop, 52), GA(group_loop, 53), \
GA(group_loop, 54),

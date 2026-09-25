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

enum { LK_BIN, LK_UN, LK_SHIFT, LK_SHD, LK_MUL, LK_DIV, LK_IDIV, LK_BS, LK_CMPXCHG, LK_PLAIN,
       LK_MMX,   /* op mm0, mm1 over edge-biased lanes */
       LK_MMXSH, /* shift mm0 by mm1: counts at the lane edges */
       LK_3DN,   /* 3DNow! op mm0, mm1 over integers; mask 1: exact */
       LK_3DNF,  /* the same over single-precision numbers */
       LK_3DNX,  /* the K6-2+/III+ extensions, integers */
       LK_3DNXF  /* the extensions over numbers */
};

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
    { "loop.bsf16", LK_BS, 16, 0x040, SEQ(0x66, 0x0f, 0xbc, 0xc3) },
    { "loop.bsr16", LK_BS, 16, 0x040, SEQ(0x66, 0x0f, 0xbd, 0xc3) },
    { "loop.bsf32", LK_BS, 32, 0x040, SEQ(0x0f, 0xbc, 0xc3) },
    { "loop.bsr32", LK_BS, 32, 0x040, SEQ(0x0f, 0xbd, 0xc3) },
    { "loop.bt16", LK_PLAIN, 16, 0x001, SEQ(0x66, 0x0f, 0xa3, 0xd8) },
    { "loop.bts16", LK_PLAIN, 16, 0x001, SEQ(0x66, 0x0f, 0xab, 0xd8) },
    { "loop.btr16", LK_PLAIN, 16, 0x001, SEQ(0x66, 0x0f, 0xb3, 0xd8) },
    { "loop.btc16", LK_PLAIN, 16, 0x001, SEQ(0x66, 0x0f, 0xbb, 0xd8) },
    { "loop.bt32", LK_PLAIN, 32, 0x001, SEQ(0x0f, 0xa3, 0xd8) },
    { "loop.bts32", LK_PLAIN, 32, 0x001, SEQ(0x0f, 0xab, 0xd8) },
    { "loop.btr32", LK_PLAIN, 32, 0x001, SEQ(0x0f, 0xb3, 0xd8) },
    { "loop.btc32", LK_PLAIN, 32, 0x001, SEQ(0x0f, 0xbb, 0xd8) },
    { "loop.xadd8", LK_PLAIN, 8, 0x8d5, SEQ(0x0f, 0xc0, 0xd8) },
    { "loop.cmpxchg8", LK_CMPXCHG, 8, 0x8d5, SEQ(0x0f, 0xb0, 0xcb) },
    { "loop.xadd16", LK_PLAIN, 16, 0x8d5, SEQ(0x66, 0x0f, 0xc1, 0xd8) },
    { "loop.cmpxchg16", LK_CMPXCHG, 16, 0x8d5, SEQ(0x66, 0x0f, 0xb1, 0xcb) },
    { "loop.xadd32", LK_PLAIN, 32, 0x8d5, SEQ(0x0f, 0xc1, 0xd8) },
    { "loop.cmpxchg32", LK_CMPXCHG, 32, 0x8d5, SEQ(0x0f, 0xb1, 0xcb) },
    { "loop.bswap", LK_PLAIN, 32, 0x0c5, SEQ(0x0f, 0xc8) },
    { "loop.punpcklbw", LK_MMX, 64, 0x001, SEQ(0x0f, 0x60, 0xc1) },
    { "loop.punpcklwd", LK_MMX, 64, 0x001, SEQ(0x0f, 0x61, 0xc1) },
    { "loop.punpckldq", LK_MMX, 64, 0x001, SEQ(0x0f, 0x62, 0xc1) },
    { "loop.packsswb", LK_MMX, 64, 0x001, SEQ(0x0f, 0x63, 0xc1) },
    { "loop.pcmpgtb", LK_MMX, 64, 0x001, SEQ(0x0f, 0x64, 0xc1) },
    { "loop.pcmpgtw", LK_MMX, 64, 0x001, SEQ(0x0f, 0x65, 0xc1) },
    { "loop.pcmpgtd", LK_MMX, 64, 0x001, SEQ(0x0f, 0x66, 0xc1) },
    { "loop.packuswb", LK_MMX, 64, 0x001, SEQ(0x0f, 0x67, 0xc1) },
    { "loop.punpckhbw", LK_MMX, 64, 0x001, SEQ(0x0f, 0x68, 0xc1) },
    { "loop.punpckhwd", LK_MMX, 64, 0x001, SEQ(0x0f, 0x69, 0xc1) },
    { "loop.punpckhdq", LK_MMX, 64, 0x001, SEQ(0x0f, 0x6a, 0xc1) },
    { "loop.packssdw", LK_MMX, 64, 0x001, SEQ(0x0f, 0x6b, 0xc1) },
    { "loop.pcmpeqb", LK_MMX, 64, 0x001, SEQ(0x0f, 0x74, 0xc1) },
    { "loop.pcmpeqw", LK_MMX, 64, 0x001, SEQ(0x0f, 0x75, 0xc1) },
    { "loop.pcmpeqd", LK_MMX, 64, 0x001, SEQ(0x0f, 0x76, 0xc1) },
    { "loop.pmullw", LK_MMX, 64, 0x001, SEQ(0x0f, 0xd5, 0xc1) },
    { "loop.psubusb", LK_MMX, 64, 0x001, SEQ(0x0f, 0xd8, 0xc1) },
    { "loop.psubusw", LK_MMX, 64, 0x001, SEQ(0x0f, 0xd9, 0xc1) },
    { "loop.pand", LK_MMX, 64, 0x001, SEQ(0x0f, 0xdb, 0xc1) },
    { "loop.paddusb", LK_MMX, 64, 0x001, SEQ(0x0f, 0xdc, 0xc1) },
    { "loop.paddusw", LK_MMX, 64, 0x001, SEQ(0x0f, 0xdd, 0xc1) },
    { "loop.pandn", LK_MMX, 64, 0x001, SEQ(0x0f, 0xdf, 0xc1) },
    { "loop.pmulhw", LK_MMX, 64, 0x001, SEQ(0x0f, 0xe5, 0xc1) },
    { "loop.psubsb", LK_MMX, 64, 0x001, SEQ(0x0f, 0xe8, 0xc1) },
    { "loop.psubsw", LK_MMX, 64, 0x001, SEQ(0x0f, 0xe9, 0xc1) },
    { "loop.por", LK_MMX, 64, 0x001, SEQ(0x0f, 0xeb, 0xc1) },
    { "loop.paddsb", LK_MMX, 64, 0x001, SEQ(0x0f, 0xec, 0xc1) },
    { "loop.paddsw", LK_MMX, 64, 0x001, SEQ(0x0f, 0xed, 0xc1) },
    { "loop.pxor", LK_MMX, 64, 0x001, SEQ(0x0f, 0xef, 0xc1) },
    { "loop.pmaddwd", LK_MMX, 64, 0x001, SEQ(0x0f, 0xf5, 0xc1) },
    { "loop.psubb", LK_MMX, 64, 0x001, SEQ(0x0f, 0xf8, 0xc1) },
    { "loop.psubw", LK_MMX, 64, 0x001, SEQ(0x0f, 0xf9, 0xc1) },
    { "loop.psubd", LK_MMX, 64, 0x001, SEQ(0x0f, 0xfa, 0xc1) },
    { "loop.paddb", LK_MMX, 64, 0x001, SEQ(0x0f, 0xfc, 0xc1) },
    { "loop.paddw", LK_MMX, 64, 0x001, SEQ(0x0f, 0xfd, 0xc1) },
    { "loop.paddd", LK_MMX, 64, 0x001, SEQ(0x0f, 0xfe, 0xc1) },
    { "loop.psrlw", LK_MMXSH, 64, 0x001, SEQ(0x0f, 0xd1, 0xc1) },
    { "loop.psrld", LK_MMXSH, 64, 0x001, SEQ(0x0f, 0xd2, 0xc1) },
    { "loop.psrlq", LK_MMXSH, 64, 0x001, SEQ(0x0f, 0xd3, 0xc1) },
    { "loop.psraw", LK_MMXSH, 64, 0x001, SEQ(0x0f, 0xe1, 0xc1) },
    { "loop.psrad", LK_MMXSH, 64, 0x001, SEQ(0x0f, 0xe2, 0xc1) },
    { "loop.psllw", LK_MMXSH, 64, 0x001, SEQ(0x0f, 0xf1, 0xc1) },
    { "loop.pslld", LK_MMXSH, 64, 0x001, SEQ(0x0f, 0xf2, 0xc1) },
    { "loop.psllq", LK_MMXSH, 64, 0x001, SEQ(0x0f, 0xf3, 0xc1) },
};

#define LOOP_N 4096
static uint32_t g_ltA[LOOP_N], g_ltB[LOOP_N], g_ltC[LOOP_N], g_ltD[LOOP_N];
/* MMX operands: MM0 from A (low) and D (high), MM1 from B and C. */
uint32_t        g_lraw, g_ldef;

/* 32 bits of MMX operand: each 16-bit lane at a saturation edge a quarter
   of the time, random otherwise. */
static uint32_t
mm_lanes(void)
{
    static const uint16_t edges[] = { 0x0000, 0x0001, 0x007f, 0x0080, 0x00ff, 0x7fff, 0x8000, 0xffff };
    uint32_t              v       = 0;
    for (int l = 0; l < 2; l++) {
        uint32_t r = rnd();
        v |= (uint32_t) ((r & 3) ? (r >> 8) & 0xffff : edges[(r >> 4) & 7]) << (16 * l);
    }
    return v;
}

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
            case LK_MMX:
            case LK_3DN:
            case LK_3DNX:
                a = mm_lanes();
                x = mm_lanes();
                b = mm_lanes();
                c = mm_lanes();
                break;
            case LK_MMXSH: {
                static const uint32_t counts[] = { 0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 255 };
                a = mm_lanes();
                x = mm_lanes();
                b = (rnd() & 1) ? counts[rnd() % (sizeof(counts) / sizeof(counts[0]))] : rnd() & 0x3f;
                c = (rnd() & 15) ? 0 : rnd_operand(); /* the high half set: everything out */
                break;
            }
            case LK_3DNF:
            case LK_3DNXF:
                a = rnd_float();
                x = rnd_float();
                b = rnd_float();
                c = rnd_float();
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

/* The MMX and 3DNow! loops: MM0 = D:A, MM1 = C:B, the instruction, then
   both halves of MM0 folded (into the defined checksum too, unless the
   instruction rounds one way per model: 3DNow! with mask 0). */
static void
emit_loop_mmx(struct emit *e, const struct loop_def *d)
{
    emit_seq(e, 2, 0x31, 0xed); /* xor ebp, ebp */
    emit_seq(e, 2, 0xc7, 0x05);
    e32(e, (uint32_t) &g_lraw);
    e32(e, 0);
    emit_seq(e, 2, 0xc7, 0x05);
    e32(e, (uint32_t) &g_ldef);
    e32(e, 0);
    uint8_t *top = e->p;
    emit_seq(e, 3, 0x0f, 0x6e, 0x04); /* movd mm0, [A + ebp*4] */
    e8(e, 0xad);
    e32(e, (uint32_t) g_ltA);
    emit_seq(e, 3, 0x0f, 0x6e, 0x14); /* movd mm2, [D + ebp*4] */
    e8(e, 0xad);
    e32(e, (uint32_t) g_ltD);
    emit_seq(e, 4, 0x0f, 0x73, 0xf2, 0x20); /* psllq mm2, 32 */
    emit_seq(e, 3, 0x0f, 0xeb, 0xc2);       /* por mm0, mm2 */
    emit_seq(e, 3, 0x0f, 0x6e, 0x0c);       /* movd mm1, [B + ebp*4] */
    e8(e, 0xad);
    e32(e, (uint32_t) g_ltB);
    emit_seq(e, 3, 0x0f, 0x6e, 0x14); /* movd mm2, [C + ebp*4] */
    e8(e, 0xad);
    e32(e, (uint32_t) g_ltC);
    emit_seq(e, 4, 0x0f, 0x73, 0xf2, 0x20); /* psllq mm2, 32 */
    emit_seq(e, 3, 0x0f, 0xeb, 0xca);       /* por mm1, mm2 */
    ebytes(e, d->b, d->len);
    emit_seq(e, 3, 0x0f, 0x7e, 0xc0);       /* movd eax, mm0 */
    emit_seq(e, 4, 0x0f, 0x73, 0xd0, 0x20); /* psrlq mm0, 32 */
    emit_seq(e, 3, 0x0f, 0x7e, 0xc3);       /* movd ebx, mm0 */
    emit_fold(e, &g_lraw, R_EAX);
    emit_fold(e, &g_lraw, R_EBX);
    if (d->mask) {
        emit_fold(e, &g_ldef, R_EAX);
        emit_fold(e, &g_ldef, R_EBX);
    }
    e8(e, 0x45);                /* inc ebp */
    emit_seq(e, 2, 0x81, 0xfd); /* cmp ebp, LOOP_N */
    e32(e, LOOP_N);
    emit_seq(e, 2, 0x0f, 0x82); /* jb top */
    e32(e, (uint32_t) (top - (e->p + 4)));
    emit_seq(e, 2, 0x0f, 0x77); /* emms */
    emit_seq(e, 2, 0x8b, 0x2d); /* mov ebp, [g_ldef] */
    e32(e, (uint32_t) &g_ldef);
    emit_seq(e, 2, 0x8b, 0x3d); /* mov edi, [g_lraw] */
    e32(e, (uint32_t) &g_lraw);
}

static void
emit_loop(struct emit *e, const struct variant *v)
{
    const struct loop_def *d = &loop_defs[v->arg2];
    int                    shifts = d->kind == LK_SHIFT || d->kind == LK_SHD;

    if (d->kind >= LK_MMX) {
        emit_loop_mmx(e, d);
        return;
    }

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
    if (d->kind >= LK_MMX && !(g_cpu.features & CPUID_MMX)) {
        group_skip(d->name, "no MMX");
        return;
    }
    if (d->kind >= LK_3DN && !(g_cpu.ext_features & EXT_3DNOW)) {
        group_skip(d->name, "no 3DNow!");
        return;
    }
    if (d->kind >= LK_3DNX && !(g_cpu.ext_features & EXT_3DNOWEXT)) {
        group_skip(d->name, "no 3DNow! extensions");
        return;
    }
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
GA(group_loop, 54), GA(group_loop, 55), GA(group_loop, 56), GA(group_loop, 57), GA(group_loop, 58), GA(group_loop, 59), \
GA(group_loop, 60), GA(group_loop, 61), GA(group_loop, 62), GA(group_loop, 63), GA(group_loop, 64), GA(group_loop, 65), \
GA(group_loop, 66), GA(group_loop, 67), GA(group_loop, 68), GA(group_loop, 69), GA(group_loop, 70), GA(group_loop, 71), \
GA(group_loop, 72), GA(group_loop, 73), GA(group_loop, 74), GA(group_loop, 75), GA(group_loop, 76), GA(group_loop, 77), \
GA(group_loop, 78), GA(group_loop, 79), GA(group_loop, 80), GA(group_loop, 81), GA(group_loop, 82), GA(group_loop, 83), \
GA(group_loop, 84), GA(group_loop, 85), GA(group_loop, 86), GA(group_loop, 87), GA(group_loop, 88), GA(group_loop, 89), \
GA(group_loop, 90), GA(group_loop, 91), GA(group_loop, 92), GA(group_loop, 93), GA(group_loop, 94), GA(group_loop, 95), \
GA(group_loop, 96), GA(group_loop, 97), GA(group_loop, 98), GA(group_loop, 99), GA(group_loop, 100), GA(group_loop, 101), \
GA(group_loop, 102), GA(group_loop, 103), GA(group_loop, 104), GA(group_loop, 105), GA(group_loop, 106), GA(group_loop, 107), \
GA(group_loop, 108), GA(group_loop, 109), GA(group_loop, 110), GA(group_loop, 111), GA(group_loop, 112), GA(group_loop, 113), \
GA(group_loop, 114), GA(group_loop, 115), GA(group_loop, 116), GA(group_loop, 117),

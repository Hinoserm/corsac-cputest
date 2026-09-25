/*
 * Exhaustive 8-bit flag math. Included into groups.inc after
 * groups_more.inc; append only.
 */

/* ---- exhaustive 8-bit flag math ------------------------------------------- */

/* One test per instruction, whose code walks every input itself: EDX counts
   down through the input space, each iteration sets AL, BL, CL, CF or AF
   from it, runs the instruction, and folds AX and the flags into two
   checksums: EBP with only the defined flags, EDI with all of them. EBP
   goes into the defined CRC, EDI into the raw one. On 86Box the loop is
   compiled after its first iterations, so these check against a real
   CPU's checksums rather than against the interpreter. */
enum { EX_BIN, EX_UN, EX_SHIFT, EX_BCD, EX_MUL, EX_AAD, EX_SAHF };

static const struct {
    uint8_t  kind, len, b[2];
    uint16_t mask; /* the defined flags; for shifts, see ex_shift_mask() */
} ex_ops[] = {
    { EX_BIN, SEQ(0x00, 0xd8), 0x8d5 },   /* add al, bl */
    { EX_BIN, SEQ(0x10, 0xd8), 0x8d5 },   /* adc */
    { EX_BIN, SEQ(0x28, 0xd8), 0x8d5 },   /* sub */
    { EX_BIN, SEQ(0x18, 0xd8), 0x8d5 },   /* sbb */
    { EX_BIN, SEQ(0x20, 0xd8), 0x8c5 },   /* and: AF undefined */
    { EX_BIN, SEQ(0x08, 0xd8), 0x8c5 },   /* or */
    { EX_BIN, SEQ(0x30, 0xd8), 0x8c5 },   /* xor */
    { EX_BIN, SEQ(0x38, 0xd8), 0x8d5 },   /* cmp */
    { EX_UN, SEQ(0xfe, 0xc0), 0x8d5 },    /* inc al (CF passes through) */
    { EX_UN, SEQ(0xfe, 0xc8), 0x8d5 },    /* dec al */
    { EX_UN, SEQ(0xf6, 0xd8), 0x8d5 },    /* neg al */
    { EX_UN, SEQ(0xf6, 0xd0), 0x0c5 },    /* not al: the setup's CF SF ZF PF pass through */
    { EX_SHIFT, SEQ(0xd2, 0xc0), 0 },     /* rol al, cl */
    { EX_SHIFT, SEQ(0xd2, 0xc8), 0 },     /* ror */
    { EX_SHIFT, SEQ(0xd2, 0xd0), 0 },     /* rcl */
    { EX_SHIFT, SEQ(0xd2, 0xd8), 0 },     /* rcr */
    { EX_SHIFT, SEQ(0xd2, 0xe0), 0 },     /* shl */
    { EX_SHIFT, SEQ(0xd2, 0xe8), 0 },     /* shr */
    { EX_SHIFT, SEQ(0xd2, 0xf8), 0 },     /* sar */
    { EX_BCD, SEQ(0x27), 0x0d5 },         /* daa: OF undefined */
    { EX_BCD, SEQ(0x2f), 0x0d5 },         /* das */
    { EX_BCD, SEQ(0x37), 0x011 },         /* aaa: AF and CF only */
    { EX_BCD, SEQ(0x3f), 0x011 },         /* aas */
    { EX_MUL, SEQ(0xf6, 0xe3), 0x801 },   /* mul bl: CF and OF */
    { EX_MUL, SEQ(0xf6, 0xeb), 0x801 },   /* imul bl */
    { EX_AAD, SEQ(0xd4, 0x0a), 0x0c4 },   /* aam: SF ZF PF */
    { EX_AAD, SEQ(0xd5, 0x0a), 0x0c4 },   /* aad */
    { EX_SAHF, SEQ(0x9e), 0x0d5 },        /* sahf */
};

/* Per count 0-31: the flags a shift or rotate of AL by CL defines. The
   setup leaves CF SF ZF PF defined, which a zero count (and a rotate's
   untouched flags) pass through. */
static uint32_t g_ex_masks[32];

static void
ex_shift_masks(uint8_t modrm)
{
    int op = (modrm >> 3) & 7;
    for (int c = 0; c < 32; c++) {
        uint32_t m = 0x0c5;
        if (c == 1)
            m |= 0x800;
        if (c >= 8 && (op == 4 || op == 5))
            m &= ~1u; /* SHL, SHR: CF undefined once everything is out */
        g_ex_masks[c] = m;
    }
}

static void
emit_seq(struct emit *e, int n, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, n);
    for (int i = 0; i < n; i++)
        e8(e, (uint8_t) __builtin_va_arg(ap, int));
    __builtin_va_end(ap);
}

static void
emit_exhaust(struct emit *e, const struct variant *v)
{
    int      k    = v->arg2, kind = ex_ops[k].kind;
    uint32_t bits = kind == EX_BIN ? 17 : kind == EX_UN ? 9 : kind == EX_SHIFT ? 14 : kind == EX_BCD ? 10 : kind == EX_SAHF ? 8 : 16;

    emit_seq(e, 4, 0x31, 0xed, 0x31, 0xff); /* xor ebp, ebp; xor edi, edi */
    e8(e, 0xba);                            /* mov edx, 2^bits - 1 */
    e32(e, (1u << bits) - 1);
    uint8_t *top = e->p;
    emit_seq(e, 2, 0x89, 0xd0); /* mov eax, edx */
    switch (kind) {
        case EX_BIN:
            emit_seq(e, 3, 0xc1, 0xe8, 0x08);             /* shr eax, 8: AL = bits 8-15 */
            emit_seq(e, 2, 0x89, 0xd3);                   /* mov ebx, edx: BL = bits 0-7 */
            emit_seq(e, 5, 0x89, 0xd6, 0xc1, 0xee, 0x11); /* mov esi, edx; shr esi, 17: CF = bit 16 */
            break;
        case EX_UN:
            emit_seq(e, 5, 0x89, 0xd6, 0xc1, 0xee, 0x09); /* CF = bit 8 */
            break;
        case EX_SHIFT:
            emit_seq(e, 8, 0x89, 0xd1, 0xc1, 0xe9, 0x08, 0x83, 0xe1, 0x1f); /* mov ecx, edx; shr ecx, 8; and ecx, 31 */
            emit_seq(e, 5, 0x89, 0xd6, 0xc1, 0xee, 0x0e);                   /* CF = bit 13 */
            break;
        case EX_BCD:
            /* EFLAGS = 2 | CF (bit 8) | AF (bit 9) */
            emit_seq(e, 5, 0x89, 0xd6, 0xc1, 0xee, 0x08); /* mov esi, edx; shr esi, 8 */
            emit_seq(e, 2, 0x89, 0xf3);                   /* mov ebx, esi */
            emit_seq(e, 3, 0x83, 0xe6, 0x01);             /* and esi, 1 */
            emit_seq(e, 3, 0x83, 0xe3, 0x02);             /* and ebx, 2 */
            emit_seq(e, 3, 0xc1, 0xe3, 0x03);             /* shl ebx, 3 */
            emit_seq(e, 2, 0x09, 0xde);                   /* or esi, ebx */
            emit_seq(e, 3, 0x83, 0xce, 0x02);             /* or esi, 2 */
            emit_seq(e, 2, 0x56, 0x9d);                   /* push esi; popfd */
            break;
        case EX_MUL:
            emit_seq(e, 3, 0xc1, 0xe8, 0x08); /* AL = bits 8-15 */
            emit_seq(e, 2, 0x89, 0xd3);       /* BL = bits 0-7 */
            break;
        case EX_SAHF:
            emit_seq(e, 3, 0xc1, 0xe0, 0x08); /* shl eax, 8: AH = bits 0-7 */
            break;
        default: /* EX_AAD: AX = the index */
            break;
    }
    ebytes(e, ex_ops[k].b, ex_ops[k].len);
    emit_seq(e, 2, 0x9c, 0x5e);                   /* pushfd; pop esi */
    emit_seq(e, 2, 0x89, 0xf3);                   /* mov ebx, esi */
    emit_seq(e, 3, 0xc1, 0xe3, 0x10);             /* shl ebx, 16 */
    emit_seq(e, 3, 0x66, 0x89, 0xc3);             /* mov bx, ax */
    emit_seq(e, 5, 0xc1, 0xc7, 0x05, 0x31, 0xdf); /* rol edi, 5; xor edi, ebx */
    if (kind == EX_SHIFT) {
        ex_shift_masks(ex_ops[k].b[1]);
        emit_seq(e, 3, 0x23, 0x34, 0x8d); /* and esi, [g_ex_masks + ecx*4] */
        e32(e, (uint32_t) g_ex_masks);
    } else {
        emit_seq(e, 2, 0x81, 0xe6); /* and esi, mask */
        e32(e, ex_ops[k].mask);
    }
    emit_seq(e, 3, 0xc1, 0xe6, 0x10);             /* shl esi, 16 */
    emit_seq(e, 3, 0x66, 0x89, 0xc6);             /* mov si, ax */
    emit_seq(e, 5, 0xc1, 0xc5, 0x05, 0x31, 0xf5); /* rol ebp, 5; xor ebp, esi */
    e8(e, 0x4a);                                  /* dec edx */
    emit_seq(e, 2, 0x0f, 0x89);                   /* jns top */
    e32(e, (uint32_t) (top - (e->p + 4)));
}

/* The raw checksum (EDI) is only comparable within one CPU model. */
static int
dm_exhaust(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    d->edi = 0;
    return 1;
}

static void
group_exhaust8(void)
{
    const char    *name = "exhaust8";
    struct variant v    = { .group = name, .emit = emit_exhaust, .fx = FX_CUSTOM, .defmask = dm_exhaust };
    group_begin(name);
    for (unsigned k = 0; k < sizeof(ex_ops) / sizeof(ex_ops[0]); k++) {
        memcpy(v.target, ex_ops[k].b, ex_ops[k].len); /* for the reports */
        v.target_len = ex_ops[k].len;
        v.arg2       = k;
        run_inputs(&v, 1);
    }
    group_end(name);
}

/*
 * General groups beyond the recompiler's interpreter list. Included into
 * groups.inc after groups_p6.inc; append only.
 */

/* Defined or not, per variant: arg3. */
static int
dm_arg3(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    return v->arg3;
}

/* ---- self-modifying code -------------------------------------------------- */

static void
e_abs(struct emit *e, uint8_t op, uint8_t modrm, uint8_t *addr)
{
    e8(e, op);
    e8(e, modrm);
    e32(e, (uint32_t) addr);
}

/* arg2 is the kind. The patched instruction is at L; its address is only
   known once emitted, so the store's operand is filled in afterwards. */
static void
emit_smc(struct emit *e, const struct variant *v)
{
    int      kind = v->arg2;
    uint8_t *store, *l, *top = 0;

    switch (kind) {
        case 0: /* mov [L+1], cl; jmp L; L: mov al, imm8 */
        case 1: /* the same without the jump */
        case 2: /* the same, 32 bytes further on, no jump */
            store = e->p;
            e_abs(e, 0x88, 0x0d, 0);
            if (kind == 0) {
                e8(e, 0xeb);
                e8(e, 0x00);
            }
            if (kind == 2)
                for (int i = 0; i < 32; i++)
                    e8(e, 0x90);
            l = e->p;
            e8(e, 0xb0);
            e8(e, 0x00);
            put_le(store + 2, (uint32_t) (l + 1), 4);
            break;
        case 3: /* mov [L+1], edx; jmp L; L: mov esi, imm32 */
            store = e->p;
            e_abs(e, 0x89, 0x15, 0);
            e8(e, 0xeb);
            e8(e, 0x00);
            l = e->p;
            e8(e, 0xbe);
            e32(e, 0);
            put_le(store + 2, (uint32_t) (l + 1), 4);
            break;
        case 4: /* mov byte [L], 48h; jmp L; L: inc eax (now dec eax) */
            store = e->p;
            e_abs(e, 0xc6, 0x05, 0);
            e8(e, 0x48);
            e8(e, 0xeb);
            e8(e, 0x00);
            l = e->p;
            e8(e, 0x40);
            put_le(store + 2, (uint32_t) l, 4);
            break;
        default: /* a loop that repatches an instruction of its own block */
            e8(e, 0xbf); /* mov edi, 3 */
            e32(e, 3);
            top   = e->p;
            store = e->p;
            e_abs(e, 0x88, 0x0d, 0); /* mov [L+2], cl */
            if (kind == 5) {
                e8(e, 0xeb);
                e8(e, 0x00);
            }
            l = e->p;
            e8(e, 0x83); /* add esi, imm8 */
            e8(e, 0xc6);
            e8(e, 0x00);
            e8(e, 0x80); /* add cl, 5 */
            e8(e, 0xc1);
            e8(e, 0x05);
            e8(e, 0x4f); /* dec edi */
            e8(e, 0x75); /* jnz top */
            e8(e, (uint8_t) (top - (e->p + 1)));
            put_le(store + 2, (uint32_t) (l + 2), 4);
            break;
    }
}

static void
group_smc(void)
{
    /* Signatures for the reports, and whether the result is architectural:
       without a jump after the store, a 486 may still run the bytes it
       prefetched. */
    static const struct {
        uint8_t len, defined;
        uint8_t b[3];
    } kinds[] = {
        { 3, 1, { 0x88, 0xeb, 0xb0 } },
        { 2, 0, { 0x88, 0xb0 } },
        { 3, 0, { 0x88, 0x90, 0xb0 } },
        { 3, 1, { 0x89, 0xeb, 0xbe } },
        { 3, 1, { 0xc6, 0xeb, 0x40 } },
        { 3, 1, { 0x88, 0xeb, 0x83 } },
        { 2, 0, { 0x88, 0x83 } },
    };
    const char    *name = "smc";
    struct variant v    = { .group = name, .emit = emit_smc, .fx = FX_CUSTOM, .defmask = dm_arg3 };
    group_begin(name);
    for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        memcpy(v.target, kinds[k].b, kinds[k].len);
        v.target_len = kinds[k].len;
        v.arg2       = k;
        v.arg3       = kinds[k].defined;
        run_ctx(&v, ctx_some, 3, 4);
    }
    group_end(name);
}

/* ---- segment registers in protected mode ---------------------------------- */

/* The harness's GDT: 08 flat code, 10 flat data, 18 16-bit code, 20 16-bit
   data (both 64 KB), limit 27h. The rest: null, RPL 3 (#GP for data), past
   the limit (#GP), and the LDT with no LDTR (#GP). */
static const uint16_t seg_sels[] = { 0x10, 0x00, 0x08, 0x18, 0x20, 0x13, 0x28, 0x04, 0x0b };
#define N_SEG_SELS (sizeof(seg_sels) / sizeof(seg_sels[0]))

/* AX: the selector (arg3 indexes seg_sels); EBX: the buffer. For LFS the
   far pointer at [ebx] instead. For LAR and friends, CX. */
static void
fix_seg(const struct variant *v)
{
    uint16_t sel = seg_sels[v->arg3];
    g_in.ebx     = (uint32_t) g_buf + MEM_OFF;
    g_in.eax     = (g_in.eax & 0xffff0000u) | sel;
    g_in.ecx     = (g_in.ecx & 0xffff0000u) | sel;
    if (v->arg2) /* the selector of an m16:16 (arg2 = 2) or m16:32 far pointer */
        put_le(g_in.buf + MEM_OFF + (v->arg2 == 2 ? 2 : 4), sel, 2);
}

/* MOV r32, Sreg: the upper half is zero from the Pentium Pro on, and
   undefined before. arg bit 0: ESI holds one; bit 1: EDI; bit 2: EAX. LAR
   (arg bit 3): bits 16-19 of the result undefined. */
static int
dm_seg(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    if (v->arg & 1)
        d->esi &= 0xffff;
    if (v->arg & 2)
        d->edi &= 0xffff;
    if (v->arg & 4)
        d->eax &= 0xffff;
    if ((v->arg & 8) && d->fault == 0xff && (d->flags & F_ZF))
        d->eax &= ~0x000f0000u;
    if (v->arg & 16) /* LAR, LSL, VERR, VERW write ZF only */
        *undef &= ~F_ZF;
    return 1;
}

static void
seg_restore(void)
{
#ifndef HOSTTEST
    __asm__ volatile("mov %0, %%fs\n\tmov %0, %%gs" : : "r"(0x10));
#endif
}

static void
group_segments(void)
{
    static const struct {
        uint8_t len, far, mask;
        uint8_t b[16];
    } seqs[] = {
        /* mov fs, ax; mov esi, fs; mov edi, fs:[ebx]; push 10h; pop fs */
        { 11, 0, 1, { 0x8e, 0xe0, 0x8c, 0xe6, 0x64, 0x8b, 0x3b, 0x6a, 0x10, 0x0f, 0xa1 } },
        /* mov gs, ax; mov esi, gs; mov di, gs:[ebx+2]; push 10h; pop gs */
        { 13, 0, 1, { 0x8e, 0xe8, 0x8c, 0xee, 0x66, 0x65, 0x8b, 0x7b, 0x02, 0x6a, 0x10, 0x0f, 0xa9 } },
        /* lfs esi, [ebx]; mov edi, fs; push 10h; pop fs */
        { 9, 1, 2, { 0x0f, 0xb4, 0x33, 0x8c, 0xe7, 0x6a, 0x10, 0x0f, 0xa1 } },
        /* lfs si, [ebx] (m16:16); mov edi, fs; push 10h; pop fs */
        { 10, 2, 2, { 0x66, 0x0f, 0xb4, 0x33, 0x8c, 0xe7, 0x6a, 0x10, 0x0f, 0xa1 } },
        /* lgs esi, [ebx]; mov edi, gs:[ebx]; push 10h; pop gs */
        { 10, 1, 0, { 0x0f, 0xb5, 0x33, 0x65, 0x8b, 0x3b, 0x6a, 0x10, 0x0f, 0xa9 } },
        /* mov ax, ds (the 16-bit form keeps EAX's upper half) */
        { 3, 0, 0, { 0x66, 0x8c, 0xd8 } },
        /* mov eax, ss */
        { 2, 0, 4, { 0x8c, 0xd0 } },
        /* mov [ebx], cs: always 16 bits to memory */
        { 2, 0, 0, { 0x8c, 0x0b } },
        /* lar eax, ecx */
        { 3, 0, 8 | 16, { 0x0f, 0x02, 0xc1 } },
        /* lar ax, cx */
        { 4, 0, 16, { 0x66, 0x0f, 0x02, 0xc1 } },
        /* lsl edx, ecx */
        { 3, 0, 16, { 0x0f, 0x03, 0xd1 } },
        /* verr cx */
        { 3, 0, 16, { 0x0f, 0x00, 0xe1 } },
        /* verw cx */
        { 3, 0, 16, { 0x0f, 0x00, 0xe9 } },
    };
    const char    *name = "segments";
    struct variant v    = { .group = name, .fx = FX_CUSTOM, .defmask = dm_seg, .fix_input = fix_seg,
                            .faults_ok = 1, .norm = 1 << R_EBX };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        for (unsigned s = 0; s < N_SEG_SELS; s++) {
            memcpy(v.target, seqs[i].b, seqs[i].len);
            v.target_len = seqs[i].len;
            v.arg        = seqs[i].mask;
            v.arg2       = seqs[i].far;
            v.arg3       = s;
            /* Only the far-pointer and store forms write the buffer. */
            v.reg_fix = !seqs[i].far && seqs[i].b[1] != 0x8c;
            run_ctx(&v, ctx_keep, 3, 2);
            seg_restore();
        }
    }
    group_end(name);
}

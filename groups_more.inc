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

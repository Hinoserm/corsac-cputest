/*
 * General groups beyond the recompiler's interpreter list. Included into
 * groups.inc after groups_p6.inc.
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

/* The byte a test patches, and what it held: put back before every run, so
   all four start from the same code (a CPU that runs a stale prefetched
   byte would otherwise run it only the first time). */
static uint8_t *g_smc_patch;
static uint8_t  g_smc_orig;

static void
smc_restore(const struct variant *v)
{
    if (g_smc_patch)
        *g_smc_patch = g_smc_orig;
}

/* Without a jump after the store, a CPU may run the patched instruction as
   it was or as it became, and a real Pentium does both from one run to the
   next. Either is right: MOV AL, imm8 gave the new byte (CL) or the old
   one (0, put back before every run); the canonical result is the new. */
static void
canon_smc_byte(const struct variant *v, const struct kin *in, struct kout *o)
{
    if ((o->eax & 0xff) == 0)
        o->eax = (o->eax & ~0xffu) | (o->ecx & 0xff);
}

/* The loop that repatches ADD ESI, imm8 in its own block three times
   without a jump: each pass adds the byte it just stored (CL, CL+5,
   CL+10) or the one before it (0, CL, CL+5), sign-extended. Any of those
   eight sums is right; the canonical one is all new. */
static void
canon_smc_loop(const struct variant *v, const struct kin *in, struct kout *o)
{
    uint8_t  cl0 = (uint8_t) (o->ecx - 15), p[3], old[3];
    uint32_t fresh = 0, d = o->esi - in->esi;
    for (int i = 0; i < 3; i++) {
        p[i]   = (uint8_t) (cl0 + 5 * i);
        old[i] = i ? p[i - 1] : 0;
        fresh += (uint32_t) (int32_t) (int8_t) p[i];
    }
    for (int m = 0; m < 8; m++) {
        uint32_t sum = 0;
        for (int i = 0; i < 3; i++)
            sum += (uint32_t) (int32_t) (int8_t) ((m >> i) & 1 ? old[i] : p[i]);
        if (sum == d) {
            o->esi = in->esi + fresh;
            return;
        }
    }
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
            g_smc_patch = l + 1;
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
            g_smc_patch = l + 1; /* the low byte of the dword */
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
            g_smc_patch = l;
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
            g_smc_patch = l + 2;
            break;
    }
    g_smc_orig = *g_smc_patch;
}

/* Code in device memory: a routine written into off-screen VGA text
   memory (BF000h), called, its immediate patched, called again (ESI and
   EDI: both results). 86Box once fetched such code from a dummy. Not
   under a hypervisor (CPUID says so): KVM can't run code out of an
   emulated device. */
static void
smc_device(const char *name)
{
    static const uint8_t b[] = {
        0xc7, 0x05, 0x00, 0xf0, 0x0b, 0x00, 0xb8, 0x78, 0x56, 0x34, /* mov dword [BF000h], 345678B8h */
        0x66, 0xc7, 0x05, 0x04, 0xf0, 0x0b, 0x00, 0x12, 0xc3,       /* mov word [BF004h], C312h: mov eax, 12345678h; ret */
        0xb8, 0x00, 0xf0, 0x0b, 0x00, 0xff, 0xd0, 0x89, 0xc6,       /* call it; mov esi, eax */
        0xc7, 0x05, 0x01, 0xf0, 0x0b, 0x00, 0xf0, 0xde, 0xbc, 0x9a, /* the immediate: 9ABCDEF0h */
        0xb8, 0x00, 0xf0, 0x0b, 0x00, 0xff, 0xd0, 0x89, 0xc7,       /* call again; mov edi, eax */
    };
    struct variant v = { .group = name };
#ifdef HOSTTEST
    return; /* no VGA memory in a Linux process */
#endif
    if (under_hypervisor()) /* KVM can't fetch code from an emulated device */
        return;
    memcpy(v.target, b, sizeof(b));
    v.target_len = sizeof(b);
    run_plain(&v, 1);
}

/* Immediates rewritten in a loop: patch the immediate of the instruction
   under test, jump to it, fold its result and flags into ESI, and go
   round again, twelve times. A recompiler that compiles immediates in
   throws the block away at every patch; 86Box's new one reads them from
   memory instead once a block keeps being rewritten
   (CODEBLOCK_NO_IMMEDIATES), from about the third pass, and the loop
   keeps going in that state. Comanche 3 patches shift counts and byte
   moves this way in its renderer. The jump after each store makes every
   pass defined.

   EBP counts: the value patched in is EBP masked, plus a base address
   for the forms that patch an address. A count starts at a different
   place in each run, so the passes the interpreter takes cover every
   count between them, and every fourth pass patches a count of 0. */
struct smc_imm_form {
    uint8_t  len, b[8]; /* the instruction(s); the patched field is zero here */
    uint8_t  imm;       /* the patched field's size */
    int8_t   ioff;      /* its offset in b[]; -1: last */
    uint8_t  mem;       /* 1: memory at EBX in the buffer; 2: 16-bit addressing in the low buffer */
    uint8_t  count;     /* a count: see above */
    uint8_t  step, mask; /* EBP steps by step; mask 0: patched whole */
    uint8_t  base;      /* added: 1 the address in EBX; 2 the low buffer */
    uint16_t fmask;     /* the flags folded into ESI: those the instruction defines or leaves */
};

#define SMC_ALL   0x8d5 /* CF PF AF ZF SF OF */
#define SMC_LOGIC 0x8c5 /* AF undefined */
#define SMC_SHIFT 0x0c5 /* CF PF ZF SF: for a non-zero count AF is undefined, and OF past a count of 1 */
#define SMC_LAST  -1

static const struct smc_imm_form smc_imm_forms[] = {
    { 2, { 0xb2, 0 }, 1, SMC_LAST, 0, 0, 0x2b, 0, 0, SMC_ALL },                 /* mov dl, imm8 */
    { 2, { 0xb4, 0 }, 1, SMC_LAST, 0, 0, 0x2b, 0, 0, SMC_ALL },                 /* mov ah, imm8 */
    { 4, { 0x66, 0xb8, 0, 0 }, 2, SMC_LAST, 0, 0, 0x2b, 0, 0, SMC_ALL },        /* mov ax, imm16 */
    { 5, { 0xb8, 0, 0, 0, 0 }, 4, SMC_LAST, 0, 0, 0x2b, 0, 0, SMC_ALL },        /* mov eax, imm32 */
    { 3, { 0xc6, 0xc4, 0 }, 1, SMC_LAST, 0, 0, 0x2b, 0, 0, SMC_ALL },           /* mov ah, imm8 (C6) */
    { 3, { 0xc6, 0x03, 0 }, 1, SMC_LAST, 1, 0, 0x2b, 0, 0, SMC_ALL },           /* mov byte [ebx], imm8 */
    { 5, { 0x66, 0xc7, 0x03, 0, 0 }, 2, SMC_LAST, 1, 0, 0x2b, 0, 0, SMC_ALL },  /* mov word [ebx], imm16 */
    { 6, { 0xc7, 0xc0, 0, 0, 0, 0 }, 4, SMC_LAST, 0, 0, 0x2b, 0, 0, SMC_ALL },  /* mov eax, imm32 (C7) */
    { 6, { 0xc7, 0x03, 0, 0, 0, 0 }, 4, SMC_LAST, 1, 0, 0x2b, 0, 0, SMC_ALL },  /* mov dword [ebx], imm32 */
    { 3, { 0xc0, 0xe0, 0 }, 1, SMC_LAST, 0, 1, 1, 7, 0, SMC_SHIFT },            /* shl al, imm8 */
    { 3, { 0xc0, 0xf8, 0 }, 1, SMC_LAST, 0, 1, 3, 0, 0, SMC_SHIFT },            /* sar al, imm8: to 31 */
    { 3, { 0xc0, 0xc4, 0 }, 1, SMC_LAST, 0, 1, 3, 0, 0, SMC_SHIFT },            /* rol ah, imm8: to 31 */
    { 3, { 0xc0, 0x2b, 0 }, 1, SMC_LAST, 1, 1, 1, 7, 0, SMC_SHIFT },            /* shr byte [ebx], imm8 */
    { 3, { 0xc0, 0x0b, 0 }, 1, SMC_LAST, 1, 1, 3, 0, 0, SMC_SHIFT },            /* ror byte [ebx], imm8: to 31 */
    { 4, { 0x66, 0xc1, 0xe8, 0 }, 1, SMC_LAST, 0, 1, 1, 15, 0, SMC_SHIFT },     /* shr ax, imm8 */
    { 4, { 0x66, 0xc1, 0xc8, 0 }, 1, SMC_LAST, 0, 1, 3, 0, 0, SMC_SHIFT },      /* ror ax, imm8 */
    { 4, { 0x66, 0xc1, 0x23, 0 }, 1, SMC_LAST, 1, 1, 1, 15, 0, SMC_SHIFT },     /* shl word [ebx], imm8 */
    { 3, { 0xc1, 0xe0, 0 }, 1, SMC_LAST, 0, 1, 3, 0, 0, SMC_SHIFT },            /* shl eax, imm8 */
    { 3, { 0xc1, 0x3b, 0 }, 1, SMC_LAST, 1, 1, 3, 0, 0, SMC_SHIFT },            /* sar dword [ebx], imm8 */
    { 3, { 0xc1, 0x03, 0 }, 1, SMC_LAST, 1, 1, 3, 0, 0, SMC_SHIFT },            /* rol dword [ebx], imm8 */
    { 5, { 0xc1, 0xc0, 0, 0xd1, 0xc2 }, 1, 2, 0, 1, 1, 1, 0, SMC_SHIFT },       /* rol eax, 0 or 1; rol edx, 1 */
    { 4, { 0x0f, 0xa4, 0xd0, 0 }, 1, SMC_LAST, 0, 1, 3, 0, 0, SMC_SHIFT },      /* shld eax, edx, imm8 */
    { 4, { 0x0f, 0xac, 0xd0, 0 }, 1, SMC_LAST, 0, 1, 3, 0, 0, SMC_SHIFT },      /* shrd eax, edx, imm8 */
    { 5, { 0x66, 0x0f, 0xa4, 0xd0, 0 }, 1, SMC_LAST, 0, 1, 1, 15, 0, SMC_SHIFT }, /* shld ax, dx, imm8 */
    { 5, { 0x66, 0x0f, 0xac, 0xd0, 0 }, 1, SMC_LAST, 0, 1, 1, 15, 0, SMC_SHIFT }, /* shrd ax, dx, imm8 */
    { 4, { 0x0f, 0xa4, 0x13, 0 }, 1, SMC_LAST, 1, 1, 3, 0, 0, SMC_SHIFT },      /* shld [ebx], edx, imm8 */
    { 5, { 0x66, 0x0f, 0xac, 0x13, 0 }, 1, SMC_LAST, 1, 1, 1, 15, 0, SMC_SHIFT }, /* shrd word [ebx], dx, imm8 */
    { 3, { 0x83, 0xc0, 0 }, 1, SMC_LAST, 0, 0, 0x2b, 0, 0, SMC_ALL },           /* add eax, imm8 */
    { 3, { 0x83, 0xd0, 0 }, 1, SMC_LAST, 0, 0, 0x2b, 0, 0, SMC_ALL },           /* adc eax, imm8 */
    { 3, { 0x83, 0xe0, 0 }, 1, SMC_LAST, 0, 0, 0x2b, 0, 0, SMC_LOGIC },         /* and eax, imm8 */
    { 3, { 0x83, 0xf8, 0 }, 1, SMC_LAST, 0, 0, 0x2b, 0, 0, SMC_ALL },           /* cmp eax, imm8 */
    { 4, { 0x66, 0x83, 0xe8, 0 }, 1, SMC_LAST, 0, 0, 0x2b, 0, 0, SMC_ALL },     /* sub ax, imm8 */
    { 4, { 0x66, 0x83, 0xda, 0 }, 1, SMC_LAST, 0, 0, 0x2b, 0, 0, SMC_ALL },     /* sbb dx, imm8 */
    { 3, { 0x83, 0x03, 0 }, 1, SMC_LAST, 1, 0, 0x2b, 0, 0, SMC_ALL },           /* add dword [ebx], imm8 */
    { 3, { 0x83, 0x13, 0 }, 1, SMC_LAST, 1, 0, 0x2b, 0, 0, SMC_ALL },           /* adc dword [ebx], imm8 */
    { 4, { 0x66, 0x83, 0x1b, 0 }, 1, SMC_LAST, 1, 0, 0x2b, 0, 0, SMC_ALL },     /* sbb word [ebx], imm8 */
    { 4, { 0x66, 0x83, 0x3b, 0 }, 1, SMC_LAST, 1, 0, 0x2b, 0, 0, SMC_ALL },     /* cmp word [ebx], imm8 */
    { 4, { 0x66, 0x83, 0x0b, 0 }, 1, SMC_LAST, 1, 0, 0x2b, 0, 0, SMC_LOGIC },   /* or word [ebx], imm8 */
    { 6, { 0x81, 0xc0, 0, 0, 0, 0 }, 4, SMC_LAST, 0, 0, 0x2b, 0, 0, SMC_ALL },  /* add eax, imm32 */
    { 5, { 0x66, 0x81, 0x33, 0, 0 }, 2, SMC_LAST, 1, 0, 0x2b, 0, 0, SMC_LOGIC }, /* xor word [ebx], imm16 */
    { 3, { 0x80, 0x03, 0 }, 1, SMC_LAST, 1, 0, 0x2b, 0, 0, SMC_ALL },           /* add byte [ebx], imm8 */
    { 3, { 0x80, 0x13, 0 }, 1, SMC_LAST, 1, 0, 0x2b, 0, 0, SMC_ALL },           /* adc byte [ebx], imm8 */
    { 3, { 0x80, 0x1b, 0 }, 1, SMC_LAST, 1, 0, 0x2b, 0, 0, SMC_ALL },           /* sbb byte [ebx], imm8 */
    { 3, { 0x80, 0x33, 0 }, 1, SMC_LAST, 1, 0, 0x2b, 0, 0, SMC_LOGIC },         /* xor byte [ebx], imm8 */
    { 3, { 0x80, 0x3b, 0 }, 1, SMC_LAST, 1, 0, 0x2b, 0, 0, SMC_ALL },           /* cmp byte [ebx], imm8 */
    { 3, { 0xf6, 0xc4, 0 }, 1, SMC_LAST, 0, 0, 0x2b, 0, 0, SMC_LOGIC },         /* test ah, imm8 */
    { 5, { 0x66, 0xf7, 0xc2, 0, 0 }, 2, SMC_LAST, 0, 0, 0x2b, 0, 0, SMC_LOGIC }, /* test dx, imm16 */
    { 6, { 0xf7, 0x03, 0, 0, 0, 0 }, 4, SMC_LAST, 1, 0, 0x2b, 0, 0, SMC_LOGIC }, /* test dword [ebx], imm32 */
    { 3, { 0x6a, 0, 0x58 }, 1, 1, 0, 0, 0x2b, 0, 0, SMC_ALL },                  /* push imm8; pop eax */
    { 6, { 0x68, 0, 0, 0, 0, 0x5a }, 4, 1, 0, 0, 0x2b, 0, 0, SMC_ALL },         /* push imm32; pop edx */
    { 5, { 0x66, 0x6a, 0, 0x66, 0x58 }, 1, 2, 0, 0, 0x2b, 0, 0, SMC_ALL },      /* push word imm8; pop ax */
    { 6, { 0x66, 0x68, 0, 0, 0x66, 0x5a }, 2, 2, 0, 0, 0x2b, 0, 0, SMC_ALL },   /* push word imm16; pop dx */
    { 3, { 0x8b, 0x43, 0 }, 1, SMC_LAST, 1, 0, 0x2b, 0x1c, 0, SMC_ALL },        /* mov eax, [ebx+disp8] */
    { 4, { 0x8b, 0x44, 0x23, 0 }, 1, SMC_LAST, 1, 0, 0x2b, 0x1c, 0, SMC_ALL },  /* mov eax, [ebx+disp8] through a SIB */
    { 3, { 0x01, 0x53, 0 }, 1, SMC_LAST, 1, 0, 0x2b, 0x1c, 0, SMC_ALL },        /* add [ebx+disp8], edx */
    { 5, { 0xa1, 0, 0, 0, 0 }, 4, SMC_LAST, 1, 0, 0x2b, 0x1c, 1, SMC_ALL },     /* mov eax, [moffs32] */
    { 5, { 0xa0, 0, 0, 0, 0 }, 4, SMC_LAST, 1, 0, 0x2b, 0x1f, 1, SMC_ALL },     /* mov al, [moffs32] */
    { 5, { 0xa3, 0, 0, 0, 0 }, 4, SMC_LAST, 1, 0, 0x2b, 0x1c, 1, SMC_ALL },     /* mov [moffs32], eax */
    { 6, { 0x66, 0xa3, 0, 0, 0, 0 }, 4, SMC_LAST, 1, 0, 0x2b, 0x1e, 1, SMC_ALL }, /* mov [moffs32], ax */
    { 4, { 0x67, 0x8a, 0x41, 0 }, 1, SMC_LAST, 2, 0, 0x2b, 7, 0, SMC_ALL },     /* mov al, [bx+di+disp8] */
    { 5, { 0x67, 0x8a, 0x81, 0, 0 }, 2, SMC_LAST, 2, 0, 0x2b, 7, 0, SMC_ALL },  /* mov al, [bx+di+disp16] */
    { 5, { 0x67, 0x8a, 0x06, 0, 0 }, 2, SMC_LAST, 2, 0, 0x2b, 15, 2, SMC_ALL }, /* mov al, [disp16] */
    { 4, { 0x67, 0xa0, 0, 0 }, 2, SMC_LAST, 2, 0, 0x2b, 15, 2, SMC_ALL },       /* mov al, [moffs16] */
    { 4, { 0x67, 0xa2, 0, 0 }, 2, SMC_LAST, 2, 0, 0x2b, 15, 2, SMC_ALL },       /* mov [moffs16], al */
};

#define SMC_ITERS 12

/* The run this variant is, 0-5: where a count starts. */
static int
smc_imm_slot(const struct variant *v)
{
    return 2 * g_case + (v->producer != 0);
}

static void
emit_smc_imm(struct emit *e, const struct variant *v)
{
    const struct smc_imm_form *f    = &smc_imm_forms[v->arg2];
    const int                  ioff = (f->ioff == SMC_LAST) ? f->len - f->imm : f->ioff;
    uint32_t                   base = 0;
    uint8_t                   *store, *top, *l, *m;

    if (f->base == 1)
        base = (uint32_t) g_buf + MEM_OFF;
    else if (f->base == 2)
        base = (uint32_t) LOWBUF;
    if (f->count) {
        e8(e, 0xbd); /* mov ebp, start */
        e32(e, 3u * f->step * smc_imm_slot(v));
    }
    e8(e, 0xbf); /* mov edi, SMC_ITERS */
    e32(e, SMC_ITERS);
    top = e->p;
    e8(e, 0x8b); /* mov ecx, ebp */
    e8(e, 0xcd);
    if (f->mask) {
        e8(e, 0x81); /* and ecx, mask */
        e8(e, 0xe1);
        e32(e, f->mask);
    }
    if (f->count) {
        static const uint8_t zero4[] = {
            0xf7, 0xc7, 0x03, 0, 0, 0, /* test edi, 3 */
            0x75, 0x02,                /* jnz +2 */
            0x31, 0xc9,                /* xor ecx, ecx: every fourth pass a count of 0 */
        };
        ebytes(e, zero4, sizeof(zero4));
    }
    if (base) {
        e8(e, 0x81); /* add ecx, base */
        e8(e, 0xc1);
        e32(e, base);
    }
    store = e->p;
    if (f->imm == 2)
        e8(e, 0x66);
    e_abs(e, f->imm == 1 ? 0x88 : 0x89, 0x0d, 0); /* mov [field], cl / cx / ecx */
    e8(e, 0xeb); /* jmp L */
    e8(e, 0x00);
    l = e->p;
    ebytes(e, f->b, f->len);
    put_le(store + (f->imm == 2 ? 3 : 2), (uint32_t) (l + ioff), 4);
    static const uint8_t fold[] = {
        0x9c,                         /* pushfd */
        0x81, 0x24, 0x24, 0, 0, 0, 0, /* and dword [esp], fmask */
        0xc1, 0xc6, 0x05,             /* rol esi, 5 */
        0x33, 0x34, 0x24,             /* xor esi, [esp] */
        0x83, 0xc4, 0x04,             /* add esp, 4 */
        0xc1, 0xc6, 0x05,             /* rol esi, 5 */
        0x01, 0xc6,                   /* add esi, eax */
        0x01, 0xd6,                   /* add esi, edx */
    };
    m = e->p;
    ebytes(e, fold, sizeof(fold));
    put_le(m + 4, f->fmask, 4);
    if (f->mem == 1) {
        e8(e, 0x03); /* add esi, [ebx] */
        e8(e, 0x33);
    }
    e8(e, 0x81); /* add ebp, step */
    e8(e, 0xc5);
    e32(e, f->count ? f->step : 0x3b5d1f07u);
    e8(e, 0x4f); /* dec edi */
    e8(e, 0x75); /* jnz top */
    e8(e, (uint8_t) (top - (e->p + 1)));
}

static void
group_smc(void)
{
    /* Signatures for the reports. The kinds without a jump accept either
       outcome (canon_smc_byte, canon_smc_loop). */
    static const struct {
        uint8_t defined, len;
        uint8_t b[3];
    } kinds[] = {
        { 1, SEQ(0x88, 0xeb, 0xb0) },
        { 0, SEQ(0x88, 0xb0) },
        { 0, SEQ(0x88, 0x90, 0xb0) },
        { 1, SEQ(0x89, 0xeb, 0xbe) },
        { 1, SEQ(0xc6, 0xeb, 0x40) },
        { 1, SEQ(0x88, 0xeb, 0x83) },
        { 0, SEQ(0x88, 0x83) },
    };
    const char    *name = "smc";
    struct variant v    = { .group = name, .emit = emit_smc, .fx = FX_CUSTOM, .defmask = dm_arg3, .before_run = smc_restore };
    group_begin(name);
    for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        memcpy(v.target, kinds[k].b, kinds[k].len);
        v.target_len = kinds[k].len;
        v.arg2       = k;
        v.arg3       = 1;
        v.canon      = k == 6 ? canon_smc_loop : kinds[k].defined ? 0 : canon_smc_byte;
        run_plain(&v, 2);
    }
    for (unsigned k = 0; k < sizeof(smc_imm_forms) / sizeof(smc_imm_forms[0]); k++) {
        struct variant w = { .group = name, .emit = emit_smc_imm, .fx = FX_CUSTOM, .defmask = dm_arg3, .arg2 = k, .arg3 = 1 };
        const struct smc_imm_form *f = &smc_imm_forms[k];

        memcpy(w.target, f->b, f->len); /* the signature for the reports */
        w.target_len = f->len;
        if (f->mem == 1) {
            w.fix_input = fix_mem_ebx;
            w.arg       = MEM_OFF;
            if (f->base == 1)
                w.norm = 1 << R_ECX; /* the address last patched in */
        } else if (f->mem == 2) {
            w.fix_input = fix_mem16;
            w.no_ref    = 1;
        }
        run_plain(&w, f->count ? 3 : 2);
    }
    smc_device(name);
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

/* LDS LES LFS LGS LSS from a far pointer in the buffer, 32- and 16-bit
   offsets, with a data, null, code, 64 KB and out-of-GDT selector; what
   was loaded is read back with MOV CX, Sreg. DS and ES are put back
   straight after; LSS's SS:ESP too, ESP kept in EDI meanwhile (which then
   holds the loaded offset), SS through AX. */
static void
segments_lds(const char *name)
{
    static const struct {
        uint8_t n, op[2], sreg;
    } ops[] = {
        { 1, { 0xc5 }, 3 },       /* lds */
        { 1, { 0xc4 }, 0 },       /* les */
        { 2, { 0x0f, 0xb4 }, 4 }, /* lfs */
        { 2, { 0x0f, 0xb5 }, 5 }, /* lgs */
        { 2, { 0x0f, 0xb2 }, 2 }, /* lss */
    };
    static const uint8_t sels[] = { 0x10, 0x00, 0x08, 0x20, 0x28 };
    struct variant v = { .group = name, .faults_ok = 1, .fix_input = fix_mem_ebx, .arg = MEM_OFF };
#ifdef HOSTTEST
    return;
#endif
    for (unsigned o = 0; o < sizeof(ops) / sizeof(ops[0]); o++) {
        int sr = ops[o].sreg;
        for (int w = 0; w < 2; w++) {
            for (unsigned s = 0; s < sizeof(sels); s++) {
                int      n = 0;
                uint8_t *t = v.target;
                t[n++]     = 0x66; /* mov word [ebx+4 or +2], sel */
                t[n++]     = 0xc7;
                t[n++]     = 0x43;
                t[n++]     = w ? 2 : 4;
                t[n++]     = sels[s];
                t[n++]     = 0;
                if (sr == 2) {
                    t[n++] = 0x89; /* mov edi, esp */
                    t[n++] = 0xe7;
                }
                if (w)
                    t[n++] = 0x66;
                memcpy(t + n, ops[o].op, ops[o].n);
                n += ops[o].n;
                t[n++] = 0x33; /* esi, [ebx] */
                t[n++] = 0x66; /* mov cx, sreg */
                t[n++] = 0x8c;
                t[n++] = 0xc1 | (sr << 3);
                if (sr == 2) {
                    t[n++] = 0x87; /* xchg edi, esp */
                    t[n++] = 0xe7;
                }
                if (sr != 4 && sr != 5) {
                    t[n++] = 0x66; /* mov ax, 10h; mov sreg, ax */
                    t[n++] = 0xb8;
                    t[n++] = 0x10;
                    t[n++] = 0x00;
                    t[n++] = 0x8e;
                    t[n++] = 0xc0 | (sr << 3);
                }
                v.target_len = n;
                run_plain(&v, 1);
            }
        }
    }
}

/* MOV r32, Sreg (the upper half: zero on a P6, the model's own before),
   MOV r16, Sreg, MOV [m], Sreg (16 bits), and PUSH Sreg with a 32-bit
   operand onto a slot holding FFFFFFFFh: a 16-bit write or a zero-
   extended one. Raw. */
static void
segments_sreg(const char *name)
{
    static const uint8_t pushes[6][2] = { { 0x06 }, { 0x0e }, { 0x16 }, { 0x1e }, { 0x0f, 0xa0 }, { 0x0f, 0xa8 } };
    struct variant v = { .group = name, .fx = FX_CUSTOM, .defmask = dm_fxarg, .fx_arg = 0,
                         .fix_input = fix_mem_ebx, .arg = MEM_OFF };
    for (int s = 0; s < 6; s++) {
        uint8_t mod = 0xc0 | (s << 3); /* ModRM for Sreg s */
        v.target[0] = 0x8c;             /* mov eax, sreg (EAX random going in) */
        v.target[1] = mod;
        v.target_len = 2;
        run_plain(&v, 1);
        v.target[0] = 0x66; /* mov ax, sreg */
        v.target[1] = 0x8c;
        v.target[2] = mod;
        v.target_len = 3;
        run_plain(&v, 1);
        v.target[0] = 0x8c; /* mov [ebx], sreg */
        v.target[1] = 0x03 | (s << 3);
        v.target_len = 2;
        run_plain(&v, 1);
        int n = 0; /* mov dword [esp-4], -1; push sreg; pop eax */
        v.target[n++] = 0xc7;
        v.target[n++] = 0x44;
        v.target[n++] = 0x24;
        v.target[n++] = 0xfc;
        put_le(v.target + n, 0xffffffff, 4);
        n += 4;
        v.target[n++] = pushes[s][0];
        if (pushes[s][1])
            v.target[n++] = pushes[s][1];
        v.target[n++] = 0x58;
        v.target_len  = n;
        run_plain(&v, 1);
    }
}

/* Far pointers at the end of 16-bit addressing: through FS as the 64 KB
   data segment, a 6-byte pointer at FFFEh is #GP, a 4-byte one at FFFCh
   loads and one at FFFEh is #GP; through flat DS, the 4-byte one at FFFEh
   is read across 10000h, not wrapped to 0 (a 386 and later don't wrap).
   LFS only, so a model that wraps can only change FS (put back after
   each run). The pointers are stored first: 0010h:1234h at FFFCh; for
   the flat case offset 5678h at FFFEh and selector 0010h at 10000h. */
static void
segments_wrap(const char *name)
{
    static const struct {
        uint8_t len, b[40];
    } seqs[] = {
        { SEQ(0xc7, 0x05, 0xfc, 0xff, 0x00, 0x00, 0x34, 0x12, 0x10, 0x00,   /* [FFFCh] = 0010h:1234h */
              0x66, 0xb8, 0x20, 0x00, 0x8e, 0xe0, 0xbb, 0xfe, 0xff, 0x00, 0x00, /* fs = 20h; ebx = FFFEh */
              0x64, 0x67, 0x0f, 0xb4, 0x37) },                                /* lfs esi, fs:[bx]: #GP */
        { SEQ(0xc7, 0x05, 0xfc, 0xff, 0x00, 0x00, 0x34, 0x12, 0x10, 0x00,
              0x66, 0xb8, 0x20, 0x00, 0x8e, 0xe0, 0xbb, 0xfc, 0xff, 0x00, 0x00,
              0x66, 0x64, 0x67, 0x0f, 0xb4, 0x37, 0x8c, 0xe7) },              /* lfs si, fs:[bx] at FFFCh; mov edi, fs */
        { SEQ(0x66, 0xb8, 0x20, 0x00, 0x8e, 0xe0, 0xbb, 0xfe, 0xff, 0x00, 0x00,
              0x66, 0x64, 0x67, 0x0f, 0xb4, 0x37) },                          /* at FFFEh: #GP */
        { SEQ(0xc7, 0x05, 0xfc, 0xff, 0x00, 0x00, 0x00, 0x00, 0x78, 0x56,   /* [FFFEh] = 5678h */
              0xc7, 0x05, 0x00, 0x00, 0x01, 0x00, 0x10, 0x00, 0x00, 0x00,   /* [10000h] = 0010h */
              0xbb, 0xfe, 0xff, 0x00, 0x00, 0x66, 0x67, 0x0f, 0xb4, 0x37,     /* lfs si, [bx] (flat DS) */
              0x8c, 0xe7) },                                                  /* mov edi, fs */
    };
    struct variant v = { .group = name, .faults_ok = 1 };
#ifdef HOSTTEST
    return; /* segment loads (host build) */
#endif
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        run_plain(&v, 1);
    }
}

static void
group_segments(void)
{
    static const struct {
        uint8_t far, mask, len;
        uint8_t b[16];
    } seqs[] = {
        /* mov fs, ax; mov esi, fs; mov edi, fs:[ebx]; push 10h; pop fs */
        { 0, 1, SEQ(0x8e, 0xe0, 0x8c, 0xe6, 0x64, 0x8b, 0x3b, 0x6a, 0x10, 0x0f, 0xa1) },
        /* mov gs, ax; mov esi, gs; mov di, gs:[ebx+2]; push 10h; pop gs */
        { 0, 1, SEQ(0x8e, 0xe8, 0x8c, 0xee, 0x66, 0x65, 0x8b, 0x7b, 0x02, 0x6a, 0x10, 0x0f, 0xa9) },
        /* lfs esi, [ebx]; mov edi, fs; push 10h; pop fs */
        { 1, 2, SEQ(0x0f, 0xb4, 0x33, 0x8c, 0xe7, 0x6a, 0x10, 0x0f, 0xa1) },
        /* lfs si, [ebx] (m16:16); mov edi, fs; push 10h; pop fs */
        { 2, 2, SEQ(0x66, 0x0f, 0xb4, 0x33, 0x8c, 0xe7, 0x6a, 0x10, 0x0f, 0xa1) },
        /* lgs esi, [ebx]; mov edi, gs:[ebx]; push 10h; pop gs */
        { 1, 0, SEQ(0x0f, 0xb5, 0x33, 0x65, 0x8b, 0x3b, 0x6a, 0x10, 0x0f, 0xa9) },
        /* mov ax, ds (the 16-bit form keeps EAX's upper half) */
        { 0, 0, SEQ(0x66, 0x8c, 0xd8) },
        /* mov eax, ss */
        { 0, 4, SEQ(0x8c, 0xd0) },
        /* mov [ebx], cs: always 16 bits to memory */
        { 0, 0, SEQ(0x8c, 0x0b) },
        /* lar eax, ecx */
        { 0, 8 | 16, SEQ(0x0f, 0x02, 0xc1) },
        /* lar ax, cx */
        { 0, 16, SEQ(0x66, 0x0f, 0x02, 0xc1) },
        /* lsl edx, ecx */
        { 0, 16, SEQ(0x0f, 0x03, 0xd1) },
        /* verr cx */
        { 0, 16, SEQ(0x0f, 0x00, 0xe1) },
        /* verw cx */
        { 0, 16, SEQ(0x0f, 0x00, 0xe9) },
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
            v.reg_fix = !seqs[i].far && !(seqs[i].b[0] == 0x8c && seqs[i].b[1] < 0xc0);
            run_keep(&v, 1);
            seg_restore();
        }
    }
    segments_lds(name);
    segments_sreg(name);
    segments_wrap(name);
    group_end(name);
}

/*
 * The test groups. Included into harness.c.
 *
 * Each group builds variants (the bytes under test, what runs before them,
 * whether they start a block) and runs each with a few inputs. The order of
 * groups and the variants inside a group never change once a result has
 * been recorded: the CRCs depend on them. Add new groups at the end.
 */

/* Register-to-register ModRM. */
#define MODRM_RR(reg, rm) (0xc0 | ((reg) << 3) | (rm))

static void
fix_mem_ebx(const struct variant *v)
{
    g_in.ebx = (uint32_t) g_buf + v->arg;
}

/* 16-bit addressing, [bx+di]: BX points at the low buffer, DI a little way
   into it; the upper halves are random and must be ignored. */
static void
fix_mem16(const struct variant *v)
{
    (void) v;
#ifdef HOSTTEST
    g_skip = 1; /* nothing is mapped below 64 KB in a Linux process */
#endif
    g_in.ebx = (rnd() & 0xffff0000) | (uint32_t) LOWBUF;
    g_in.edi = (rnd() & 0xffff0000) | (rnd() & 7);
}

/* BT with a register bit offset into memory: the offset reaches up to 32
   bytes either side of [ebx], which stays inside the buffer. */
static void
fix_bt_mem_reg(const struct variant *v)
{
    int32_t off = (int32_t) (rnd() % 512) - 256;
    g_in.ebx    = (uint32_t) g_buf + 32;
    if (v->arg)
        g_in.edx = (rnd() & 0xffff0000) | (uint16_t) (int16_t) off;
    else
        g_in.edx = (uint32_t) off;
}

static void
run_inputs(struct variant *v, int inputs)
{
    for (int i = 0; i < inputs; i++)
        run_variant(v);
}

/* Every producer, both with and without a block boundary. */
static void
run_all_contexts(struct variant *v, int inputs)
{
    for (unsigned p = 0; p < N_PRODUCERS; p++) {
        for (int b = 0; b < 2; b++) {
            v->producer = p;
            v->boundary = b;
            run_inputs(v, inputs);
        }
    }
}

/* A few producers, both with and without a block boundary. */
static void
run_some_contexts(struct variant *v, int inputs)
{
    static const uint8_t some[] = { 0, 3, 2 }; /* none, add, sub */
    for (unsigned p = 0; p < sizeof(some); p++) {
        for (int b = 0; b < 2; b++) {
            v->producer = some[p];
            v->boundary = b;
            run_inputs(v, inputs);
        }
    }
}

/* ALU r/m,r and r,r/m, register forms, 8/16/32-bit. Already compiled; the
   baseline for the host-register work and a check of the harness. */
static void
group_alu(void)
{
    const char     *name = "alu.rr";
    struct variant  v    = { .group = name };
    group_begin(name);
    for (int op = 0; op < 8; op++) {
        for (int form = 0; form < 4; form++) {
            for (int size = 0; size < 3; size++) { /* 8, 32, 16 */
                if (size == 0 && (form & 1))
                    continue;
                if (size != 0 && !(form & 1))
                    continue;
                for (int reg = 0; reg < 8; reg++) {
                    for (int rm = 0; rm < 8; rm++) {
                        int dst = (form & 2) ? reg : rm;
                        if (size != 0 && dst == R_ESP)
                            continue;
                        int n = 0;
                        if (size == 2)
                            v.target[n++] = 0x66;
                        v.target[n++] = (op << 3) | form;
                        v.target[n++] = MODRM_RR(reg, rm);
                        v.target_len  = n;
                        v.fix_input   = 0;
                        v.fx          = FX_ALU;
                        v.fx_arg      = op;
                        v.no_ref      = size != 0 && (reg == R_ESP || rm == R_ESP);
                        v.producer    = 0;
                        v.boundary    = 0;
                        run_inputs(&v, 1);
                        v.producer = 3;
                        v.boundary = 1;
                        run_inputs(&v, 1);
                    }
                }
            }
        }
    }
    group_end(name);
}

/* SETcc: all sixteen conditions into every byte register and three memory
   forms, after every flag producer, with and without a block boundary. */
static void
group_setcc(void)
{
    const char    *name = "setcc";
    struct variant v    = { .group = name };
    group_begin(name);
    for (int cc = 0; cc < 16; cc++) {
        for (int form = 0; form < 11; form++) {
            int n         = 0;
            v.fix_input   = 0;
            if (form == 10)
                v.target[n++] = 0x67;
            v.target[n++] = 0x0f;
            v.target[n++] = 0x90 | cc;
            if (form < 8)
                v.target[n++] = MODRM_RR(0, form);
            else if (form == 8) {
                v.target[n++] = 0x03; /* [ebx] */
                v.fix_input   = fix_mem_ebx;
                v.arg         = 8;
            } else if (form == 9) {
                v.target[n++] = 0x43; /* [ebx+16] */
                v.target[n++] = 0x10;
                v.fix_input   = fix_mem_ebx;
                v.arg         = 8;
            } else {
                v.target[n++] = 0x01; /* [bx+di] */
                v.fix_input   = fix_mem16;
            }
            v.target_len = n;
            v.fx         = FX_SETCC;
            v.fx_arg     = cc;
            v.no_ref     = form == 10;
            run_all_contexts(&v, 3);
        }
    }
    group_end(name);
}

/* LAHF and SAHF, alone and together, after every flag producer. */
static void
group_lahf_sahf(void)
{
    static const uint8_t seqs[4][2] = { { 0x9f }, { 0x9e }, { 0x9e, 0x9f }, { 0x9f, 0x9e } };
    static const uint8_t lens[4]    = { 1, 1, 2, 2 };
    const char          *name       = "lahf.sahf";
    struct variant       v          = { .group = name };
    group_begin(name);
    for (int s = 0; s < 4; s++) {
        memcpy(v.target, seqs[s], lens[s]);
        v.target_len = lens[s];
        v.fix_input  = 0;
        v.fx         = FX_LAHF_SAHF;
        v.no_ref     = 0;
        run_all_contexts(&v, 32);
    }
    group_end(name);
}

/* BSWAP on every register but ESP, with and without an operand-size prefix
   (undefined on real hardware; the interpreter's answer is the reference).
   A 386-class CPU (RapidCAD) must fault with #UD the same way every run. */
static void
group_bswap(void)
{
    const char    *name = "bswap";
    struct variant v    = { .group = name };
    group_begin(name);
    for (int r = 0; r < 8; r++) {
        if (r == R_ESP)
            continue;
        for (int pfx = 0; pfx < 2; pfx++) {
            int n = 0;
            if (pfx)
                v.target[n++] = 0x66;
            v.target[n++] = 0x0f;
            v.target[n++] = 0xc8 | r;
            v.target_len  = n;
            v.fix_input   = 0;
            v.fx          = pfx ? FX_UNDEF : FX_NONE;
            v.no_ref      = 0;
            run_some_contexts(&v, 8);
        }
    }
    group_end(name);
}

/* MOVZX and MOVSX, byte and word sources, 32- and 16-bit destinations, from
   registers and three memory forms (one misaligned). */
static void
group_movx(void)
{
    static const uint8_t ops[4] = { 0xb6, 0xb7, 0xbe, 0xbf };
    const char          *name   = "movzx.movsx";
    struct variant       v      = { .group = name };
    group_begin(name);
    for (int o = 0; o < 4; o++) {
        for (int size16 = 0; size16 < 2; size16++) {
            for (int dst = 0; dst < 8; dst++) {
                if (dst == R_ESP)
                    continue;
                for (int form = 0; form < 11; form++) {
                    int n       = 0;
                    v.fix_input = 0;
                    if (form == 10)
                        v.target[n++] = 0x67;
                    if (size16)
                        v.target[n++] = 0x66;
                    v.target[n++] = 0x0f;
                    v.target[n++] = ops[o];
                    if (form < 8)
                        v.target[n++] = MODRM_RR(dst, form);
                    else if (form == 8) {
                        v.target[n++] = 0x03 | (dst << 3);
                        v.fix_input   = fix_mem_ebx;
                        v.arg         = 8;
                    } else if (form == 9) {
                        v.target[n++] = 0x43 | (dst << 3); /* [ebx+31], misaligned */
                        v.target[n++] = 0x1f;
                        v.fix_input   = fix_mem_ebx;
                        v.arg         = 8;
                    } else {
                        v.target[n++] = 0x01 | (dst << 3);
                        v.fix_input   = fix_mem16;
                    }
                    /* The 16-bit form reads DI; with DI as the destination
                       the address would come from a random input. */
                    if (form == 10 && dst == R_EDI)
                        continue;
                    v.target_len = n;
                    v.fx         = FX_NONE;
                    /* SP as a word source; the low buffer's address */
                    v.no_ref     = (form == R_ESP && (ops[o] & 1)) || form == 10;
                    run_some_contexts(&v, 2);
                }
            }
        }
    }
    group_end(name);
}

/* BT, BTS, BTR, BTC: register destinations with a register or immediate
   bit offset, and memory destinations with either. */
static void
group_bt(void)
{
    static const uint8_t rr_ops[4] = { 0xa3, 0xab, 0xb3, 0xbb };
    static const uint8_t imms[11]  = { 0, 1, 7, 8, 15, 16, 31, 32, 33, 63, 255 };
    const char          *name      = "bt";
    struct variant       v         = { .group = name };
    group_begin(name);

    for (int o = 0; o < 4; o++) {
        for (int size16 = 0; size16 < 2; size16++) {
            /* bt r, r */
            for (int dst = 0; dst < 8; dst++) {
                if (dst == R_ESP)
                    continue;
                for (int src = 0; src < 8; src++) {
                    int n = 0;
                    if (size16)
                        v.target[n++] = 0x66;
                    v.target[n++] = 0x0f;
                    v.target[n++] = rr_ops[o];
                    v.target[n++] = MODRM_RR(src, dst);
                    v.target_len  = n;
                    v.fix_input   = 0;
                    v.fx          = FX_BT;
                    v.no_ref      = src == R_ESP;
                    run_some_contexts(&v, 2);
                }
            }
            /* bt r, imm8 */
            for (int dst = 0; dst < 8; dst++) {
                if (dst == R_ESP)
                    continue;
                for (unsigned i = 0; i < sizeof(imms); i++) {
                    int n = 0;
                    if (size16)
                        v.target[n++] = 0x66;
                    v.target[n++] = 0x0f;
                    v.target[n++] = 0xba;
                    v.target[n++] = MODRM_RR(4 + o, dst);
                    v.target[n++] = imms[i];
                    v.target_len  = n;
                    v.fix_input   = 0;
                    v.fx          = FX_BT;
                    v.no_ref      = 0;
                    v.producer    = 0;
                    v.boundary    = 0;
                    run_inputs(&v, 1);
                    v.producer = 3;
                    v.boundary = 1;
                    run_inputs(&v, 1);
                }
            }
            /* bt [ebx], imm8 */
            for (unsigned i = 0; i < sizeof(imms); i++) {
                int n = 0;
                if (size16)
                    v.target[n++] = 0x66;
                v.target[n++] = 0x0f;
                v.target[n++] = 0xba;
                v.target[n++] = 0x03 | ((4 + o) << 3);
                v.target[n++] = imms[i];
                v.target_len  = n;
                v.fix_input   = fix_mem_ebx;
                v.arg         = 16;
                v.fx          = FX_BT;
                v.no_ref      = 0;
                run_some_contexts(&v, 2);
            }
            /* bt [ebx], edx: the bit string reaches beyond the operand */
            {
                int n = 0;
                if (size16)
                    v.target[n++] = 0x66;
                v.target[n++] = 0x0f;
                v.target[n++] = rr_ops[o];
                v.target[n++] = 0x03 | (R_EDX << 3);
                v.target_len  = n;
                v.fix_input   = fix_bt_mem_reg;
                v.arg         = size16;
                v.fx          = FX_BT;
                v.no_ref      = 0;
                run_some_contexts(&v, 16);
            }
        }
    }
    group_end(name);
}

#include "groups_ops.inc"
#include "groups_mmx.inc"
#include "groups_p6.inc"
#include "groups_more.inc"

static void (*const groups[])(void) = {
    group_alu, group_setcc, group_lahf_sahf, group_bswap, group_movx, group_bt,
    /* Everything below was added after the first six were recorded. */
    group_imul, group_muldiv, group_shift, group_shd, group_bs, group_cmpxchg,
    group_bcd, group_string, group_popf, group_stack, group_far, group_int,
    group_bound_arpl, group_misc, group_mmx, group_mmx_x87, group_3dnow,
    group_cmov, group_rdtsc, group_nop_p6, group_smc,
    group_segments,
};

/* Each group twice: counted first, so the status line knows how many are
   left, then run. */
static void
run_groups(void)
{
    g_group_count = sizeof(groups) / sizeof(groups[0]);
    for (int i = 0; i < g_group_count; i++) {
        g_group_no = i + 1;
        g_counting = 1;
        groups[i]();
        g_counting = 0;
        groups[i]();
    }
}

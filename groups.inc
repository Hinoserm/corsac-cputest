/*
 * The test groups. Included into harness.c.
 *
 * Each group builds variants (the bytes under test, what runs before them,
 * whether they start a block) and runs each with a few inputs. The order of
 * groups and the variants inside a group never change once a result has
 * been recorded: the CRCs depend on them. Add new groups at the end.
 *
 * How much each variant runs:
 * - Inputs: where an instruction's edge cases are known (a count of 0, a
 *   divisor of 0, equal or not), the first inputs of a variant are those
 *   cases, one each, in order (see edge()); then one or two random ones
 *   for what nobody listed. Where none are known, random inputs, biased
 *   toward edge values (rnd_operand()).
 * - Registers: a few representative pairs (different, the same register
 *   twice, a high byte, memory), not every combination.
 * - The flag state before: every producer, with and without a block
 *   boundary, only for instructions that read the flags. The rest run
 *   once plain and once after ADD across a block boundary.
 */

/* Register-to-register ModRM. */
#define MODRM_RR(reg, rm) (0xc0 | ((reg) << 3) | (rm))

/* A byte sequence and its length, counted by the compiler:
   { SEQ(0x0f, 0xa2) } gives len = 2, b = { 0x0f, 0xa2 }. */
#define SEQ(...) sizeof((const uint8_t[]) { __VA_ARGS__ }), { __VA_ARGS__ }

/* The index of the input being built (0, 1, ...). */
static int g_case;

/* Fixers call this with the number of edge cases they know: it returns
   which one this input is, or -1 once they have all been run. */
static int
edge(int n)
{
    return g_case < n ? g_case : -1;
}

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

static void
run_inputs(struct variant *v, int inputs)
{
    for (int i = 0; i < inputs; i++) {
        g_case = i;
        run_variant(v);
    }
}

/* Every producer, both with and without a block boundary: for
   instructions that read the flags. */
static void
run_flags(struct variant *v, int inputs)
{
    for (unsigned p = 0; p < N_PRODUCERS; p++) {
        for (int b = 0; b < 2; b++) {
            v->producer = p;
            v->boundary = b;
            run_inputs(v, inputs);
        }
    }
}

/* No producer, then ADD across a block boundary. */
static void
run_plain(struct variant *v, int inputs)
{
    v->producer = 0;
    v->boundary = 0;
    run_inputs(v, inputs);
    v->producer = 3;
    v->boundary = 1;
    run_inputs(v, inputs);
}

/* The same with CMP, which leaves ECX and EDX alone: for tests whose input
   (a count in CL, an operand chosen by the fixer) is in one of them. */
static void
run_keep(struct variant *v, int inputs)
{
    v->producer = 0;
    v->boundary = 0;
    run_inputs(v, inputs);
    v->producer = 1;
    v->boundary = 1;
    run_inputs(v, inputs);
}

/* Representative register pairs (reg, rm): different registers, the same
   one twice, and the rest of the file. For bytes, high halves too. */
static const uint8_t pairs32[4][2] = { { R_EAX, R_ECX }, { R_EDX, R_EDX }, { R_EBX, R_ESI }, { R_EDI, R_EBP } };
static const uint8_t pairs8[4][2]  = { { 0, 1 }, { 4, 7 }, { 2, 2 }, { 5, 3 } }; /* AL,CL AH,BH DL,DL CH,BL */

/* ALU r/m,r and r,r/m, register forms, 8/16/32-bit. The flag math itself
   is covered exhaustively in exhaust8; this covers the encodings and
   registers. ADC and SBB read CF: every producer, on two pairs. */
static void
group_alu(void)
{
    const char     *name = "alu.rr";
    struct variant  v    = { .group = name, .fx = FX_ALU };
    group_begin(name);
    for (int op = 0; op < 8; op++) {
        int reads_cf = op == 2 || op == 3;
        for (int dir = 0; dir < 2; dir++) {
            for (int size = 8; size <= 32; size *= 2) {
                for (int p = 0; p < (reads_cf ? 2 : 4); p++) {
                    const uint8_t *pr = size == 8 ? pairs8[p] : pairs32[p];
                    int            n  = 0;
                    if (size == 16)
                        v.target[n++] = 0x66;
                    v.target[n++] = (op << 3) | (dir << 1) | (size != 8);
                    v.target[n++] = MODRM_RR(pr[0], pr[1]);
                    v.target_len  = n;
                    v.fx_arg      = op;
                    if (reads_cf)
                        run_flags(&v, 2);
                    else
                        run_plain(&v, 4);
                }
            }
        }
    }
    group_end(name);
}

/* SETcc: all sixteen conditions into a low and a high byte register and two
   memory forms, after every flag producer. */
static void
group_setcc(void)
{
    const char    *name = "setcc";
    struct variant v    = { .group = name, .fx = FX_SETCC };
    group_begin(name);
    for (int cc = 0; cc < 16; cc++) {
        for (int form = 0; form < 4; form++) { /* AL, AH, [ebx], [bx+di] */
            int n       = 0;
            v.fix_input = 0;
            v.no_ref    = 0;
            if (form == 3)
                v.target[n++] = 0x67;
            v.target[n++] = 0x0f;
            v.target[n++] = 0x90 | cc;
            if (form < 2)
                v.target[n++] = MODRM_RR(0, form ? 4 : 0);
            else if (form == 2) {
                v.target[n++] = 0x03; /* [ebx] */
                v.fix_input   = fix_mem_ebx;
                v.arg         = 8;
            } else {
                v.target[n++] = 0x01; /* [bx+di] */
                v.fix_input   = fix_mem16;
                v.no_ref      = 1;
            }
            v.target_len = n;
            v.fx_arg     = cc;
            run_flags(&v, 2);
        }
    }
    group_end(name);
}

/* LAHF and SAHF, alone and together, after every flag producer. SAHF over
   every AH is in exhaust8. */
static void
group_lahf_sahf(void)
{
    static const struct {
        uint8_t len, b[2];
    } seqs[] = { { SEQ(0x9f) }, { SEQ(0x9e) }, { SEQ(0x9e, 0x9f) }, { SEQ(0x9f, 0x9e) } };
    const char    *name = "lahf.sahf";
    struct variant v    = { .group = name, .fx = FX_LAHF_SAHF };
    group_begin(name);
    for (int s = 0; s < 4; s++) {
        memcpy(v.target, seqs[s].b, seqs[s].len);
        v.target_len = seqs[s].len;
        run_flags(&v, 2);
    }
    group_end(name);
}

/* BSWAP on every register but ESP (the register is in the opcode), with and
   without an operand-size prefix (undefined; the interpreter's answer is
   the reference). A 386-class CPU (RapidCAD) must fault with #UD. */
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
            v.fx          = pfx ? FX_UNDEF : FX_NONE;
            run_plain(&v, 2);
        }
    }
    group_end(name);
}

/* MOVZX and MOVSX, byte and word sources, 32- and 16-bit destinations: from
   a register, a high byte (word: another register), the destination
   itself, memory, misaligned memory, and 16-bit addressing. */
static void
group_movx(void)
{
    static const uint8_t ops[4] = { 0xb6, 0xb7, 0xbe, 0xbf };
    /* destination, source (0-7, or 8 [ebx], 9 [ebx+31], 10 [bx+di]) */
    static const uint8_t forms[6][2] = { { R_EAX, 1 }, { R_EDX, 4 }, { R_EBX, 3 }, { R_ESI, 8 }, { R_EDI, 9 }, { R_EBP, 10 } };
    const char          *name        = "movzx.movsx";
    struct variant       v           = { .group = name };
    group_begin(name);
    for (int o = 0; o < 4; o++) {
        for (int size16 = 0; size16 < 2; size16++) {
            for (int f = 0; f < 6; f++) {
                int dst = forms[f][0], src = forms[f][1], n = 0;
                if (src == 4 && (ops[o] & 1))
                    src = R_ESI; /* a word from SP would depend on the machine */
                v.fix_input = 0;
                v.no_ref    = 0;
                if (src == 10)
                    v.target[n++] = 0x67;
                if (size16)
                    v.target[n++] = 0x66;
                v.target[n++] = 0x0f;
                v.target[n++] = ops[o];
                if (src < 8)
                    v.target[n++] = MODRM_RR(dst, src);
                else if (src == 8) {
                    v.target[n++] = 0x03 | (dst << 3);
                    v.fix_input   = fix_mem_ebx;
                    v.arg         = 8;
                } else if (src == 9) {
                    v.target[n++] = 0x43 | (dst << 3); /* [ebx+31] */
                    v.target[n++] = 0x1f;
                    v.fix_input   = fix_mem_ebx;
                    v.arg         = 8;
                } else {
                    v.target[n++] = 0x01 | (dst << 3);
                    v.fix_input   = fix_mem16;
                    v.no_ref      = 1;
                }
                v.target_len = n;
                run_plain(&v, 4);
            }
        }
    }
    group_end(name);
}

/* BT with a register bit offset: the known edges of the offset for the
   operand size, then random ones. arg: the size; arg2: the offset
   register. */
static void
fix_bt_rr(const struct variant *v)
{
    int      size    = v->arg;
    uint32_t edges[] = { 0, 1, size - 1, size, size + 1, 31, 32, 0xffffffffu, 0x80000000u, 0x7fffffffu };
    int      e       = edge(sizeof(edges) / sizeof(edges[0]));
    uint32_t off     = e >= 0 ? edges[e] : rnd();
    uint32_t *r      = in_reg(v->arg2);
    *r               = size == 16 ? (*r & 0xffff0000u) | (off & 0xffff) : off;
}

/* BT into memory with a register offset: the bit string reaches up to 32
   bytes either side of [ebx], which stays inside the buffer. The edges
   are the byte and word boundaries either side of zero. */
static void
fix_bt_mem_reg(const struct variant *v)
{
    static const int16_t edges[] = { 0, 1, 7, 8, 15, 16, 31, 32, -1, -8, -9, -16, -17, -32, -33, 255, -256 };
    int                  e       = edge(sizeof(edges) / sizeof(edges[0]));
    int32_t              off     = e >= 0 ? edges[e] : (int32_t) (rnd() % 512) - 256;
    g_in.ebx                     = (uint32_t) g_buf + 32;
    if (v->arg)
        g_in.edx = (rnd() & 0xffff0000) | (uint16_t) (int16_t) off;
    else
        g_in.edx = (uint32_t) off;
}

/* BT, BTS, BTR, BTC: register destinations with a register or immediate
   bit offset, and memory destinations with either. */
static void
group_bt(void)
{
    static const uint8_t rr_ops[4]    = { 0xa3, 0xab, 0xb3, 0xbb };
    static const uint8_t imms[11]     = { 0, 1, 7, 8, 15, 16, 31, 32, 33, 63, 255 };
    static const uint8_t rr_pairs[3][2] = { { R_ECX, R_EAX }, { R_EDX, R_EDX }, { R_EBP, R_ESI } }; /* offset, destination */
    const char          *name         = "bt";
    struct variant       v            = { .group = name, .fx = FX_BT };
    group_begin(name);

    for (int o = 0; o < 4; o++) {
        for (int size16 = 0; size16 < 2; size16++) {
            int size = size16 ? 16 : 32;
            /* bt r, r: the offset register's edges, then two random */
            for (int p = 0; p < 3; p++) {
                int n = 0;
                if (size16)
                    v.target[n++] = 0x66;
                v.target[n++] = 0x0f;
                v.target[n++] = rr_ops[o];
                v.target[n++] = MODRM_RR(rr_pairs[p][0], rr_pairs[p][1]);
                v.target_len  = n;
                v.fix_input   = fix_bt_rr;
                v.reg_fix     = 1;
                v.arg         = size;
                v.arg2        = rr_pairs[p][0];
                v.norm        = 0;
                run_keep(&v, 12);
            }
            /* bt r, imm8 */
            for (int dst = 0; dst < 8; dst += 7) { /* EAX, EDI */
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
                    v.reg_fix     = 0;
                    run_plain(&v, 1);
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
                run_plain(&v, 1);
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
                run_keep(&v, 19);
            }
        }
    }
    group_end(name);
}

#include "groups_ops.inc"
#include "groups_mmx.inc"
#include "groups_p6.inc"
#include "groups_more.inc"
#include "groups_exhaust.inc"
#include "groups_mem.inc"
#include "groups_code16.inc"
#include "groups_loop.inc"
#include "groups_map.inc"
#include "groups_cpu.inc"
#include "groups_paging.inc"
#include "groups_ring.inc"
#include "groups_ring2.inc"
#include "groups_debug.inc"
#include "groups_sys.inc"
#include "groups_desc.inc"
#include "groups_prot.inc"

/* The groups in order. A group is a function, or one of a family of
   parametrised ones (the sampled loops) with its index. */
struct group_def {
    void (*fn)(void);
    void (*fn_arg)(unsigned);
    unsigned arg;
};
#define G(f)       { f, 0, 0 }
#define GA(f, a)   { 0, f, a }

static const struct group_def groups[] = {
    G(group_alu), G(group_setcc), G(group_lahf_sahf), G(group_bswap), G(group_movx), G(group_bt),
    G(group_imul), G(group_muldiv), G(group_shift), G(group_shd), G(group_bs), G(group_cmpxchg),
    G(group_bcd), G(group_string), G(group_popf), G(group_stack), G(group_far), G(group_int),
    G(group_bound_arpl), G(group_misc), G(group_mmx), G(group_mmx_x87), G(group_3dnow),
    G(group_cmov), G(group_rdtsc), G(group_nop_p6), G(group_smc),
    G(group_segments), G(group_exhaust8), G(group_lea),
    G(group_alu_mem), G(group_unary_mem), G(group_jcc_loop),
    G(group_mov_forms), G(group_prefixes), G(group_code16),
    LOOP_GROUPS
    GA(group_opmap, 0),
    GA(group_opmap, 1),
    GA(group_opmap, 2),
    GA(group_opmap, 3),
    GA(group_opmap, 4),
    GA(group_opmap, 5),
    GA(group_opmap, 6),
    GA(group_opmap, 7),
    GA(group_opmap, 8),
    GA(group_opmap, 9),
    GA(group_opmap, 10),
    GA(group_opmap, 11),
    GA(group_opmap, 12),
    GA(group_opmap, 13),
    GA(group_opmap, 14),
    GA(group_opmap, 15),
    G(group_aliases),
    G(group_cpuid),
    G(group_msr_map),
    G(group_tsc_write),
    G(group_cyrix),
    G(group_pg_basic),
    G(group_pg_notpresent),
    G(group_pg_readonly_wp0),
    G(group_pg_readonly_wp1),
    G(group_pg_cross),
    G(group_pg_ad),
    G(group_pg_tlb),
    G(group_pg_pse),
    G(group_pg_string),
    G(group_pg_exec),
    G(group_pg_smc_alias),
    G(group_r3_basic),
    G(group_r3_priv),
    G(group_r3_io),
    G(group_r3_popf),
    G(group_r3_ac),
    G(group_r3_seg),
    G(group_r3_gates),
    G(group_r3_int),
    G(group_r3_tsd),
    G(group_r3_pf),
    G(group_r1_basic),
    G(group_r1_priv),
    G(group_r1_iopl1),
    G(group_r1_io),
    G(group_r1_popf),
    G(group_r1_seg),
    G(group_r1_gates),
    G(group_r1_int),
    G(group_r1_ac),
    G(group_r1_pf),
    G(group_r1_pf_wp),
    G(group_r1_outer),
    G(group_r1_stack),
    G(group_r1_lar),
    G(group_r2_basic),
    G(group_r2_gates),
    G(group_v86_basic),
    G(group_v86_iopl0),
    G(group_v86_io),
    G(group_v86_ud),
    G(group_v86_vme),
    G(group_r3_limits),
    G(group_r2_limits),
    G(group_r1_limits),
    G(group_r3_ss),
    G(group_r1_ss),
    G(group_conforming),
    G(group_intgate),
    G(group_r1_inner),
    G(group_r3_code16),
    G(group_r1_code16),
    G(group_r3_popseg),
    G(group_r1_popseg),
    G(group_gateparams),
    G(group_tf_step),
    G(group_tf_shadow),
    G(group_tf_rep),
    G(group_dr_exec),
    G(group_dr_data),
    G(group_dr_gd),
    G(group_dr_alias),
    G(group_dr_io),
    G(group_dr_bits),
    G(group_cr0_write),
    G(group_cr4_bits),
    G(group_cr0_ts),
    G(group_lmsw),
    G(group_cr23),
    G(group_tsc_msr),
    G(group_desc_accessed),
    G(group_ldt_load),
    G(group_ldt_seg),
    G(group_ldt_gate),
    G(group_task_call),
    G(group_task_jmp),
    G(group_task_errors),
    G(group_lock_legal),
    G(group_segovr),
    G(group_lds_forms),
    G(group_stack16),
    G(group_string_overlap),
    G(group_string_long),
    G(group_sreg_upper),
};

/* Each group twice: counted first, so the status line knows how many are
   left, then run. */
static void
run_groups(void)
{
    g_group_count = sizeof(groups) / sizeof(groups[0]);
    int first     = 0;
#ifdef HOSTTEST
    if (getenv("CPUTEST_FROM")) /* start at this group number */
        first = atoi(getenv("CPUTEST_FROM")) - 1;
#endif
    for (int i = first; i < g_group_count; i++) {
        g_group_no = i + 1;
        for (g_counting = 1; g_counting >= 0; g_counting--) {
            if (groups[i].fn)
                groups[i].fn();
            else
                groups[i].fn_arg(groups[i].arg);
        }
        g_counting = 0;
    }
}

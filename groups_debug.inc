/*
 * Debugging: single-step traps (TF), the MOV SS shadow, REP string
 * stepping, the debug registers' instruction, data and I/O breakpoints,
 * general detect, the DR4/DR5 aliases, and what DR6 and DR7 read back.
 * DOS extenders and debuggers depend on all of it (86Box issue #2561:
 * DR6.BS missing after a TF trap broke HX's debugger). Each variant runs
 * with its breakpoints loaded just before and cleared just after, and
 * its result includes DR6 (in fault_err). Included into groups.inc after
 * groups_ring2.inc.
 */

#define CPUID_DE   (1u << 2)
#define CR4_DE     (1u << 3)
#define DR7_GD     (1u << 13)

/* The breakpoints a variant wants: set by the group, or by an emitter
   that knows the address of the code it wants to stop at. */
static uint32_t g_dr[4], g_dr7, g_dbg_cr4;

static void
dbg_before(const struct variant *v)
{
#ifndef HOSTTEST
    if (g_dbg_cr4) {
        uint32_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 | g_dbg_cr4));
    }
    __asm__ volatile("mov %0, %%dr6" : : "r"(0));
    __asm__ volatile("mov %0, %%dr0" : : "r"(g_dr[0]));
    __asm__ volatile("mov %0, %%dr1" : : "r"(g_dr[1]));
    __asm__ volatile("mov %0, %%dr2" : : "r"(g_dr[2]));
    __asm__ volatile("mov %0, %%dr3" : : "r"(g_dr[3]));
    __asm__ volatile("mov %0, %%dr7" : : "r"(g_dr7));
#endif
}

/* After: breakpoints off before the harness reads anything (DR6 stays for
   the capture; the next run clears it). */
static void
dbg_after(const struct variant *v)
{
#ifndef HOSTTEST
    __asm__ volatile("mov %0, %%dr7" : : "r"(0));
    if (g_dbg_cr4) {
        uint32_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 & ~g_dbg_cr4));
    }
#endif
}

static void
dbg_clear(void)
{
    g_dr[0] = g_dr[1] = g_dr[2] = g_dr[3] = 0;
    g_dr7 = g_dbg_cr4 = 0;
}

/* A variant for these groups, at CPL 0. */
static struct variant
dbg_variant(const char *name)
{
    /* flags_mask set (to the usual mask) so the epilogue ends with PUSH 2;
       POPFD: a TF the CPU didn't trap on can't leak into the harness. */
    struct variant v = { .group = name, .faults_ok = 1, .debug = 1, .fx = FX_CUSTOM, .defmask = dm_fxarg,
                         .fx_arg = 1, .before_run = dbg_before, .after_run = dbg_after, .flags_mask = 0x0cd5 };
    return v;
}

/* The debug groups are for bare metal: a Linux process can't load debug
   registers. */
static int
dbg_begin(const char *name)
{
#ifdef HOSTTEST
    group_skip(name, "debug registers (host build)");
    return 0;
#else
    group_begin(name);
    dbg_clear();
    return 1;
#endif
}

/* pushfd; or dword [esp], 100h; popfd: TF on. The trap comes after the
   instruction after the POPFD. */
#define SET_TF 0x9c, 0x81, 0x0c, 0x24, 0x00, 0x01, 0x00, 0x00, 0x9d

/* ---- TF: after which instruction the trap comes, and DR6.BS ---------- */

/* The P6's DEBUGCTL (1D9h, arg3): with BTF (bit 1) the single-step trap
   waits for a taken branch, so it comes after the JMP, the CALL or the
   taken JZ and never for straight-line code or a branch not taken; with
   LBR (bit 0) LastBranchFromIP and LastBranchToIP (1DBh, 1DCh) hold the
   last JMP's own address and its target (ESI and EDI, made relative to
   them; 0 when right). DEBUGCTL is cleared after each run. */
static void
btf_before(const struct variant *v)
{
    dbg_before(v);
#ifndef HOSTTEST
    wrmsr_(0x1d9, v->arg3, 0);
#endif
}

static void
btf_after(const struct variant *v)
{
#ifndef HOSTTEST
    wrmsr_(0x1d9, 0, 0);
#endif
    dbg_after(v);
}

static void
emit_lbr(struct emit *e, const struct variant *v)
{
    uint32_t from = (uint32_t) e->p;
    e8(e, 0xeb); /* jmp $+2 */
    e8(e, 0x00);
    uint32_t to = (uint32_t) e->p;
    for (int k = 0; k < 2; k++) {
        e8(e, 0xb9); /* mov ecx, 1DBh or 1DCh */
        e32(e, 0x1db + k);
        e8(e, 0x0f); /* rdmsr */
        e8(e, 0x32);
        e8(e, 0x2d); /* sub eax, the address */
        e32(e, k ? to : from);
        e8(e, 0x89); /* mov esi or edi, eax */
        e8(e, k ? 0xc7 : 0xc6);
    }
    static const uint8_t clear[] = { 0x31, 0xc0, 0x31, 0xd2, 0x31, 0xc9 }; /* EAX EDX ECX: MSR values */
    ebytes(e, clear, sizeof(clear));
}

static void
tf_branches(const char *name)
{
    static const struct {
        uint8_t len, b[24];
    } seqs[] = {
        { SEQ(SET_TF, 0x01, 0xd1, 0x01, 0xd1, 0xeb, 0x00, 0x90) },       /* add; add; jmp: the trap after the JMP */
        { SEQ(SET_TF, 0x31, 0xc0, 0x75, 0x00, 0x01, 0xd1, 0x74, 0x00, 0x90) }, /* jnz not taken, jz taken */
        { SEQ(SET_TF, 0xe8, 0x00, 0x00, 0x00, 0x00, 0x5e, 0x31, 0xf6) }, /* call: at the target; pop esi; xor esi, esi */
        { SEQ(SET_TF, 0x01, 0xd1, 0x90) },                             /* no branch: no trap */
    };
    struct variant v = dbg_variant(name);
    if (!is_vendor("GenuineIntel") || ((g_cpu.signature >> 8) & 0xf) != 6)
        return;
    v.before_run = btf_before;
    v.after_run  = btf_after;
    v.arg3       = 2; /* BTF */
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        run_plain(&v, 1);
    }
    v.arg3       = 1; /* LBR */
    v.emit       = emit_lbr;
    v.target[0]  = 0xeb; /* for the reports */
    v.target[1]  = 0x00;
    v.target_len = 2;
    run_plain(&v, 1);
}

static void
group_tf_step(void)
{
    static const struct {
        uint8_t fix, len, b[24];
    } seqs[] = {
        { 0, SEQ(SET_TF, 0x01, 0xd1) },                         /* add ecx, edx */
        { 0, SEQ(SET_TF, 0x90) },                               /* nop */
        { 0, SEQ(SET_TF, 0xeb, 0x00) },                         /* jmp $+2 */
        { 0, SEQ(SET_TF, 0x74, 0x00) },                         /* jz $+2, either way */
        { 0, SEQ(SET_TF, 0xe8, 0x00, 0x00, 0x00, 0x00) },       /* call $+5: the trap at the target */
        { 1, SEQ(SET_TF, 0x8b, 0x03) },                         /* mov eax, [ebx] */
        { 1, SEQ(SET_TF, 0x89, 0x0b) },                         /* mov [ebx], ecx */
        { 0, SEQ(SET_TF, 0x0f, 0xa2) },                         /* cpuid (serialising) */
        { 0, SEQ(SET_TF, 0xfa) },                               /* cli */
        { 0, SEQ(0x6a, 0x02, SET_TF, 0x9d) },                   /* popfd clearing TF: the trap still comes */
        { 0, SEQ(SET_TF, 0x9c, 0x5e) },                         /* pushfd: does the image hold TF? */
        { 0, SEQ(SET_TF, 0xcc) },                               /* int3: #BP, not the step */
        { 0, SEQ(SET_TF, 0xf1) },                               /* icebp: #DB, BS or not */
        { 0, SEQ(SET_TF, 0x0f, 0x0b) },                         /* ud2: #UD first */
        { 0, SEQ(SET_TF, 0xe2, 0x00) },                         /* loop $+2 */
        { 0, SEQ(SET_TF, 0x66, 0x90) },                         /* a prefixed nop */
    };
    const char    *name = "tf.step";
    struct variant v    = dbg_variant(name);
    if (!dbg_begin(name))
        return;
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.fix_input  = seqs[i].fix ? fix_mem_ebx : 0;
        v.arg        = MEM_OFF;
        run_plain(&v, 1);
    }
    tf_branches(name);
    group_end(name);
}

/* MOV SS and POP SS hold off the single-step trap (and interrupts) for
   one instruction; STI does not hold off the trap. */
static void
group_tf_shadow(void)
{
    static const struct {
        uint8_t len, b[24];
    } seqs[] = {
        { SEQ(0x66, 0xb8, 0x10, 0x00, SET_TF, 0x8e, 0xd0, 0x01, 0xd1, 0x90) }, /* mov ss, ax; add; nop */
        { SEQ(0x16, SET_TF, 0x17, 0x01, 0xd1, 0x90) },                         /* push ss; ...; pop ss; add; nop */
        { SEQ(SET_TF, 0xfb, 0x01, 0xd1) },                                     /* sti; add: no shadow for TF */
        { SEQ(0x66, 0xb8, 0x10, 0x00, SET_TF, 0x8e, 0xd8, 0x01, 0xd1) },       /* mov ds, ax: no shadow */
        { SEQ(0x66, 0xb8, 0x10, 0x00, SET_TF, 0x8e, 0xd0, 0x8e, 0xd0, 0x90) }, /* mov ss twice in a row */
    };
    const char    *name = "tf.shadow";
    struct variant v    = dbg_variant(name);
    if (!dbg_begin(name))
        return;
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        run_plain(&v, 1);
    }
    group_end(name);
}

/* REP string instructions under TF trap after every iteration: the
   registers at the trap show how far one step went. A count of 0 runs
   no iteration and traps after the instruction. */
static void
fix_tfrep(const struct variant *v)
{
    static const uint32_t counts[] = { 5, 1, 0 };
    int                   e        = edge(3);
    g_in.esi                       = (uint32_t) g_buf;
    g_in.edi                       = (uint32_t) g_buf + 32;
    g_in.ecx                       = e >= 0 ? counts[e] : 2 + (rnd() & 3);
    if (rnd() & 1)
        memcpy(g_in.buf + 32, g_in.buf, 8); /* CMPS runs on while equal */
}

static void
group_tf_rep(void)
{
    static const struct {
        uint8_t len, b[16];
    } seqs[] = {
        { SEQ(SET_TF, 0xf3, 0xa4) }, /* rep movsb */
        { SEQ(SET_TF, 0xf3, 0xa5) }, /* rep movsd */
        { SEQ(SET_TF, 0xf3, 0xaa) }, /* rep stosb */
        { SEQ(SET_TF, 0xf3, 0xa6) }, /* repe cmpsb */
        { SEQ(SET_TF, 0xf2, 0xae) }, /* repne scasb */
        { SEQ(SET_TF, 0xf3, 0xac) }, /* rep lodsb */
    };
    const char    *name = "tf.rep";
    struct variant v    = dbg_variant(name);
    if (!dbg_begin(name))
        return;
    v.fix_input = fix_tfrep;
    v.norm      = (1 << R_ESI) | (1 << R_EDI);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        run_keep(&v, 4);
    }
    group_end(name);
}

/* ---- instruction breakpoints ------------------------------------------ */

/* Three instructions; a breakpoint on the one arg2 names (0-2), in slot
   arg3 & 3, enabled locally (bit 0x10 of arg3: globally instead; 0x20:
   not enabled at all, so only DR6 may show it). The second instruction
   is prefixed: arg2 = 3 puts the breakpoint on its opcode byte, past
   the prefix, where it must not fire. */
static void
emit_brk(struct emit *e, const struct variant *v)
{
    uint8_t *at[4];
    at[0] = e->p;
    e8(e, 0x01); /* add ecx, edx */
    e8(e, 0xd1);
    at[1] = e->p;
    e8(e, 0x66); /* add cx, dx */
    at[3] = e->p;
    e8(e, 0x01);
    e8(e, 0xd1);
    at[2] = e->p;
    e8(e, 0x90); /* nop */
    int slot = v->arg3 & 3;
    dbg_clear();
    g_dr[slot] = (uint32_t) at[v->arg2];
    if (!(v->arg3 & 0x20))
        g_dr7 = 1u << (slot * 2 + ((v->arg3 & 0x10) ? 1 : 0)); /* L or G, RW 00, LEN 00 */
}

static void
group_dr_exec(void)
{
    const char    *name = "dr.exec";
    struct variant v    = dbg_variant(name);
    if (!dbg_begin(name))
        return;
    v.emit = emit_brk;
    for (int at = 0; at < 4; at++) {
        for (int slot = 0; slot < 4; slot++) {
            v.target[0]  = 0x01; /* for the reports */
            v.target[1]  = at;
            v.target[2]  = slot;
            v.target_len = 3;
            v.arg2       = at;
            v.arg3       = slot;
            run_plain(&v, 1);
        }
    }
    for (int k = 0; k < 2; k++) { /* global enable; not enabled */
        v.target[0]  = 0x01;
        v.target[1]  = 1;
        v.target[2]  = k ? 0x20 : 0x10;
        v.target_len = 3;
        v.arg2       = 1;
        v.arg3       = k ? 0x20 : 0x10;
        v.fx_arg     = k == 0; /* a disabled breakpoint's DR6 bit is up to the model */
        run_plain(&v, 1);
    }
    group_end(name);
}

/* ---- data breakpoints --------------------------------------------------- */

/* A breakpoint on the buffer (at MEM_OFF + 4, which [ebx] reaches as
   [ebx+4]) for writes or for reads and writes, 1, 2 or 4 bytes long, and
   accesses that hit it, straddle it or miss it by a byte. The trap comes
   after the instruction. */
static void
group_dr_data(void)
{
    static const struct {
        uint8_t len, b[4];
    } acc[] = {
        { SEQ(0x88, 0x4b, 0x04) },       /* mov [ebx+4], cl */
        { SEQ(0x88, 0x4b, 0x03) },       /* mov [ebx+3], cl: the byte before */
        { SEQ(0x89, 0x4b, 0x02) },       /* mov [ebx+2], ecx: over it */
        { SEQ(0x66, 0x89, 0x4b, 0x05) }, /* mov [ebx+5], cx */
        { SEQ(0x89, 0x4b, 0x08) },       /* mov [ebx+8], ecx: past a 4-byte one */
        { SEQ(0x8b, 0x43, 0x04) },       /* mov eax, [ebx+4]: a read */
        { SEQ(0x01, 0x4b, 0x04) },       /* add [ebx+4], ecx: read and write */
        { SEQ(0xff, 0x73, 0x04, 0x5e) }, /* push dword [ebx+4]; pop esi */
    };
    static const uint32_t kinds[] = { 0x1, 0x3 };        /* RW: write; read or write */
    static const uint32_t lens[]  = { 0x0, 0x1, 0x3 };   /* LEN: 1, 2, 4 bytes */
    const char           *name    = "dr.data";
    struct variant        v       = dbg_variant(name);
    if (!dbg_begin(name))
        return;
    v.fix_input = fix_mem_ebx;
    v.arg       = MEM_OFF;
    for (int k = 0; k < 2; k++) {
        for (int l = 0; l < 3; l++) {
            for (unsigned a = 0; a < sizeof(acc) / sizeof(acc[0]); a++) {
                memcpy(v.target, acc[a].b, acc[a].len);
                v.target_len = acc[a].len;
                dbg_clear();
                g_dr[1] = (uint32_t) g_buf + MEM_OFF + 4;
                g_dr7   = (1u << 2) | ((kinds[k] | (lens[l] << 2)) << 20); /* L1, RW1, LEN1 */
                run_plain(&v, 1);
            }
        }
    }
    group_end(name);
}

/* ---- general detect, DR4/DR5, I/O breakpoints, DR6/DR7 ------------------ */

/* DR7.GD: the next MOV to or from a debug register faults (#DB, DR6.BD)
   and GD clears, so the harness can reach the registers again. */
static void
group_dr_gd(void)
{
    static const struct {
        uint8_t len, b[8];
    } seqs[] = {
        { SEQ(0x0f, 0x21, 0xf8) },       /* mov eax, dr7 */
        { SEQ(0x0f, 0x21, 0xf0) },       /* mov eax, dr6 */
        { SEQ(0x31, 0xc0, 0x0f, 0x23, 0xc0) }, /* xor eax, eax; mov dr0, eax */
        { SEQ(0x01, 0xd1, 0x0f, 0x21, 0xc8) }, /* add; mov eax, dr1 */
    };
    const char    *name = "dr.gd";
    struct variant v    = dbg_variant(name);
    if (!dbg_begin(name))
        return;
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        dbg_clear();
        g_dr7 = DR7_GD;
        run_plain(&v, 1);
    }
    group_end(name);
}

/* DR4 and DR5: DR6 and DR7 under other names while CR4.DE is clear, #UD
   once it is set (Pentium on). */
static void
group_dr_alias(void)
{
    static const struct {
        uint8_t de, len, b[12];
    } seqs[] = {
        { 0, SEQ(0x0f, 0x21, 0xe0) },                               /* mov eax, dr4 */
        { 0, SEQ(0x0f, 0x21, 0xe9) },                               /* mov ecx, dr5 */
        { 0, SEQ(0xb8, 0x0f, 0x00, 0x00, 0x00, 0x0f, 0x23, 0xe0, 0x0f, 0x21, 0xf1) }, /* mov dr4, 0Fh; mov ecx, dr6 */
        { 1, SEQ(0x0f, 0x21, 0xe0) },
        { 1, SEQ(0x0f, 0x21, 0xe9) },
        { 1, SEQ(0x0f, 0x23, 0xe0) },
    };
    const char    *name = "dr.alias";
    struct variant v    = dbg_variant(name);
    if (!(g_cpu.features & CPUID_DE)) {
        group_skip(name, "no CR4.DE");
        return;
    }
    if (!dbg_begin(name))
        return;
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        dbg_clear();
        g_dbg_cr4 = seqs[i].de ? CR4_DE : 0;
        run_plain(&v, 1);
    }
    group_end(name);
}

/* I/O breakpoints (CR4.DE, RW = 10): OUT to the port traps after the
   instruction; to the next port it doesn't. */
static void
group_dr_io(void)
{
    static const struct {
        uint8_t len, b[8];
    } seqs[] = {
        { SEQ(0xe6, 0x80) },                         /* out 80h, al */
        { SEQ(0x66, 0xba, 0x80, 0x00, 0xee) },       /* mov dx, 80h; out dx, al */
        { SEQ(0x66, 0xba, 0x80, 0x00, 0x89, 0xde, 0x6e) }, /* mov esi, ebx; outsb from the buffer */
        { SEQ(0xe6, 0xed) },                         /* out 0EDh, al: another port */
    };
    const char    *name = "dr.io";
    struct variant v    = dbg_variant(name);
    if (!(g_cpu.features & CPUID_DE)) {
        group_skip(name, "no CR4.DE");
        return;
    }
    if (!dbg_begin(name))
        return;
    for (int en = 0; en < 2; en++) {
        for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
            memcpy(v.target, seqs[i].b, seqs[i].len);
            v.target_len = seqs[i].len;
            v.fix_input  = fix_mem_ebx;
            v.arg        = MEM_OFF;
            dbg_clear();
            g_dbg_cr4 = en ? CR4_DE : 0; /* without DE, RW = 10 is undefined: raw */
            g_dr[0]   = 0x80;
            g_dr7     = 1u | (0x2u << 16);
            v.fx_arg  = en;
            run_plain(&v, 1);
        }
    }
    group_end(name);
}

/* What DR6 and DR7 read back after writing all zeros and all ones (the
   bits a model fixes), and DR0 as written. */
static void
group_dr_bits(void)
{
    static const struct {
        uint8_t len, b[16];
    } seqs[] = {
        { SEQ(0x31, 0xc0, 0x0f, 0x23, 0xf0, 0x0f, 0x21, 0xf1) },                   /* dr6 = 0; ecx = dr6 */
        { SEQ(0x31, 0xc0, 0xf7, 0xd0, 0x0f, 0x23, 0xf0, 0x0f, 0x21, 0xf1) },       /* dr6 = ~0 */
        { SEQ(0x31, 0xc0, 0x0f, 0x23, 0xf8, 0x0f, 0x21, 0xf9) },                   /* dr7 = 0; ecx = dr7 */
        { SEQ(0xb8, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x23, 0xc0, 0x0f, 0x21, 0xc1) }, /* dr0 round trip */
        { SEQ(0xb8, 0x78, 0x56, 0x34, 0x12, 0x0f, 0x23, 0xd8, 0x0f, 0x21, 0xd9) }, /* dr3 = 12345678h */
    };
    const char    *name = "dr.bits";
    struct variant v    = dbg_variant(name);
    if (!dbg_begin(name))
        return;
    v.fx_arg = 0; /* which bits are fixed is the model's own */
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        run_plain(&v, 1);
    }
    group_end(name);
}

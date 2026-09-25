/*
 * Protection and instruction corners: which instructions LOCK may
 * prefix, segment overrides, far-pointer loads, 16-bit stacks, string
 * copies that overlap or run long, what a segment register looks like in
 * a 32-bit register or on the stack, LAR/VERR/segment loads over every
 * descriptor type, IDT gates that must fail, the end of a 16-bit code
 * segment and operand-size prefixes inside one, SGDT and friends, the
 * IDT's limit, EFLAGS through IRET, XLAT. Included into groups.inc after
 * groups_desc.inc.
 */

/* ---- LOCK ------------------------------------------------------------------ */


/* ---- segment overrides -------------------------------------------------------- */


/* ---- far pointer loads ----------------------------------------------------------- */


/* ---- a 16-bit stack --------------------------------------------------------------- */


/* ---- string copies that overlap, and long ones --------------------------------------- */



/* ---- segment registers in 32-bit registers and on the stack ---------------------------- */


/* ---- every descriptor type --------------------------------------------------------------- */

#define SCRATCH_RAW(access, flags) (0x900 | ((uint32_t) (access) << 12) | ((uint32_t) (flags) << 20))

/* LAR, LSL and VERR of the scratch descriptor as each of the 16 system
   types (S = 0): which ones LAR and LSL accept. Raw (LAR's undefined
   bits). */
static void
group_lar_systypes(void)
{
    static const uint8_t code[] = {
        0x66, 0xb9, SEL_SCRATCH, 0x00, /* mov cx, sel */
        0x0f, 0x02, 0xc1,              /* lar eax, ecx */
        0x0f, 0x94, 0xc3,              /* setz bl */
        0x0f, 0x03, 0xd1,              /* lsl edx, ecx */
        0x0f, 0x94, 0xc7,              /* setz bh */
        0x0f, 0x00, 0xe1,              /* verr cx */
        0x0f, 0x94, 0xc5,              /* setz ch */
    };
    struct variant v = { .group = "lar.systypes", .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
                         .fix_input = fix_ring, .arg2 = RF_NONE, .fx = FX_CUSTOM, .defmask = dm_fxarg };
    if (!ring_begin(v.group, 0, 0))
        return;
    memcpy(v.target, code, sizeof(code));
    v.target_len = sizeof(code);
    for (int t = 0; t < 16; t++) {
        v.arg = SCRATCH_RAW(0x80 | t, 0x4);
        run_plain(&v, 1);
    }
    ring_end(v.group, 0, 0);
}

/* The 16 code and data types (S = 1), at DPL 0 from ring 0 and DPL 3
   from ring 3: VERR, VERW, LAR, and a load into FS, a read and a write
   through it. */
static void
group_seg_types(void)
{
    struct variant v = { .group = "seg.types", .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = ring_setup,
                         .fix_input = fix_ring, .fx = FX_CUSTOM, .defmask = dm_fxarg, .fx_arg = 1 };
    static const uint8_t checks[] = {
        0x66, 0xb9, 0x00, 0x00, /* mov cx, sel */
        0x0f, 0x00, 0xe1,       /* verr cx */
        0x0f, 0x94, 0xc3,       /* setz bl */
        0x0f, 0x00, 0xe9,       /* verw cx */
        0x0f, 0x94, 0xc7,       /* setz bh */
    };
    static const uint8_t use[] = {
        0x66, 0xb8, 0x00, 0x00, /* mov ax, sel; mov fs, ax */
        0x8e, 0xe0,
        0x64, 0x8b, 0x35, 0x10, 0x00, 0x00, 0x00, /* mov esi, fs:[10h] */
        0x64, 0x89, 0x0d, 0x20, 0x00, 0x00, 0x00, /* mov fs:[20h], ecx */
    };
    if (!ring_begin(v.group, 0, 0))
        return;
    for (int r = 0; r <= 3; r += 3) {
        for (int t = 0; t < 16; t++) {
            for (int k = 0; k < 2; k++) {
                const uint8_t *b = k ? use : checks;
                int            n = k ? (int) sizeof(use) : (int) sizeof(checks);
                memcpy(v.target, b, n);
                v.target[2]  = SEL_SCRATCH | r;
                v.target_len = n;
                v.ring       = r;
                v.arg        = SCRATCH_RAW(0x90 | (r << 5) | t, 0x4);
                v.arg2       = RF_SANDBOX;
                run_plain(&v, 1);
            }
        }
    }
    ring_end(v.group, 0, 0);
}

/* ---- IDT gates that must fail, and the IDT's limit ---------------------------------------- */

/* INT through a missing gate (#NP), a gate of an invalid type, one whose
   selector is data or null (#GP), and a 386 trap gate, from rings 0 and
   3. */
static void
group_idt_types(void)
{
    static struct ring_seq seqs[10];
    int                    n = 0;
    for (int r = 0; r <= 3; r += 3)
        for (int vec = 0x39; vec <= 0x3d; vec++) {
            struct ring_seq q = { r, 0, 0, 0, RF_NONE, 1, 2, { 0xcd, (uint8_t) vec } };
            seqs[n++]         = q;
        }
    ring_run("idt.types", seqs, n, 0, 0, 0);
}

/* The IDT's limit: an INT past it is #GP (with the IDT bit in the error
   code), and a gate only partly inside it counts as outside. Variants set
   the limit before the run (arg3: the limit) and put it back after. */
static void
idt_limit_before(const struct variant *v)
{
    ring_setup(v);
#ifndef HOSTTEST
    struct {
        uint16_t limit;
        uint32_t base;
    } __attribute__((packed)) idtr = { (uint16_t) v->arg3, (uint32_t) g_idt2 };
    __asm__ volatile("lidt %0" : : "m"(idtr));
#endif
}

static void
idt_limit_after(const struct variant *v)
{
#ifndef HOSTTEST
    struct {
        uint16_t limit;
        uint32_t base;
    } __attribute__((packed)) idtr = { sizeof(g_idt2) - 1, (uint32_t) g_idt2 };
    __asm__ volatile("lidt %0" : : "m"(idtr));
#endif
}

static void
group_idt_limit(void)
{
    static const struct {
        uint8_t  ring, vec;
        uint16_t limit;
    } cases[] = {
        { 0, 0x31, 0x31 * 8 - 1 }, /* 31h just past the limit */
        { 0, 0x30, 0x31 * 8 - 1 }, /* 30h the last one inside */
        { 0, 0x30, 0x30 * 8 + 3 }, /* 30h half inside: #GP */
        { 0, 0x03, 0x31 * 8 - 1 },
        { 3, 0x31, 0x31 * 8 - 1 }, /* from ring 3 */
        { 3, 0x30, 0x31 * 8 - 1 },
    };
    struct variant v = { .group = "idt.limit", .faults_ok = 1, .flags_mask = RING_FMASK, .before_run = idt_limit_before,
                         .after_run = idt_limit_after, .fix_input = fix_ring, .fx = FX_CUSTOM, .defmask = dm_fxarg,
                         .fx_arg = 1 };
    if (!ring_begin(v.group, 0, 0))
        return;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        v.target[0]  = 0xcd;
        v.target[1]  = cases[i].vec;
        v.target_len = 2;
        v.ring       = cases[i].ring;
        v.arg3       = cases[i].limit;
        v.arg2       = RF_NONE;
        run_plain(&v, 1);
    }
    ring_end(v.group, 0, 0);
}

/* ---- the end of a 16-bit code segment, and prefixes inside one ----------------------------- */



/* ---- SGDT and friends --------------------------------------------------------------------- */


/* ---- EFLAGS through IRETD ----------------------------------------------------------------- */


/* ---- XLAT ----------------------------------------------------------------------------------- */


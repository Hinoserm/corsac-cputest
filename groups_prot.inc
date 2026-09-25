/*
 * Protection and instruction corners: which instructions LOCK may
 * prefix, segment overrides, far-pointer loads, 16-bit stacks, string
 * copies that overlap or run long, what a segment register looks like in
 * a 32-bit register or on the stack, LAR/VERR/segment loads over every
 * descriptor type, IDT gates that must fail, the end of a 16-bit code
 * segment and operand-size prefixes inside one, SGDT and friends, the
 * IDT's limit, EFLAGS through IRET, XLAT. Included into groups.inc after
 * groups_desc.inc; append only.
 */

/* ---- LOCK ------------------------------------------------------------------ */

/* The lockable instructions with a memory destination run; everything
   else with LOCK, and those with a register destination, is #UD. The
   logic ops leave AF undefined, the bit-test ones everything but CF:
   those parts are raw only. */
static int
dm_lock(const struct variant *v, const struct kin *in, struct kout *d, uint16_t *undef)
{
    if (!v->fx_arg)
        return 0;
    if (d->fault == 0xff)
        *undef |= F_AF;
    return 1;
}

static void
group_lock_legal(void)
{
    static const struct {
        uint8_t defined, len, b[7];
    } seqs[] = {
        { 1, SEQ(0xf0, 0x00, 0x03) },             /* add [ebx], al */
        { 1, SEQ(0xf0, 0x01, 0x03) },             /* add [ebx], eax */
        { 1, SEQ(0xf0, 0x66, 0x01, 0x03) },       /* add [ebx], ax */
        { 1, SEQ(0xf0, 0x09, 0x03) },             /* or */
        { 1, SEQ(0xf0, 0x11, 0x03) },             /* adc */
        { 1, SEQ(0xf0, 0x19, 0x03) },             /* sbb */
        { 1, SEQ(0xf0, 0x21, 0x03) },             /* and */
        { 1, SEQ(0xf0, 0x29, 0x03) },             /* sub */
        { 1, SEQ(0xf0, 0x31, 0x03) },             /* xor */
        { 1, SEQ(0xf0, 0x80, 0x03, 0x01) },       /* add byte [ebx], 1 */
        { 1, SEQ(0xf0, 0x83, 0x33, 0x01) },       /* xor dword [ebx], 1 */
        { 1, SEQ(0xf0, 0x86, 0x03) },             /* xchg [ebx], al */
        { 1, SEQ(0xf0, 0x87, 0x03) },             /* xchg [ebx], eax */
        { 1, SEQ(0xf0, 0xfe, 0x03) },             /* inc byte [ebx] */
        { 1, SEQ(0xf0, 0xff, 0x0b) },             /* dec dword [ebx] */
        { 1, SEQ(0xf0, 0xf6, 0x13) },             /* not byte [ebx] */
        { 1, SEQ(0xf0, 0xf7, 0x1b) },             /* neg dword [ebx] */
        { 0, SEQ(0x83, 0xe0, 0x1f, 0xf0, 0x0f, 0xab, 0x03) }, /* and eax, 31; bts [ebx], eax */
        { 0, SEQ(0x83, 0xe0, 0x1f, 0xf0, 0x0f, 0xb3, 0x03) }, /* btr */
        { 0, SEQ(0x83, 0xe0, 0x1f, 0xf0, 0x0f, 0xbb, 0x03) }, /* btc */
        { 0, SEQ(0xf0, 0x0f, 0xba, 0x2b, 0x05) }, /* bts [ebx], 5 */
        { 0, SEQ(0xf0, 0x0f, 0xba, 0x33, 0x05) }, /* btr [ebx], 5 */
        { 0, SEQ(0xf0, 0x0f, 0xba, 0x3b, 0x05) }, /* btc [ebx], 5 */
        { 1, SEQ(0xf0, 0x0f, 0xb0, 0x0b) },       /* cmpxchg [ebx], cl */
        { 1, SEQ(0xf0, 0x0f, 0xb1, 0x0b) },       /* cmpxchg [ebx], ecx */
        { 1, SEQ(0xf0, 0x0f, 0xc0, 0x03) },       /* xadd [ebx], al */
        { 1, SEQ(0xf0, 0x0f, 0xc1, 0x03) },       /* xadd [ebx], eax */
        { 1, SEQ(0xf0, 0x0f, 0xc7, 0x0b) },       /* cmpxchg8b [ebx] */
        { 1, SEQ(0xf0, 0x39, 0x03) },             /* cmp: #UD */
        { 1, SEQ(0xf0, 0x03, 0x03) },             /* add eax, [ebx]: destination a register: #UD */
        { 1, SEQ(0xf0, 0x85, 0x03) },             /* test: #UD */
        { 1, SEQ(0xf0, 0xf6, 0x03, 0x01) },       /* test byte [ebx], 1: #UD */
        { 1, SEQ(0xf0, 0x88, 0x03) },             /* mov [ebx], al: #UD */
        { 1, SEQ(0xf0, 0x8b, 0x03) },             /* mov eax, [ebx]: #UD */
        { 1, SEQ(0xf0, 0x8d, 0x03) },             /* lea: #UD */
        { 1, SEQ(0x83, 0xe0, 0x1f, 0xf0, 0x0f, 0xa3, 0x03) }, /* bt [ebx], eax: #UD */
        { 1, SEQ(0xf0, 0x0f, 0xba, 0x23, 0x05) }, /* bt [ebx], 5: #UD */
        { 1, SEQ(0xf0, 0xff, 0x33) },             /* push [ebx]: #UD */
        { 1, SEQ(0xf0, 0x8f, 0x03) },             /* pop [ebx]: #UD */
        { 1, SEQ(0xf0, 0xf7, 0x23) },             /* mul dword [ebx]: #UD */
        { 1, SEQ(0xf0, 0x0f, 0xb6, 0x03) },       /* movzx: #UD */
        { 1, SEQ(0xf0, 0x0f, 0xbc, 0x03) },       /* bsf: #UD */
        { 1, SEQ(0xf0, 0x01, 0xc8) },             /* add eax, ecx: #UD */
        { 1, SEQ(0xf0, 0x87, 0xc8) },             /* xchg eax, ecx: #UD */
        { 1, SEQ(0xf0, 0xff, 0xc0) },             /* inc eax: #UD */
        { 1, SEQ(0xf0, 0x40) },                   /* inc eax (one byte): #UD */
    };
    const char    *name = "lock.legal";
    struct variant v    = { .group = name, .faults_ok = 1, .fx = FX_CUSTOM, .defmask = dm_lock,
                            .fix_input = fix_mem_ebx, .arg = MEM_OFF };
    group_begin(name);
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        memcpy(v.target, seqs[i].b, seqs[i].len);
        v.target_len = seqs[i].len;
        v.fx_arg     = seqs[i].defined;
        run_plain(&v, 1);
    }
    group_end(name);
}

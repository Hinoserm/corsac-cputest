/*
 * Pentium Pro (P6) and later: CMOVcc, the multi-byte NOP, and RDTSC.
 * Included into groups.inc after groups_mmx.inc; append only.
 */

#define CPUID_TSC   (1u << 4)
#define CPUID_CMOV  (1u << 15)

/* CMOVcc: all sixteen conditions, 16- and 32-bit, from a register, the
   same register, and memory (aligned and misaligned; the load happens
   whether or not the move does), after every flag producer. */
static void
group_cmov(void)
{
    static const struct {
        uint8_t dst, src, disp;
    } forms[] = {
        { R_EAX, R_ECX, 0 },
        { R_EDX, R_EDX, 0 },
        { R_ESI, R_EDI, 0 },
        { R_EBP, RM_MEM, 0 },
        { R_EDI, RM_MEM, 31 }, /* [ebx+31] */
    };
    const char    *name = "cmov";
    struct variant v    = { .group = name, .fx = FX_SETCC };

    if (!(g_cpu.features & CPUID_CMOV)) {
        group_skip(name, "no CMOV");
        return;
    }
    group_begin(name);
    for (int cc = 0; cc < 16; cc++) {
        for (int size = 16; size <= 32; size += 16) {
            for (unsigned f = 0; f < sizeof(forms) / sizeof(forms[0]); f++) {
                int n = 0;
                if (size == 16)
                    v.target[n++] = 0x66;
                v.target[n++] = 0x0f;
                v.target[n++] = 0x40 | cc;
                if (forms[f].disp) {
                    v.target[n++] = 0x43 | (forms[f].dst << 3);
                    v.target[n++] = forms[f].disp;
                } else
                    n = put_modrm(v.target, n, forms[f].dst, forms[f].src);
                v.target_len = n;
                v.fx_arg     = cc;
                v.fix_input  = forms[f].src == RM_MEM ? fix_mem_ebx : 0;
                v.arg        = MEM_OFF;
                run_all_contexts(&v, 2);
            }
        }
    }
    group_end(name);
}

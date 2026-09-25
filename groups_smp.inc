/*
 * Multiprocessor machines: the BIOS's MP and ACPI tables, the local APIC,
 * the I/O APIC, and the other processors. Everything here is for bare
 * metal; the Linux build skips it. Included into groups.inc last.
 */

/* ---- the tables ------------------------------------------------------------ */

/* The processors and I/O APICs as the BIOS lists them (mp.inc): the MP
   floating pointer and configuration table, an MP default configuration,
   or the ACPI MADT. The board's answer, not the CPU's: no reference and
   never a stop; a missing or bad table only turns off the tests that need
   it. EAX the status (1 MP table, 2 default configuration, 3 MADT only, 0
   none; 4-10 a bad table: floating pointer, signature, checksum, length,
   entry, extended checksum, MADT), ECX the processors listed, EDX the I/O
   APICs, ESI the MP spec revision, EDI the IMCR flag. The entries are
   printed too. */
static void
group_mp_table(void)
{
    const char *name = "mp.table";
#ifdef HOSTTEST
    group_skip(name, "BIOS (host build)");
#else
    struct variant v = { .group = name, .no_ref = 1 };
    group_begin(name);
    if (!g_counting) {
        puts_("MP fps=");
        puthex(g_mp.fps, 8);
        puts_(" table=");
        puthex(g_mp.table, 8);
        puts_(" lapic=");
        puthex(g_mp.lapic, 8);
        puts_(" default=");
        putdec(g_mp.deftype);
        putch('\n');
        for (unsigned i = 0; i < g_mp.n_cpus; i++) {
            puts_("MP cpu apic=");
            puthex(g_mp.cpu[i].apic_id, 2);
            puts_(" ver=");
            puthex(g_mp.cpu[i].apic_ver, 2);
            puts_(" flags=");
            puthex(g_mp.cpu[i].flags, 2);
            puts_(" sig=");
            puthex(g_mp.cpu[i].sig, 8);
            putch('\n');
        }
        for (unsigned i = 0; i < g_mp.n_ioapics; i++) {
            puts_("MP ioapic id=");
            puthex(g_mp.io[i].id, 2);
            puts_(" ver=");
            puthex(g_mp.io[i].ver, 2);
            puts_(" en=");
            putdec(g_mp.io[i].en);
            puts_(" addr=");
            puthex(g_mp.io[i].addr, 8);
            putch('\n');
        }
    }
    static const uint8_t op[5] = { 0xb8, 0xb9, 0xba, 0xbe, 0xbf }; /* mov eax/ecx/edx/esi/edi, imm32 */
    uint32_t val[5] = { g_mp.status, g_mp.cpus_listed, g_mp.n_ioapics, g_mp.spec, g_mp.imcrp };
    int      n      = 0;
    for (int i = 0; i < 5; i++) {
        v.target[n++] = op[i];
        put_le(v.target + n, val[i], 4);
        n += 4;
    }
    v.target_len = n;
    run_plain(&v, 1);
    group_end(name);
#endif
}

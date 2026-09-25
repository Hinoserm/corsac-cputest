/*
 * The machine's processors and I/O APICs, from the BIOS's tables: the MP
 * floating pointer structure and configuration table (MP spec 1.1 and
 * 1.4, or one of its default configurations), else the ACPI MADT. Read
 * once at start-up, before any test can touch low memory, and copied
 * here, like the E820 map. The tables are the board's, not the CPU's: a
 * missing or bad one is a result (group mp.table) and only turns off the
 * tests that need it. Every read is bounded by the table's own length and
 * a sane maximum, so a corrupt table can't run the search away.
 *
 * Also the PIT channel 2 delay the SMP tests time their waits with.
 * Included into harness.c after the E820 code.
 */

#define MP_MAX_CPUS    16
#define MP_MAX_IOAPICS 4

enum {
    MP_NONE,     /* no MP structure and no MADT */
    MP_OK,       /* an MP configuration table */
    MP_DEFAULT,  /* an MP default configuration (1-7) */
    MP_MADT,     /* no MP structure, but an ACPI MADT */
    MP_FPS_BAD,  /* an MP floating pointer with a bad checksum or length */
    MP_CT_SIG,   /* its configuration table isn't "PCMP" or is out of reach */
    MP_CT_SUM,   /* the configuration table's checksum */
    MP_CT_LEN,   /* its length: under 44 bytes, over 64 KB */
    MP_CT_ENTRY, /* an entry of an unknown type or running past the end */
    MP_EXT_SUM,  /* the 1.4 extended table's checksum (the base table is used) */
    MP_MADT_BAD, /* no MP structure, and an ACPI table that fails its checks */
};

struct mp_cpu {
    uint8_t  apic_id, apic_ver, flags; /* flags: bit 0 enabled, bit 1 the boot processor */
    uint32_t sig;
};

struct mp_ioapic {
    uint8_t  id, ver, en;
    uint32_t addr;
};

static struct {
    unsigned         status, spec, deftype, imcrp, n_cpus, n_ioapics, cpus_listed;
    uint32_t         fps, table, lapic;
    struct mp_cpu    cpu[MP_MAX_CPUS];
    struct mp_ioapic io[MP_MAX_IOAPICS];
} g_mp;

/* Tables are read through physical addresses (paging is off). */
static const volatile uint8_t *
phys(uint32_t a)
{
    const volatile uint8_t *p;
    __asm__("" : "=r"(p) : "0"(a)); /* an address, not an object the compiler can bound */
    return p;
}

static uint32_t
phys32(uint32_t a)
{
    return phys(a)[0] | phys(a)[1] << 8 | phys(a)[2] << 16 | (uint32_t) phys(a)[3] << 24;
}

static uint8_t
sum8(uint32_t a, uint32_t n)
{
    uint8_t s = 0;
    while (n--)
        s += phys(a++)[0];
    return s;
}

static int
sig_at(uint32_t a, const char *sig, int n)
{
    for (int i = 0; i < n; i++)
        if (phys(a)[i] != (uint8_t) sig[i])
            return 0;
    return 1;
}

/* A table address worth following: not null, below the APICs and ROMs at
   the top of the address space, and not wrapping. */
static int
reachable(uint32_t a, uint32_t len)
{
    return a >= 0x400 && a < 0xfec00000u && a + len > a && a + len <= 0xfec00000u;
}

static void
mp_add_cpu(uint8_t id, uint8_t ver, uint8_t flags, uint32_t sig)
{
    g_mp.cpus_listed++;
    if (g_mp.n_cpus < MP_MAX_CPUS)
        g_mp.cpu[g_mp.n_cpus++] = (struct mp_cpu) { id, ver, flags, sig };
}

static void
mp_add_ioapic(uint8_t id, uint8_t ver, uint8_t en, uint32_t addr)
{
    if (g_mp.n_ioapics < MP_MAX_IOAPICS)
        g_mp.io[g_mp.n_ioapics++] = (struct mp_ioapic) { id, ver, en, addr };
}

/* "_MP_" on a 16-byte boundary in [a, a+len): the first with a good
   checksum and length. A bad one is remembered in case no good one turns
   up. */
static uint32_t
fps_scan(uint32_t a, uint32_t len, int *bad)
{
    for (uint32_t p = a & ~15u; p + 16 <= a + len; p += 16) {
        if (!sig_at(p, "_MP_", 4))
            continue;
        if (phys(p)[8] == 1 && sum8(p, 16) == 0)
            return p;
        *bad = 1;
    }
    return 0;
}

static void
mp_default(unsigned type)
{
    g_mp.status  = MP_DEFAULT;
    g_mp.deftype = type;
    g_mp.lapic   = 0xfee00000u;
    mp_add_cpu(0, type <= 4 ? 0x01 : 0x11, 3, 0);
    mp_add_cpu(1, type <= 4 ? 0x01 : 0x11, 1, 0);
    mp_add_ioapic(2, type <= 4 ? 0x01 : 0x11, 1, 0xfec00000u);
}

static void
mp_table(uint32_t ct)
{
    if (!reachable(ct, 44) || !sig_at(ct, "PCMP", 4)) {
        g_mp.status = MP_CT_SIG;
        return;
    }
    uint32_t len = phys(ct)[4] | phys(ct)[5] << 8;
    if (len < 44 || !reachable(ct, len)) {
        g_mp.status = MP_CT_LEN;
        return;
    }
    if (sum8(ct, len)) {
        g_mp.status = MP_CT_SUM;
        return;
    }
    g_mp.table        = ct;
    g_mp.lapic        = phys32(ct + 36);
    unsigned entries  = phys(ct)[34] | phys(ct)[35] << 8;
    uint32_t p        = ct + 44, end = ct + len;
    for (unsigned i = 0; i < entries; i++) {
        uint8_t  type = p < end ? phys(p)[0] : 0xff;
        uint32_t elen = type == 0 ? 20 : 8;
        if (type > 4 || p + elen > end) {
            g_mp.status = MP_CT_ENTRY;
            return;
        }
        if (type == 0)
            mp_add_cpu(phys(p)[1], phys(p)[2], phys(p)[3], phys32(p + 4));
        else if (type == 2)
            mp_add_ioapic(phys(p)[1], phys(p)[2], phys(p)[3] & 1, phys32(p + 4));
        p += elen;
    }
    g_mp.status = MP_OK;
    /* 1.4: the extended entries follow, with their own checksum. */
    uint32_t ext = phys(ct)[40] | phys(ct)[41] << 8;
    if (g_mp.spec >= 4 && ext) {
        if (!reachable(end, ext) || (uint8_t) (sum8(end, ext) + phys(ct)[42]) != 0)
            g_mp.status = MP_EXT_SUM;
    }
}

/* The ACPI MADT ("APIC"), through the RSDP and RSDT. */
static void
madt_read(void)
{
    uint32_t rsdp = 0, ebda = (uint32_t) (phys(0x40e)[0] | phys(0x40e)[1] << 8) << 4;
    uint32_t areas[2][2] = { { ebda, 1024 }, { 0xe0000, 0x20000 } };
    for (int k = 0; k < 2 && !rsdp; k++) {
        if (k == 0 && (ebda < 0x80000 || ebda >= 0xa0000))
            continue;
        for (uint32_t p = areas[k][0]; p + 20 <= areas[k][0] + areas[k][1]; p += 16)
            if (sig_at(p, "RSD PTR ", 8) && sum8(p, 20) == 0) {
                rsdp = p;
                break;
            }
    }
    if (!rsdp)
        return; /* MP_NONE */
    g_mp.status  = MP_MADT_BAD;
    uint32_t rsdt = phys32(rsdp + 16);
    if (!reachable(rsdt, 36) || !sig_at(rsdt, "RSDT", 4))
        return;
    uint32_t len = phys32(rsdt + 4);
    if (len < 36 || len > 0x10000 || !reachable(rsdt, len) || sum8(rsdt, len))
        return;
    for (uint32_t e = rsdt + 36; e + 4 <= rsdt + len; e += 4) {
        uint32_t t = phys32(e);
        if (!reachable(t, 44) || !sig_at(t, "APIC", 4))
            continue;
        uint32_t tlen = phys32(t + 4);
        if (tlen < 44 || tlen > 0x10000 || !reachable(t, tlen) || sum8(t, tlen))
            return;
        g_mp.lapic = phys32(t + 36);
        for (uint32_t p = t + 44; p + 2 <= t + tlen;) {
            uint8_t type = phys(p)[0], elen = phys(p)[1];
            if (elen < 2 || p + elen > t + tlen)
                return;
            if (type == 0 && elen >= 8) /* flags bit 0: enabled; the MADT has no BSP flag */
                mp_add_cpu(phys(p)[3], 0, phys(p)[4] & 1, 0);
            else if (type == 1 && elen >= 12)
                mp_add_ioapic(phys(p)[2], 0, 1, phys32(p + 4));
            p += elen;
        }
        g_mp.status = MP_MADT;
        return;
    }
}

static void
mp_read(void)
{
#ifndef HOSTTEST
    int      bad  = 0;
    uint32_t ebda = (uint32_t) (phys(0x40e)[0] | phys(0x40e)[1] << 8) << 4;
    uint32_t kb   = phys(0x413)[0] | phys(0x413)[1] << 8;
    uint32_t fps  = 0;
    if (ebda >= 0x80000 && ebda < 0xa0000)
        fps = fps_scan(ebda, 1024, &bad);
    if (!fps && kb >= 512 && kb <= 640)
        fps = fps_scan(kb * 1024 - 1024, 1024, &bad);
    if (!fps)
        fps = fps_scan(0xf0000, 0x10000, &bad);
    if (fps) {
        g_mp.fps   = fps;
        g_mp.spec  = phys(fps)[9];
        g_mp.imcrp = phys(fps)[12] >> 7;
        if (phys(fps)[11] > 7)
            g_mp.status = MP_FPS_BAD; /* no such default configuration */
        else if (phys(fps)[11])
            mp_default(phys(fps)[11]);
        else
            mp_table(phys32(fps + 4));
        return;
    }
    madt_read();
    if (g_mp.status == MP_NONE && bad)
        g_mp.status = MP_FPS_BAD;
#endif
}

/* Tables good enough to find the other processors and I/O APICs by. */
static int __attribute__((unused))
mp_usable(void)
{
    return g_mp.status == MP_OK || g_mp.status == MP_DEFAULT || g_mp.status == MP_MADT || g_mp.status == MP_EXT_SUM;
}

/* ---- time without interrupts: PIT channel 2, polled ---------------------- */

/* Wait count ticks of the PIT's 1.193182 MHz (at most 65535): channel 2 in
   mode 0 with its gate on and the speaker off, polling OUT2 (port 61h bit
   5). 0 if OUT2 never rises (no PIT there: the caller treats it as a
   timeout of its own). */
#ifndef HOSTTEST
static int
pit_wait(uint32_t count)
{
    outb(0x61, (inb(0x61) & 0xfc) | 0x01);
    outb(0x43, 0xb0);
    outb(0x42, count & 0xff);
    outb(0x42, (count >> 8) & 0xff);
    for (uint32_t spin = 0; spin < 20000000; spin++)
        if (inb(0x61) & 0x20)
            return 1;
    return 0;
}

static int __attribute__((unused))
delay_us(uint32_t us)
{
    while (us) {
        uint32_t n = us > 50000 ? 50000 : us;
        if (!pit_wait(n * 1193 / 1000 + 1))
            return 0;
        us -= n;
    }
    return 1;
}
#endif

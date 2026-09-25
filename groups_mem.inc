/*
 * Addressing and memory forms. Included into groups.inc after
 * groups_exhaust.inc; append only.
 */

/* ---- LEA: every ModRM and SIB addressing form ------------------------------ */

static const uint8_t  lea_d8[]  = { 0x00, 0x01, 0x7f, 0x80, 0xff };
static const uint32_t lea_d32[] = { 0x00000000, 0x7fffffff, 0x80000000, 0xffffffff, 0x12345678 };
static const uint16_t lea_d16[] = { 0x0000, 0x7fff, 0x8000, 0xffff };

/* The displacement for a variant: the lists above in turn, so every size
   of displacement meets each of its edges across the group. */
static int
put_disp(uint8_t *t, int n, int bytes, unsigned k)
{
    if (bytes == 1)
        t[n++] = lea_d8[k % sizeof(lea_d8)];
    else if (bytes == 2) {
        put_le(t + n, lea_d16[k % (sizeof(lea_d16) / sizeof(lea_d16[0]))], 2);
        n += 2;
    } else if (bytes == 4) {
        put_le(t + n, lea_d32[k % (sizeof(lea_d32) / sizeof(lea_d32[0]))], 4);
        n += 4;
    }
    return n;
}

/* LEA computes the address and nothing else, so every form runs with the
   registers as they come. A base of ESP runs on the private stack, and the
   result is taken relative to the buffer like ESP itself. */
static void
group_lea(void)
{
    const char    *name = "lea";
    struct variant v    = { .group = name };
    unsigned       k    = 0;
    group_begin(name);

    /* 32-bit addressing without SIB: mod 0-2, rm 0-7 but 4. */
    for (int mod = 0; mod < 3; mod++) {
        for (int rm = 0; rm < 8; rm++) {
            if (rm == 4)
                continue;
            int dsize = mod == 1 ? 1 : mod == 2 || rm == 5 ? 4 : 0;
            int n     = 0;
            v.target[n++] = 0x8d;
            v.target[n++] = (mod << 6) | (R_EAX << 3) | rm;
            n             = put_disp(v.target, n, dsize, k++);
            v.target_len  = n;
            v.stack       = 0;
            v.norm        = 0;
            run_plain(&v, 2);
        }
    }
    /* 32-bit with SIB: every base (index ECX*4), and every index and scale
       (base EBX), at each mod. Base 5 with mod 0 is disp32 and no base;
       index 4 is no index. */
    for (int mod = 0; mod < 3; mod++) {
        for (int form = 0; form < 8 + 32; form++) {
            int base  = form < 8 ? form : R_EBX;
            int index = form < 8 ? R_ECX : (form - 8) & 7;
            int scale = form < 8 ? 2 : (form - 8) >> 3;
            int dsize = mod == 1 ? 1 : mod == 2 || (mod == 0 && base == 5) ? 4 : 0;
            int n     = 0;
            v.target[n++] = 0x8d;
            v.target[n++] = (mod << 6) | (R_EAX << 3) | 4;
            v.target[n++] = (scale << 6) | (index << 3) | base;
            n             = put_disp(v.target, n, dsize, k++);
            v.target_len  = n;
            v.stack       = base == R_ESP;
            v.norm        = base == R_ESP ? 1 << R_EAX : 0;
            run_plain(&v, 2);
        }
    }
    v.stack = 0;
    v.norm  = 0;
    /* The destination is the base register itself. */
    {
        static const struct {
            uint8_t len, b[4];
        } seqs[] = {
            { SEQ(0x8d, 0x1c, 0x9b) },       /* lea ebx, [ebx+ebx*4] */
            { SEQ(0x8d, 0x76, 0x80) },       /* lea esi, [esi-128] */
            { SEQ(0x8d, 0x3c, 0x3f) },       /* lea edi, [edi+edi] */
            { SEQ(0x8d, 0x6c, 0x2d, 0x7f) }, /* lea ebp, [ebp+ebp+127] */
        };
        for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
            memcpy(v.target, seqs[i].b, seqs[i].len);
            v.target_len = seqs[i].len;
            run_plain(&v, 2);
        }
    }
    /* 16-bit addressing: mod 0-2, rm 0-7 (mod 0 rm 6 is disp16), into a
       32-bit register (the address zero-extended) and a 16-bit one. */
    for (int size16 = 0; size16 < 2; size16++) {
        for (int mod = 0; mod < 3; mod++) {
            for (int rm = 0; rm < 8; rm++) {
                int dsize = mod == 1 ? 1 : mod == 2 || (mod == 0 && rm == 6) ? 2 : 0;
                int n     = 0;
                v.target[n++] = 0x67;
                if (size16)
                    v.target[n++] = 0x66;
                v.target[n++] = 0x8d;
                v.target[n++] = (mod << 6) | (R_EAX << 3) | rm;
                n             = put_disp(v.target, n, dsize, k++);
                v.target_len  = n;
                run_plain(&v, 2);
            }
        }
    }
    /* A 16-bit destination with 32-bit addressing: the low half of the
       address. */
    {
        static const struct {
            uint8_t len, b[8];
        } seqs[] = {
            { SEQ(0x66, 0x8d, 0x84, 0x8b, 0xff, 0xff, 0x00, 0x00) }, /* lea ax, [ebx+ecx*4+0FFFFh] */
            { SEQ(0x66, 0x8d, 0x46, 0x80) },                         /* lea ax, [esi-128] */
        };
        for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
            memcpy(v.target, seqs[i].b, seqs[i].len);
            v.target_len = seqs[i].len;
            run_plain(&v, 2);
        }
    }
    group_end(name);
}

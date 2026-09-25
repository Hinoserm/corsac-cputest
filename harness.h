#ifndef CPUTEST_HARNESS_H
#define CPUTEST_HARNESS_H

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef signed int         int32_t;
typedef signed short       int16_t;
typedef unsigned long long uint64_t;

#define BUF_SIZE    64
#define LOWBUF_SIZE 16

struct cpu_info {
    int      is486, has_cpuid;
    char     vendor[13];
    uint32_t signature, features;
};

/* What a test starts from. */
struct kin {
    uint32_t flags;
    uint32_t eax, ecx, edx, ebx, ebp, esi, edi;
    uint8_t  buf[BUF_SIZE];
    uint8_t  lowbuf[LOWBUF_SIZE];
};

/* What it ends with: the generated code fills flags and the registers, the
   harness the rest. Compared byte for byte between runs. */
struct kout {
    uint32_t flags;
    uint32_t eax, ecx, edx, ebx, ebp, esi, edi;
    uint32_t fault, fault_ip, fault_err;
    uint8_t  buf[BUF_SIZE];
    uint8_t  lowbuf[LOWBUF_SIZE];
};

/* Filled by isr_common (rt.asm); the offsets there must match. */
struct fault {
    uint32_t vec, err, eip, eflags;
    uint32_t eax, ecx, edx, ebx, ebp, esi, edi;
    uint32_t pad;
};

extern struct cpu_info g_cpu;
extern uint8_t         g_buf[BUF_SIZE];

void *memcpy(void *d, const void *s, unsigned n);
void *memset(void *d, int c, unsigned n);

#endif

#ifndef CPUTEST_HARNESS_H
#define CPUTEST_HARNESS_H

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef signed int         int32_t;
typedef signed short       int16_t;
typedef signed char        int8_t;
typedef unsigned long long uint64_t;

#define BUF_SIZE    64
#define LOWBUF_SIZE 16

struct cpu_info {
    int      is486, has_cpuid;
    char     vendor[13];
    uint32_t signature, features;
    uint32_t ext_features; /* CPUID 80000001h EDX, 0 without it */
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
    uint32_t sandbox_crc; /* all of the sandbox, to catch writes outside buf */
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

/* Memory operands point into the middle of a 16 KB sandbox that is refilled
   before every run, so a write outside the operand (a wrong address) shows
   in the result instead of landing in the harness. */
extern uint8_t *g_sandbox;
#define SANDBOX      g_sandbox
#define SANDBOX_SIZE 0x4000u
#define g_buf        (SANDBOX + 0x2000 - BUF_SIZE / 2)

void *memcpy(void *d, const void *s, unsigned n);
void *memset(void *d, int c, unsigned n);

#endif

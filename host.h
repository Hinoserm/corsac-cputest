/*
 * The harness's platform half for the host build (build.py --host): an
 * ordinary 32-bit Linux process. The generated tests run on the host CPU,
 * faults come back through signals, output goes to stdout. Included into
 * harness.c in place of the port I/O section.
 */
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

static void
serial_init(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
}

static void
putch(char c)
{
    putchar(c);
}

static void
puts_(const char *s)
{
    fputs(s, stdout);
}

static void
puthex(uint32_t v, int digits)
{
    printf("%0*x", digits, v);
}

static void
putdec(uint32_t v)
{
    printf("%u", v);
}

static void
post(uint8_t code)
{
    (void) code;
}

static uint32_t g_ram_top = 0x04000000; /* the arena ends 1 MB below this */

static void
ram_detect(void)
{
}

static void
idt_init(void)
{
}

static void
host_arena(uint32_t base, uint32_t len)
{
    void *p = mmap((void *) base, len, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *) base) {
        perror("arena mmap");
        exit(2);
    }
}

struct fault g_fault;
static sigjmp_buf g_jb;

static void
on_fault(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    greg_t     *g  = uc->uc_mcontext.gregs;
    (void) si;
    g_fault.vec    = (sig == SIGILL) ? 6 : (sig == SIGFPE) ? 0 : 13;
    g_fault.err    = 0;
    g_fault.eip    = g[REG_EIP];
    g_fault.eflags = g[REG_EFL];
    g_fault.eax    = g[REG_EAX];
    g_fault.ecx    = g[REG_ECX];
    g_fault.edx    = g[REG_EDX];
    g_fault.ebx    = g[REG_EBX];
    g_fault.ebp    = g[REG_EBP];
    g_fault.esi    = g[REG_ESI];
    g_fault.edi    = g[REG_EDI];
    siglongjmp(g_jb, 1);
}

int
run_kernel(void *code)
{
    static int installed;
    if (!installed) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_fault;
        sa.sa_flags     = SA_SIGINFO | SA_NODEFER;
        sigaction(SIGILL, &sa, NULL);
        sigaction(SIGFPE, &sa, NULL);
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        installed = 1;
    }
    g_fault.vec = 0xff;
    if (sigsetjmp(g_jb, 1)) {
        __asm__ volatile("cld");
        return g_fault.vec;
    }
    __asm__ volatile("pushal\n\t"
                     "call *%0\n\t"
                     "popal\n\t"
                     "cld"
                     :
                     : "r"(code)
                     : "memory", "cc");
    return 0xff;
}

void cmain(uint32_t rom_base);

int
main(void)
{
    cmain(0);
    return 0;
}

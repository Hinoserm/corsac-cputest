; CPU accuracy test ROM: the payload's runtime. Entry, GDT, exception
; handlers and the trampoline every test runs through.

bits 32

extern  cmain
extern  __bss_start
extern  __bss_end
global  _start
global  run_kernel
global  isr_table
global  g_saved_esp
global  g_fault
global  gdt
global  isr_48
global  g_in_kernel
global  irq_table
global  g_irq_hits
global  g_irq_log
global  g_irq_nlog
global  g_irq_eip
global  g_irq_eoi
extern  g_lapic
extern  harness_fault

section .text.entry
_start:
        cli
        cld
        mov     ebp, eax                ; ROM linear base, from the stub
        ; .bss is not in the ROM image: clear it (the stack is in it too).
        mov     edi, __bss_start
        mov     ecx, __bss_end
        sub     ecx, edi
        shr     ecx, 2
        xor     eax, eax
        rep     stosd
        mov     eax, ebp
        mov     esp, stack_top
        lgdt    [gdtr]
        jmp     0x08:.reload
.reload:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        push    ebp                     ; ROM linear base, from the stub
        call    cmain
.halt:
        cli
        hlt
        jmp     .halt

section .text

; int run_kernel(void *code)
; Runs one generated test. Every register but ESP may be changed by it. A
; fault anywhere inside comes back here through isr_common with the vector
; in g_fault; the return value is the vector, or 0xff for none.
run_kernel:
        mov     eax, [esp + 4]
        pushad
        mov     [g_saved_esp], esp
        mov     dword [g_fault + FAULT_VEC], 0xff
        mov     dword [g_in_kernel], 1
        call    eax
kernel_return:
        cld
        mov     dword [g_in_kernel], 0
        mov     ax, 0x10                ; a ring or V86 test may have left others
        mov     ss, ax                  ; (and a 16-bit stack test its SS; ESP is already ours)
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        popad
        mov     eax, [g_fault + FAULT_VEC]
        ret

; Exceptions 0-31. The ones without an error code push a zero so the frame
; is the same shape.
%macro ISR_NOERR 1
isr_%1:
        push    dword 0
        push    dword %1
        jmp     isr_common
%endmacro
%macro ISR_ERR 1
isr_%1:
        push    dword %1
        jmp     isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31
ISR_NOERR 48                            ; INT 30h: the way back from rings 1-3 and V86

; Frame: [vec] [err] [eip] [cs] [eflags]. Record the registers as the
; fault left them, then unwind to run_kernel's caller.
isr_common:
        pushad                          ; edi esi ebp esp ebx edx ecx eax
        mov     ax, 0x10                ; from V86, DS and ES arrive null
        mov     ds, ax
        mov     es, ax
        mov     eax, [esp + 28]
        mov     [g_fault + FAULT_EAX], eax
        mov     eax, [esp + 24]
        mov     [g_fault + FAULT_ECX], eax
        mov     eax, [esp + 20]
        mov     [g_fault + FAULT_EDX], eax
        mov     eax, [esp + 16]
        mov     [g_fault + FAULT_EBX], eax
        mov     eax, [esp + 8]
        mov     [g_fault + FAULT_EBP], eax
        mov     eax, [esp + 4]
        mov     [g_fault + FAULT_ESI], eax
        mov     eax, [esp + 0]
        mov     [g_fault + FAULT_EDI], eax
        mov     eax, [esp + 32]
        mov     [g_fault + FAULT_VEC], eax
        mov     eax, [esp + 36]
        mov     [g_fault + FAULT_ERR], eax
        mov     eax, [esp + 40]
        mov     [g_fault + FAULT_EIP], eax
        mov     eax, [esp + 48]
        mov     [g_fault + FAULT_EFLAGS], eax
        cmp     dword [g_in_kernel], 0
        je      .harness                ; not in a test: the harness itself faulted
        mov     esp, [g_saved_esp]
        jmp     kernel_return
.harness:
        mov     esp, panic_stack_top    ; a stack of its own: the old one may be the problem
        call    harness_fault           ; prints everything and halts
.halt:
        cli
        hlt
        jmp     .halt

; Interrupts for the APIC groups (their own IDT): vectors 20h-FFh and NMI
; count, log their order, keep the interrupted EIP, and EOI the local APIC
; if g_irq_eoi says so (never for NMI or the spurious vector FFh).
%macro IRQ 1
irq_%1:
        push    dword %1
        jmp     irq_common
%endmacro

%assign i 32
%rep 224
IRQ i
%assign i i+1
%endrep
IRQ 2

irq_common:
        pushad
        mov     eax, [esp + 32]         ; the vector
        inc     dword [g_irq_hits + eax * 4]
        mov     ecx, [g_irq_nlog]
        cmp     ecx, 16
        jae     .logged
        mov     [g_irq_log + ecx * 4], eax
        inc     dword [g_irq_nlog]
.logged:
        mov     ecx, [esp + 36]         ; the interrupted EIP
        mov     [g_irq_eip], ecx
        cmp     eax, 2
        je      .done
        cmp     eax, 0xff
        je      .done
        cmp     dword [g_irq_eoi], 0
        je      .done
        mov     ecx, [g_lapic]
        xor     edx, edx
        xchg    [ecx + 0xb0], edx       ; EOI
.done:
        popad
        add     esp, 4
        iretd

section .rodata
        align   4
irq_table:                              ; 256 entries: 0 where the harness's own handler stays
%assign i 0
%rep 256
%if i >= 32
        dd      irq_ %+ i
%elif i == 2
        dd      irq_2
%else
        dd      0
%endif
%assign i i+1
%endrep

isr_table:
%assign i 0
%rep 32
        dd      isr_ %+ i
%assign i i+1
%endrep

        align   8
gdt:
        dq      0
        dq      0x00cf9a000000ffff      ; 0x08: code, flat, 32-bit
        dq      0x00cf92000000ffff      ; 0x10: data, flat
        dq      0x00009a000000ffff      ; 0x18: code, 64 KB, 16-bit
        dq      0x000092000000ffff      ; 0x20: data, 64 KB, 16-bit
gdt_end:
gdtr:
        dw      gdt_end - gdt - 1
        dd      gdt

; Offsets into struct fault (harness.h).
FAULT_VEC       equ 0
FAULT_ERR       equ 4
FAULT_EIP       equ 8
FAULT_EFLAGS    equ 12
FAULT_EAX       equ 16
FAULT_ECX       equ 20
FAULT_EDX       equ 24
FAULT_EBX       equ 28
FAULT_EBP       equ 32
FAULT_ESI       equ 36
FAULT_EDI       equ 40

section .bss
        alignb  16
g_irq_hits:     resd 256
g_irq_log:      resd 16
g_irq_nlog:     resd 1
g_irq_eip:      resd 1
g_irq_eoi:      resd 1
g_saved_esp:
        resd    1
g_fault:
        resd    12
g_in_kernel:                            ; 1 while a test runs: a fault then is its result
        resd    1
        alignb  16
        resb    65536
stack_top:
        resb    8192                    ; for harness_fault
panic_stack_top:

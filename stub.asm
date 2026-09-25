; CPU accuracy test: the loader.
;
; The boot sector loads this to 1000:0000 and far-calls offset 3 in real
; mode (the layout of an option ROM, which it once also was). From here:
; interrupts off, A20 on, a flat GDT, protected mode, the payload copied to
; 1 MB, and a jump to it. Nothing returns.

bits 16
org 0

PAYLOAD_BASE equ 0x100000

rom_start:
        db      0x55, 0xaa
        db      IMAGE_BLOCKS            ; size in 512-byte blocks, set by build.py
        jmp     init

        align   16
init:
        cli
        cld
        xor     ax, ax
        mov     ss, ax
        mov     sp, 0x7000

        ; A20: port 92h (fast A20), then the keyboard controller for boards
        ; without it.
        in      al, 0x92
        or      al, 0x02
        and     al, 0xfe
        out     0x92, al
        call    kbc_wait
        mov     al, 0xd1
        out     0x64, al
        call    kbc_wait
        mov     al, 0xdf
        out     0x60, al
        call    kbc_wait

        ; Linear address of this ROM.
        mov     ax, cs
        movzx   ebp, ax
        shl     ebp, 4

        ; GDTR on the stack (the ROM is not writable).
        sub     sp, 8
        mov     bx, sp
        mov     word [ss:bx], gdt_end - gdt - 1
        lea     edx, [ebp + gdt]
        mov     [ss:bx + 2], edx
        o32 lgdt [ss:bx]
        add     sp, 8

        mov     eax, cr0
        or      al, 1
        mov     cr0, eax

        ; Far return into 32-bit code at the ROM's linear address.
        push    dword 0x08
        lea     eax, [ebp + pm32]
        push    eax
        o32 retf

kbc_wait:
        mov     cx, 0xffff
.loop:  in      al, 0x64
        test    al, 0x02
        jz      .done
        loop    .loop
.done:  ret

bits 32
pm32:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     esp, 0x90000

        lea     esi, [ebp + payload]
        mov     edi, PAYLOAD_BASE
        mov     ecx, (payload_end - payload + 3) / 4
        rep     movsd

        ; The payload's own GDT replaces this one; tell it where the ROM is.
        mov     eax, ebp
        jmp     0x08:PAYLOAD_BASE

        align   8
gdt:
        dq      0
        dq      0x00cf9a000000ffff      ; 0x08: code, flat, 32-bit
        dq      0x00cf92000000ffff      ; 0x10: data, flat
gdt_end:

        align   16
payload:
        incbin  "payload.bin"
payload_end:

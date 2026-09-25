; CPU accuracy test: the loader.
;
; The boot sector loads this to 1000:0000 and far-calls offset 3 in real
; mode (the layout of an option ROM, which it once also was). From here:
; interrupts off, A20 on, a flat GDT, protected mode, the payload copied to
; 1 MB, and a jump to it. Nothing returns.

bits 16
org 0

PAYLOAD_BASE equ 0x100000
E820_AT      equ 0x5000             ; the BIOS memory map, for the harness
E820_MAX     equ 64
IDT_AT       equ 0x6000             ; the start-up IDT, until the harness loads its own

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
        mov     si, msg_map             ; each stage says it started, so a board
        call    rprint                  ; that stops here shows where
        ; The BIOS memory map (INT 15h, AX=E820h) for the harness, at
        ; 0000:E820_AT: a status word, an entry count, then 24-byte
        ; entries. The status is itself a result (a BIOS without the call
        ; is no fault of the CPU), so every way the call can fail ends the
        ; loop with its own status instead of a hang or a bad table:
        ; 1 read, 2 no such call (CF at once), 3 not "SMAP" back, 4 more
        ; than E820_MAX entries, 5 an entry shorter than 20 bytes.
        xor     ax, ax                  ; (rprint above left AX 0E20h)
        mov     ds, ax
        mov     es, ax
        mov     dword [E820_AT], 0
        mov     di, E820_AT + 8
        xor     ebx, ebx
        sti
.e820:
        mov     eax, 0xe820
        mov     edx, 0x534d4150         ; "SMAP"
        mov     ecx, 24
        mov     dword [di + 20], 1      ; ACPI 3 attributes: valid unless the BIOS writes them
        int     0x15
        mov     si, 0                   ; DS again, whatever the BIOS left in it
        mov     ds, si
        jc      .e820_cf
        cmp     eax, 0x534d4150
        jne     .e820_sig
        cmp     ecx, 20
        jb      .e820_short
        inc     word [E820_AT + 2]
        add     di, 24
        test    ebx, ebx
        jz      .e820_ok
        cmp     word [E820_AT + 2], E820_MAX
        jb      .e820
        mov     word [E820_AT], 4
        jmp     .e820_done
.e820_cf:                               ; CF after some entries: the end of the list
        cmp     word [E820_AT + 2], 0
        jne     .e820_ok
        mov     word [E820_AT], 2
        jmp     .e820_done
.e820_sig:
        mov     word [E820_AT], 3
        jmp     .e820_done
.e820_short:
        mov     word [E820_AT], 5
        jmp     .e820_done
.e820_ok:
        mov     word [E820_AT], 1
.e820_done:
        mov     al, [E820_AT]           ; the status, as a digit
        add     al, '0'
        call    rputch
        mov     si, msg_a20
        call    rprint

        ; A20, the way Linux does it: on already (many BIOSes leave it so)?
        ; else the BIOS (INT 15h AX=2401h), port 92h (fast A20), and last
        ; the keyboard controller, a wrap test after each; the message says
        ; which one did it. The keyboard controller only when the others
        ; didn't: some boards (ASUS with Award Medallion 6.0) reset soon
        ; after its output port is written.
        mov     si, msg_a20_was
        call    a20_on
        jnc     .a20_ok
        mov     ax, 0x2401
        int     0x15
        cli
        mov     si, msg_a20_bios
        call    a20_on
        jnc     .a20_ok
        in      al, 0x92
        or      al, 0x02
        and     al, 0xfe
        out     0x92, al
        mov     si, msg_a20_92
        call    a20_on
        jnc     .a20_ok
        call    kbc_wait
        mov     al, 0xd1
        out     0x64, al
        call    kbc_wait
        mov     al, 0xdf
        out     0x60, al
        call    kbc_wait
        mov     si, msg_a20_kbc
        call    a20_on
        jnc     .a20_ok
        mov     si, msg_off
        call    rprint
.stop:  hlt                             ; nothing sensible runs with A20 off
        jmp     .stop
.a20_ok:
        call    rprint
        mov     si, msg_pm
        call    rprint

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

        ; A protected-mode IDT before the switch, and NMI off across it: an
        ; exception or NMI between here and the harness's own IDT then marks
        ; 'F' and halts, where the real-mode IVT would triple-fault and
        ; reset the machine. 32 gates at 0000:6000, all to pm_fault.
        mov     al, 0x8d                ; NMI off (port 70h bit 7)
        out     0x70, al
        xor     ax, ax
        mov     ds, ax
        lea     edx, [ebp + pm_fault]
        mov     di, IDT_AT
        mov     cx, 32
.idt:   mov     [di], dx
        mov     word [di + 2], 0x08
        mov     word [di + 4], 0x8e00
        ror     edx, 16
        mov     [di + 6], dx
        ror     edx, 16
        add     di, 8
        loop    .idt
        sub     sp, 8
        mov     bx, sp
        mov     word [ss:bx], 32 * 8 - 1
        mov     dword [ss:bx + 2], IDT_AT
        o32 lidt [ss:bx]
        add     sp, 8

        mov     eax, cr0
        or      al, 1
        mov     cr0, eax

        ; Far return into 32-bit code at the ROM's linear address.
        push    dword 0x08
        lea     eax, [ebp + pm32]
        push    eax
        o32 retf

; Real-mode messages through the BIOS; the strings are in this segment.
rprint:
        push    ds
        push    cs
        pop     ds
.next:  lodsb
        test    al, al
        jz      .done
        call    rputch
        jmp     .next
.done:  pop     ds
        ret

rputch:
        push    bx
        mov     ah, 0x0e
        mov     bx, 0x0007
        int     0x10
        pop     bx
        ret

; CF clear if A20 is on: 0000:0500 and FFFF:0510 are the same byte with it
; off.
a20_on:
        push    ds
        push    es
        xor     ax, ax
        mov     ds, ax
        not     ax
        mov     es, ax
        mov     bl, [0x0500]
        mov     bh, [es:0x0510]
        mov     byte [0x0500], 0x00
        mov     byte [es:0x0510], 0xff
        cmp     byte [0x0500], 0xff
        mov     [es:0x0510], bh
        mov     [0x0500], bl
        pop     es
        pop     ds
        je      .off
        clc
        ret
.off:   stc
        ret

msg_map db "CPUTEST: memory map ", 0
msg_a20 db ", A20 ", 0
msg_a20_was  db "on already", 0
msg_a20_bios db "on by the BIOS", 0
msg_a20_92   db "on by port 92h", 0
msg_a20_kbc  db "on by the keyboard controller", 0
msg_off db "OFF: cannot run", 13, 10, 0
msg_pm  db ", protected mode", 13, 10, 0

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
        mov     al, 'A'                 ; in protected mode
        call    mark

        lea     esi, [ebp + payload]
        mov     edi, PAYLOAD_BASE
        mov     ecx, (payload_end - payload + 3) / 4
        rep     movsd
        mov     al, 'B'                 ; the payload copied
        call    mark

        ; The payload's own GDT replaces this one; tell it where the ROM is.
        mov     eax, ebp
        jmp     0x08:PAYLOAD_BASE

; An exception or NMI before the harness has its own IDT: 'F' in the marks'
; place (column 79) and on COM1, then stop.
pm_fault:
        mov     ax, 0x10
        mov     ds, ax
        mov     word [0xb8000 + (24 * 80 + 79) * 2], 0x4f46
        mov     dx, 0x3f8
        mov     al, 'F'
        out     dx, al
.stop:  cli
        hlt
        jmp     .stop

; A start-up stage passed: its letter, white on red, at the bottom right
; of the text screen (column 76 on), and out of COM1 as it is. A board that
; stops before the harness prints shows how far it got.
mark:
        movzx   edx, al
        sub     edx, 'A'
        mov     ah, 0x4f
        mov     [0xb8000 + (24 * 80 + 76) * 2 + edx * 2], ax
        mov     dx, 0x3f8
        out     dx, al
        ret

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

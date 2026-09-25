; CPU accuracy test disk: the boot sector.
;
; The BIOS loads this to 0000:7C00. It reads the test ROM image (the same
; one that goes on an ISA ROM card) from the sectors after it into 2000:0000
; and far-calls its entry at offset 3, exactly as a BIOS calls an option
; ROM. Reads use the INT 13h extensions where the BIOS has them, else CHS
; one sector at a time with the drive's own geometry.

bits 16
org 0x7c00

LOAD_SEG equ 0x2000                     ; 20000h: some BIOSes (ASUS, Award Medallion 6.0) use 1000:0000

start:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7c00
        sti
        mov     [drive], dl

        mov     si, msg_load
        call    print

        ; INT 13h extensions?
        mov     ah, 0x41
        mov     bx, 0x55aa
        mov     dl, [drive]
        int     0x13
        jc      chs
        cmp     bx, 0xaa55
        jne     chs
        test    cl, 1
        jz      chs

        ; Extended reads, 32 sectors at a time: some BIOSes take no more
        ; than 127 in one call.
.ext:
        mov     si, dap
        mov     ah, 0x42
        mov     dl, [drive]
        int     0x13
        jc      chs
        add     word [dap + 6], 32 * 512 / 16   ; next segment
        add     word [dap + 8], 32              ; next LBA
        sub     word [left], 32
        ja      .ext
        jmp     loaded

chs:
        ; Geometry, then sector by sector.
        mov     ah, 0x08
        mov     dl, [drive]
        push    es
        int     0x13
        pop     es
        jc      fail
        and     cx, 0x3f
        mov     [spt], cx
        movzx   ax, dh
        inc     ax
        mov     [heads], ax

        mov     word [lba], 1
        mov     word [dest], LOAD_SEG
.next:
        mov     ax, [lba]
        xor     dx, dx
        div     word [spt]              ; ax = track, dx = sector - 1
        mov     cl, dl
        inc     cl
        xor     dx, dx
        div     word [heads]            ; ax = cylinder, dx = head
        mov     ch, al
        shl     ah, 6
        or      cl, ah
        mov     dh, dl
        mov     dl, [drive]
        mov     bx, [dest]
        mov     es, bx
        xor     bx, bx
        mov     ax, 0x0201
        int     0x13
        jc      fail
        add     word [dest], 0x20       ; 512 bytes
        inc     word [lba]
        mov     ax, [lba]
        cmp     ax, IMAGE_SECTORS + 1
        jbe     .next

loaded:
        ; What was read must be what was written: the 16-bit sum of all its
        ; words against the one build.py stored. A BIOS that reads wrong
        ; data, or uses this memory, stops here with a message.
        mov     bx, LOAD_SEG
        mov     cx, IMAGE_SECTORS
        xor     dx, dx
.sum:   mov     es, bx
        xor     di, di
        push    cx
        mov     cx, 256
.word:  add     dx, [es:di]
        add     di, 2
        loop    .word
        pop     cx
        add     bx, 0x20                ; the next sector
        loop    .sum
        cmp     dx, IMAGE_SUM
        je      .good
        mov     si, msg_bad
        call    print
        jmp     fail.halt
.good:
        mov     si, msg_run
        call    print
        call    LOAD_SEG:0x0003         ; as a BIOS calls an option ROM
fail:
        mov     si, msg_fail
        call    print
fail.halt:
        cli
        hlt
        jmp     fail.halt

print:
        lodsb
        test    al, al
        jz      .done
        mov     ah, 0x0e
        mov     bx, 0x0007
        int     0x10
        jmp     print
.done:  ret

msg_load db "CPUTEST: loading", 13, 10, 0
msg_run  db "CPUTEST: starting", 13, 10, 0
msg_fail db "CPUTEST: disk read failed", 13, 10, 0
msg_bad  db "CPUTEST: image damaged in memory", 13, 10, 0

drive   db 0
        align 2
spt     dw 0
heads   dw 0
lba     dw 0
dest    dw 0

left    dw IMAGE_SECTORS

        align 4
dap:
        db      0x10, 0
        dw      32
        dw      0, LOAD_SEG             ; offset, segment
        dq      1                       ; from LBA 1

        times 446 - ($ - $$) db 0
        ; One active partition over the image, for BIOSes that want one.
        db      0x80, 0, 2, 0, 0xda, 0, 0, 0
        dd      1, IMAGE_SECTORS
        times 510 - ($ - $$) db 0
        db      0x55, 0xaa

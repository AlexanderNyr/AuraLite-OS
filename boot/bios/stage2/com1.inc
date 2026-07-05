; =============================================================================
; boot/bios/stage2/com1.inc -- 16-bit COM1 (UART 16550) driver.
;
; Included by stage2_start.asm.  All routines preserve every register
; except AX/DX unless documented otherwise.  Only used before we jump
; to the kernel; the kernel installs its own UART driver from
; drivers/uart/uart.c.
;
; COM1 base is 0x3F8 (IBM AT convention, always at this port in QEMU).
; =============================================================================

COM1_BASE   equ 0x3F8
COM1_DATA   equ COM1_BASE + 0
COM1_IER    equ COM1_BASE + 1
COM1_FCR    equ COM1_BASE + 2
COM1_LCR    equ COM1_BASE + 3
COM1_MCR    equ COM1_BASE + 4
COM1_LSR    equ COM1_BASE + 5

; com1_init -- 115200 8N1, FIFO enabled, DLAB manipulation.
com1_init:
    push ax
    push dx
    ; Disable interrupts on this UART.
    mov  dx, COM1_IER
    xor  al, al
    out  dx, al
    ; DLAB=1, so we can write the divisor.
    mov  dx, COM1_LCR
    mov  al, 0x80
    out  dx, al
    ; Divisor = 1 -> 115200 baud (base clock 1843200 Hz / 16 / 115200 = 1).
    mov  dx, COM1_DATA          ; DLL when DLAB=1
    mov  al, 0x01
    out  dx, al
    mov  dx, COM1_IER           ; DLM when DLAB=1
    xor  al, al
    out  dx, al
    ; 8N1, DLAB=0.
    mov  dx, COM1_LCR
    mov  al, 0x03
    out  dx, al
    ; Enable & clear FIFOs, 14-byte threshold.
    mov  dx, COM1_FCR
    mov  al, 0xC7
    out  dx, al
    ; DTR + RTS on, OUT2 on (needed under some virtualisers).
    mov  dx, COM1_MCR
    mov  al, 0x0B
    out  dx, al
    pop  dx
    pop  ax
    ret

; com1_putc -- write AL to COM1, waiting for the transmit holding register
; to be empty (LSR bit 5).
com1_putc:
    push ax
    push dx
    push cx
    mov  cx, ax                 ; save char in CL
    mov  dx, COM1_LSR
.wait:
    in   al, dx
    test al, 0x20               ; THR empty?
    jz   .wait
    mov  al, cl
    mov  dx, COM1_DATA
    out  dx, al
    pop  cx
    pop  dx
    pop  ax
    ret

; com1_puts -- write zero-terminated string at DS:SI to COM1.
com1_puts:
    push ax
    push si
.loop:
    lodsb
    test al, al
    jz   .done
    call com1_putc
    jmp  .loop
.done:
    pop  si
    pop  ax
    ret

; com1_puthex16 -- print AX as four uppercase hex digits.
com1_puthex16:
    push ax
    push bx
    push cx
    mov  bx, ax
    mov  cx, 4
.loop:
    rol  bx, 4
    mov  ax, bx
    and  al, 0x0F
    cmp  al, 10
    jl   .digit
    add  al, 'A' - 10
    jmp  .out
.digit:
    add  al, '0'
.out:
    call com1_putc
    loop .loop
    pop  cx
    pop  bx
    pop  ax
    ret

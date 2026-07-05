; =============================================================================
; boot/bios/stage2/uart16.inc -- 16-bit UART 16550 serial driver.
;
; Included by stage2_start.asm.  All routines preserve every register
; except AX/DX unless documented otherwise.  Only used before we jump
; to the kernel; the kernel installs its own UART driver from
; drivers/uart/uart.c.
;
; UART base is 0x3F8 (IBM AT first-serial-port convention in QEMU).
; =============================================================================

UART16_BASE   equ 0x3F8
UART16_DATA   equ UART16_BASE + 0
UART16_IER    equ UART16_BASE + 1
UART16_FCR    equ UART16_BASE + 2
UART16_LCR    equ UART16_BASE + 3
UART16_MCR    equ UART16_BASE + 4
UART16_LSR    equ UART16_BASE + 5

; uart16_init -- 115200 8N1, FIFO enabled, DLAB manipulation.
uart16_init:
    push ax
    push dx
    ; Disable interrupts on this UART.
    mov  dx, UART16_IER
    xor  al, al
    out  dx, al
    ; DLAB=1, so we can write the divisor.
    mov  dx, UART16_LCR
    mov  al, 0x80
    out  dx, al
    ; Divisor = 1 -> 115200 baud (base clock 1843200 Hz / 16 / 115200 = 1).
    mov  dx, UART16_DATA          ; DLL when DLAB=1
    mov  al, 0x01
    out  dx, al
    mov  dx, UART16_IER           ; DLM when DLAB=1
    xor  al, al
    out  dx, al
    ; 8N1, DLAB=0.
    mov  dx, UART16_LCR
    mov  al, 0x03
    out  dx, al
    ; Enable & clear FIFOs, 14-byte threshold.
    mov  dx, UART16_FCR
    mov  al, 0xC7
    out  dx, al
    ; DTR + RTS on, OUT2 on (needed under some virtualisers).
    mov  dx, UART16_MCR
    mov  al, 0x0B
    out  dx, al
    pop  dx
    pop  ax
    ret

; uart16_putc -- write AL to the UART, waiting for the transmit holding register
; to be empty (LSR bit 5).
uart16_putc:
    push ax
    push dx
    push cx
    mov  cx, ax                 ; save char in CL
    mov  dx, UART16_LSR
.wait:
    in   al, dx
    test al, 0x20               ; THR empty?
    jz   .wait
    mov  al, cl
    mov  dx, UART16_DATA
    out  dx, al
    pop  cx
    pop  dx
    pop  ax
    ret

; uart16_puts -- write zero-terminated string at DS:SI to the UART.
uart16_puts:
    push ax
    push si
.loop:
    lodsb
    test al, al
    jz   .done
    call uart16_putc
    jmp  .loop
.done:
    pop  si
    pop  ax
    ret

; uart16_puthex16 -- print AX as four uppercase hex digits.
uart16_puthex16:
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
    call uart16_putc
    loop .loop
    pop  cx
    pop  bx
    pop  ax
    ret

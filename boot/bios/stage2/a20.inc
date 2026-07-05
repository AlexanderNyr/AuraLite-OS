; =============================================================================
; boot/bios/stage2/a20.inc -- enable the A20 address line.
;
; Legacy 8086 systems mask address bit 20 for backward compatibility;
; every modern OS needs it enabled to access memory above 1 MiB.  We
; try three methods in order of decreasing safety:
;   1) BIOS INT 15h AX=2401h   -- clean, but not universally supported.
;   2) 8042 keyboard-controller output-port bit 1.
;   3) "Fast A20" via port 0x92 bit 1 (present on nearly every PC
;      chipset since the mid-90s including QEMU/Bochs/VirtualBox).
;
; After each attempt we re-run check_a20 and stop as soon as it reports
; success.  If all three fail we simply return -- the CPU will #GP
; later when it accesses >1 MiB, giving an obvious symptom.
; =============================================================================

; enable_a20 -- best-effort enable A20; no meaningful return value.
enable_a20:
    pusha
    call check_a20
    jnz  .done                          ; already enabled -- most PCs

    ; Method 1: BIOS INT 15h AX=2401h.
    mov  ax, 0x2401
    int  0x15
    call check_a20
    jnz  .done

    ; Method 3: Fast A20 (port 0x92 bit 1 set, bit 0 cleared).
    ; We put the port-0x92 method BEFORE the 8042 method because it
    ; never blocks -- some virtualised 8042s (looking at you, certain
    ; BIOS+chipset combos) leave the "output buffer full" bit stuck
    ; and a20_via_kbd would loop forever waiting for it to clear.
    in   al, 0x92
    or   al, 0x02
    and  al, 0xFE
    out  0x92, al
    call check_a20
    jnz  .done

    ; Method 2: 8042 keyboard controller.  Last resort because of the
    ; wait-loop hazard above.  We put a bounded number of retries on
    ; every 8042 wait so we can never spin forever.
    call a20_via_kbd
    call check_a20
    ; whether or not this last attempt worked, fall through -- BL3
    ; continues, and BL4 will detect the failure when it first tries
    ; to access memory above 1 MiB.
.done:
    popa
    ret

; ---------------------------------------------------------------------------
; check_a20 -- returns AX=1 in AX if A20 is on, AX=0 if masked.
; Also sets ZF accordingly (NZ = ON, EQ = OFF) so callers can use
; either "test ax, ax / jnz" or the ZF form.
;
; Trick: write two different bytes to physical 0x000500 and 0x100500.
; Without A20 the second address wraps to the first and both cells hold
; the same byte; with A20 they hold distinct values.
; ---------------------------------------------------------------------------
check_a20:
    push ds
    push es
    push si
    push di
    push cx
    push dx

    xor  ax, ax
    mov  es, ax                         ; ES = 0
    not  ax                             ; AX = 0xFFFF
    mov  ds, ax                         ; DS = 0xFFFF (so DS:0x510 = phys 0x100500)

    mov  di, 0x500
    mov  si, 0x510

    ; Save originals.
    mov  cl, [es:di]                    ; original @ 0x000500
    mov  ch, [ds:si]                    ; original @ 0x100500 (or aliased)

    ; Write a distinct pattern to each virtual address.
    mov  byte [es:di], 0x00
    mov  byte [ds:si], 0xFF
    ; If A20 is masked, both writes land on the same physical byte and
    ; [es:di] now reads 0xFF.  If A20 is enabled they hit distinct
    ; bytes and [es:di] still reads 0x00.
    mov  dl, [es:di]

    ; Restore originals BEFORE branching so we always leave memory clean.
    mov  [es:di], cl
    mov  [ds:si], ch

    ; Decide return value.
    xor  ax, ax
    cmp  dl, 0xFF                       ; ZF=1 => aliased (FAIL)
    je   .off
    inc  ax                             ; AX = 1 (ON); also clears ZF.
    jmp  .done
.off:
    ; AX already 0; ensure ZF=1.
    or   ax, ax
.done:
    pop  dx
    pop  cx
    pop  di
    pop  si
    pop  es
    pop  ds
    ret

; ---------------------------------------------------------------------------
; a20_via_kbd -- classic 8042 output-port toggle.
;
; Command 0xAD/0xAE disable/enable the keyboard so we can drive the
; controller ports without contention.  Command 0xD0/0xD1 read/write
; the "output port" byte; bit 1 of that byte is A20.
; ---------------------------------------------------------------------------
a20_via_kbd:
    call .kbd_wait_in
    mov  al, 0xAD                       ; disable keyboard
    out  0x64, al

    call .kbd_wait_in
    mov  al, 0xD0                       ; read output port
    out  0x64, al
    call .kbd_wait_out
    in   al, 0x60
    or   al, 0x02                       ; A20 = 1
    push ax

    call .kbd_wait_in
    mov  al, 0xD1                       ; write output port
    out  0x64, al
    call .kbd_wait_in
    pop  ax
    out  0x60, al

    call .kbd_wait_in
    mov  al, 0xAE                       ; re-enable keyboard
    out  0x64, al
    call .kbd_wait_in
    ret

; Bounded busy-wait: up to ~65535 IN opcodes (~ms on real hardware,
; instant in emulators).  Guarantees enable_a20 cannot hang forever.
.kbd_wait_in:
    mov  cx, 0xFFFF
.kwi_loop:
    in   al, 0x64
    test al, 0x02                       ; input buffer full?
    jz   .kwi_done
    loop .kwi_loop
.kwi_done:
    ret

.kbd_wait_out:
    mov  cx, 0xFFFF
.kwo_loop:
    in   al, 0x64
    test al, 0x01                       ; output buffer full?
    jnz  .kwo_done
    loop .kwo_loop
.kwo_done:
    ret

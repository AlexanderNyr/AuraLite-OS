; =============================================================================
; boot/bios/stage2/lmcheck.inc -- long-mode capability check (I386_PLAN I0).
;
; The 64-bit path in longmode.inc executes WRMSR against IA32_EFER and
; far-jumps into an L-bit code segment without ever asking the CPU whether
; it implements either.  On a CPU without long mode that sequence is
; undefined at best (#UD on WRMSR) and a silent triple fault at worst --
; measured on qemu-system-i386: the log ends at
; "[BL4] entering long mode; jumping to kernel _start" and nothing
; further ever appears.  Every 32-bit machine that boots this image today
; presents as a hang with no diagnostic.
;
; This module answers one question, in real mode, before anything is
; committed: does this CPU implement x86_64 long mode?
;
; Sequence (Intel SDM Vol.2 CPUID; AMD APM Vol.3 E.4.2):
;   1. EFLAGS.ID (bit 21) toggle test -- a CPU where the bit cannot be
;      flipped has no CPUID instruction at all (i486 and earlier).
;   2. CPUID leaf 0x80000000 must report at least 0x80000001, otherwise
;      the extended-feature leaf does not exist (Pentium through P6).
;   3. CPUID leaf 0x80000001: EDX bit 29 (LM) is long mode itself.
;
; All three failures land in the same place: CF=1, no long mode.  The
; caller decides what to do with that -- phase I0 prints an honest
; refusal; phase I1 tries KERNEL32.ELF first.
;
; Public entry
; ------------
;   check_long_mode
;       On return: CF=0  long mode available.
;                  CF=1  no CPUID / no extended leaves / no LM bit.
;       Trashes EAX, ECX (CPUID also clobbers EBX, EDX -- preserved here).
; =============================================================================

check_long_mode:
    push ebx
    push edx

    ; ---- 1. Is CPUID implemented?  EFLAGS.ID must be toggleable. ----
    pushfd
    pop  eax                        ; EAX = current EFLAGS
    mov  ecx, eax                   ; ECX = original copy
    xor  eax, (1 << 21)             ; flip ID
    push eax
    popfd                           ; try to write it back
    pushfd
    pop  eax                        ; re-read
    push ecx
    popfd                           ; restore original EFLAGS either way
    xor  eax, ecx
    test eax, (1 << 21)
    jz   .no_lm                     ; bit would not flip -> no CPUID

    ; ---- 2. Does the extended-feature leaf exist? ----
    mov  eax, 0x80000000
    cpuid
    cmp  eax, 0x80000001
    jb   .no_lm                     ; 32-bit CPUs typically report 0x80000004
                                    ; or nothing above 0x80000000 here

    ; ---- 3. The LM bit itself. ----
    mov  eax, 0x80000001
    cpuid
    test edx, (1 << 29)             ; EDX.LM
    jz   .no_lm

    pop  edx
    pop  ebx
    clc
    ret

.no_lm:
    pop  edx
    pop  ebx
    stc
    ret

; ---------------------------------------------------------------------------
; check_i686 -- the 32-bit kernel floor (I386_PLAN I3, decision D1).
;
; The i386 kernel assumes P6-class features: PSE (the direct map is
; built from 4 MiB pages), CMPXCHG8B and CMOV.  A CPU below that floor
; must get the I0 refusal, not a #UD on `mov cr4` three instructions
; before the kernel banner.  Called only after check_long_mode said
; "no", so CPUID presence must be re-established (a 486 reaches here).
;
; On return: CF=0  i686-class, safe for KERNEL32.ELF.
;            CF=1  below the floor -- refuse honestly.
; ---------------------------------------------------------------------------
check_i686:
    push ebx
    push ecx
    push edx

    ; CPUID present?  (EFLAGS.ID toggle; a 486 fails right here.)
    pushfd
    pop  eax
    mov  ecx, eax
    xor  eax, (1 << 21)
    push eax
    popfd
    pushfd
    pop  eax
    push ecx
    popfd
    xor  eax, ecx
    test eax, (1 << 21)
    jz   .below_floor

    ; Leaf 1 EDX: PSE (bit 3), CX8 (bit 8), CMOV (bit 15).
    mov  eax, 1
    cpuid
    test edx, (1 << 3)
    jz   .below_floor
    test edx, (1 << 8)
    jz   .below_floor
    test edx, (1 << 15)
    jz   .below_floor

    pop  edx
    pop  ecx
    pop  ebx
    clc
    ret

.below_floor:
    pop  edx
    pop  ecx
    pop  ebx
    stc
    ret

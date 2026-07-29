; =============================================================================
; boot/bios/stage2/acpi.inc -- locate the RSDP and enumerate CPUs via the
; ACPI MADT (Multiple APIC Description Table).
;
; This is what makes real SMP possible: without it the kernel has no way to
; learn how many logical CPUs exist or their Local APIC IDs (needed to target
; each one with an INIT-SIPI-SIPI sequence), and unconditionally treats the
; machine as single-CPU.
;
; Must run AFTER go_unreal (this code uses FS-relative 32-bit flat addressing
; to read tables that can live anywhere in the low 4 GiB -- the RSDT/XSDT and
; MADT are frequently placed by firmware above the 1 MiB real-mode boundary).
;
; References:
;   ACPI Specification 6.5, s.5.2.5 (RSDP), s.5.2.8 (MADT), s.5.2.12.2
;   (Processor Local APIC Structure).
; =============================================================================

; ---------------------------------------------------------------------------
; acpi_find_rsdp -- search for the "RSD PTR " signature + valid checksum.
;
; The RSDP lives in one of two places (ACPI 6.5 s.5.2.5.1):
;   1. The first 1 KiB of the Extended BIOS Data Area (EBDA). The EBDA
;      segment is stored as a paragraph pointer at real-mode address
;      0x0000:0x040E (in the BIOS Data Area).
;   2. The BIOS read-only memory region 0xE0000 .. 0xFFFFF, on a 16-byte
;      boundary.
;
; On exit: EAX = flat physical address of the RSDP, or 0 if not found.
; ---------------------------------------------------------------------------
acpi_find_rsdp:
    push ds

    ; ---- 1) EBDA base paragraph pointer at 0x0000:0x040E. ----
    xor  ax, ax
    mov  ds, ax
    movzx eax, word [0x040E]
    shl  eax, 4                    ; paragraph -> linear byte address
    test eax, eax
    jz   .try_bios_rom             ; some BIOSes leave this zero; skip to ROM

    mov  ecx, 1024 / 16            ; scan 1 KiB in 16-byte steps
    call .scan_range
    test eax, eax
    jnz  .found

.try_bios_rom:
    ; ---- 2) 0xE0000 .. 0xFFFFF, 16-byte aligned. ----
    mov  eax, 0x000E0000
    mov  ecx, (0x00100000 - 0x000E0000) / 16
    call .scan_range
    test eax, eax
    jnz  .found

    xor  eax, eax                  ; not found
.found:
    pop  ds
    ret

; .scan_range -- EAX = flat start address, ECX = number of 16-byte steps.
; Returns EAX = flat address of a validated match, or 0.  Reads via
; FS-relative flat addressing (unreal mode active), independent of DS/limits.
.scan_range:
    push ebx
    push edx
    push esi
.scan_loop:
    mov  ebx, [fs:eax]
    cmp  ebx, 0x20445352            ; "RSD " (little-endian dword)
    jne  .scan_next
    mov  edx, [fs:eax + 4]
    cmp  edx, 0x20525450            ; "PTR " (little-endian dword)
    jne  .scan_next

    ; Verify the ACPI 1.0 checksum: the first 20 bytes must sum to 0 mod
    ; 256.  Accumulate directly in an 8-bit register so the wrap-around is
    ; free (no separate AND needed).
    push eax                        ; keep the candidate address safe
    xor  ecx, ecx                   ; cl = running checksum
    xor  esi, esi                   ; loop index 0..19
.chk_loop:
    movzx edx, byte [fs:eax + esi]
    add  cl, dl
    inc  esi
    cmp  esi, 20
    jb   .chk_loop
    pop  eax                        ; restore the candidate address
    test cl, cl
    jnz  .scan_next                 ; checksum failed: false positive

    ; Found a valid RSDP at EAX.
    pop  esi
    pop  edx
    pop  ebx
    ret

.scan_next:
    add  eax, 16
    loop .scan_loop
    xor  eax, eax
    pop  esi
    pop  edx
    pop  ebx
    ret

; ---------------------------------------------------------------------------
; acpi_parse_madt -- given a flat RSDP address in EAX, locate the MADT and
; fill boot_info.cpus[] / cpu_count.
;
; On entry: EAX = flat RSDP address (from acpi_find_rsdp), non-zero.
; On exit : boot_info fields populated.  If anything along the way looks
;           malformed, cpu_count is left at 0 -- the caller pre-sets it to 1
;           as a safe single-CPU fallback before calling this routine, and
;           only overwrites it once acpi_parse_madt_body has actually found
;           at least one usable Local APIC entry.
; ---------------------------------------------------------------------------
acpi_parse_madt:
    push ebp
    mov  ebp, esp
    sub  esp, 8
    mov  [ebp - 4], eax             ; [ebp-4] = rsdp flat addr

    ; Publish rsdp_phys (qword; the high dword is always 0 for BIOS boot --
    ; every table we touch here lives below 4 GiB).
    push es
    mov  bx, BOOT_INFO_SEG
    mov  es, bx
    mov  dword [es:BOOT_RSDP_OFF + 0], eax
    mov  dword [es:BOOT_RSDP_OFF + 4], 0
    pop  es

    ; ACPI revision byte at RSDP+15: 0 = ACPI 1.0 (32-bit RSDT ptr at +16),
    ; >=2 = ACPI 2.0+ (prefer the 64-bit XSDT ptr at +24, falling back to
    ; RSDT if XSDT is absent/zero).
    mov  eax, [ebp - 4]
    movzx ecx, byte [fs:eax + 15]
    test ecx, ecx
    jz   .use_rsdt

    mov  edx, [fs:eax + 24]         ; XsdtAddress low dword
    test edx, edx
    jnz  .have_sdt
.use_rsdt:
    mov  edx, [fs:eax + 16]         ; RsdtAddress
    test edx, edx
    jz   .done                      ; no SDT pointer at all: bail out safely
.have_sdt:
    ; EDX = flat address of RSDT/XSDT.  Both share the same 36-byte ACPI
    ; SDT header, followed by an array of pointers to further tables
    ; (4-byte entries for RSDT, 8-byte entries for XSDT -- we only read the
    ; low dword of each XSDT entry since every table we care about lives
    ; below 4 GiB on the platforms AuraLite targets).
    mov  esi, edx                   ; esi = SDT flat address
    mov  eax, [fs:esi + 4]          ; Length field
    sub  eax, 36
    jle  .done                      ; no entries: nothing to enumerate
    mov  [ebp - 8], eax             ; [ebp-8] = bytes of entries

    mov  eax, [fs:esi]              ; signature dword
    cmp  eax, 0x54445352            ; "RSDT" (little-endian)
    je   .stride4
    mov  ebx, 8
    jmp  .have_stride
.stride4:
    mov  ebx, 4
.have_stride:
    mov  ecx, [ebp - 8]             ; ecx = total entry-array bytes
    xor  edi, edi                   ; edi = byte offset into entry array

.entry_loop:
    cmp  edi, ecx
    jae  .done
    mov  eax, [fs:esi + 36 + edi]   ; low dword of this entry (table ptr)
    test eax, eax
    jz   .entry_next
    mov  edx, [fs:eax]              ; candidate table's signature dword
    cmp  edx, 0x43495041            ; "APIC" (little-endian) = MADT sig
    jne  .entry_next
    ; Found the MADT.  EAX = flat MADT address.
    call acpi_parse_madt_body
    jmp  .done

.entry_next:
    add  edi, ebx
    jmp  .entry_loop

.done:
    mov  esp, ebp
    pop  ebp
    ret

; ---------------------------------------------------------------------------
; acpi_parse_madt_body -- EAX = flat MADT address.  Walks the variable-length
; interrupt-controller-structure list and records every "Processor Local
; APIC" entry (type 0) whose Enabled flag bit is set, per ACPI 6.5
; s.5.2.12.2.  Fills boot_info.cpus[]/cpu_count.
;
; MADT layout (on top of the 36-byte common SDT header):
;   +36  dword  Local Interrupt Controller Address (LAPIC MMIO phys base)
;   +40  dword  Flags
;   +44  ...    interrupt controller structures (TLV: type(1) length(1) data)
;
; Local APIC structure (type 0), 8 bytes:
;   +0  byte  Type = 0
;   +1  byte  Length = 8
;   +2  byte  ACPI Processor ID
;   +3  byte  APIC ID (the LAPIC ID SMP wake-up needs)
;   +4  dword Flags (bit 0 = Enabled)
;
; The "Processor Local x2APIC" structure (type 9, used when the APIC ID
; exceeds 255) is deliberately not handled: BOOT_MAX_CPUS is 64 and no
; machine AuraLite targets needs x2APIC IDs, so a plain byte APIC ID always
; suffices.
; ---------------------------------------------------------------------------
acpi_parse_madt_body:
    push ebp
    mov  ebp, esp
    sub  esp, 8
    mov  [ebp - 4], eax             ; [ebp-4] = madt flat addr

    mov  edx, [fs:eax + 4]          ; MADT Length
    lea  ecx, [eax + edx]           ; ecx = flat end address of MADT
    mov  [ebp - 8], ecx             ; [ebp-8] = end address
    lea  esi, [eax + 44]            ; esi = first ICS entry

    xor  edi, edi                   ; edi = count of CPUs recorded so far

.ics_loop:
    mov  ecx, [ebp - 8]
    cmp  esi, ecx
    jae  .done
    ; Need at least 2 bytes (type+length) left to keep parsing safely.
    lea  eax, [esi + 2]
    cmp  eax, ecx
    ja   .done

    movzx eax, byte [fs:esi]        ; ICS type
    movzx edx, byte [fs:esi + 1]    ; ICS length
    test edx, edx
    jz   .done                       ; zero-length entry: malformed, stop
    mov  ebx, edx                    ; ebx = ICS length (survives edx reuse
                                      ; below); used at .ics_next to advance

    cmp  eax, 0                      ; type 0 = Processor Local APIC
    jne  .ics_next
    cmp  edx, 8
    jb   .ics_next                   ; too short to be a real entry

    ; Flags dword is at ICS+4; bit 0 = Enabled.
    mov  eax, [fs:esi + 4]
    test eax, 1
    jz   .ics_next                   ; disabled/not-present CPU: skip

    cmp  edi, 64                     ; BOOT_MAX_CPUS
    jae  .ics_next                   ; table already full: keep scanning
                                      ; (harmless) but stop recording

    ; Record this CPU into boot_info.cpus[edi].
    movzx eax, byte [fs:esi + 2]     ; ACPI Processor ID
    movzx edx, byte [fs:esi + 3]     ; APIC ID

    push es
    mov  cx, BOOT_INFO_SEG
    mov  es, cx
    ; boot_cpu_t is 24 bytes: processor_id(4) lapic_id(4) goto_address(8)
    ; extra_argument(8).  BOOT_CPUS_OFF is the offset of cpus[0].
    push edi
    imul edi, edi, 24
    add  edi, BOOT_CPUS_OFF
    mov  [es:edi + 0], eax           ; processor_id
    mov  [es:edi + 4], edx           ; lapic_id
    mov  dword [es:edi + 8],  0      ; goto_address low
    mov  dword [es:edi + 12], 0      ; goto_address high
    mov  dword [es:edi + 16], 0      ; extra_argument low
    mov  dword [es:edi + 20], 0      ; extra_argument high
    pop  edi
    pop  es

    inc  edi

.ics_next:
    add  esi, ebx
    jmp  .ics_loop

.done:
    ; Publish cpu_count only if we actually found at least one usable
    ; entry; otherwise leave the caller's single-CPU fallback (cpu_count
    ; pre-set to 1) untouched.
    test edi, edi
    jz   .no_cpus
    push es
    mov  bx, BOOT_INFO_SEG
    mov  es, bx
    mov  dword [es:BOOT_CPUCNT_OFF], edi
    pop  es
.no_cpus:
    mov  esp, ebp
    pop  ebp
    ret

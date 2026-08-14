; =============================================================================
; boot/bios/stage2/stage2_start.asm -- AuraLite OS BIOS Stage 2 entry point.
;
; Phase BL3-BL4 of the custom-bootloader plan.  Loaded by the Stage 1 MBR
; at physical address 0x0000:8000 in 16-bit real mode with:
;   * DL      = BIOS boot-drive number.
;   * DS = ES = SS = 0, SP = 0x7C00.
;   * Interrupts enabled.
;
; Stage 2 layout (flat binary, ORG = 0x8000).  Everything is stitched
; together with %include so the whole thing links as a single flat
; image without a linker script.
;
; This file is BL3 scope: real-mode services and the top-level flow.
; The protected-mode + long-mode transition and the ELF loader come in
; BL4.  During BL3 the entry point demonstrates the collected data by
; writing a "[BL3] stage2 alive" banner to the first PC serial port and halting.
; =============================================================================

bits 16
org  0x8000

; --------------------------------------------------------------------------
; Physical memory layout used by the loader
; --------------------------------------------------------------------------
;   0x00007C00  Stage 1 (MBR) -- reclaimable after Stage 2 starts.
;   0x00008000  Stage 2 code + data (this file and included modules).
;   0x00010000  boot_info_t (~9 KiB, reserved through end-of-boot).
;   0x00011000  Stage 2 scratch buffers (FAT sector, VBE info, ...).
;   0x00100000  Loaded kernel PT_LOAD segments.
;   0x00200000  kernel.elf staging buffer (BL4, temporary).
;   0x01000000  Page tables (BL4).
;   0x01800000  initrd.tar (up to 16 MiB, inside PMM's early reserve
;               which the kernel raised to 40 MiB -- see kernel/mm/pmm.c).
;
; The kernel image lives at 0x100000 because the kernel is linked at
; virtual 0xFFFFFFFF80100000 and the higher-half mapping (built in BL4)
; puts that virtual page onto physical 0x100000.
BOOT_INFO_PHYS       equ 0x00010000
STAGE2_SCRATCH_PHYS  equ 0x00011000
INITRD_LOAD_PHYS     equ 0x01800000
INITRD_MAX_BYTES     equ 0x01000000

; Segment/offset used to access the boot_info block from 16-bit code.
; 0x1000:0000 = physical 0x00010000.  Fields inside the ~9 KiB struct are
; addressed as [es:<offset>] with ES = BOOT_INFO_SEG.
BOOT_INFO_SEG       equ 0x1000

; Generated from boot/shared/boot_info.h by tools/gen_boot_offsets.c.
; Keeps BIOS Stage 2 field writes in sync with the C handoff structure.
%include "build/boot_offsets.inc"
%if BOOT_INFO_SIZEOF > 12*1024
    %error "boot_info_t exceeds the 12 KiB Stage 2 zero-fill reservation"
%endif

; --------------------------------------------------------------------------
; Entry
; --------------------------------------------------------------------------
global stage2_entry
stage2_entry:
    cli
    ; DL is the boot drive.  Save it *first*, before any INT that might
    ; clobber it (some BIOSes leave DL alone across teletype INTs but
    ; not all).
    mov  [boot_drive], dl

    ; Re-normalise segments and stack.  Stage 1 already set them but
    ; we cannot rely on their values after intermediate BIOS calls.
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7C00
    sti

    ; UART early-init so subsequent modules can log through uart16_putc.
    call uart16_init
    mov  si, msg_hello
    call uart16_puts

    ; Zero the boot_info_t block via ES:DI = BOOT_INFO_SEG:0000.
    ; The generated BOOT_INFO_SIZEOF guard above ensures the fixed 12 KiB
    ; reservation remains large enough.  12 KiB / 2 = 6144 words.
    push es
    mov  ax, BOOT_INFO_SEG
    mov  es, ax
    xor  di, di
    xor  ax, ax
    mov  cx, (12*1024) / 2
    rep  stosw

    ; ---- Fill the constant fields that BL3 owns (BL4 fills the rest).
    ; Field offsets come from build/boot_offsets.inc, generated with
    ; offsetof(boot_info_t, field) from boot/shared/boot_info.h.

    ; magic: 0x4155524142544C44 -> bytes 44 4C 54 42 41 52 55 41 (LE).
    mov  word [es:BOOT_MAGIC_OFF + 0], 0x4C44
    mov  word [es:BOOT_MAGIC_OFF + 2], 0x4254
    mov  word [es:BOOT_MAGIC_OFF + 4], 0x5241
    mov  word [es:BOOT_MAGIC_OFF + 6], 0x4155

    ; hhdm_offset: qword 0xFFFF800000000000.
    ; Full little-endian bytes: 00 00 00 00 00 80 FF FF.
    ; The low dword stays zero from the block-clear above.
    mov  word [es:BOOT_HHDM_OFF + 4], 0x8000     ; bytes 4..5 = 00 80
    mov  word [es:BOOT_HHDM_OFF + 6], 0xFFFF     ; bytes 6..7 = FF FF

    ; boot_from_uefi = 0 (already zero, but write explicitly for clarity).
    mov  byte [es:BOOT_UEFI_OFF], 0

    ; cpu_count defaults to 1 (BSP-only, single-CPU fallback).  ACPI MADT
    ; parsing after go_unreal below overwrites this with the real logical
    ; CPU count if it can locate and validate the MADT; otherwise this
    ; default stands and the kernel runs BSP-only exactly as before.
    mov  dword [es:BOOT_CPUCNT_OFF], 1
    pop  es

    ; ---- Real-mode services (BL3) ----
    call detect_memory              ; E820 -> boot_info.mmap[]
    mov  si, msg_e820_ok
    call uart16_puts

    call enable_a20                 ; ensure A20 is on
    mov  si, msg_a20_ok
    call uart16_puts

    ; ---- Disk read self-test (BL3.disk) ----------------------------
    ; Read LBA 0 (the boot sector / MBR) back into a scratch buffer at
    ; 0x00011000 and verify the 0x55AA boot signature at byte 510.  This
    ; proves the disk_read_lba helper is wired up and the DAP layout is
    ; correct, independent of where this Stage 2 binary happens to live on
    ; the medium.
    ; (The old check read LBA 1 and compared it against the loaded Stage 2.
    ;  That is correct for `make iso-bios` (Stage 2 at LBA 1) but the dual
    ;  hybrid image from `make iso` puts Stage 2 at LBA 34, so the check
    ;  always printed FAIL there even though the read itself worked.)
    push es
    mov  ax, 0x1100                     ; scratch segment
    mov  es, ax
    xor  bx, bx                         ; ES:BX = 0x1100:0000 = 0x11000
    xor  eax, eax                       ; LBA 0 = boot sector / MBR
    mov  cx, 1
    call disk_read_lba
    pop  es
    jc   .disk_fail
    ; Verify the 0x55AA boot signature at offset 510.
    push ds
    mov  ax, 0x1100
    mov  ds, ax
    mov  al, [ds:510]
    mov  ah, [ds:511]
    pop  ds
    cmp  ax, 0xAA55                     ; little-endian boot signature
    jne  .disk_fail
    mov  si, msg_disk_ok
    call uart16_puts
    jmp  .disk_done
.disk_fail:
    mov  si, msg_disk_fail
    call uart16_puts
.disk_done:

    ; ---- Unreal-mode self-test (BL3.unreal) ------------------------
    ; Enter unreal mode, then write a distinctive sentinel dword at
    ; physical 0x00100000 via FS-relative 32-bit addressing.  The
    ; smoke test dumps memory 0x100000 through the QEMU monitor and
    ; asserts that the sentinel is there -- which is only possible if
    ; FS's hidden descriptor cache was successfully lifted to flat.
    call go_unreal
    ; Write 0xDEADC0DE at flat linear 0x00100000.
    mov  edi, 0x00100000
    mov  eax, 0xDEADC0DE
    mov  [fs:edi], eax
    ; Read it back through FS to be sure the store was seen.
    mov  eax, 0
    mov  eax, [fs:edi]
    cmp  eax, 0xDEADC0DE
    jne  .unreal_fail
    mov  si, msg_unreal_ok
    call uart16_puts
    jmp  .unreal_done
.unreal_fail:
    mov  si, msg_unreal_fail
    call uart16_puts
.unreal_done:

    ; ---- ACPI MADT CPU enumeration (BL9.smp) ------------------------
    ; Locate the RSDP, then walk RSDT/XSDT -> MADT to discover every
    ; enabled Local APIC (one per logical CPU) and its APIC ID.  Requires
    ; unreal mode (FS flat) to read tables that may live above 1 MiB.
    ; boot_info.cpu_count was pre-set to 1 above; acpi_parse_madt only
    ; overwrites it once it has validated at least one usable CPU entry,
    ; so any failure here safely leaves the machine in the existing
    ; BSP-only single-CPU mode.
    call acpi_find_rsdp
    test eax, eax
    jz   .acpi_no_rsdp
    mov  si, msg_acpi_rsdp_ok
    call uart16_puts
    call acpi_parse_madt
    mov  si, msg_acpi_madt_done
    call uart16_puts
    jmp  .acpi_done
.acpi_no_rsdp:
    mov  si, msg_acpi_no_rsdp
    call uart16_puts
.acpi_done:

    ; ---- FAT32 lookup + load self-test (BL4.fat) -------------------
    ; The FAT32 partition can live at LBA 128 (BL5 hybrid MBR image)
    ; or LBA 256 (BL7 dual-boot GPT image; the 34..159 slot right
    ; after the GPT primary array is reserved for Stage 2 itself).
    ; Try both in order and use whichever fat_init accepts.
    mov  eax, 128                         ; BL5 layout
    call fat_init
    jnc  .fat_init_done
    mov  eax, 256                         ; BL7 layout (past GPT + Stage 2)
    call fat_init
    jc   .fat_skip
.fat_init_done:
    mov  si, msg_fat_init_ok
    call uart16_puts

    ; Look up KERNEL.ELF -- 11-byte 8.3, space-padded, uppercase.
    mov  si, name_kernel
    call fat_find
    jc   .fat_no_kernel
    mov  si, msg_fat_found
    call uart16_puts

    ; Load the file to flat 0x00200000 (2 MiB) as a staging buffer.
    ; ELF parsing then copies each PT_LOAD segment to its physical
    ; destination (typically 0x00100000+).  Choosing 0x00200000 as the
    ; staging area keeps a clean 1 MiB window for the kernel image
    ; between STAGE2_SCRATCH_PHYS (0x11000) and KERNEL_LOAD_PHYS
    ; (0x100000) plus the loaded kernel's own extent.
    mov  eax, [fat_result_cluster]
    mov  edx, [fat_result_size]
    mov  edi, 0x00200000
    call fat_load
    jc   .fat_load_fail
    mov  si, msg_fat_load_ok
    call uart16_puts

    ; Parse the ELF and copy its PT_LOAD segments to their physical
    ; addresses.  Requires FS still be in unreal-mode flat form; we
    ; never touched FS after go_unreal, so we are fine.
    mov  eax, 0x00200000
    mov  edx, [fat_result_size]
    call elf_load
    jc   .elf_fail
    mov  si, msg_elf_ok
    call uart16_puts

    ; Load the optional initrd into the upper half of the kernel's fixed
    ; 0..32 MiB early-boot reservation.  The PMM keeps this region allocated,
    ; so the archive remains intact while the VFS serves files from it.
    mov  si, name_initrd
    call fat_find
    jc   .initrd_not_found
    mov  edx, [fat_result_size]
    test edx, edx
    jz   .initrd_not_found
    cmp  edx, INITRD_MAX_BYTES
    ja   .initrd_too_large
    mov  eax, [fat_result_cluster]
    mov  edi, INITRD_LOAD_PHYS
    call fat_load
    jc   .initrd_load_fail

    ; Publish the physical address and byte size in boot_info_t.  The high
    ; dwords are zero because BIOS Stage 2 only loads below 4 GiB.
    push es
    mov  ax, BOOT_INFO_SEG
    mov  es, ax
    mov  dword [es:BOOT_INITRD_P_OFF + 0], INITRD_LOAD_PHYS
    mov  dword [es:BOOT_INITRD_P_OFF + 4], 0
    mov  eax, [fat_result_size]
    mov  dword [es:BOOT_INITRD_S_OFF + 0], eax
    mov  dword [es:BOOT_INITRD_S_OFF + 4], 0
    pop  es
    mov  si, msg_initrd_ok
    call uart16_puts
    jmp  .initrd_done

.initrd_not_found:
    mov  si, msg_initrd_missing
    call uart16_puts
    jmp  .initrd_done
.initrd_too_large:
    mov  si, msg_initrd_too_large
    call uart16_puts
    jmp  .initrd_done
.initrd_load_fail:
    mov  si, msg_initrd_load_fail
    call uart16_puts
.initrd_done:

    ; Build the 4-level page tables at PT_BASE.  Uses FS (unreal flat).
    call build_page_tables
    mov  si, msg_pt_ok
    call uart16_puts

    ; Announce final hand-off, then take the CPU into long mode.
    ; This call never returns; the last real-mode byte we execute is
    ; the `mov cr0, eax` inside enter_long_mode.
    mov  si, msg_lm_go
    call uart16_puts
    call enter_long_mode
    ; unreachable
    jmp  .fat_done

.elf_fail:
    mov  si, msg_elf_fail
    call uart16_puts
    jmp  .fat_done
.fat_no_kernel:
    mov  si, msg_fat_no_kernel
    call uart16_puts
    jmp  .fat_done
.fat_load_fail:
    mov  si, msg_fat_load_fail
    call uart16_puts
.fat_skip:
.fat_done:

    ; ---- End of BL3+BL4.fat scope ----
    ; BL4 will call FAT init, load kernel/initrd, enter protected mode,
    ; build page tables, enter long mode, and jump to _start.  For now
    ; we announce success and halt so the smoke test can grep for it.
    mov  si, msg_bl3_done
    call uart16_puts

.hang:
    hlt
    jmp  .hang

; --------------------------------------------------------------------------
; Static data
; --------------------------------------------------------------------------
msg_hello:       db 0x0D, 0x0A, "[BL3] AuraLite stage2 alive", 0x0D, 0x0A, 0
msg_e820_ok:     db "[BL3] E820 done",  0x0D, 0x0A, 0
msg_a20_ok:      db "[BL3] A20 gate on", 0x0D, 0x0A, 0
msg_disk_ok:     db "[BL3] disk read OK", 0x0D, 0x0A, 0
msg_disk_fail:   db "[BL3] disk read FAIL", 0x0D, 0x0A, 0
msg_unreal_ok:   db "[BL3] unreal mode OK", 0x0D, 0x0A, 0
msg_unreal_fail: db "[BL3] unreal mode FAIL", 0x0D, 0x0A, 0
msg_acpi_rsdp_ok:   db "[BL9] ACPI RSDP found", 0x0D, 0x0A, 0
msg_acpi_no_rsdp:   db "[BL9] ACPI RSDP not found; BSP-only", 0x0D, 0x0A, 0
msg_acpi_madt_done: db "[BL9] ACPI MADT parsed", 0x0D, 0x0A, 0
msg_fat_init_ok: db "[BL4] FAT32 BPB parsed", 0x0D, 0x0A, 0
msg_fat_found:   db "[BL4] kernel.elf located", 0x0D, 0x0A, 0
msg_fat_load_ok: db "[BL4] kernel.elf loaded to 0x00200000", 0x0D, 0x0A, 0
msg_elf_ok:      db "[BL4] ELF PT_LOAD segments copied to phys", 0x0D, 0x0A, 0
msg_elf_fail:    db "[BL4] ELF parse FAILED", 0x0D, 0x0A, 0
msg_initrd_ok:   db "[BL4] initrd.tar loaded to 0x01800000", 0x0D, 0x0A, 0
msg_initrd_missing: db "[BL4] initrd.tar not found; continuing", 0x0D, 0x0A, 0
msg_initrd_too_large: db "[BL4] initrd.tar exceeds 16 MiB; continuing", 0x0D, 0x0A, 0
msg_initrd_load_fail: db "[BL4] initrd.tar load FAILED; continuing", 0x0D, 0x0A, 0
msg_pt_ok:       db "[BL4] page tables built at 0x01000000", 0x0D, 0x0A, 0
msg_lm_go:       db "[BL4] entering long mode; jumping to kernel _start", 0x0D, 0x0A, 0
msg_fat_no_kernel: db "[BL4] kernel.elf NOT FOUND", 0x0D, 0x0A, 0
msg_fat_load_fail: db "[BL4] kernel.elf load FAILED", 0x0D, 0x0A, 0
msg_bl3_done:    db "[BL3] real-mode services complete; halting", 0x0D, 0x0A, 0

; File names in 8.3 uppercase, space-padded, 11 bytes exact.
name_kernel: db "KERNEL  ELF"
name_initrd: db "INITRD  TAR"

boot_drive: db 0

; --------------------------------------------------------------------------
; Included modules (order matters only for symbol resolution)
; --------------------------------------------------------------------------
%include "boot/bios/stage2/uart16.inc"
%include "boot/bios/stage2/e820.inc"
%include "boot/bios/stage2/a20.inc"
%include "boot/bios/stage2/disk.inc"
%include "boot/bios/stage2/unreal.inc"
%include "boot/bios/stage2/acpi.inc"
%include "boot/bios/stage2/fat.inc"
%include "boot/bios/stage2/elf.inc"
%include "boot/bios/stage2/paging.inc"
%include "boot/bios/stage2/longmode.inc"

; --------------------------------------------------------------------------
; Padding: Stage 2 must fit in the 63 KiB the MBR loads.  We pad to a
; multiple of 512 so the disk-image tools do not need to zero-fill.
; --------------------------------------------------------------------------
align 512, db 0

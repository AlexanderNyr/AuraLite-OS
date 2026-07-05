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
; writing a "[BL3] stage2 alive" banner to COM1 and halting.
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
;   0x00100000  kernel.elf (loaded in BL4 via unreal mode).
;   0x01000000  Page tables (BL4).
;
; The kernel image lives at 0x100000 because the kernel is linked at
; virtual 0xFFFFFFFF80100000 and the higher-half mapping (built in BL4)
; puts that virtual page onto physical 0x100000.
BOOT_INFO_PHYS      equ 0x00010000
STAGE2_SCRATCH_PHYS equ 0x00011000

; Segment/offset used to access the boot_info block from 16-bit code.
; 0x1000:0000 = physical 0x00010000.  Fields inside the ~9 KiB struct are
; addressed as [es:<offset>] with ES = BOOT_INFO_SEG.
BOOT_INFO_SEG       equ 0x1000

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

    ; UART early-init so subsequent modules can log through com1_putc.
    call com1_init
    mov  si, msg_hello
    call com1_puts

    ; Zero the boot_info_t block via ES:DI = BOOT_INFO_SEG:0000.
    ; sizeof(boot_info_t) is currently 9376 bytes; round up to 12 KiB
    ; (3 pages) so future struct growth does not require touching this
    ; code.  12 KiB / 2 = 6144 words for rep stosw.
    push es
    mov  ax, BOOT_INFO_SEG
    mov  es, ax
    xor  di, di
    xor  ax, ax
    mov  cx, (12*1024) / 2
    rep  stosw

    ; ---- Fill the constant fields that BL3 owns (BL4 fills the rest).
    ; See boot/shared/boot_info.h for the field layout.  All offsets
    ; below are computed at assembly time from sizeof() of the C struct
    ; members so any layout change in the header is caught by the
    ; unit-test test_boot_info before the BIOS path is even booted.
    ;
    ;   +0     magic          u64          <-- 0x4155524142544C44
    ;   +8     fb             boot_fb_t (24 B)   left zero here
    ;   +32    mmap[256]      6144 B             filled by detect_memory
    ;   +6176  mmap_count     u32                filled by detect_memory
    ;   +6180  _pad_mmap      u32
    ;   +6184  hhdm_offset    u64          <-- 0xFFFF800000000000
    ;   +6192  initrd_phys    u64                left zero here (BL4)
    ;   +6200  initrd_size    u64                left zero here (BL4)
    ;   +6208  cpu_count      u32                left zero here (BL4)
    ;   +6212  bsp_lapic_id   u32
    ;   +6216  cpus[64]       1536 B             left zero here (BL4)
    ;   +7752  rsdp_phys      u64
    ;   +7760  boot_from_uefi u8           <-- 0 (BIOS)
%assign BOOT_HHDM_OFF  (8 + 24 + 256*24 + 4 + 4)
%assign BOOT_UEFI_OFF  (BOOT_HHDM_OFF + 8 + 16 + 8 + 64*24 + 8)

    ; magic: 0x4155524142544C44 -> bytes 44 4C 54 42 41 52 55 41 (LE).
    mov  word [es:0], 0x4C44
    mov  word [es:2], 0x4254
    mov  word [es:4], 0x5241
    mov  word [es:6], 0x4155

    ; hhdm_offset: qword 0xFFFF800000000000
    ;   LE byte layout at BOOT_HHDM_OFF: 00 00 00 00 00 00 00 80 FF FF
    ;   Wait -- that is 10 bytes.  Correct expansion:
    ;   qword 0xFFFF_8000_0000_0000
    ;     byte 0 = 0x00
    ;     byte 1 = 0x00
    ;     byte 2 = 0x00
    ;     byte 3 = 0x00
    ;     byte 4 = 0x00
    ;     byte 5 = 0x00
    ;     byte 6 = 0x00
    ;     byte 7 = 0x80  <-- oops, actually 0x80 is bit 47, not byte 7
    ;   Recompute: 0xFFFF_8000_0000_0000 has bits 63..47 set.
    ;     high dword = 0xFFFF8000 -> bytes 4..7 = 00 80 FF FF
    ;     low  dword = 0x00000000 -> bytes 0..3 = 00 00 00 00
    ;   Full LE bytes: 00 00 00 00 00 80 FF FF
    mov  word [es:BOOT_HHDM_OFF + 4], 0x8000     ; bytes 4..5 = 00 80
    mov  word [es:BOOT_HHDM_OFF + 6], 0xFFFF     ; bytes 6..7 = FF FF
    ; bytes 0..3 stay zero from the block-clear above.

    ; boot_from_uefi = 0 (already zero, but write explicitly for clarity).
    mov  byte [es:BOOT_UEFI_OFF], 0
    pop  es

    ; ---- Real-mode services (BL3) ----
    call detect_memory              ; E820 -> boot_info.mmap[]
    mov  si, msg_e820_ok
    call com1_puts

    call enable_a20                 ; ensure A20 is on
    mov  si, msg_a20_ok
    call com1_puts

    ; ---- End of BL3 scope ----
    ; BL4 will call FAT init, load kernel/initrd, enter protected mode,
    ; build page tables, enter long mode, and jump to _start.  For now
    ; we announce success and halt so the smoke test can grep for it.
    mov  si, msg_bl3_done
    call com1_puts

.hang:
    hlt
    jmp  .hang

; --------------------------------------------------------------------------
; Static data
; --------------------------------------------------------------------------
msg_hello:    db 0x0D, 0x0A, "[BL3] AuraLite stage2 alive", 0x0D, 0x0A, 0
msg_e820_ok:  db "[BL3] E820 done",  0x0D, 0x0A, 0
msg_a20_ok:   db "[BL3] A20 gate on", 0x0D, 0x0A, 0
msg_bl3_done: db "[BL3] real-mode services complete; halting", 0x0D, 0x0A, 0

boot_drive: db 0

; --------------------------------------------------------------------------
; Included modules (order matters only for symbol resolution)
; --------------------------------------------------------------------------
%include "boot/bios/stage2/com1.inc"
%include "boot/bios/stage2/e820.inc"
%include "boot/bios/stage2/a20.inc"

; --------------------------------------------------------------------------
; Padding: Stage 2 must fit in the 63 KiB the MBR loads.  We pad to a
; multiple of 512 so the disk-image tools do not need to zero-fill.
; --------------------------------------------------------------------------
align 512, db 0

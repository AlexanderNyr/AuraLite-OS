; =============================================================================
; boot/bios/stage2/vbe.inc -- RESIDUE3 C3 / RES-12: VBE linear framebuffer.
;
; Until this file existed the BIOS path never left VGA text mode 3, so the
; i386 kernel could never have a framebuffer console (RES-11): boot_info_t.fb
; was always zero on the BIOS path and only UEFI GOP ever filled it.  This
; module closes that asymmetry from the loader side.
;
;   vbe_probe   -- VBE presence (INT 10h AX=4F00), copy the mode list into
;                  the Stage 2 image, then scan it twice: 1024x768x32 first,
;                  800x600x32 second.  Sets NO mode, so every message printed
;                  between probe and commit still lands on the VGA text
;                  console -- the caller prints the selection line on BOTH
;                  consoles while text mode is still live.
;   vbe_commit  -- INT 10h AX=4F02 with bit 14 set (LIN_FA), re-query the
;                  mode block, fill boot_info_t.fb.
;
; Why the block is re-queried after the set: ModeInfoBlock values are what
; the BIOS *promises* for a mode; PhysBasePtr and LinBytesPerScanLine after
; 4F02 are what it actually did.  The published values come from the post-set
; block, so the kernel never inherits a promise the BIOS did not keep.
;
; Failure is never fatal: no VBE, no 32-bpp linear mode, or a refused set
; leaves the machine exactly as it was (text mode 3, fb block zero) -- a
; working machine with a coarser console.
;
; Segment discipline (the reason for the copies below): Stage 2 runs in unreal
; mode with FS and GS lifted flat; DS/ES/SS stay 0 with a 64 KiB limit.  A
; BIOS mode list lives in the BIOS's own segment (often 0xC000), which DS
; cannot reach, and swapping DS for the duration of the scan would cost access
; to every variable in this file.  So both BIOS blocks are copied into buffers
; inside the Stage 2 image (see vbe_list_copy / vbe_block_copy in
; stage2_start.asm) and then addressed with plain DS-relative moves.  FS is
; never touched: build_page_tables later runs 32-bit linear addresses through
; it.
;
; Physical layout used:
;   0x11200  VbeInfoBlock   (512 bytes)  ES = 0x1100, DI = VBE_INFO_OFF
;   0x11400  ModeInfoBlock  (256 bytes)  ES = 0x1100, DI = VBE_MODE_OFF
; =============================================================================

VBE_INFO_SEG        equ 0x1100
VBE_INFO_OFF        equ 0x0200          ; 0x11200
VBE_MODE_OFF        equ 0x0400          ; 0x11400
VBE_LIST_MAX        equ 256             ; 512 bytes of mode numbers is plenty
VBE_MODE_MAX        equ 0xFFFF          ; the list terminator

; ModeInfoBlock field offsets (VBE 2.0 spec s.4).
VBE_MI_ATTR         equ 0
VBE_MI_XRES         equ 18
VBE_MI_YRES         equ 20
VBE_MI_BPP          equ 25
VBE_MI_PHYSBASE     equ 40
VBE_MI_LINPITCH     equ 50
VBE_MI_REDPOS       equ 55
VBE_MI_GREENPOS     equ 57
VBE_MI_BLUEPOS      equ 59

VBE_AT_SUPPORTED    equ 0x0001
VBE_AT_LINEAR       equ 0x0080

; --------------------------------------------------------------------------
; vbe_probe -- find a usable mode.  Returns CF=1 when there is none.  On
; success [vbe_mode] holds the VBE mode number and [vbe_which] says which
; candidate won (1 = 1024x768, 2 = 800x600).  Sets no video mode.
; --------------------------------------------------------------------------
vbe_probe:
    push ds
    push es
    push si
    push di
    push cx
    push dx
    push bx
    push ax

    ; Zero the 512-byte VbeInfoBlock first: a BIOS that fills only part of it
    ; must not leave a stale "VESA" signature from an earlier call.
    mov  ax, VBE_INFO_SEG
    mov  es, ax
    mov  di, VBE_INFO_OFF
    mov  cx, 256
    xor  ax, ax
    rep  stosw

    mov  ax, 0x4F00                    ; controller information
    mov  di, VBE_INFO_OFF
    int  0x10
    cmp  ax, 0x004F
    jne  .fail
    mov  di, VBE_INFO_OFF
    cmp  dword [es:di], 0x41534556     ; "VESA", little-endian
    jne  .fail

    ; The mode list is a far pointer at +14 (offset) / +16 (segment).
    mov  si, [es:di+14]
    test si, si
    jz   .fail
    mov  [vbe_list_off], si
    mov  ax, [es:di+16]
    mov  [vbe_list_seg], ax

    ; ---- copy up to VBE_LIST_MAX entries into the image buffer ----
    mov  es, ax                        ; ES = the BIOS's list segment
    mov  si, [vbe_list_off]
    mov  di, vbe_list_copy             ; DS (0) side of the copy
    mov  cx, VBE_LIST_MAX
.copy:
    mov  ax, [es:si]
    add  si, 2
    mov  [di], ax
    add  di, 2
    cmp  ax, VBE_MODE_MAX
    je   .copied
    dec  cx
    jnz  .copy
    ; Ran out of room without a terminator: pretend we saw one, so the scan
    ; is bounded by a 0xFFFF word rather than by luck.
    mov  word [di], VBE_MODE_MAX
.copied:
    mov  ax, VBE_INFO_SEG              ; ES back to the scratch segment
    mov  es, ax

    ; ---- pass 1: 1024x768x32, pass 2: 800x600x32 ----
    mov  word [vbe_walk], vbe_list_copy
    mov  dl, 1
    call vbe_scan
    jnc  .found
    mov  word [vbe_walk], vbe_list_copy
    mov  dl, 2
    call vbe_scan
    jnc  .found
    jmp  .fail
.found:
    mov  [vbe_which], dl
    clc
    jmp  .out
.fail:
    stc
.out:
    pop  ax
    pop  bx
    pop  dx
    pop  cx
    pop  di
    pop  si
    pop  es
    pop  ds
    ret

; --------------------------------------------------------------------------
; vbe_scan -- walk the mode-list copy for the resolution named by DL.
; In:  [vbe_walk] = list cursor, DL = 1 (1024x768x32) or 2 (800x600x32),
;      ES = VBE_INFO_SEG (INT 10h AX=4F01 fills ES:DI).
; Out: CF=0 and [vbe_mode] set on a hit, CF=1 otherwise.
; The cursor lives in memory, not in a register: INT 10h may clobber any
; general register, and a scan that loses its place because a BIOS reused SI
; is a bug only real hardware would show.
; --------------------------------------------------------------------------
vbe_scan:
    push ax
    push bx
    push cx
    push dx
    push di
    push si
.loop:
    mov  si, [vbe_walk]
    mov  bx, [si]                      ; mode number (DS = 0)
    add  si, 2
    mov  [vbe_walk], si
    cmp  bx, VBE_MODE_MAX
    je   .none
    test bx, bx
    jz   .none

    mov  ax, 0x4F01                    ; mode information
    mov  cx, bx
    mov  di, VBE_MODE_OFF
    int  0x10
    cmp  ax, 0x004F
    jne  .loop
    mov  di, VBE_MODE_OFF

    ; Supported AND linear-framebuffer capable: a banked mode is not a
    ; console, and the kernel has nowhere to draw.
    ; Note the bare [es:di]: VBE_MI_ATTR is 0, and nasm encodes a zero
    ; displacement as NO displacement while mini-asm emits a +0x0 byte --
    ; which the SH4 asm-parity gate would (correctly) report as a
    ; difference.  Writing the no-displacement form keeps both assemblers
    ; byte-identical.
    mov  ax, [es:di]
    and  ax, (VBE_AT_SUPPORTED | VBE_AT_LINEAR)
    cmp  ax, (VBE_AT_SUPPORTED | VBE_AT_LINEAR)
    jne  .loop

    ; 32 bpp: the fb console, the compositor and paging_fb_set_wc all assume
    ; 8:8:8:8.  Anything else is refused here, not discovered as a
    ; wrong-coloured screen.
    cmp  byte [es:di+VBE_MI_BPP], 32
    jne  .loop

    mov  ax, [es:di+VBE_MI_XRES]
    mov  cx, [es:di+VBE_MI_YRES]
    cmp  dl, 1
    jne  .try800
    cmp  ax, 1024
    jne  .loop
    cmp  cx, 768
    jne  .loop
    jmp  .hit
.try800:
    cmp  ax, 800
    jne  .loop
    cmp  cx, 600
    jne  .loop
.hit:
    mov  [vbe_mode], bx
    clc
    jmp  .out
.none:
    stc
.out:
    pop  si
    pop  di
    pop  dx
    pop  cx
    pop  bx
    pop  ax
    ret

; --------------------------------------------------------------------------
; vbe_commit -- set [vbe_mode] with LIN_FA and publish the framebuffer.
; CF=0: mode live, boot_info_t.fb filled.  CF=1: BIOS refused, fb left zero
; and text mode keeps working.
; --------------------------------------------------------------------------
vbe_commit:
    push ds
    push es
    push si
    push di
    push ax
    push bx
    push cx
    push dx

    mov  ax, VBE_INFO_SEG
    mov  es, ax

    mov  ax, 0x4F02                    ; set mode
    mov  bx, [vbe_mode]
    or   bx, 0x4000                    ; bit 14 = linear framebuffer
    int  0x10
    cmp  ax, 0x004F
    jne  .setfail

    ; Re-query: publish what the BIOS has now, not what it advertised.
    mov  ax, 0x4F01
    mov  cx, [vbe_mode]
    mov  di, VBE_MODE_OFF
    int  0x10
    cmp  ax, 0x004F
    jne  .setfail

    ; Copy the 256-byte mode block into the image buffer, so the fb fields
    ; below are readable with DS = 0 while ES points at boot_info_t.
    push ds
    mov  ax, VBE_INFO_SEG
    mov  ds, ax                        ; DS:SI = the BIOS's block
    xor  ax, ax
    mov  es, ax                        ; ES:DI = our copy
    mov  si, VBE_MODE_OFF
    mov  di, vbe_block_copy
    mov  cx, 128                       ; 128 words = 256 bytes
.copy:
    mov  ax, [si]
    mov  [es:di], ax
    add  si, 2
    add  di, 2
    dec  cx
    jnz  .copy
    pop  ds                            ; DS = 0 again

    ; ---- fill boot_info_t.fb ----
    push es
    mov  ax, BOOT_INFO_SEG
    mov  es, ax

    ; phys_base: the block's 32-bit PhysBasePtr zero-extended into the
    ; struct's 64-bit field (a VBE LFB is always below 4 GiB).
    mov  eax, [vbe_block_copy + VBE_MI_PHYSBASE]
    test eax, eax
    jz   .nofb                         ; 0 base = no linear fb at all

    mov  dword [es:BOOT_FB_PHYS_OFF], eax
    mov  dword [es:BOOT_FB_PHYS_OFF + 4], 0

    mov  eax, [vbe_block_copy + VBE_MI_XRES]
    and  eax, 0x0000FFFF
    mov  dword [es:BOOT_FB_WIDTH_OFF], eax
    mov  eax, [vbe_block_copy + VBE_MI_YRES]
    and  eax, 0x0000FFFF
    mov  dword [es:BOOT_FB_HEIGHT_OFF], eax

    ; pitch: LinBytesPerScanLine; 0 means "the BIOS declined to say", and a
    ; 32-bpp LFB row is width*4 by construction.
    movzx eax, word [vbe_block_copy + VBE_MI_LINPITCH]
    test eax, eax
    jnz  .pitch_ok
    mov  eax, [vbe_block_copy + VBE_MI_XRES]
    and  eax, 0x0000FFFF
    shl  eax, 2
.pitch_ok:
    mov  dword [es:BOOT_FB_PITCH_OFF], eax

    mov  byte [es:BOOT_FB_BPP_OFF], 32
    mov  al, [vbe_block_copy + VBE_MI_REDPOS]
    mov  [es:BOOT_FB_RSHIFT_OFF], al
    mov  al, [vbe_block_copy + VBE_MI_GREENPOS]
    mov  [es:BOOT_FB_GSHIFT_OFF], al
    mov  al, [vbe_block_copy + VBE_MI_BLUEPOS]
    mov  [es:BOOT_FB_BSHIFT_OFF], al
    pop  es

    ; Serial-only receipt: the mode switch has already happened, so the VGA
    ; text console no longer exists -- INT 10h teletype here would scribble
    ; into the framebuffer the kernel is about to own.
    mov  si, msg_vbe_ready
    call uart16_puts
    clc
    jmp  .out

.nofb:
    mov  dword [es:BOOT_FB_PHYS_OFF], 0
    mov  dword [es:BOOT_FB_PHYS_OFF + 4], 0
    pop  es
    stc
    jmp  .out

.setfail:
    ; Nothing was published (the block is still all-zero from the Stage 2
    ; clear), so there is nothing to undo -- only to report.
    mov  si, msg_vbe_fail
    call log16_puts
    stc
.out:
    pop  dx
    pop  cx
    pop  bx
    pop  ax
    pop  di
    pop  si
    pop  es
    pop  ds
    ret

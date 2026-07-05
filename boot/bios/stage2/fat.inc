; =============================================================================
; boot/bios/stage2/fat.inc -- minimal FAT32 reader.
;
; Scope
; -----
;   * FAT32 only.
;   * 8.3 short names only (LFN entries skipped).
;   * Root directory only (no subdirectories).
;   * Cluster chains walked one FAT sector at a time with a 1-sector
;     cache -- adequate for tens of MiB of files.
;
; Public entry points
; -------------------
;   fat_init(EAX = partition base LBA)
;       Reads the BPB, records geometry.  CF clear on success.
;
;   fat_find(DS:SI = 11-byte 8.3 name)
;       Scans the root directory for a matching entry.
;       On success: CF=0, [fat_result_cluster] = first cluster,
;                        [fat_result_size]    = file size in bytes.
;       On failure: CF=1.
;
;   fat_load(EAX = first cluster,
;            EDX = file size in bytes,
;            EDI = flat 32-bit destination address)
;       Copies the entire cluster chain to [EDI..EDI+size).  Uses
;       FS = flat (must be in unreal mode).  CF clear on success.
;
; The three functions share a single scratch layout at
;   FAT_SCRATCH_SEG:0000  BPB / one-shot sector
;   FAT_SCRATCH_SEG:0200  cached FAT sector
;   FAT_SCRATCH_SEG:0400  directory / data sector staging
; =============================================================================

FAT_SCRATCH_SEG    equ 0x1100          ; physical 0x11000
FAT_BPB_OFF        equ 0x0000
FAT_FAT_OFF        equ 0x0200
FAT_STAGE_OFF      equ 0x0400          ; 512-byte staging buffer

; ---- State (persistent across calls) ---------------------------------------
fat_partition_lba: dd 0
fat_fat_lba:       dd 0
fat_data_lba:      dd 0
fat_root_cluster:  dd 0
fat_spc:           db 0
                   db 0                ; padding

; Cache: which FAT sector currently sits in FAT_FAT_OFF, or 0xFFFFFFFF.
fat_cached:        dd 0xFFFFFFFF

; Output of fat_find:
fat_result_cluster: dd 0
fat_result_size:    dd 0

; ---------------------------------------------------------------------------
; fat_init
; EAX = partition LBA.  CF=1 on failure, CF=0 on success.
; ---------------------------------------------------------------------------
fat_init:
    push eax
    push ebx
    push ecx
    push edx
    push es

    mov  [fat_partition_lba], eax

    ; Read BPB.
    mov  bx, FAT_SCRATCH_SEG
    mov  es, bx
    mov  bx, FAT_BPB_OFF
    mov  cx, 1
    call disk_read_lba
    jc   .fail

    ; Signature check.
    cmp  word [es:FAT_BPB_OFF + 0x1FE], 0xAA55
    jne  .fail

    ; Sector size must be 512.
    cmp  word [es:FAT_BPB_OFF + 11], 512
    jne  .fail

    ; Extract BPB fields.
    mov  al, [es:FAT_BPB_OFF + 13]
    mov  [fat_spc], al

    movzx eax, word [es:FAT_BPB_OFF + 14]      ; reserved sectors
    add  eax, [fat_partition_lba]
    mov  [fat_fat_lba], eax

    movzx ebx, byte [es:FAT_BPB_OFF + 16]      ; number of FATs
    mov  ecx, [es:FAT_BPB_OFF + 36]            ; FATSz32 (sectors per FAT)
    imul ebx, ecx                              ; total FAT sectors
    add  eax, ebx
    mov  [fat_data_lba], eax                   ; = fat_fat_lba + N*FATSz

    mov  eax, [es:FAT_BPB_OFF + 44]            ; root cluster
    mov  [fat_root_cluster], eax

    mov  dword [fat_cached], 0xFFFFFFFF

    pop  es
    pop  edx
    pop  ecx
    pop  ebx
    pop  eax
    clc
    ret
.fail:
    pop  es
    pop  edx
    pop  ecx
    pop  ebx
    pop  eax
    stc
    ret

; ---------------------------------------------------------------------------
; Internal helper: read one FAT sector into FAT_FAT_OFF, caching by LBA.
; EAX = FAT sector LBA (absolute).  Trashes only flags (returns via clc/stc).
; ---------------------------------------------------------------------------
fat_read_fat_sector:
    cmp  eax, [fat_cached]
    je   .cached
    mov  [fat_cached], eax
    push bx
    push cx
    push es
    mov  bx, FAT_SCRATCH_SEG
    mov  es, bx
    mov  bx, FAT_FAT_OFF
    mov  cx, 1
    call disk_read_lba
    pop  es
    pop  cx
    pop  bx
    ret                                        ; CF from disk_read_lba
.cached:
    clc
    ret

; ---------------------------------------------------------------------------
; Internal: next cluster in the chain.
; EAX = current cluster -> EAX = next cluster (or 0x0FFFFFFF on read err).
; ---------------------------------------------------------------------------
fat_next_cluster:
    push ebx
    push ecx
    push es

    mov  ebx, eax
    shl  ebx, 2                                ; ebx = cluster*4 (byte offset)
    mov  ecx, ebx
    shr  ecx, 9                                ; sector inside FAT
    and  ebx, 0x1FF                            ; offset inside that sector

    mov  eax, [fat_fat_lba]
    add  eax, ecx
    call fat_read_fat_sector
    jc   .err

    mov  ax, FAT_SCRATCH_SEG
    mov  es, ax
    mov  eax, [es:FAT_FAT_OFF + ebx]
    and  eax, 0x0FFFFFFF

    pop  es
    pop  ecx
    pop  ebx
    ret
.err:
    mov  eax, 0x0FFFFFFF                       ; end-of-chain sentinel
    pop  es
    pop  ecx
    pop  ebx
    ret

; ---------------------------------------------------------------------------
; Internal: cluster -> first sector LBA.  EAX = cluster in, LBA out.
; ---------------------------------------------------------------------------
fat_cluster_lba:
    push ebx
    sub  eax, 2
    movzx ebx, byte [fat_spc]
    imul eax, ebx
    add  eax, [fat_data_lba]
    pop  ebx
    ret

; ---------------------------------------------------------------------------
; fat_find
; DS:SI = 11-byte name.  Result in fat_result_cluster / fat_result_size.
; CF clear on match, CF set on not-found or disk error.
; ---------------------------------------------------------------------------
fat_find:
    push eax
    push ebx
    push ecx
    push edx
    push edi
    push es

    mov  eax, [fat_root_cluster]
.cluster_loop:
    ; Save current cluster; convert to first LBA of its sectors.
    mov  [.cur_cluster], eax
    call fat_cluster_lba                       ; EAX -> LBA
    mov  ebx, eax                              ; EBX = base LBA of this cluster

    movzx ecx, byte [fat_spc]                  ; sectors per cluster
    xor  edx, edx                              ; sector index inside cluster
.sect_loop:
    ; Read sector (ebx + edx) into staging buffer.
    push ebx
    push ecx
    push edx
    mov  eax, ebx
    add  eax, edx
    mov  bx, FAT_SCRATCH_SEG
    mov  es, bx
    mov  bx, FAT_STAGE_OFF
    mov  cx, 1
    call disk_read_lba
    pop  edx
    pop  ecx
    pop  ebx
    jc   .fail

    ; Scan 16 entries in this sector.
    xor  di, di                                ; entry index in sector
.entry_loop:
    mov  ax, di
    shl  ax, 5                                 ; ax = entry_index*32
    mov  bp, ax
    add  bp, FAT_STAGE_OFF                     ; BP = offset of this entry

    mov  al, [es:bp + 0]
    test al, al
    jz   .not_found                            ; 0x00 => end of directory
    cmp  al, 0xE5
    je   .next_entry                           ; deleted

    mov  al, [es:bp + 11]
    test al, 0x08                              ; volume label
    jnz  .next_entry
    test al, 0x10                              ; subdirectory
    jnz  .next_entry
    cmp  al, 0x0F                              ; LFN
    je   .next_entry

    ; Compare 11 bytes at [ds:si] with [es:bp].
    push si
    push cx
    mov  cx, 11
.cmp_loop:
    mov  al, [ds:si]
    cmp  al, [es:bp]
    jne  .no_match
    inc  si
    inc  bp
    loop .cmp_loop
    ; Match!
    pop  cx
    pop  si
    ; Restore BP to entry start (loop advanced it by 11).
    mov  ax, di
    shl  ax, 5
    mov  bp, ax
    add  bp, FAT_STAGE_OFF
    movzx eax, word [es:bp + 20]               ; hi16 of first cluster
    shl  eax, 16
    mov  ax, [es:bp + 26]                      ; lo16
    mov  [fat_result_cluster], eax
    mov  eax, [es:bp + 28]                     ; file size (u32)
    mov  [fat_result_size], eax
    pop  es
    pop  edi
    pop  edx
    pop  ecx
    pop  ebx
    pop  eax
    clc
    ret
.no_match:
    pop  cx
    pop  si

.next_entry:
    inc  di
    cmp  di, 16
    jb   .entry_loop

    inc  edx
    cmp  edx, ecx                              ; ecx = spc
    jb   .sect_loop

    ; End of this cluster -- advance chain.
    mov  eax, [.cur_cluster]
    call fat_next_cluster
    cmp  eax, 0x0FFFFFF8
    jae  .not_found
    jmp  .cluster_loop

.not_found:
.fail:
    pop  es
    pop  edi
    pop  edx
    pop  ecx
    pop  ebx
    pop  eax
    stc
    ret

.cur_cluster: dd 0

; ---------------------------------------------------------------------------
; fat_load -- copy cluster chain to a flat 32-bit destination.
; EAX = first cluster, EDX = file size (bytes), EDI = flat linear dest.
; Requires unreal mode (FS holds a flat 4 GiB descriptor).
;
; Strategy: read one sector at a time into the staging buffer at
; FAT_SCRATCH_SEG:FAT_STAGE_OFF, then byte-copy up to 512 bytes into
; FS:[dest], decrement remaining, advance dest.  Simple and slow but
; the kernel image is only a few MiB at boot time.
; ---------------------------------------------------------------------------
fat_load:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi
    push ebp
    push es
    push ds

    mov  [.cur_cluster], eax
    mov  [.remaining],   edx
    mov  [.dest],        edi

.cluster:
    mov  eax, [.cur_cluster]
    call fat_cluster_lba                       ; EAX = first LBA of cluster
    mov  [.base_lba], eax                      ; remember base across the loop

    movzx ecx, byte [fat_spc]                  ; sectors per cluster
    xor  ebx, ebx                              ; sector index within cluster
.sect:
    push ebx
    push ecx

    ; Read absolute LBA (base + sector index) into staging buffer.
    mov  eax, [.base_lba]
    add  eax, ebx
    mov  bx, FAT_SCRATCH_SEG
    mov  es, bx
    mov  bx, FAT_STAGE_OFF
    mov  cx, 1
    call disk_read_lba
    jc   .fail_pop

    ; Determine how many bytes of this sector to copy: min(512, remaining).
    mov  ebp, 512
    cmp  ebp, [.remaining]
    jbe  .have_count
    mov  ebp, [.remaining]
.have_count:

    ; Copy EBP bytes from FAT_SCRATCH_SEG:FAT_STAGE_OFF up to FS:[.dest].
    ; Do it byte-by-byte with 32-bit addressing on the destination side.
    ; We keep DS=0 throughout and use GS temporarily for the source: GS
    ; is one of the two flat 4 GiB segments unreal mode set up, so
    ; reading via [gs:esi] with a 32-bit linear source works too.
    mov  esi, FAT_SCRATCH_SEG * 16 + FAT_STAGE_OFF
    mov  edi, [.dest]
    mov  ecx, ebp                              ; loop counter (bytes)
.copy_loop:
    mov  al, [gs:esi]
    mov  [fs:edi], al
    inc  esi
    inc  edi
    dec  ecx
    jnz  .copy_loop

    ; Advance destination and remaining by EBP.
    mov  [.dest], edi
    sub  [.remaining], ebp

    pop  ecx                                   ; restore cluster-loop count
    pop  ebx                                   ; restore sector index

    ; Done?
    cmp  dword [.remaining], 0
    je   .done

    inc  ebx
    cmp  ebx, ecx
    jb   .sect

    ; End of this cluster -- fetch next in chain.
    mov  eax, [.cur_cluster]
    call fat_next_cluster
    cmp  eax, 0x0FFFFFF8
    jae  .short_read
    mov  [.cur_cluster], eax
    jmp  .cluster

.done:
    pop  ds
    pop  es
    pop  ebp
    pop  edi
    pop  esi
    pop  edx
    pop  ecx
    pop  ebx
    pop  eax
    clc
    ret
.fail_pop:
    pop  ecx
    pop  ebx
.short_read:
.fail:
    pop  ds
    pop  es
    pop  ebp
    pop  edi
    pop  esi
    pop  edx
    pop  ecx
    pop  ebx
    pop  eax
    stc
    ret

.cur_cluster: dd 0
.remaining:   dd 0
.dest:        dd 0
.base_lba:    dd 0

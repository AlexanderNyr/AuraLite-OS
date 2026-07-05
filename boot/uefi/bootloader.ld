/* boot/uefi/bootloader.ld -- linker script for BOOTX64.EFI.
 *
 * Emits a PEI-format (COFF-derived) image that ld.lld converts to
 * PE32+ via `--oformat=pei-x86-64`.  Entry point is efi_main; the
 * subsystem is EFI Application (subsystem = 10).
 *
 * We do NOT set --image-base explicitly; UEFI relocates the image
 * anywhere in memory so a fixed base would only hurt.  The image
 * carries base-relocations because clang emits them for the Windows
 * target by default.
 */

OUTPUT_FORMAT(pei-x86-64)
ENTRY(efi_main)

SECTIONS
{
    . = 0;

    .text : ALIGN(4096) {
        *(.text .text.*)
    }

    .rdata : ALIGN(4096) {
        *(.rdata .rdata.*)
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4096) {
        *(.data .data.*)
        *(.bss .bss.*)
        *(COMMON)
    }

    /* Discard PDB/CV debug leftovers that neither UEFI nor lld needs. */
    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame)
    }
}

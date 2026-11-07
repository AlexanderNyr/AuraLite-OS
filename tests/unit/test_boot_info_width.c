/* tests/unit/test_boot_info_width.c -- the cross-width boot_info_t
 * contract (I386_PLAN I6).
 *
 * boot_info_t is written by 16-bit assembly against offsets generated
 * from ONE compile (tools/gen_boot_offsets.c on the host) and read by
 * TWO kernels at different pointer widths.  The contract is that all
 * three agree on every field offset.
 *
 * This file is compile-only: every claim is a _Static_assert, so
 * "the test ran" IS "the layout matched".  The build system compiles
 * it twice -- --target=x86_64-elf plain, and --target=i686-elf with
 * -malign-double -- and then, as the negative control, compiles it a
 * third time for i686 WITHOUT -malign-double and requires that build
 * to FAIL.  That failing build is a regression test for the actual
 * bug I1 measured: the i386 psABI aligns uint64_t to 4 bytes where
 * AMD64 uses 8, the struct packs differently, and the I1 stub read
 * mmap[] 8 bytes early ("mmap entries: 0").
 *
 * The expected values are the generated build/boot_offsets.h -- the
 * same source of truth Stage 2's assembly consumes, so this test
 * cannot drift from the loader without failing.
 */

#include <stddef.h>

#include "boot/shared/boot_info.h"
#include "build/boot_offsets.h"   /* generated: make boot-offsets */

#define CHECK(field, macro) \
    _Static_assert(offsetof(boot_info_t, field) == (macro), \
                   "boot_info_t." #field " moved: 16-bit loader offsets, " \
                   "the 64-bit kernel and the 32-bit kernel no longer agree")

CHECK(magic,          BOOT_MAGIC_OFF_C);
CHECK(fb,             BOOT_FB_OFF_C);
CHECK(mmap,           BOOT_MMAP_OFF_C);
CHECK(mmap_count,     BOOT_MMAP_CNT_OFF_C);
CHECK(hhdm_offset,    BOOT_HHDM_OFF_C);
CHECK(initrd_phys,    BOOT_INITRD_P_OFF_C);
CHECK(initrd_size,    BOOT_INITRD_S_OFF_C);
CHECK(cpu_count,      BOOT_CPUCNT_OFF_C);
CHECK(bsp_lapic_id,   BOOT_BSP_LAPIC_OFF_C);
CHECK(cpus,           BOOT_CPUS_OFF_C);
CHECK(rsdp_phys,      BOOT_RSDP_OFF_C);
CHECK(boot_from_uefi, BOOT_UEFI_OFF_C);

_Static_assert(sizeof(boot_info_t) == BOOT_INFO_SIZEOF_C,
               "boot_info_t total size changed");

/* Sub-struct layout that the framebuffer console and the mmap walk
 * depend on at both widths. */
/* RESIDUE3 C3 (RES-12): Stage 2 now WRITES the framebuffer block field by
 * field (INT 10h AX=4F01/4F02 is the source), so these numbers stopped being
 * a description and became an interface the assembly depends on.  Pinned as
 * literals on purpose: the generated macros are self-consistent by
 * construction, and would happily follow a boot_fb_t that grew a field --
 * while the hand-written `mov dword [es:BOOT_FB_WIDTH_OFF], eax` in
 * boot/bios/stage2/vbe.inc would silently start writing mid-struct. */
_Static_assert(BOOT_FB_PHYS_OFF_C  == 8,  "boot_fb_t.phys_base moved");
_Static_assert(BOOT_FB_WIDTH_OFF_C == 16, "boot_fb_t.width moved");
_Static_assert(BOOT_FB_HEIGHT_OFF_C== 20, "boot_fb_t.height moved");
_Static_assert(BOOT_FB_PITCH_OFF_C == 24, "boot_fb_t.pitch moved");
_Static_assert(BOOT_FB_BPP_OFF_C   == 28, "boot_fb_t.bpp moved");
_Static_assert(BOOT_FB_RSHIFT_OFF_C== 29, "boot_fb_t.red_shift moved");
_Static_assert(BOOT_FB_GSHIFT_OFF_C== 30, "boot_fb_t.green_shift moved");
_Static_assert(BOOT_FB_BSHIFT_OFF_C== 31, "boot_fb_t.blue_shift moved");

_Static_assert(sizeof(boot_mmap_entry_t) == 24, "mmap entry size");
_Static_assert(sizeof(boot_fb_t) == 24 + 8,     "fb struct size");
_Static_assert(offsetof(boot_mmap_entry_t, type) == 16, "mmap.type");

/* Something for the object file to contain. */
int boot_info_width_contract_holds = 1;

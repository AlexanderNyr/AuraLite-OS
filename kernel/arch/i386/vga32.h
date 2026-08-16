/* kernel/arch/i386/vga32.h -- VGA text-mode console (I386_PLAN I7).
 *
 * The 64-bit kernel gets a linear framebuffer from its loader and
 * draws glyphs; Stage 2's 32-bit path sets no VBE mode, so the
 * machine is still in text mode 3 -- 80x25 cells at phys 0xB8000,
 * reachable through the direct map.  That is not a limitation to
 * apologise for: it is the output path that works on every VGA-class
 * machine with zero mode-setting risk, which is exactly what a
 * bring-up console is for.  VBE graphics are I8 residue and say so
 * in the plan.
 *
 * kprintf32 fans out to BOTH sinks (UART + VGA) once vga32_init has
 * run -- same shape as the 64-bit kprintf's console fan-out.
 */

#ifndef AURALITE_ARCH_I386_VGA32_H
#define AURALITE_ARCH_I386_VGA32_H

void vga32_init(void);
void vga32_putc(char c);
int  vga32_active(void);

#endif /* AURALITE_ARCH_I386_VGA32_H */

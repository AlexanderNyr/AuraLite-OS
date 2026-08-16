/* kernel/arch/i386/kprintf32.h -- minimal kernel console for the i386
 * bring-up (I386_PLAN I2).
 *
 * Deliberately NOT kernel/lib/kprintf.c: that fans out to the
 * framebuffer console and klog ring, which arrive on i386 in I7.  This
 * is the smallest formatted-output core that lets I2/I3 print their
 * self-test verdicts on COM1; it dies when the shared kprintf becomes
 * width-clean in I6 and the i386 kernel adopts it.  Format subset:
 * %s %c %d %u %x %p %%, and that is all the bring-up code uses.
 */

#ifndef AURALITE_ARCH_I386_KPRINTF32_H
#define AURALITE_ARCH_I386_KPRINTF32_H

void uart32_init(void);
void kputc32(char c);
void kputs32(const char *s);
void kprintf32(const char *fmt, ...);

#endif /* AURALITE_ARCH_I386_KPRINTF32_H */

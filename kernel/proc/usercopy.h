#ifndef AURALITE_PROC_USERCOPY_H
#define AURALITE_PROC_USERCOPY_H

#include <stdint.h>
#include <stddef.h>

/*
 * M3 — Fault-recovering uaccess (MATURITY_PLAN.md phase M3)
 *
 * SECURITY CONTRACT:
 *
 *   Every user-space pointer that enters the kernel through a syscall MUST
 *   pass through one of these primitives before the kernel dereferences it:
 *
 *     validate_user_range()  — bounds + page-table check
 *     copy_from_user()       — validate + fault-recovering asm copy (read)
 *     copy_to_user()         — validate + fault-recovering asm copy (write)
 *     copy_string_from_user()— byte-by-byte validate + copy until NUL
 *
 *   No direct dereference of a user pointer is permitted anywhere in the
 *   kernel.  The full M3 audit (grep of syscall.c, socket.c, gui_syscalls.c,
 *   gpu_syscalls.c, clone.c, signal.c, sysvipc.c) confirmed zero violations.
 *
 * TOCTOU SAFETY:
 *
 *   validate_user_range() is an OPTIMISTIC check against the current page
 *   tables.  Between validation and the actual copy, another thread could
 *   munmap the page.  The asm copy primitive (uaccess_copy_asm in
 *   usercopy_fault.asm) is the PESSIMISTIC gate: if a #PF occurs during
 *   REP MOVSB, the exception handler rewrites RIP to the fixup label and
 *   the copy returns -1 instead of panicking.
 *
 *   For large transfers (read/write/readv/writev syscalls), the kernel
 *   uses a bounce-buffer pattern: copy into a kernel-local SYSCALL_IO_CHUNK
 *   buffer first, then act on the kernel buffer.  This eliminates any
 *   window where a user pointer could be dereferenced directly by VFS/net
 *   code.  See syscall_vfs_read/write/pread/pwrite in syscall.c.
 *
 * HOSTILE-POINTER TEST BATTERY:
 *
 *   /tests/usertest (userspace/tests/usertest/usertest.c) exercises 30
 *   hostile-pointer shapes across every syscall category.  The kernel must
 *   return -EFAULT for each; a panic is an M3 failure.
 */

/* Maximum positive-canonical userspace address in the current x86_64 layout.
 * User mappings live below 0x0000800000000000; kernel/HHDM are in the high half.
 */
#define USER_VADDR_TOP 0x0000800000000000ULL

/* Validate that [user_ptr, user_ptr + len) is a canonical userspace range,
 * present in the current address space, marked USER, and writable if
 * write_required != 0.  len == 0 is accepted.
 */
int validate_user_range(const void *user_ptr, uint64_t len, int write_required);

/* Fault-recovering user copies.  They validate the complete range before
 * copying and also recover from a kernel #PF if the mapping changes before or
 * during the actual copy.  Return 0 on success, -1 on failure.
 */
int copy_from_user(void *kernel_dst, const void *user_src, uint64_t len);
int copy_to_user(void *user_dst, const void *kernel_src, uint64_t len);

/* Page-fault recovery hook used by the x86_64 exception handler.  If a kernel
 * #PF occurs while a uaccess copy primitive is active, this rewrites the saved
 * RIP to the copy primitive's fixup label and returns 1. */
int usercopy_recover_fault(uint64_t *saved_rip);

/* Copy a NUL-terminated userspace string into kernel_dst.
 * kernel_dst_size includes the trailing NUL.  Returns 0 on success, -1 if the
 * pointer is invalid or no NUL appears before kernel_dst_size bytes.
 */
int copy_string_from_user(char *kernel_dst, const char *user_src,
                          uint64_t kernel_dst_size);

#endif /* AURALITE_PROC_USERCOPY_H */

#ifndef AURALITE_PROC_USERCOPY_H
#define AURALITE_PROC_USERCOPY_H

#include <stdint.h>
#include <stddef.h>

/* Maximum positive-canonical userspace address in the current x86_64 layout.
 * User mappings live below 0x0000800000000000; kernel/HHDM are in the high half.
 */
#define USER_VADDR_TOP 0x0000800000000000ULL

/* Validate that [user_ptr, user_ptr + len) is a canonical userspace range,
 * present in the current address space, marked USER, and writable if
 * write_required != 0.  len == 0 is accepted.
 */
int validate_user_range(const void *user_ptr, uint64_t len, int write_required);

/* Safe-ish user copies for the current AuraLite model.  They validate the
 * complete range before copying.  Return 0 on success, -1 on failure.
 */
int copy_from_user(void *kernel_dst, const void *user_src, uint64_t len);
int copy_to_user(void *user_dst, const void *kernel_src, uint64_t len);

/* Copy a NUL-terminated userspace string into kernel_dst.
 * kernel_dst_size includes the trailing NUL.  Returns 0 on success, -1 if the
 * pointer is invalid or no NUL appears before kernel_dst_size bytes.
 */
int copy_string_from_user(char *kernel_dst, const char *user_src,
                          uint64_t kernel_dst_size);

#endif /* AURALITE_PROC_USERCOPY_H */

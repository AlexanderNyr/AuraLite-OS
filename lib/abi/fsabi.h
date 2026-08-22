/* lib/abi/fsabi.h -- the bring-up file ABI (PARITY_PLAN.md P4).
 *
 * One struct layout for every port and both sides of the trap.  The
 * kernel dispatchers (user_rv.c / user_a64.c / user32.c) and the
 * three bring-up libcs include THIS file -- the layout cannot drift,
 * because there is exactly one of it.  Fixed-width fields only, and
 * sizes that agree on -m32: aura_stat is 16 bytes, aura_dirent 64.
 */

#ifndef AURALITE_LIB_ABI_FSABI_H
#define AURALITE_LIB_ABI_FSABI_H

struct aura_stat {
    unsigned long long size;
    unsigned int       is_dir;
    unsigned int       _pad;
};

struct aura_dirent {
    char         name[60];
    unsigned int is_dir;
};

/* lseek whence, the POSIX values. */
#define AURA_SEEK_SET 0
#define AURA_SEEK_CUR 1
#define AURA_SEEK_END 2

#endif /* AURALITE_LIB_ABI_FSABI_H */

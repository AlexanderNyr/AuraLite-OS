#ifndef AURALITE_LIBC_STDBOOL_H
#define AURALITE_LIBC_STDBOOL_H

/* stdbool.h — C99 boolean type for AuraLite user programs (freestanding).
 *
 * In C99/C11 `_Bool` is a built-in type; this header only provides the
 * convenience macros mandated by the standard. */

#ifndef __cplusplus

#define bool  _Bool
#define true  1
#define false 0

#endif /* __cplusplus */

#define __bool_true_false_are_defined 1

#endif /* AURALITE_LIBC_STDBOOL_H */

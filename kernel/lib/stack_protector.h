#ifndef AURALITE_LIB_STACK_PROTECTOR_H
#define AURALITE_LIB_STACK_PROTECTOR_H

#include <stdint.h>

/* Seed the kernel stack-protector guard early during boot. */
void stack_protector_init(void);

/* FIX_R2: how many times __stack_chk_fail() has tripped since boot.
 * > 1 means a halted machine kept running long enough to trip again --
 * that distinguishes repeated independent corruption from one event. */
uint32_t stack_protector_trip_count(void);

#endif /* AURALITE_LIB_STACK_PROTECTOR_H */

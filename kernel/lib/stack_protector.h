#ifndef AURALITE_LIB_STACK_PROTECTOR_H
#define AURALITE_LIB_STACK_PROTECTOR_H

/* Seed the kernel stack-protector guard early during boot. */
void stack_protector_init(void);

#endif /* AURALITE_LIB_STACK_PROTECTOR_H */

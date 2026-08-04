#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern void rsbr_print(const uint8_t *msg, size_t len);
extern int  rsbr_getpid(void);
extern void rsbr_exit(int code);
extern int  rsbr_hello(const char *name);

void rsbr_bridge_print(const char *msg) {
    if (!msg) return;
    rsbr_print((const uint8_t*)msg, strlen(msg));
}

int rsbr_bridge_getpid(void) {
    return rsbr_getpid();
}

void rsbr_bridge_exit(int code) {
    rsbr_exit(code);
}

int rsbr_bridge_hello(const char *name) {
    return rsbr_hello(name);
}

void rsbr_bridge_init(void) {
}
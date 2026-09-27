/* W32A-11 named clipboard-format registry shared by A/W USER32 calls.
 * Format ID is stable for a name and unique among names in this process.
 * Registering a name is not equivalent to placing clipboard data on it.
 * SPDX-License-Identifier: Apache-2.0 */
#include "w32/user32.h"
#include "w32/w32_utf.h"
#include "w32/w32_errno.h"
#include <stdint.h>
#include <string.h>

#define W32_FMT_MAX 256
#define W32_FMT_BYTES 255
static struct { char name[W32_FMT_BYTES+1]; W32_UINT id; }
    formats[W32_FMT_MAX];
static unsigned used;
static volatile int fmt_lock;
static void lock_fmt(void) {
    while (__sync_lock_test_and_set(&fmt_lock, 1))
        while (fmt_lock) __asm__ volatile("pause" ::: "memory");
}
static void unlock_fmt(void) { __sync_lock_release(&fmt_lock); }
static unsigned char fold(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 'a' - 'A') : c;
}
static int same(const char *a, const char *b) {
    while (*a && *b && fold((unsigned char)*a) == fold((unsigned char)*b)) {
        a++; b++;
    }
    return !*a && !*b;
}
static W32_UINT intern(const char *name) {
    if (!name || !*name) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0;
    }
    size_t n = 0;
    while (n <= W32_FMT_BYTES && name[n]) ++n;
    if (n > W32_FMT_BYTES) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0;
    }
    lock_fmt();
    for (unsigned i = 0; i < used; ++i) {
        if (same(formats[i].name, name)) {
            W32_UINT id = formats[i].id;
            unlock_fmt(); return id;
        }
    }
    if (used == W32_FMT_MAX) {
        unlock_fmt(); w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0;
    }
    unsigned slot = used++;
    memcpy(formats[slot].name, name, n+1);
    formats[slot].id = (W32_UINT)(0xC000u + slot);
    W32_UINT id = formats[slot].id;
    unlock_fmt(); return id;
}
W32ABI W32_UINT RegisterClipboardFormatA(const char *name) {
    return intern(name);
}
W32ABI W32_UINT RegisterClipboardFormatW(const uint16_t *name) {
    if (!name) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    size_t units = w32_utf16_len(name, W32_FMT_BYTES + 1);
    if (!units || units > W32_FMT_BYTES) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0;
    }
    char u8[W32_FMT_BYTES + 1]; size_t n = 0;
    if (w32_utf16_to_utf8(name, units, u8, W32_FMT_BYTES, &n) != W32_UTF_OK) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0;
    }
    u8[n] = 0;
    return intern(u8);
}

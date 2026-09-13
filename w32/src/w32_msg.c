/* w32/src/w32_msg.c — W32A-2 message table and FormatMessage.
 *
 * Every Win32 code this personality can set has one English sentence here,
 * written fresh for this file (never copied from vendor documentation), so
 * FormatMessage(FROM_SYSTEM) always has something true to say.  The QA rule
 * is mechanical: tools/check_w32app_claims.py fails the build when a
 * W32_ERROR_* constant lacks a row.  FROM_STRING formats support %1..%99
 * with !s!d!u!x!X!c! renders; ALLOCATE_BUFFER allocates through LocalAlloc;
 * MAX_WIDTH wraps on spaces.
 */

#include "w32/kernel32.h"
#include "w32/w32_handle.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"

#ifndef AURALITE_W32_HOST_TEST
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#endif

struct msg_row {
    W32_DWORD code;
    const char *text;           /* UTF-8, no inserts, no trailing newline */
};

static const struct msg_row msg_table[] = {
    { 0u, "The operation completed successfully." },
    { 1u, "The device does not recognize the requested function." },
    { 2u, "The named file was not found." },
    { 3u, "One of the directories in the path was not found." },
    { 4u, "Too many files are open for this process." },
    { 5u, "Permission to the object was denied." },
    { 6u, "The handle is not a live handle of the required kind." },
    { 8u, "There is not enough memory to complete the request." },
    { 13u, "The supplied data is not valid for this call." },
    { 17u, "The source and destination are on different volumes." },
    { 18u, "The enumeration has no more entries." },
    { 19u, "The volume is read-only." },
    { 25u, "The drive could not seek to the requested position." },
    { 29u, "The write to the device failed." },
    { 30u, "The read from the device failed." },
    { 32u, "The file is open elsewhere without a compatible share mode." },
    { 38u, "The end of the file was reached." },
    { 50u, "This personality does not implement the request." },
    { 53u, "The network path needs a provider that is not installed." },
    { 80u, "A file with that name already exists." },
    { 87u, "One of the parameters is not valid." },
    { 109u, "The pipe's far end has closed." },
    { 112u, "The volume has no free space left." },
    { 121u, "The wait ran out of time." },
    { 122u, "The buffer is too small for the result." },
    { 123u, "The name is misspelled or uses an unknown drive." },
    { 127u, "The module or procedure was not found." },
    { 145u, "The directory still contains entries." },
    { 183u, "The object already exists." },
    { 193u, "The file is neither a runnable program nor a library image." },
    { 203u, "The environment has no variable with that name." },
    { 206u, "The name is longer than the limit allows." },
    { 231u, "Every instance of the pipe is busy." },
    { 233u, "The pipe's other end has not connected yet." },
    { 258u, "The wait finished without the condition becoming true." },
    { 259u, "No more data is available." },
    { 267u, "The path names a file where a directory is required." },
    { 535u, "The pipe is already connected." },
    { 995u, "The operation was cancelled before it finished." },
    { 1113u, "The text could not convert between the requested encodings." },
    { 1222u, "No network is attached to this machine." },
};

#define MSG_COUNT (sizeof(msg_table) / sizeof(msg_table[0]))

static const char *msg_lookup(W32_DWORD code) {
    size_t i;
    for (i = 0; i < MSG_COUNT; i++) {
        if (msg_table[i].code == code)
            return msg_table[i].text;
    }
    return NULL;
}

/* Format state: the output grows in a WCHAR buffer, then copies out. */
struct msg_buf {
    W32_WCHAR *w;
    size_t len;
    size_t cap;
};

static int msg_push(struct msg_buf *b, W32_WCHAR c) {
    if (b->len + 1 >= b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 128;
        W32_WCHAR *nw = (W32_WCHAR *)realloc(b->w, ncap * sizeof(W32_WCHAR));
        if (!nw)
            return -1;
        b->w = nw;
        b->cap = ncap;
    }
    b->w[b->len++] = c;
    return 0;
}

static int msg_push_ascii(struct msg_buf *b, const char *s) {
    while (*s) {
        if (msg_push(b, (W32_WCHAR)(unsigned char)*s++) != 0)
            return -1;
    }
    return 0;
}

static int msg_push_w(struct msg_buf *b, const W32_WCHAR *s) {
    if (!s)
        return msg_push_ascii(b, "(null)");
    while (*s) {
        if (msg_push(b, *s++) != 0)
            return -1;
    }
    return 0;
}

static int msg_push_u64(struct msg_buf *b, uint64_t v, int base, int upper) {
    W32_WCHAR dig[64];
    int n = 0;
    int i;
    if (v == 0)
        return msg_push(b, (W32_WCHAR)'0');
    while (v > 0) {
        unsigned d = (unsigned)(v % (uint64_t)base);
        if (d < 10)
            dig[n++] = (W32_WCHAR)('0' + d);
        else
            dig[n++] = (W32_WCHAR)((upper ? 'A' : 'a') + d - 10);
        v /= (uint64_t)base;
    }
    for (i = n - 1; i >= 0; i--) {
        if (msg_push(b, dig[i]) != 0)
            return -1;
    }
    return 0;
}

/* Render one insert argument.  `kind` is the !x! letter (0 for bare %n,
 * which renders as an unsigned decimal). */
static int msg_insert(struct msg_buf *b, uintptr_t arg, char kind) {
    switch (kind) {
    case 0:
    case 'd': {
        int64_t v = (int64_t)(int32_t)(uint32_t)arg;
        if (v < 0) {
            if (msg_push(b, (W32_WCHAR)'-') != 0)
                return -1;
            v = -v;
        }
        return msg_push_u64(b, (uint64_t)v, 10, 0);
    }
    case 'u':
        return msg_push_u64(b, (uint64_t)(uint32_t)arg, 10, 0);
    case 'x':
        return msg_push_u64(b, (uint64_t)(uint32_t)arg, 16, 0);
    case 'X':
        return msg_push_u64(b, (uint64_t)(uint32_t)arg, 16, 1);
    case 'p':
        if (msg_push_ascii(b, "0x") != 0)
            return -1;
        return msg_push_u64(b, (uint64_t)arg, 16, 0);
    case 'c':
        return msg_push(b, (W32_WCHAR)(uint16_t)arg);
    case 's': {
        const W32_WCHAR *s = (const W32_WCHAR *)arg;
        return msg_push_w(b, s);
    }
    case 'S': {
        const char *s = (const char *)arg;
        if (!s)
            return msg_push_ascii(b, "(null)");
        while (*s) {
            unsigned char c0 = (unsigned char)*s;
            if (c0 < 0x80) {
                if (msg_push(b, (W32_WCHAR)c0) != 0)
                    return -1;
                s++;
            } else {
                size_t seqlen = (c0 >= 0xF0) ? 4 : (c0 >= 0xE0) ? 3 : 2;
                uint16_t units[2];
                size_t need = 0;
                size_t avail = strlen(s);
                int rc;
                if (seqlen > avail)
                    seqlen = avail;
                rc = w32_utf8_to_utf16(s, seqlen, units, 2, &need);
                if (rc == W32_UTF_OK && need >= 1 && need <= 2) {
                    size_t k;
                    for (k = 0; k < need; k++) {
                        if (msg_push(b, units[k]) != 0)
                            return -1;
                    }
                    s += seqlen;
                } else {
                    if (msg_push(b, (W32_WCHAR)'?') != 0)
                        return -1;
                    s++;
                }
            }
        }
        return 0;
    }
    default:
        /* Unknown render: the argument as decimal, so nothing hides. */
        return msg_push_u64(b, (uint64_t)(uint32_t)arg, 10, 0);
    }
}

/* Expand a format string.  `args` points at insert 1 (an array of
 * uintptr_t in both the argument-array form and the plain form — the two
 * are equivalent here, documented on the API). */
static int msg_expand(struct msg_buf *b, const W32_WCHAR *fmt,
                      const uintptr_t *args, int ignoreInserts) {
    const W32_WCHAR *p = fmt;
    while (*p) {
        if (*p != (W32_WCHAR)'%') {
            if (msg_push(b, *p++) != 0)
                return -1;
            continue;
        }
        p++;
        if (*p == (W32_WCHAR)'%') {
            if (msg_push(b, *p++) != 0)
                return -1;
            continue;
        }
        if (*p == (W32_WCHAR)'0') {
            /* %0 ends the message; the rest is ignored. */
            return 0;
        }
        if (*p >= (W32_WCHAR)'1' && *p <= (W32_WCHAR)'9') {
            unsigned n = 0;
            char kind = 0;
            while (*p >= (W32_WCHAR)'0' && *p <= (W32_WCHAR)'9') {
                n = n * 10 + (unsigned)(*p - (W32_WCHAR)'0');
                p++;
            }
            if (*p == (W32_WCHAR)'!') {
                p++;
                if (*p && *p != (W32_WCHAR)'!') {
                    kind = (char)*p++;
                    if (*p == (W32_WCHAR)'!')
                        p++;
                    else {
                        /* Unterminated !x: render literally. */
                        if (msg_push_ascii(b, "%!") != 0)
                            return -1;
                        continue;
                    }
                } else if (*p == (W32_WCHAR)'!') {
                    p++;
                }
            }
            if (ignoreInserts || n == 0 || n > 99 || !args) {
                continue;       /* inserts dropped, not fabricated */
            }
            if (msg_insert(b, args[n - 1], kind) != 0)
                return -1;
            continue;
        }
        /* A % followed by anything else passes through literally. */
        if (msg_push(b, (W32_WCHAR)'%') != 0)
            return -1;
        if (*p && msg_push(b, *p++) != 0)
            return -1;
    }
    return 0;
}

/* Wrap at `width` columns on spaces, breaking long words when forced. */
static int msg_wrap(struct msg_buf *b, unsigned width) {
    W32_WCHAR *out = NULL;
    size_t cap = 0;
    size_t len = 0;
    size_t i = 0;

    if (width == 0)
        return 0;
    while (i < b->len) {
        size_t j = i;
        size_t lastSpace = (size_t)-1;
        size_t k;
        /* Find the wrap point within the next width columns. */
        while (j < b->len && j - i < width) {
            if (b->w[j] == (W32_WCHAR)' ')
                lastSpace = j;
            if (b->w[j] == (W32_WCHAR)'\r' || b->w[j] == (W32_WCHAR)'\n')
                break;
            j++;
        }
        if (j < b->len && b->w[j] != (W32_WCHAR)'\r' &&
            b->w[j] != (W32_WCHAR)'\n') {
            if (lastSpace != (size_t)-1 && lastSpace > i)
                j = lastSpace;
        }
        while (len + (j - i) + 3 >= cap) {
            size_t ncap = cap ? cap * 2 : 128;
            W32_WCHAR *nw = (W32_WCHAR *)realloc(out,
                ncap * sizeof(W32_WCHAR));
            if (!nw) {
                free(out);
                return -1;
            }
            out = nw;
            cap = ncap;
        }
        for (k = i; k < j; k++)
            out[len++] = b->w[k];
        if (j < b->len && (b->w[j] == (W32_WCHAR)'\r' ||
                           b->w[j] == (W32_WCHAR)'\n')) {
            out[len++] = b->w[j++];
            if (j < b->len && b->w[j - 1] == (W32_WCHAR)'\r' &&
                b->w[j] == (W32_WCHAR)'\n')
                out[len++] = b->w[j++];
            while (j < b->len && b->w[j] == (W32_WCHAR)' ')
                j++;
        } else if (j < b->len) {
            out[len++] = (W32_WCHAR)'\r';
            out[len++] = (W32_WCHAR)'\n';
            j = (b->w[j] == (W32_WCHAR)' ') ? j + 1 : j;
        }
        i = j;
    }
    free(b->w);
    b->w = out;
    b->len = len;
    b->cap = cap;
    return 0;
}

W32ABI W32_DWORD FormatMessageW(W32_DWORD flags, const void *src,
                               W32_DWORD msgId, W32_DWORD langId,
                               W32_LPWSTR buf, W32_DWORD cch, void *args) {
    const char *sys = NULL;
    const W32_WCHAR *fmt = NULL;
    W32_WCHAR *sysW = NULL;
    struct msg_buf b;
    int ignoreInserts;
    unsigned width;
    W32_DWORD ret = 0;
    size_t i;

    (void)langId;               /* one language exists */
    if (flags & ~(W32_FORMAT_MESSAGE_ALLOCATE_BUFFER |
                  W32_FORMAT_MESSAGE_IGNORE_INSERTS |
                  W32_FORMAT_MESSAGE_FROM_STRING |
                  W32_FORMAT_MESSAGE_FROM_SYSTEM |
                  W32_FORMAT_MESSAGE_ARGUMENT_ARRAY |
                  W32_FORMAT_MESSAGE_MAX_WIDTH_MASK)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!(flags & (W32_FORMAT_MESSAGE_FROM_STRING |
                   W32_FORMAT_MESSAGE_FROM_SYSTEM))) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((flags & W32_FORMAT_MESSAGE_FROM_STRING) &&
        (flags & W32_FORMAT_MESSAGE_FROM_SYSTEM)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((flags & W32_FORMAT_MESSAGE_FROM_STRING) && !src) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    ignoreInserts = (flags & W32_FORMAT_MESSAGE_IGNORE_INSERTS) != 0;
    width = flags & W32_FORMAT_MESSAGE_MAX_WIDTH_MASK;
    if (flags & W32_FORMAT_MESSAGE_FROM_SYSTEM) {
        sys = msg_lookup(msgId);
        if (!sys) {
            w32_set_last_error(W32_ERROR_PROC_NOT_FOUND);
            return 0;
        }
        /* Table texts are ASCII: widen directly. */
        {
            size_t n = strlen(sys);
            sysW = (W32_WCHAR *)malloc((n + 1) * sizeof(W32_WCHAR));
            if (!sysW) {
                w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
            for (i = 0; i <= n; i++)
                sysW[i] = (W32_WCHAR)(unsigned char)sys[i];
        }
        fmt = sysW;
    } else {
        fmt = (const W32_WCHAR *)src;
    }
    b.w = NULL;
    b.len = 0;
    b.cap = 0;
    if (msg_expand(&b, fmt, (const uintptr_t *)args, ignoreInserts) != 0) {
        free(sysW);
        free(b.w);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    free(sysW);
    if (msg_wrap(&b, width) != 0) {
        free(b.w);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (flags & W32_FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        /* buf points at the caller's LPWSTR slot; cch is the minimum. */
        W32_WCHAR **slot = (W32_WCHAR **)buf;
        W32_WCHAR *mem;
        if (!slot) {
            free(b.w);
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return 0;
        }
        /* cch is the caller's minimum in characters; LocalAlloc wants
         * bytes.  Pass the character count and the block overflows by
         * nearly half (ASan heap-buffer-overflow on the NUL). */
        {
            W32_SIZE_T need =
                (W32_SIZE_T)(b.len + 1) * sizeof(W32_WCHAR);
            W32_SIZE_T min = (W32_SIZE_T)cch * sizeof(W32_WCHAR);
            mem = (W32_WCHAR *)LocalAlloc(W32_LMEM_FIXED,
                need > min ? need : min);
        }
        if (!mem) {
            free(b.w);
            return 0;           /* LocalAlloc set the error */
        }
        for (i = 0; i < b.len; i++)
            mem[i] = b.w[i];
        mem[b.len] = 0;
        *slot = mem;
        ret = (W32_DWORD)b.len;
        free(b.w);
        return ret;
    }
    if (cch == 0 || !buf) {
        free(b.w);
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (b.len >= cch) {
        /* Truncated: copy what fits, NUL-terminate, report the shortfall
         * the way a short buffer does. */
        for (i = 0; i + 1 < cch; i++)
            buf[i] = b.w[i];
        buf[cch - 1] = 0;
        free(b.w);
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (i = 0; i < b.len; i++)
        buf[i] = b.w[i];
    buf[b.len] = 0;
    ret = (W32_DWORD)b.len;
    free(b.w);
    return ret;
}

W32ABI W32_DWORD FormatMessageA(W32_DWORD flags, const void *src,
                               W32_DWORD msgId, W32_DWORD langId,
                               W32_LPSTR buf, W32_DWORD cch, void *args) {
    /* Render wide, then narrow.  FROM_STRING's format arrives as ANSI and
     * widens first (ACP is UTF-8, strict — a bad byte fails the call). */
    const W32_WCHAR *fmtW = NULL;
    W32_WCHAR *fmtAlloc = NULL;
    W32_WCHAR stack[512];
    W32_WCHAR *wide = NULL;
    W32_DWORD got;
    size_t need = 0;
    size_t need2 = 0;
    int rc;

    if ((flags & W32_FORMAT_MESSAGE_ALLOCATE_BUFFER) &&
        (flags & W32_FORMAT_MESSAGE_FROM_STRING) && src) {
        /* Both paths below need the widened format; share it. */
    }
    if ((flags & W32_FORMAT_MESSAGE_FROM_STRING) && src) {
        const char *s = (const char *)src;
        rc = w32_utf8_to_utf16(s, strlen(s), NULL, 0, &need);
        if (rc != W32_UTF_OK) {
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        fmtAlloc = (W32_WCHAR *)malloc((need + 1) * sizeof(W32_WCHAR));
        if (!fmtAlloc) {
            w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        rc = w32_utf8_to_utf16(s, strlen(s), fmtAlloc, need, &need2);
        if (rc != W32_UTF_OK || need2 != need) {
            free(fmtAlloc);
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        fmtAlloc[need] = 0;
        fmtW = fmtAlloc;
        src = fmtW;
    }
    if (flags & W32_FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        W32_WCHAR *wmem = NULL;
        W32_DWORD n;
        char *amem;
        n = FormatMessageW(flags, src, msgId, langId, (W32_LPWSTR)&wmem, cch,
            args);
        free(fmtAlloc);
        if (n == 0)
            return 0;
        rc = w32_utf16_to_utf8(wmem, n, NULL, 0, &need);
        if (rc != W32_UTF_OK) {
            LocalFree(wmem);
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        amem = (char *)LocalAlloc(W32_LMEM_FIXED, need + 1);
        if (!amem) {
            LocalFree(wmem);
            return 0;
        }
        rc = w32_utf16_to_utf8(wmem, n, amem, need, &need2);
        LocalFree(wmem);
        if (rc != W32_UTF_OK || need2 != need) {
            LocalFree(amem);
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        amem[need] = '\0';
        *(char **)buf = amem;
        return (W32_DWORD)need;
    }
    wide = stack;
    got = FormatMessageW(flags & ~W32_FORMAT_MESSAGE_ALLOCATE_BUFFER, src,
        msgId, langId, wide, 512, args);
    if (got == 0 && w32_get_last_error_raw() == W32_ERROR_INSUFFICIENT_BUFFER) {
        /* Retry spacious: formats can exceed the stack buffer. */
        wide = (W32_WCHAR *)malloc(8192 * sizeof(W32_WCHAR));
        if (!wide) {
            free(fmtAlloc);
            w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        got = FormatMessageW(flags & ~W32_FORMAT_MESSAGE_ALLOCATE_BUFFER,
            src, msgId, langId, wide, 8192, args);
        if (got == 0) {
            free(wide);
            free(fmtAlloc);
            return 0;
        }
        rc = w32_utf16_to_utf8(wide, got, NULL, 0, &need);
        if (rc != W32_UTF_OK || need >= cch || cch == 0 || !buf) {
            free(wide);
            free(fmtAlloc);
            w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        rc = w32_utf16_to_utf8(wide, got, buf, need, &need2);
        free(wide);
        free(fmtAlloc);
        if (rc != W32_UTF_OK || need2 != need) {
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        buf[need] = '\0';
        return (W32_DWORD)need;
    }
    free(fmtAlloc);
    if (got == 0)
        return 0;
    rc = w32_utf16_to_utf8(wide, got, NULL, 0, &need);
    if (rc != W32_UTF_OK || need >= cch || cch == 0 || !buf) {
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    rc = w32_utf16_to_utf8(wide, got, buf, need, &need2);
    if (rc != W32_UTF_OK || need2 != need) {
        w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
        return 0;
    }
    buf[need] = '\0';
    return (W32_DWORD)need;
}

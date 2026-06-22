#include <stdint.h>
#include "kernel/lib/klog.h"
#include "kernel/lib/string.h"

#define KLOG_BUF_SIZE 131072u

static char klog_buf[KLOG_BUF_SIZE];
static uint64_t klog_len = 0;
static klog_sink_t klog_sink = 0;
static int flushing = 0;

void klog_putchar(char c) {
    if (flushing) return;
    if (klog_len < KLOG_BUF_SIZE) {
        klog_buf[klog_len++] = c;
    } else {
        /* Drop oldest byte on overflow, preserving recent boot diagnostics. */
        memmove(klog_buf, klog_buf + 1, KLOG_BUF_SIZE - 1);
        klog_buf[KLOG_BUF_SIZE - 1] = c;
    }
}

void klog_set_sink(klog_sink_t sink) {
    klog_sink = sink;
}

void klog_flush(void) {
    if (!klog_sink || klog_len == 0 || flushing) return;
    flushing = 1;
    uint64_t off = 0;
    while (off < klog_len) {
        uint64_t chunk = klog_len - off;
        if (chunk > 4096) chunk = 4096;
        if (klog_sink(klog_buf + off, chunk) != 0) {
            break;
        }
        off += chunk;
    }
    if (off > 0) {
        if (off < klog_len) {
            memmove(klog_buf, klog_buf + off, klog_len - off);
        }
        klog_len -= off;
    }
    flushing = 0;
}

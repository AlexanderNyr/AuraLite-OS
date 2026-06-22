#ifndef AURALITE_LIB_KLOG_H
#define AURALITE_LIB_KLOG_H

#include <stdint.h>

/* Kernel log fanout for persistent sinks. kprintf writes characters into this
 * buffer; once a filesystem/block sink is available it can attach a writer and
 * flush the backlog to disk. */
typedef int (*klog_sink_t)(const char *data, uint64_t len);

void klog_putchar(char c);
void klog_set_sink(klog_sink_t sink);
void klog_flush(void);

#endif /* AURALITE_LIB_KLOG_H */

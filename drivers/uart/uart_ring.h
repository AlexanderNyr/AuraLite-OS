/* uart_ring.h — the TX ring index core, pure C (OPT_PLAN.md O3).
 *
 * No I/O, no locking, no statics: just the head/tail arithmetic, split
 * out so the host unit test (tests/unit/test_uart_ring.c) can exercise
 * the three classic off-by-ones — wrap, full, empty — without a UART in
 * the room.  uart.c composes these under its own lock.
 *
 * Convention: head and tail are FREE-RUNNING uint32 counters (they wrap
 * mod 2^32, never mod size); an index into the buffer is `counter &
 * (size - 1)`, so size MUST be a power of two.  count == head - tail
 * works across the 2^32 wrap by unsigned arithmetic — that wrap is
 * exactly what the unit test pins down.
 */
#ifndef AURALITE_DRIVERS_UART_RING_H
#define AURALITE_DRIVERS_UART_RING_H

#include <stdint.h>

typedef struct {
    uint32_t head;      /* producer counter (writes advance it)  */
    uint32_t tail;      /* consumer counter (drains advance it)  */
} uart_ring_t;

static inline uint32_t uring_count(const uart_ring_t *r) {
    return r->head - r->tail;
}

static inline int uring_empty(const uart_ring_t *r) {
    return r->head == r->tail;
}

static inline int uring_full(const uart_ring_t *r, uint32_t size) {
    return uring_count(r) == size;
}

/* Push one byte.  Caller must have checked !uring_full(). */
static inline void uring_push(uart_ring_t *r, uint8_t *buf, uint32_t size,
                              uint8_t byte) {
    buf[r->head & (size - 1)] = byte;
    r->head++;
}

/* Pop one byte.  Caller must have checked !uring_empty(). */
static inline uint8_t uring_pop(uart_ring_t *r, const uint8_t *buf,
                                uint32_t size) {
    uint8_t b = buf[r->tail & (size - 1)];
    r->tail++;
    return b;
}

#endif /* AURALITE_DRIVERS_UART_RING_H */

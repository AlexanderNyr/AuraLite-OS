#ifndef AURALITE_ARCH_X86_64_CPU_H
#define AURALITE_ARCH_X86_64_CPU_H

#include <stdint.h>

/*
 * Low-level x86_64 primitives: control registers, MSRs, and TLB invalidation.
 * These are used by the paging VMM, the exception handler (CR2), and later by
 * the syscall and SMP code.
 */

/* ---- Control registers (Intel SDM Vol.3, 2.5) ---- */

static inline uint64_t read_cr0(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr0" :: "r"(v) : "memory");
}

/* CR2: page-fault linear address (Intel SDM Vol.3, 4.7). */
static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

/* CR3: physical base of the current PML4 page table. */
static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr3" :: "r"(v) : "memory");
}

static inline uint64_t read_cr4(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr4" :: "r"(v) : "memory");
}

static inline void cpuid_count(uint32_t leaf, uint32_t subleaf,
                               uint32_t *a, uint32_t *b,
                               uint32_t *c, uint32_t *d) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(leaf), "c"(subleaf));
    if (a) *a = eax;
    if (b) *b = ebx;
    if (c) *c = ecx;
    if (d) *d = edx;
}

static inline uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* Published by paging.c once CR4.SMAP is enabled. */
extern volatile int cpu_smap_is_active;

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    if (flags & 0x200ULL) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

static inline void user_access_enable(void) {
    if (cpu_smap_is_active) {
        __asm__ volatile ("stac" ::: "memory");
    }
}

static inline void user_access_disable(void) {
    if (cpu_smap_is_active) {
        __asm__ volatile ("clac" ::: "memory");
    }
}

static inline uint64_t user_access_begin(void) {
    uint64_t flags = irq_save();
    user_access_enable();
    return flags;
}

static inline void user_access_end(uint64_t flags) {
    user_access_disable();
    irq_restore(flags);
}

/* ---- Model-Specific Registers (Intel SDM Vol.4) ---- */

static inline uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline void write_msr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr" :: "a"((uint32_t)val),
                      "d"((uint32_t)(val >> 32)), "c"(msr));
}

/* ---- TLB ---- */

/* Invalidate the TLB entry for a single virtual page (Intel SDM Vol.2, INVLPG). */
static inline void invlpg(uint64_t virt) {
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

#endif /* AURALITE_ARCH_X86_64_CPU_H */

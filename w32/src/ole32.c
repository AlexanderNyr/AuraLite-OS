/* W32A-11 COM-lite partial: genuine per-thread balanced init and task heap.
 * No native TLS segment in AuraLite libc: bind depth to a stable Win32 TID.
 * CLSID/IID activation remains typed TODO until pinned-app observations.
 * SPDX-License-Identifier: Apache-2.0 */
#include "w32/ole32.h"
#include "w32/kernel32.h"
#include "w32/w32_errno.h"
#include <stdlib.h>
#include <stdint.h>

#define W32_COM_MAX_THREADS 128
static struct { W32_DWORD id, co_depth, ole_depth; int used; }
    com_threads[W32_COM_MAX_THREADS];
static volatile int com_lock;
static void lock_com(void) {
    while (__sync_lock_test_and_set(&com_lock, 1))
        while (com_lock) __asm__ volatile("pause" ::: "memory");
}
static void unlock_com(void) { __sync_lock_release(&com_lock); }
static int find_thread(W32_DWORD id, int create) {
    int empty = -1;
    for (int i = 0; i < W32_COM_MAX_THREADS; ++i) {
        if (com_threads[i].used && com_threads[i].id == id) return i;
        if (!com_threads[i].used && empty < 0) empty = i;
    }
    if (!create || empty < 0) return -1;
    com_threads[empty].id = id;
    com_threads[empty].co_depth = 0;
    com_threads[empty].ole_depth = 0;
    com_threads[empty].used = 1;
    return empty;
}
W32ABI W32_DWORD CoInitialize(void *reserved) {
    if (reserved) return W32_COM_E_INVALIDARG;
    lock_com();
    int slot = find_thread(GetCurrentThreadId(), 1);
    if (slot < 0 || com_threads[slot].co_depth == UINT32_MAX) {
        unlock_com(); return W32_COM_E_OUTOFMEMORY;
    }
    W32_DWORD rc = com_threads[slot].co_depth++ ? W32_COM_S_FALSE : W32_COM_S_OK;
    unlock_com(); return rc;
}
W32ABI void CoUninitialize(void) {
    lock_com();
    int slot = find_thread(GetCurrentThreadId(), 0);
    if (slot >= 0 && com_threads[slot].co_depth &&
        !--com_threads[slot].co_depth) {
        com_threads[slot].ole_depth = 0;
        com_threads[slot].used = 0;
    }
    unlock_com();
}
W32ABI W32_DWORD OleInitialize(void *reserved) {
    W32_DWORD rc = CoInitialize(reserved);
    if (rc != W32_COM_S_OK && rc != W32_COM_S_FALSE) return rc;
    lock_com();
    int slot = find_thread(GetCurrentThreadId(), 0);
    if (slot >= 0 && com_threads[slot].ole_depth != UINT32_MAX) {
        com_threads[slot].ole_depth++;
        unlock_com(); return rc;
    }
    unlock_com();
    CoUninitialize();
    return W32_COM_E_OUTOFMEMORY;
}
W32ABI void OleUninitialize(void) {
    int valid = 0;
    lock_com();
    int slot = find_thread(GetCurrentThreadId(), 0);
    if (slot >= 0 && com_threads[slot].ole_depth) {
        --com_threads[slot].ole_depth;
        valid = 1;
    }
    unlock_com();
    if (valid) CoUninitialize();
}
W32ABI void *CoTaskMemAlloc(size_t bytes) {
    void *ptr = malloc(bytes ? bytes : 1);
    if (!ptr) w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    return ptr;
}
W32ABI void *CoTaskMemRealloc(void *ptr, size_t bytes) {
    if (!bytes) { free(ptr); return NULL; }
    void *next = realloc(ptr, bytes);
    if (!next) w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    return next;
}
W32ABI void CoTaskMemFree(void *ptr) { free(ptr); }

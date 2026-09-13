/* w32/tests/w32a2_heap.c — W32A-2 guest fixture: heaps and memory.
 *
 * HeapAlloc/ReAlloc/Size/Free, Global*, Local*, VirtualProtect, and
 * GetLargePageMinimum.  Exits 55/1.
 */

#include "w32a2_common.h"

void __stdcall winstart(void) {
    HANDLE heap = GetProcessHeap();
    void *p;
    DWORD old;
    int i;

    w32a2_out = GetStdHandle(STD_OUTPUT_HANDLE);

    CHECKX(heap != NULL, "heap-token");
    CHECKX(HeapAlloc((HANDLE)0x42, 0, 8) == NULL, "heap-badheap-fails");
    CHECKX(GetLastError() == ERROR_INVALID_HANDLE, "heap-badheap-code");

    p = HeapAlloc(heap, HEAP_ZERO_MEMORY, 64);
    CHECKX(p != NULL, "heap-alloc");
    if (p != NULL) {
        volatile char *c = (volatile char *)p;
        int okz = 1;
        CHECKX(HeapSize(heap, 0, p) == 64, "heap-size");
        for (i = 0; i < 64; i++) {
            if (c[i] != 0)
                okz = 0;
            c[i] = (char)0xAB;
        }
        CHECKX(okz, "heap-zeroed");
        p = HeapReAlloc(heap, HEAP_ZERO_MEMORY, p, 128);
        CHECKX(p != NULL, "heap-realloc");
        if (p != NULL) {
            int okp = 1;
            c = (volatile char *)p;
            for (i = 0; i < 64; i++) {
                if (c[i] != (char)0xAB)
                    okp = 0;
            }
            CHECKX(okp, "heap-preserved");
            CHECKX(HeapSize(heap, 0, p) == 128, "heap-size2");
            okz = 1;
            for (i = 64; i < 128; i++) {
                if (c[i] != 0)
                    okz = 0;
            }
            CHECKX(okz, "heap-rezeroed");
            CHECKX(HeapFree(heap, 0, p), "heap-free");
        }
    }
    CHECKX(HeapSize(heap, 0, (void *)0x1234) == (SIZE_T)-1,
        "heap-size-bogus");
    CHECKX(HeapFree(heap, 0, NULL), "heap-free-null");

    p = GlobalAlloc(GMEM_ZEROINIT, 32);
    CHECKX(p != NULL, "heap-global");
    if (p != NULL) {
        CHECKX(GlobalLock(p) == p, "heap-glock");
        CHECKX(!GlobalUnlock(p), "heap-gunlock-fails");
        CHECKX(GetLastError() == ERROR_SUCCESS, "heap-gunlock-code");
        CHECKX(GlobalSize(p) == 32, "heap-gsize");
        CHECKX(GlobalFree(p) == NULL, "heap-gfree");
    }
    p = LocalAlloc(LPTR, 16);
    CHECKX(p != NULL, "heap-local");
    if (p != NULL)
        CHECKX(LocalFree(p) == NULL, "heap-lfree");

    {
        char stack[64];
        CHECKX(VirtualProtect(stack, sizeof(stack), PAGE_READWRITE,
            &old), "heap-vprot-rw");
        CHECKX(old == PAGE_READWRITE, "heap-vprot-old");
        CHECKX(!VirtualProtect(stack, sizeof(stack), PAGE_READONLY,
            &old), "heap-vprot-ro-fails");
        CHECKX(GetLastError() == ERROR_INVALID_PARAMETER,
            "heap-vprot-ro-code");
        CHECKX(!VirtualProtect(NULL, 8, PAGE_READWRITE, &old),
            "heap-vprot-null-fails");
        CHECKX(GetLastError() == ERROR_INVALID_PARAMETER,
            "heap-vprot-null-code");
    }
    CHECKX(GetLargePageMinimum() == 0, "heap-largepage");

    w32a2_done("HEAP");
}

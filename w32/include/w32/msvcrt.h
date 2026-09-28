/* msvcrt.h — W32APP_PLAN.md phase W32A-13: the msvcrt bridge.
 *
 * The 34+22 msvcrt symbols the 7-Zip gate (7zFM-24.09 / 7z-24.09) imports,
 * forwarded onto the native libc and the W32A-2/3/4 runtimes.  The phase's
 * load-bearing invariant is HEAP UNITY: msvcrt malloc/free/realloc allocate
 * from the SAME process heap kernel32 hands out (GetProcessHeap), so
 * HeapSize(malloc(n)) == n, HeapReAlloc works on CRT pointers, and the two
 * heaps that would otherwise "pretend to be one" cannot drift.
 *
 * The C++ exception surface (_CxxThrowException / ?terminate / _purecall /
 * __C_specific_handler / _XcptFilter) is W32A-4's and is bound there; this
 * phase adds __CxxFrameHandler (honest D7 continue-search: cleanups then a
 * NAMED terminate, never a fabricated catch) and type_info's destructor.
 *
 * Every export is W32ABI: msvcrt is a PE DLL and its callers cross the
 * Windows-x64 boundary exactly like every other w32 export.
 *
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef AURALITE_W32_MSVCRT_H
#define AURALITE_W32_MSVCRT_H

#include "w32/w32_abi.h"
#include <stddef.h>
#include <stdint.h>

/* The CRT's two initialiser/finaliser function-pointer shapes.  _PVFV is the
 * void(void) table entry _initterm walks; _onexit_t is the int(void) callback
 * _onexit/atexit register (its int return is the success flag). */
typedef void (W32ABI *w32_PVFV)(void);
typedef int  (W32ABI *w32_onexit_t)(void);

/* ---- heap unity (the phase invariant) -------------------------------------
 * All three route through GetProcessHeap()/HeapAlloc/HeapFree/HeapReAlloc so
 * a CRT pointer is a process-heap pointer, byte for byte. */
W32ABI void *w32_msvcrt_malloc(size_t size);
W32ABI void  w32_msvcrt_free(void *ptr);
W32ABI void *w32_msvcrt_realloc(void *ptr, size_t size);

/* ---- strings / memory (REAL, self-contained) ------------------------------ */
W32ABI int    w32_msvcrt_memcmp(const void *a, const void *b, size_t n);
W32ABI void  *w32_msvcrt_memcpy(void *d, const void *s, size_t n);
W32ABI void  *w32_msvcrt_memmove(void *d, const void *s, size_t n);
W32ABI void  *w32_msvcrt_memset(void *d, int c, size_t n);
W32ABI size_t w32_msvcrt_strlen(const char *s);
W32ABI int    w32_msvcrt_strcmp(const char *a, const char *b);
W32ABI char  *w32_msvcrt_strstr(const char *hay, const char *needle);
W32ABI char  *w32_msvcrt_strchr(const char *s, int c);
W32ABI int    w32_msvcrt_wcscmp(const uint16_t *a, const uint16_t *b);
W32ABI size_t w32_msvcrt_wcslen(const uint16_t *s);
W32ABI uint16_t *w32_msvcrt_wcsstr(const uint16_t *hay, const uint16_t *needle);

/* ---- pseudo-random (the exact MSVCRT LCG, RAND_MAX 0x7fff) ----------------- */
W32ABI int  w32_msvcrt_rand(void);
W32ABI void w32_msvcrt_srand(unsigned int seed);

/* ---- startup / exit -------------------------------------------------------- */
W32ABI int  w32_msvcrt_getmainargs(int *argc, char ***argv, char ***env,
                                   int expand_wildcards, int *new_mode);
W32ABI void w32_msvcrt_initterm(w32_PVFV *first, w32_PVFV *last);
W32ABI w32_onexit_t w32_msvcrt_onexit(w32_onexit_t func);
W32ABI w32_onexit_t w32_msvcrt_dllonexit(w32_onexit_t func,
                                         w32_PVFV **pbegin, w32_PVFV **pend);
W32ABI void w32_msvcrt_exit(int code)  __attribute__((noreturn));  /* exit  */
W32ABI void w32_msvcrt__exit(int code) __attribute__((noreturn));  /* _exit */
W32ABI void w32_msvcrt_cexit(void);    /* _cexit:  callbacks, then RETURN */
W32ABI void w32_msvcrt_c_exit(void);   /* _c_exit: no callbacks, RETURN   */
W32ABI void w32_msvcrt_set_app_type(int app_type);
W32ABI void w32_msvcrt_setusermatherr(void *handler);

/* ---- threading (onto W32A-3 CreateThread) --------------------------------- */
W32ABI uintptr_t w32_msvcrt_beginthreadex(void *security, unsigned stack_size,
                                          void *start, void *arglist,
                                          unsigned initflag, unsigned *thrdaddr);

/* ---- C++ EH residue (D7: honest, never a fabricated catch) ----------------- */
W32ABI int32_t w32_msvcrt_CxxFrameHandler(void *record, uint64_t frame,
                                          void *context, void *dispatch);
W32ABI void   *w32_msvcrt_type_info_dtor(void *self);

/* ---- data exports (bound as the ADDRESS of these cells) -------------------- */
extern char *w32_msvcrt_acmdln;   /* _acmdln:  the ANSI command line     */
extern int   w32_msvcrt_fmode;    /* _fmode:   O_TEXT (0) by default     */
extern int   w32_msvcrt_commode;  /* _commode: 0 (no commit-on-flush)    */

/* Wire _acmdln to kernel32's command line once it exists (called from
 * w32_kernel32_init, after the line is built). */
void w32_msvcrt_init(void);

/* Test-only: run and clear the process onexit chain (LIFO).  Exposed so the
 * host unit test can drive it without terminating the test process. */
int  w32_msvcrt_run_atexit(void);

#endif /* AURALITE_W32_MSVCRT_H */

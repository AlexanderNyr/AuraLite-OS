/* w32_abi.h — the Windows x64 <-> System V calling-convention boundary.
 *
 * WIN32_PLAN.md phase W32-4, and section 2.4 of that plan.
 *
 * Every function a PE image calls crosses this boundary:
 *
 *                    Windows x64            System V AMD64 (AuraLite)
 *   integer args     RCX RDX R8 R9          RDI RSI RDX RCX R8 R9
 *   shadow space     32 bytes, by caller    none
 *   callee-saved     + RSI, RDI, XMM6-15    RBX RBP R12-R15
 *   struct return    hidden ptr in RCX      hidden ptr in RDI
 *
 * Clang implements this with __attribute__((ms_abi)), which turns a whole
 * class of hand-written assembly into a compiler problem.  The danger is that
 * *forgetting* it is silent: the code compiles, links, and returns plausible
 * garbage, and the corruption surfaces somewhere else entirely.
 *
 * So W32ABI is not a convenience alias.  It exists so that every export is
 * spelled the same way, so a missing annotation is visible in review as a
 * missing token rather than as an absence, and so the tests below have one
 * thing to verify.
 */

#ifndef AURALITE_W32_ABI_H
#define AURALITE_W32_ABI_H

#if defined(__clang__) || defined(__GNUC__)
#  if defined(__x86_64__)
#    define W32ABI __attribute__((ms_abi))
#  else
#    define W32ABI
#  endif
#else
#  error "w32 requires a compiler with ms_abi support"
#endif

/* Win32 fundamental types.
 *
 * Declared here, from the documented widths, rather than pulled from a
 * vendored header: nothing is vendored yet (see w32/PROVENANCE.md), and these
 * few widths are facts about an ABI.  When mingw-w64's headers are imported
 * for the wider API surface, these stay compatible by construction because
 * the widths are what they are. */
#include <stdint.h>

typedef int32_t   W32_BOOL;
typedef uint32_t  W32_DWORD;
typedef uint16_t  W32_WORD;
typedef uint8_t   W32_BYTE;
typedef void     *W32_HANDLE;
typedef uint64_t  W32_ULONGLONG;

#define W32_TRUE   1
#define W32_FALSE  0

/* INVALID_HANDLE_VALUE is (HANDLE)-1, not NULL: a Win32 program tests
 * CreateFile's result against it, and getting this wrong makes every failure
 * look like success. */
#define W32_INVALID_HANDLE_VALUE ((W32_HANDLE)(intptr_t)-1)

#endif /* AURALITE_W32_ABI_H */

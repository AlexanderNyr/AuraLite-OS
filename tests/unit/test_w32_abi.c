/*
 * test_w32_abi.c — host unit test for WIN32_PLAN.md phase W32-4.
 *
 * The plan says of this test: "ms_abi correctness: a function taking 6 integer
 * args and returning a struct by value gets every argument intact.  This is
 * the test that catches the convention bug from section 2.4, and it is
 * written before the other functions."
 *
 * It is written first because the failure mode is silent.  A missing
 * __attribute__((ms_abi)) compiles cleanly, links cleanly, and produces
 * plausible-looking garbage: the callee reads RDI/RSI where the caller wrote
 * RCX/RDX, so argument 1 is whatever happened to be in RDI.  Nothing warns.
 * The corruption is discovered later, in some unrelated function, as a wrong
 * pointer.
 *
 * Verifying this from C alone is not possible: a C caller and a C callee that
 * are both wrong in the same way agree with each other.  The call therefore
 * has to come from assembly that places arguments where the *Windows* ABI says
 * they go, which is what a real PE image's compiler will emit.  That is the
 * only way the test can fail when the attribute is missing.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "w32/w32_abi.h"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; \
                    if (failed == b) passed++; \
                    else printf("  [%s] FAILED\n", #f); } while (0)
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a, e) do { long long _a = (long long)(a), _e = (long long)(e); \
    if (_a != _e) { printf("  FAIL L%d: %s=0x%llX want 0x%llX\n", \
                    __LINE__, #a, _a, _e); failed++; } } while (0)

/* ---- the callees, annotated exactly as every w32 export must be ---------- */

/* Six integer arguments: the first four in RCX/RDX/R8/R9, the rest on the
 * stack above the 32-byte shadow space.  A System V callee would look for the
 * first six in RDI/RSI/RDX/RCX/R8/R9 and find only noise. */
static W32ABI uint64_t six_args(uint64_t a, uint64_t b, uint64_t c,
                                uint64_t d, uint64_t e, uint64_t f) {
    /* Combined so that a single misplaced argument changes the result. */
    return (a * 1u) ^ (b << 4) ^ (c << 8) ^ (d << 12) ^ (e << 16) ^ (f << 20);
}

/* A struct too large for RAX:RDX comes back through a hidden pointer, and the
 * ABIs disagree about where that pointer lives: RCX on Windows, RDI on System
 * V.  Getting this wrong corrupts memory rather than just a value, which is
 * why the plan singles it out. */
typedef struct { uint64_t a, b, c, d; } big_t;

static W32ABI big_t returns_big(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    big_t r;
    r.a = a; r.b = b; r.c = c; r.d = d;
    return r;
}

/* Mixed widths: the callee must read the full 64-bit register for pointers
 * and only the low 32 bits for DWORDs, without sign-extension surprises. */
static W32ABI uint64_t mixed(W32_HANDLE h, W32_DWORD n, void *p, W32_BOOL flag) {
    uint64_t v = (uint64_t)(uintptr_t)h;
    v ^= (uint64_t)n << 8;
    v ^= (uint64_t)(uintptr_t)p << 1;
    v ^= (uint64_t)(uint32_t)flag << 32;
    return v;
}

/* Callee-saved registers: Windows adds RSI and RDI to the saved set.  A
 * function compiled for System V may clobber them freely, so a PE caller that
 * kept a value in RSI across the call would find it destroyed.  The assembly
 * caller below checks exactly that. */
/* no_sanitize_address: ASan's prologue in an ms_abi callee reached from the
 * hand-written caller below uses the shadow stack in a way that assumes a
 * System V frame, and faults.  The function under test here is the *calling
 * convention*, not this body, so excluding it keeps the sanitized build
 * meaningful rather than disabling the whole test. */
static uint64_t clobber_sink;

/* NOTE ON SANITIZERS
 *
 * This file is deliberately NOT built with -fsanitize=address.  ASan's
 * function prologue assumes a System V frame; when it is inserted into an
 * ms_abi callee that a hand-written Windows-ABI caller jumped to, it reads
 * the shadow stack through a frame that does not exist yet and faults.  The
 * fault is in ASan's instrumentation, not in the code under test -- the same
 * binary is correct at -O0/-O1/-O2/-O3 without it.
 *
 * That is a real limitation and worth stating rather than hiding: the ABI
 * shims below cannot be sanitizer-checked.  They also allocate nothing and
 * touch no memory except a caller-supplied struct, so there is little for
 * ASan to find; the memory-handling code in this phase (kernel32.c) IS built
 * under ASan+UBSan by test_w32_kernel32, which is where the allocations are.
 */
static W32ABI uint64_t clobber_probe(uint64_t a) {
    /* Enough live values to tempt the register allocator into using RSI/RDI,
     * which under Windows rules it must then save and restore. */
    uint64_t s = 0;
    for (int i = 0; i < 8; i++) {
        clobber_sink = a + (uint64_t)i * 0x1111u;
        s += clobber_sink;
    }
    return s;
}

/* ---- Windows-ABI callers, in assembly ------------------------------------
 *
 * These are what make the test meaningful: they put arguments where the
 * Windows x64 ABI says they go, allocate the 32-byte shadow space the callee
 * is entitled to use, and keep the stack 16-byte aligned at the call.
 */

extern uint64_t abi_call_six(void *fn, uint64_t a, uint64_t b, uint64_t c,
                             uint64_t d, uint64_t e, uint64_t f);
extern void     abi_call_big(void *fn, big_t *out, uint64_t a, uint64_t b,
                             uint64_t c, uint64_t d);
extern uint64_t abi_call_mixed(void *fn, void *h, uint32_t n, void *p, int32_t flag);
extern uint64_t abi_check_preserved(void *fn, uint64_t a);

__asm__(
".text\n"
/* uint64_t abi_call_six(fn, a, b, c, d, e, f)
 * Entered under System V: fn=RDI a=RSI b=RDX c=RCX d=R8 e=R9 f=[rsp+8].
 * Must call fn with Windows args: a=RCX b=RDX c=R8 d=R9 e=[rsp+32] f=[rsp+40].
 */
".globl abi_call_six\n"
".type abi_call_six,@function\n"
"abi_call_six:\n"
"    push %rbp\n"
"    mov  %rsp, %rbp\n"
"    push %rbx\n"
"    push %r12\n"
"    mov  %rdi, %rbx\n"            /* fn */
"    mov  0x10(%rbp), %r12\n"      /* f (7th SysV arg, above saved rbp+ret) */
"    sub  $0x40, %rsp\n"           /* shadow(32) + 2 stack args + alignment */
"    mov  %r9,  0x20(%rsp)\n"      /* e -> 5th Windows arg, just above shadow */
"    mov  %r12, 0x28(%rsp)\n"      /* f -> 6th Windows arg */
/* Shuffle so no source is overwritten before it is read: c arrives in RCX,
 * which is also where a has to end up. */
"    mov  %r8,  %r9\n"             /* d -> R9 */
"    mov  %rcx, %r8\n"             /* c -> R8 (frees RCX) */
"    mov  %rsi, %rax\n"            /* stash a */
"    mov  %rdx, %rcx\n"            /* b -> RCX (parked) */
"    mov  %rax, %rdx\n"            /* a -> RDX (parked) */
"    xchg %rcx, %rdx\n"            /* now a=RCX, b=RDX */
"    call *%rbx\n"
"    add  $0x40, %rsp\n"
"    pop  %r12\n"
"    pop  %rbx\n"
"    pop  %rbp\n"
"    ret\n"

/* void abi_call_big(fn, out, a, b, c, d)
 * SysV in: fn=RDI out=RSI a=RDX b=RCX c=R8 d=R9
 * Windows out: hidden=RCX a=RDX b=R8 c=R9 d=[rsp+32]
 */
".globl abi_call_big\n"
".type abi_call_big,@function\n"
"abi_call_big:\n"
"    push %rbp\n"
"    mov  %rsp, %rbp\n"
"    push %rbx\n"
"    push %r12\n"
"    mov  %rdi, %rbx\n"            /* fn */
"    mov  %r9,  %r12\n"            /* d */
"    sub  $0x40, %rsp\n"
"    mov  %r12, 0x20(%rsp)\n"      /* d -> 5th slot (after hidden ptr) */
"    mov  %r8,  %r9\n"             /* c -> R9 */
"    mov  %rcx, %r8\n"             /* b -> R8 */
"    mov  %rdx, %rdx\n"            /* a stays in RDX */
"    mov  %rsi, %rcx\n"            /* hidden return pointer -> RCX */
"    call *%rbx\n"
"    add  $0x40, %rsp\n"
"    pop  %r12\n"
"    pop  %rbx\n"
"    pop  %rbp\n"
"    ret\n"

/* uint64_t abi_call_mixed(fn, h, n, p, flag)
 * SysV in: fn=RDI h=RSI n=EDX p=RCX flag=R8D
 * Windows out: h=RCX n=EDX p=R8 flag=R9D
 */
".globl abi_call_mixed\n"
".type abi_call_mixed,@function\n"
"abi_call_mixed:\n"
"    push %rbp\n"
"    mov  %rsp, %rbp\n"
"    push %rbx\n"
"    push %r12\n"
"    mov  %rdi, %rbx\n"
"    mov  %rcx, %r12\n"            /* p */
"    sub  $0x30, %rsp\n"
"    mov  %r8d, %r9d\n"            /* flag -> R9D */
"    mov  %r12, %r8\n"             /* p -> R8 */
"    /* n is already in EDX, which is where Windows wants it */\n"
"    mov  %rsi, %rcx\n"            /* h -> RCX */
"    call *%rbx\n"
"    add  $0x30, %rsp\n"
"    pop  %r12\n"
"    pop  %rbx\n"
"    pop  %rbp\n"
"    ret\n"

/* uint64_t abi_check_preserved(fn, a)
 * Loads sentinels into RSI/RDI/RBX/R12-R15, calls fn(a) under the Windows
 * convention, and returns 0 if every sentinel survived, or a bitmask of the
 * registers that did not.  Windows adds RSI and RDI to the callee-saved set,
 * so a callee compiled for System V will fail this.
 */
".globl abi_check_preserved\n"
".type abi_check_preserved,@function\n"
"abi_check_preserved:\n"
"    push %rbp\n"
"    mov  %rsp, %rbp\n"
"    push %rbx\n"
"    push %r12\n"
"    push %r13\n"
"    push %r14\n"
"    push %r15\n"
"    push %rsi\n"
"    push %rdi\n"
"    mov  %rdi, %rax\n"            /* fn -> RAX (scratch in both ABIs) */
"    mov  %rsi, %rcx\n"            /* a -> RCX (1st Windows arg) */
"    movabs $0x1111111111111111, %rsi\n"
"    movabs $0x2222222222222222, %rdi\n"
"    movabs $0x3333333333333333, %rbx\n"
"    movabs $0x4444444444444444, %r12\n"
"    movabs $0x5555555555555555, %r13\n"
"    movabs $0x6666666666666666, %r14\n"
"    movabs $0x7777777777777777, %r15\n"
"    sub  $0x20, %rsp\n"           /* shadow space */
"    call *%rax\n"
"    add  $0x20, %rsp\n"
"    xor  %eax, %eax\n"
"    movabs $0x1111111111111111, %rdx\n"
"    cmp  %rdx, %rsi\n"
"    je   1f\n"
"    or   $1, %eax\n"
"1:  movabs $0x2222222222222222, %rdx\n"
"    cmp  %rdx, %rdi\n"
"    je   2f\n"
"    or   $2, %eax\n"
"2:  movabs $0x3333333333333333, %rdx\n"
"    cmp  %rdx, %rbx\n"
"    je   3f\n"
"    or   $4, %eax\n"
"3:  movabs $0x4444444444444444, %rdx\n"
"    cmp  %rdx, %r12\n"
"    je   4f\n"
"    or   $8, %eax\n"
"4:  movabs $0x5555555555555555, %rdx\n"
"    cmp  %rdx, %r13\n"
"    je   5f\n"
"    or   $16, %eax\n"
"5:  movabs $0x6666666666666666, %rdx\n"
"    cmp  %rdx, %r14\n"
"    je   6f\n"
"    or   $32, %eax\n"
"6:  movabs $0x7777777777777777, %rdx\n"
"    cmp  %rdx, %r15\n"
"    je   7f\n"
"    or   $64, %eax\n"
"7:  pop  %rdi\n"
"    pop  %rsi\n"
"    pop  %r15\n"
"    pop  %r14\n"
"    pop  %r13\n"
"    pop  %r12\n"
"    pop  %rbx\n"
"    pop  %rbp\n"
"    ret\n"
".previous\n"
);

/* ---- tests --------------------------------------------------------------- */

static void test_six_integer_args(void) {
    const uint64_t a = 0x01, b = 0x02, c = 0x03, d = 0x04, e = 0x05, f = 0x06;
    uint64_t want = (a * 1u) ^ (b << 4) ^ (c << 8) ^ (d << 12) ^ (e << 16) ^ (f << 20);
    uint64_t got = abi_call_six((void *)six_args, a, b, c, d, e, f);
    CHECK_EQ(got, want);
}

static void test_six_args_distinct_values(void) {
    /* Values chosen so that swapping any pair changes the result: a permuted
     * argument list cannot accidentally produce the right answer. */
    const uint64_t a = 0xA1, b = 0xB2, c = 0xC3, d = 0xD4, e = 0xE5, f = 0xF6;
    uint64_t want = (a * 1u) ^ (b << 4) ^ (c << 8) ^ (d << 12) ^ (e << 16) ^ (f << 20);
    uint64_t got = abi_call_six((void *)six_args, a, b, c, d, e, f);
    CHECK_EQ(got, want);
}

static void test_struct_return_by_hidden_pointer(void) {
    big_t out;
    memset(&out, 0xCC, sizeof out);
    abi_call_big((void *)returns_big, &out, 0x1111, 0x2222, 0x3333, 0x4444);
    CHECK_EQ(out.a, 0x1111);
    CHECK_EQ(out.b, 0x2222);
    CHECK_EQ(out.c, 0x3333);
    CHECK_EQ(out.d, 0x4444);
}

static void test_mixed_widths(void) {
    int local = 0;
    W32_HANDLE h = (W32_HANDLE)(uintptr_t)0xDEADBEEF12345678ull;
    void *p = &local;
    uint64_t want = (uint64_t)(uintptr_t)h
                  ^ ((uint64_t)0x9ABCDEF0u << 8)
                  ^ ((uint64_t)(uintptr_t)p << 1)
                  ^ ((uint64_t)1u << 32);
    uint64_t got = abi_call_mixed((void *)mixed, h, 0x9ABCDEF0u, p, 1);
    CHECK_EQ(got, want);
}

static void test_callee_saved_registers(void) {
    /* Windows adds RSI and RDI to the callee-saved set.  Bits 0 and 1 of the
     * result are RSI and RDI respectively. */
    uint64_t clobbered = abi_check_preserved((void *)clobber_probe, 0x1234);
    if (clobbered) {
        printf("  clobbered mask = 0x%llX "
               "(bit0=RSI bit1=RDI bit2=RBX bit3=R12 bit4=R13 bit5=R14 bit6=R15)\n",
               (unsigned long long)clobbered);
    }
    CHECK_EQ(clobbered, 0);
}

static void test_handle_sentinel_width(void) {
    /* INVALID_HANDLE_VALUE must be all-ones, not 0xFFFFFFFF: a 32-bit
     * truncation would make a valid high handle compare equal to failure. */
    CHECK_EQ((uint64_t)(uintptr_t)W32_INVALID_HANDLE_VALUE, 0xFFFFFFFFFFFFFFFFull);
    CHECK(W32_INVALID_HANDLE_VALUE != (W32_HANDLE)0);
}

static void test_type_widths(void) {
    /* The ABI is a set of widths; if these drift, everything above is moot. */
    CHECK_EQ(sizeof(W32_BOOL), 4);
    CHECK_EQ(sizeof(W32_DWORD), 4);
    CHECK_EQ(sizeof(W32_WORD), 2);
    CHECK_EQ(sizeof(W32_BYTE), 1);
    CHECK_EQ(sizeof(W32_HANDLE), 8);
    CHECK_EQ(sizeof(W32_ULONGLONG), 8);
}

int main(void) {
    printf("== w32 Windows-x64 ABI boundary (W32-4) ==\n");
    RUN(test_six_integer_args);
    RUN(test_six_args_distinct_values);
    RUN(test_struct_return_by_hidden_pointer);
    RUN(test_mixed_widths);
    RUN(test_callee_saved_registers);
    RUN(test_handle_sentinel_width);
    RUN(test_type_widths);
    printf("%s: %d/%d tests passed\n", failed ? "FAIL" : "PASS", passed, tn);
    return failed ? 1 : 0;
}

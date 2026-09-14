/* w32/include/w32/w32_seh.h — W32APP_PLAN.md phase W32A-4.
 *
 * Table-driven Win64 structured exception handling over .pdata/.xdata.
 * This header is the whole ABI surface: the on-disk unwind structures
 * (RUNTIME_FUNCTION / UNWIND_INFO / scope tables), the dispatcher records
 * (EXCEPTION_RECORD / CONTEXT / DISPATCHER_CONTEXT), and the runtime API
 * (Rtl* + RaiseException + __C_specific_handler + the unhandled filter).
 *
 * The implementation (w32/src/w32_seh.c) is shared between the host unit
 * test and the guest loader (the W32-3 D2 pattern): the parser and the
 * unwinder are pure code over caller-supplied image bytes, with no OS
 * calls on the parse/unwind path.  Only the fault entry (signals) and the
 * terminate box (compositor) are guest-only.
 *
 * Documented divergences (residue, not silence):
 *   - Fault CONTEXTs carry zeroed XMM registers: AuraLite's mcontext
 *     (lib/libc/include/signal.h) does not save them, so there is nothing
 *     to restore.  Frames unwound past the fault read real XMM save slots
 *     from the stack; only the faulting frame's XMMs read zero.
 *   - RtlUnwindEx's HistoryTable is accepted and ignored (a caching hint
 *     on Windows, not a correctness input).
 *   - Typed C++ catch matching is the D7 gap: _CxxThrowException runs
 *     cleanups frame by frame and then terminates with W32-CXX-
 *     TYPED-CATCH-GAP instead of matching catch clauses.
 */
#ifndef AURALITE_W32_SEH_H
#define AURALITE_W32_SEH_H

#include <stddef.h>
#include <stdint.h>

#include "w32/w32_abi.h"

/* ---- on-disk unwind structures (PE .pdata / .xdata) ----------------------
 *
 * All addresses in these tables are image RVAs (uint32), exactly as the
 * Microsoft x64 unwind documentation lays them out.  Field order is the
 * interface: parsers read these off the mapped image.
 */

/* One .pdata entry: 12 bytes, sorted by BeginAddress. */
typedef struct {
    uint32_t begin_address;   /* RVA of the function's first byte */
    uint32_t end_address;     /* RVA one past the function's last byte */
    uint32_t unwind_data;     /* RVA of UNWIND_INFO (bit 0 set: chained
                               * RUNTIME_FUNCTION indirection instead) */
} w32_runtime_function_t;

#define W32_RUNTIME_FUNCTION_INDIRECT 1u

/* Unwind operation codes (UNWIND_CODE low nibble). */
#define W32_UWOP_PUSH_NONVOL    0u
#define W32_UWOP_ALLOC_LARGE    1u
#define W32_UWOP_ALLOC_SMALL    2u
#define W32_UWOP_SET_FPREG      3u
#define W32_UWOP_SAVE_NONVOL    4u
#define W32_UWOP_SAVE_NONVOL_FAR 5u
#define W32_UWOP_EPILOG         6u   /* v2 only; refused, never misread */
#define W32_UWOP_SPARE          7u   /* never valid */
#define W32_UWOP_SAVE_XMM128    8u
#define W32_UWOP_SAVE_XMM128_FAR 9u
#define W32_UWOP_PUSH_MACHFRAME 10u

/* UNWIND_INFO flags: the HIGH 5 bits of the version byte (the low 3 are
 * the version).  Byte values, matching what the parser masks out. */
#define W32_UNW_FLAG_EHANDLER 0x08u
#define W32_UNW_FLAG_UHANDLER 0x10u
#define W32_UNW_FLAG_CHAININFO 0x20u

/* One unwind code: 2 bytes.  Codes are stored in reverse prolog order
 * (the last prolog operation comes first). */
typedef struct {
    uint8_t code_offset;      /* offset into the prolog of the op's end */
    uint8_t op_info_op;       /* high nibble OpInfo, low nibble UnwindOp */
} w32_unwind_code_t;

/* UNWIND_INFO header, followed by CountOfCodes unwind codes (padded to an
 * even count with a zero slot), then optionally the handler RVA and the
 * handler-specific data (scope table / C++ funclet map / chained
 * RUNTIME_FUNCTION triple). */
typedef struct {
    uint8_t ver_flags;        /* low 3 bits version (1), high 5 flags */
    uint8_t size_of_prolog;
    uint8_t count_of_codes;
    uint8_t frame_reg_off;    /* low nibble frame register, high nibble
                               * scaled frame offset (x16) */
} w32_unwind_info_t;

#define W32_UNW_VERSION_MASK 0x07u
#define W32_UNW_VERSION_1    0x01u

/* Nonvolatile register numbers as the unwind codes name them. */
#define W32_UNW_REG_RAX 0u
#define W32_UNW_REG_RCX 1u
#define W32_UNW_REG_RDX 2u
#define W32_UNW_REG_RBX 3u
#define W32_UNW_REG_RSP 4u
#define W32_UNW_REG_RBP 5u
#define W32_UNW_REG_RSI 6u
#define W32_UNW_REG_RDI 7u
/* 8..15 are R8..R15. */

/* __C_specific_handler scope table (HandlerData for EHANDLER frames).
 * ScopeRecord.HandlerAddress == 1 (pure cleanup, __finally) runs on the
 * unwind pass; otherwise it is a filter RVA whose EXCEPTION_* return value
 * decides the dispatch.  JumpTarget is the RVA control resumes at when the
 * filter says EXECUTE_HANDLER. */
typedef struct {
    uint32_t begin_address;
    uint32_t end_address;
    uint32_t handler_address; /* filter RVA, or 1 for a pure cleanup */
    uint32_t jump_target;
} w32_scope_record_t;

typedef struct {
    uint32_t count;
    /* w32_scope_record_t scopes[count] follows. */
} w32_scope_table_t;

#define W32_SCOPE_CLEANUP 1u

/* ---- dispatcher records -------------------------------------------------- */

/* Dispositions a language handler returns. */
#define W32_EXCEPTION_CONTINUE_EXECUTION_NT 0   /* resume at fault pc */
#define W32_EXCEPTION_CONTINUE_SEARCH_NT    1   /* try the caller frame */
#define W32_EXCEPTION_NESTED_EXCEPTION_NT   2   /* nested record chained */
#define W32_EXCEPTION_COLLIDED_UNWIND_NT    3   /* unwind during unwind */

/* EXCEPTION_RECORD.ExceptionFlags bits. */
#define W32_EXCEPTION_NONCONTINUABLE  0x01u
#define W32_EXCEPTION_UNWINDING       0x02u
#define W32_EXCEPTION_EXIT_UNWIND     0x04u
#define W32_EXCEPTION_STACK_INVALID   0x08u
#define W32_EXCEPTION_NESTED_CALL     0x10u
#define W32_EXCEPTION_TARGET_UNWIND   0x20u
#define W32_EXCEPTION_COLLIDED_UNWIND 0x40u

#define W32_EXCEPTION_MAXIMUM_PARAMETERS 15u

typedef struct w32_exception_record w32_exception_record_t;
struct w32_exception_record {
    uint32_t code;
    uint32_t flags;
    struct w32_exception_record *nested;
    void *address;
    uint32_t num_params;
    uint64_t params[W32_EXCEPTION_MAXIMUM_PARAMETERS];
};

/* 128-bit FP slot (Windows M128A). */
typedef struct {
    uint64_t low;
    int64_t high;
} w32_m128a_t;

/* Win64 CONTEXT, the documented 1232-byte layout.  The unwinder reads and
 * writes the integer half; the FP half round-trips so personality routines
 * and debuggers see a well-formed record. */
typedef struct {
    uint64_t p1_home, p2_home, p3_home, p4_home, p5_home, p6_home;
    uint32_t context_flags;
    uint32_t mx_csr;
    uint16_t seg_cs, seg_ds, seg_es, seg_fs, seg_gs, seg_ss;
    uint32_t eflags;
    uint64_t dr0, dr1, dr2, dr3, dr6, dr7;
    uint64_t rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip;
    /* XMM_SAVE_AREA32 (512 bytes): 32 bytes of legacy FP header, 8 FP
     * slots, 16 XMM slots, 96 bytes reserved. */
    uint8_t flt_legacy[32];
    w32_m128a_t flt_regs[8];
    w32_m128a_t xmm[16];
    uint8_t flt_reserved[96];
    w32_m128a_t vector_reg[26];
    uint64_t vector_control;
    uint64_t debug_control;
    uint64_t last_branch_to_rip;
    uint64_t last_branch_from_rip;
    uint64_t last_exception_to_rip;
    uint64_t last_exception_from_rip;
} w32_context_t;

#define W32_CONTEXT_AMD64 0x100000u
#define W32_CONTEXT_CONTROL (W32_CONTEXT_AMD64 | 0x01u)
#define W32_CONTEXT_INTEGER (W32_CONTEXT_AMD64 | 0x02u)
#define W32_CONTEXT_SEGMENTS (W32_CONTEXT_AMD64 | 0x04u)
#define W32_CONTEXT_FLOATING_POINT (W32_CONTEXT_AMD64 | 0x08u)
#define W32_CONTEXT_DEBUG_REGISTERS (W32_CONTEXT_AMD64 | 0x10u)
#define W32_CONTEXT_FULL (W32_CONTEXT_CONTROL | W32_CONTEXT_INTEGER | \
                          W32_CONTEXT_FLOATING_POINT)
#define W32_CONTEXT_ALL (W32_CONTEXT_FULL | W32_CONTEXT_SEGMENTS | \
                         W32_CONTEXT_DEBUG_REGISTERS)

typedef struct {
    w32_exception_record_t *record;
    w32_context_t *context;
} w32_exception_pointers_t;

/* What the dispatcher hands a language handler.  TargetIp is set on the
 * unwind pass only; ScopeIndex is __C_specific_handler's scratch. */
typedef struct {
    uint64_t control_pc;
    uint64_t image_base;
    w32_runtime_function_t *function_entry;
    uint64_t establisher_frame;
    uint64_t target_ip;
    w32_context_t *context_record;
    void *language_handler;
    void *handler_data;
    void *history_table;      /* accepted, ignored (see header note) */
    uint32_t scope_index;
    uint32_t control_pc_is_unwinding;
} w32_dispatcher_context_t;

typedef int32_t (W32ABI *w32_language_handler_fn)(
    w32_exception_record_t *record,
    uint64_t establisher_frame,
    w32_context_t *context,
    w32_dispatcher_context_t *dispatch);

/* ---- the parser (host + loader shared) ----------------------------------- */

/* Binary-search .pdata for the entry covering pc_rva.  Returns NULL when
 * none covers it.  pdata_bytes must be a multiple of 12; anything else
 * refuses (hostile input, never a partial read). */
const w32_runtime_function_t *w32_seh_lookup(const uint8_t *pdata,
                                             size_t pdata_bytes,
                                             uint32_t pc_rva);

/* Validate one UNWIND_INFO chain reachable from @unwind_rva: version,
 * code counts, chained-info recursion (capped), handler/data RVAs inside
 * @image_span.  Returns 0 when the chain is well-formed, negative when it
 * is hostile.  RtlVirtualUnwind calls this before trusting a chain. */
int w32_seh_validate_chain(const uint8_t *image, size_t image_span,
                           uint32_t unwind_rva);

/* ---- the runtime (REAL exports) ------------------------------------------ */

w32_runtime_function_t *W32ABI RtlLookupFunctionEntry(uint64_t control_pc,
                                                      uint64_t *image_base,
                                                      void *history_table);
void *W32ABI RtlPcToFileHeader(void *pc, void **image_base);
void W32ABI RtlCaptureContext(w32_context_t *context);
void *W32ABI RtlVirtualUnwind(uint32_t handler_type,
                              uint64_t image_base,
                              uint64_t control_pc,
                              w32_runtime_function_t *function_entry,
                              w32_context_t *context,
                              void **handler_data,
                              uint64_t *establisher_frame,
                              void *context_pointers);
void W32ABI RtlUnwind(void *target_frame, void *target_ip,
                      w32_exception_record_t *record, void *return_value);
void W32ABI RtlUnwindEx(void *target_frame, void *target_ip,
                        w32_exception_record_t *record, void *return_value,
                        w32_context_t *context, void *history_table);

int32_t W32ABI __C_specific_handler(w32_exception_record_t *record,
                                    uint64_t establisher_frame,
                                    w32_context_t *context,
                                    w32_dispatcher_context_t *dispatch);
int32_t W32ABI _XcptFilter(uint32_t code, w32_exception_pointers_t *info);

void W32ABI RaiseException(uint32_t code, uint32_t flags, uint32_t num_args,
                           const uint64_t *args);

typedef int32_t (W32ABI *w32_top_filter_fn)(w32_exception_pointers_t *info);
w32_top_filter_fn W32ABI SetUnhandledExceptionFilter(w32_top_filter_fn filter);
int32_t W32ABI UnhandledExceptionFilter(w32_exception_pointers_t *info);

/* msvcrt C++ runtime surface (W32A-13-owned, implemented here because the
 * phase requires the named termination).  _CxxThrowException runs cleanups
 * frame by frame, then terminates with W32-CXX-TYPED-CATCH-GAP (D7: catch
 * clauses are never matched). */
void W32ABI _CxxThrowException(void *obj, void *throw_info);
void W32ABI w32_cxx_terminate(void);   /* bound as ?terminate@@YAXXZ */
void W32ABI _purecall(void);

/* Named termination messages (stderr + serial + dialog text).  Greppable
 * residue: a crash shaped like a mystery would print none of these. */
#define W32_MSG_CXX_TYPED_CATCH_GAP "W32-CXX-TYPED-CATCH-GAP"
#define W32_MSG_CXX_TERMINATE "W32-CXX-TERMINATE"
#define W32_MSG_PURECALL "W32-PURECALL"
#define W32_MSG_COLLIDED_UNWIND "W32-COLLIDED-UNWIND"
#define W32_MSG_CXX_DTOR_THROW "W32-CXX-DTOR-THROW"
#define W32_MSG_UNHANDLED "W32-SEH-UNHANDLED"

/* ---- dispatch-frame lifecycle (loader-internal) -------------------------- */

/* Birth-allocate / free one thread's dispatch frame (opaque).  Never
 * called on the fault path (allocation there could deadlock against a
 * fault inside the allocator); thr.c mirrors the tls_blocks sites. */
void *w32_seh_frame_new(void);
void w32_seh_frame_free(void *frame);

#ifndef AURALITE_W32_HOST_TEST
/* The current thread's dispatch frame (kernel32_thr.c owns it). */
void *w32_thr_current_seh_frame(void);
#endif

#ifdef AURALITE_W32_HOST_TEST
/* Host-test hooks: synthetic images for the module queries, and the
 * harness-owned frame RaiseException/RtlUnwind find. */
void w32_seh_test_add_image(uint64_t base, size_t span);
void w32_seh_test_reset(void);
void w32_seh_test_set_frame(void *frame);
#endif

/* ---- fault entry + teardown (loader-internal) ---------------------------- */

/* Arm the fault entry: SIGSEGV/SIGFPE/SIGILL/SIGBUS (and SIGTRAP for int3)
 * dispatch through the unwinder.  Idempotent; returns 0, or -1 when the
 * handlers could not be installed.  Replaces the W32-6 sigsetjmp arming
 * with the same call shape, so the w32run startup sequence is unchanged. */
int w32_seh_init(void);

/* True once w32_seh_init armed the handlers. */
int w32_seh_armed(void);

#endif /* AURALITE_W32_SEH_H */

/* w32_teb.h — the W32A-3 TEB-lite: per-thread state reached through %gs.
 *
 * One 4 KiB page per thread, installed as the thread's USER GS.base via
 * ARCH_SET_GS (the main thread at w32_thr_init, every other thread in its
 * CreateThread trampoline before user code runs).  The kernel's swapgs
 * protocol (see the audit note in kernel/arch/x86_64/isr_stubs.asm) makes
 * this page live exactly while Ring 3 runs.
 *
 * OFFSET PROVENANCE (read before "fixing" a number): only ONE offset in
 * this struct is constrained from the outside — ThreadLocalStoragePointer
 * at +0x58, which W32APP_PLAN.md names as the GS:[0x58]-family contract
 * the ladder's CRT uses for __declspec(thread) variables (TEB+0x58 points
 * at the per-module block array; the compiler indexes it by the module's
 * TLS slot).  Every other offset is an AuraLite choice, pinned by
 * static_assert below and by the guest fixtures, and W32A-13 (msvcrt)
 * re-validates the whole layout against the real CRT's raw GS reads when
 * that phase measures them.  Nothing here is copied from memory of the
 * Windows TEB: resemblance of +0x30 (Self) and +0x58 to the real thing is
 * deliberate compatibility surface, documented as such.
 *
 * FS is NOT this struct: libc owns FS (errno at %fs:8, see the FIX_R3 note
 * in lib/libc/include/pthread_tls.h).  w32 threads carry a separate FS
 * cell (struct w32_fscell in kernel32_thr.c) whose first fields mirror the
 * pthread TCB so libc errno keeps working; that coupling is ABI, pinned by
 * numeric static_asserts there.
 */

#ifndef AURALITE_W32_TEB_H
#define AURALITE_W32_TEB_H

#include "w32/w32_abi.h"

#include <stddef.h>
#include <stdint.h>

/* Slot counts.  64 TLS slots and 128 FLS slots are the Win32 minima a
 * program may rely on (TLS_MINIMUM_AVAILABLE / FLS_MAXIMUM_AVAILABLE);
 * the ladder never needs more, and the page budget fits exactly these. */
#define W32_TEB_TLS_SLOTS 64
#define W32_TEB_FLS_SLOTS 128

/* Loader-owned per-module TLS slot count: the exe plus this many DLLs can
 * carry implicit (__declspec(thread)) TLS at once.  Slot 0 is always the
 * main image.  The ladder's heaviest image uses a handful; 16 is headroom
 * with a loud refusal (not silent truncation) past it. */
#define W32_TLS_MODULES_MAX 16

/* ------------------------------------------------------------------ PEB
 * Real programs read the PEB.  Notepad++ 8.8.9 reaches it while loading its
 * configuration: TEB+0x60 (ProcessEnvironmentBlock) -> PEB+0x20
 * (ProcessParameters) -> dereference.  Our TEB used to keep the TlsAlloc
 * slots at +0x60, so that read handed the CRT a NULL and it faulted (the
 * "shcore.dll NULL dereference" of the 2026-10-16 receipts).  The offsets
 * below are the published x64 ones for every field a program can observe;
 * fields AuraLite does not implement stay zero and say so.
 */
struct w32_unicode_string_teb {
    uint16_t length;              /* bytes, not characters */
    uint16_t maximum_length;
    void    *buffer;
};

struct w32_rtl_user_process_parameters {
    uint32_t maximum_length;      /* +0x00 */
    uint32_t length;              /* +0x04 */
    uint32_t flags;               /* +0x08 */
    uint32_t debug_flags;         /* +0x0c */
    void    *console_handle;      /* +0x10 */
    uint32_t console_flags;       /* +0x18 */
    uint32_t pad_1c;
    void    *std_input;           /* +0x20 */
    void    *std_output;          /* +0x28 */
    void    *std_error;           /* +0x30 */
    struct w32_unicode_string_teb current_directory_path; /* +0x38 */
    void    *current_directory_handle;                   /* +0x48 */
    struct w32_unicode_string_teb dll_path;              /* +0x50 */
    struct w32_unicode_string_teb image_path_name;       /* +0x60 */
    struct w32_unicode_string_teb command_line;          /* +0x70 */
    void    *environment;         /* +0x80 */
    uint32_t starting_x;          /* +0x88 */
    uint32_t starting_y;          /* +0x8c */
    uint32_t count_x;             /* +0x90 */
    uint32_t count_y;             /* +0x94 */
    uint32_t count_chars_x;       /* +0x98 */
    uint32_t count_chars_y;       /* +0x9c */
    uint32_t fill_attribute;      /* +0xa0 */
    uint32_t window_flags;        /* +0xa4 */
    uint32_t show_window_flags;   /* +0xa8 */
    uint32_t pad_ac;
};

struct w32_peb {
    uint8_t  inherited_address_space;        /* +0x00 */
    uint8_t  read_image_file_exec_options;   /* +0x01 */
    uint8_t  being_debugged;                 /* +0x02: honest 0 (no debugger) */
    uint8_t  bitfield;                       /* +0x03 */
    uint64_t mutant;                         /* +0x08: unused */
    void    *image_base_address;             /* +0x10: the mapped EXE */
    void    *ldr;                            /* +0x18: NULL (no loader data) */
    struct w32_rtl_user_process_parameters *process_parameters; /* +0x20 */
    uint64_t sub_system_data;                /* +0x28: 0 */
    void    *process_heap;                   /* +0x30: sentinel (see below) */
    uint64_t fast_peb_lock;                  /* +0x38: 0 */
    uint64_t atl_thunk_slist_ptr;            /* +0x40: 0 */
    uint64_t ifeo_key;                       /* +0x48: 0 */
    uint32_t cross_process_flags;            /* +0x50 */
    uint32_t pad_54;
    void    *kernel_callback_table;          /* +0x58: NULL */
    uint32_t system_reserved;                /* +0x60 */
    uint32_t atl_thunk_slist_ptr32;
    void    *api_set_map;                    /* +0x68: NULL (no api-set map) */
    uint32_t tls_expansion_counter;          /* +0x70 */
    uint32_t pad_74;
    void    *tls_bitmap;                     /* +0x78: NULL */
    uint32_t tls_bitmap_bits[2];             /* +0x80 */
    void    *read_only_shared_memory_base;   /* +0x88: NULL */
    void    *shared_data;                    /* +0x90: NULL */
    void   **read_only_static_server_data;   /* +0x98: NULL */
    void    *ansi_code_page_data;            /* +0xa0: NULL */
    void    *oem_code_page_data;             /* +0xa8: NULL */
    void    *unicode_case_table_data;        /* +0xb0: NULL */
    uint32_t number_of_processors;           /* +0xb8 */
    uint32_t nt_global_flag;                 /* +0xbc */
    uint64_t critical_section_timeout;       /* +0xc0 */
    uint64_t heap_segment_reserve;           /* +0xc8 */
    uint64_t heap_segment_commit;            /* +0xd0 */
    uint64_t heap_decommit_total_free_threshold; /* +0xd8 */
    uint64_t heap_decommit_free_block_threshold; /* +0xe0 */
    uint32_t number_of_heaps;                /* +0xe8 */
    uint32_t maximum_number_of_heaps;        /* +0xec */
    void   **process_heaps;                  /* +0xf0: NULL */
    void    *gdi_shared_handle_table;        /* +0xf8: NULL */
    void    *process_starter_helper;         /* +0x100: NULL */
    uint32_t gdi_dc_attribute_list;          /* +0x108 */
    uint32_t loader_lock;                    /* +0x10c */
    uint32_t os_major_version;               /* +0x110 */
    uint32_t os_minor_version;               /* +0x114 */
    uint16_t os_build_number;                /* +0x118 */
    uint16_t os_csd_version;                 /* +0x11a */
    uint32_t os_platform_id;                 /* +0x11c */
    uint32_t image_subsystem;                /* +0x120 */
    uint32_t image_subsystem_major;          /* +0x124 */
    uint32_t image_subsystem_minor;          /* +0x128 */
    uint64_t active_process_affinity_mask;   /* +0x130 */
    uint32_t gdi_handle_buffer[60];          /* +0x138 */
    void    *post_process_init_routine;      /* +0x228: NULL */
    void    *tls_expansion_bitmap;           /* +0x230: NULL */
    uint32_t tls_expansion_bitmap_bits[32];  /* +0x238 */
    uint32_t session_id;                     /* +0x2b8 */
    uint64_t app_compat_flags;               /* +0x2c0 */
    uint64_t app_compat_flags_user;          /* +0x2c8 */
    void    *shim_data;                      /* +0x2d0: NULL */
    void    *app_compat_info;                /* +0x2d8: NULL */
    struct w32_unicode_string_teb csd_version; /* +0x2e0 */
};

struct w32_teb {
    uint64_t nt_exception_list;   /* +0x00: reserved, always 0 (no SEH chain) */
    uint64_t nt_stack_base;       /* +0x08: thread stack top (highest addr) */
    uint64_t nt_stack_limit;      /* +0x10: thread stack bottom (lowest addr) */
    uint64_t nt_subsystem_tib;    /* +0x18: reserved, always 0 */
    uint64_t nt_fiber_data;       /* +0x20: reserved (fibers: none, honest) */
    uint64_t nt_arbitrary;        /* +0x28: reserved, always 0 */
    uint64_t self;                /* +0x30: this TEB's own address */
    uint32_t client_pid;          /* +0x38: process id (main thread's tid) */
    uint32_t client_tid;          /* +0x3c: this thread's tid */
    void    *thread_obj;          /* +0x40: w32-internal struct w32_thread* */
    uint32_t pad_48;              /* +0x48: real TEB: ActiveRpcHandle area */
    uint32_t pad_4c;
    uint64_t pad_50;              /* +0x50: reserved */
    uint64_t tls_storage_ptr;     /* +0x58: ThreadLocalStoragePointer */
    struct w32_peb *peb;          /* +0x60: ProcessEnvironmentBlock */
    uint32_t last_error;          /* +0x68: LastErrorValue (published offset) */
    uint32_t last_status;         /* +0x6c */
    uint32_t count_owned_locks;   /* +0x70 */
    uint32_t hard_errors_disabled;/* +0x74 */
    /* Self-sizing: whatever is left between the fixed head and TlsSlots.
     * Fillers sized by hand break the instant the head changes. */
    uint64_t reserved_1[(0x1480 - 0x78) / 8];
    void    *tls_slots[W32_TEB_TLS_SLOTS]; /* +0x1480: TlsAlloc slots */
    void    *fls_slots[W32_TEB_FLS_SLOTS]; /* +0x1680: FlsAlloc slots
                                            * (Windows keeps FLS in the PEB;
                                            *  ours lives here and the TEB is
                                            *  the only per-thread block we
                                            *  install) */
};

/* Two pages: 0x1480 + 64*8 + 128*8 = 0x1a80 no longer fits in one. */
#define W32_TEB_SIZE 8192

/* The layout contract, pinned at compile time.  The guest fixtures pin it
 * again at run time with raw %gs reads. */
_Static_assert(offsetof(struct w32_teb, self) == 0x30, "TEB Self");
_Static_assert(offsetof(struct w32_teb, tls_storage_ptr) == 0x58,
               "TEB ThreadLocalStoragePointer");
_Static_assert(offsetof(struct w32_teb, peb) == 0x60, "TEB PEB");
_Static_assert(offsetof(struct w32_teb, last_error) == 0x68,
               "TEB LastErrorValue");
_Static_assert(offsetof(struct w32_teb, tls_slots) == 0x1480, "TEB TlsSlots");
_Static_assert(offsetof(struct w32_teb, fls_slots) == 0x1680, "TEB FlsSlots");
_Static_assert(sizeof(struct w32_teb) <= W32_TEB_SIZE, "TEB fits two pages");
_Static_assert(offsetof(struct w32_peb, process_parameters) == 0x20,
               "PEB ProcessParameters");
_Static_assert(offsetof(struct w32_peb, process_heap) == 0x30, "PEB ProcessHeap");
_Static_assert(offsetof(struct w32_peb, number_of_processors) == 0xb8,
               "PEB NumberOfProcessors");
_Static_assert(offsetof(struct w32_peb, os_major_version) == 0x110, "PEB OSMajor");
_Static_assert(offsetof(struct w32_peb, image_subsystem) == 0x120,
               "PEB ImageSubsystem");
_Static_assert(offsetof(struct w32_rtl_user_process_parameters,
                        image_path_name) == 0x60, "RTL ImagePathName");
_Static_assert(offsetof(struct w32_rtl_user_process_parameters,
                        command_line) == 0x70, "RTL CommandLine");

/* The process PEB, allocated by w32_thr_init() and shared by every thread
 * (one process, one PEB -- Windows behaves the same way).  NULL before
 * w32_thr_init has run. */
struct w32_peb *w32_peb_get(void);

/* Called by the module registrar once the main image is mapped: publishes
 * ImageBaseAddress and ImageSubsystem from the image's own optional header.
 * Either argument may be 0 (not yet known); a later call overwrites. */
void w32_peb_publish_exe(void *image_base, uint32_t subsystem);

/* The current thread's TEB, or NULL before w32_thr_init ran.  Valid in any
 * thread running w32 code: the main TEB is installed by process init and
 * every worker's by its trampoline before user code.  Reads %gs:0x30; the
 * ready flag (not GS itself) decides NULL, because probing an
 * uninstalled GS would fault instead of answering. */
struct w32_teb *w32_teb_self(void);

/* Process init: install the main thread's TEB + ARCH_SET_GS, cache the
 * pid, zero the slot allocators.  Idempotent (the host suite re-inits).
 * Runs before any other w32 call, from w32_kernel32_init. */
void w32_thr_init(void);
void w32_thr_checkpoint(void);

/* 1 once w32_thr_init has run (the w32_teb_self NULL gate). */
int w32_thr_ready(void);

#endif /* AURALITE_W32_TEB_H */

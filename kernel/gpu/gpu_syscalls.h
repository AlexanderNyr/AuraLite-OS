/* kernel/gpu/gpu_syscalls.h — user-space access to the virtio-gpu 3D path.
 *
 * Phase K1 of GL_PLAN.md.
 *
 * WHY THIS EXISTS
 *
 * drivers/gpu/virtio_gpu.c already implements 3D contexts, resources, fenced
 * SUBMIT_3D and scanout present — but entirely kernel-side.  libgl runs in user
 * space by design (GL_PLAN.md decision D3), so the GL stack has no way to reach
 * any of it.  This syscall is that bridge, and it is the prerequisite for the
 * VirGL backend stub in libgl/src/glvirgl.c to become real.
 *
 * THE SECURITY MODEL IS THE POINT
 *
 * A VirGL command stream is a program the host GPU executes.  Forwarding an
 * unvalidated one from user space is the most dangerous thing this OS could
 * do, so the rules are stated here rather than left implicit:
 *
 *   1. NO RAW USER POINTERS REACH THE DRIVER.  Every buffer is validated with
 *      validate_user_range() and copied into kernel memory first, exactly as
 *      the GUI_OP_BLIT path does.
 *
 *   2. RESOURCE IDS ARE PER-PROCESS.  A process names its resources with small
 *      integers that mean nothing outside its own table; the kernel translates
 *      them to real device ids on every call.  A process therefore cannot name,
 *      read or destroy another process's resources even by guessing.
 *
 *   3. COMMAND STREAMS ARE WALKED BEFORE FORWARDING.  Every VirGL packet header
 *      declares its own length; the validator checks each one fits inside the
 *      remaining buffer before the stream is handed to the device.  A stream
 *      whose header lies is rejected whole.
 *
 *   4. QUOTAS ARE ENFORCED.  A process gets a bounded number of resources and a
 *      bounded number of bytes, so it cannot exhaust GPU memory.
 *
 *   5. EVERYTHING IS REAPED ON EXIT, alongside the existing GUI window cleanup
 *      in thread_exit().
 */
#ifndef AURALITE_KERNEL_GPU_SYSCALLS_H
#define AURALITE_KERNEL_GPU_SYSCALLS_H

#include <stdint.h>

/* Syscall number, continuing the GUI block (200..202). */
#define SYS_GPU_CALL 203

/* Sub-ops for SYS_GPU_CALL. */
enum {
    GPU_OP_INFO = 1,        /* a2 = user gpu_info_t*                        */
    GPU_OP_CTX_CREATE,      /* returns a context handle, or negative errno  */
    GPU_OP_CTX_DESTROY,     /* a2 = handle                                  */
    GPU_OP_RES_CREATE,      /* a2 = user gpu_res_create_t*; returns handle  */
    GPU_OP_RES_DESTROY,     /* a2 = resource handle                         */
    GPU_OP_TRANSFER,        /* a2 = user gpu_transfer_t*                    */
    GPU_OP_SUBMIT,          /* a2 = user gpu_submit_t*                      */
    GPU_OP_SET_SCANOUT,     /* a2 = user gpu_scanout_t*                     */
    GPU_OP_FLUSH,           /* a2 = user gpu_flush_t*                       */
};

/* Per-process limits.  Deliberately modest: these exist to bound damage from a
 * buggy or hostile process, not to be generous. */
#define GPU_MAX_CTX_PER_PROC   4
#define GPU_MAX_RES_PER_PROC   64
#define GPU_MAX_BYTES_PER_PROC (64u * 1024u * 1024u)

/* Largest command stream accepted in one submit.  A stream longer than this is
 * almost certainly a mistake, and the cap keeps the kernel-side bounce buffer
 * a fixed, predictable size. */
#define GPU_MAX_CMD_BYTES      (256u * 1024u)

/* Largest single transfer, same reasoning. */
#define GPU_MAX_XFER_BYTES     (16u * 1024u * 1024u)

/* ---- Argument blocks (part of the user/kernel ABI) ----
 *
 * SYS_GPU_CALL carries only five 64-bit arguments, which is not enough for the
 * wider operations, so those take a pointer to a struct.  Explicit padding
 * keeps the layout identical regardless of compiler alignment choices.
 */

typedef struct {
    uint32_t present;        /* non-zero when a virtio-gpu was found        */
    uint32_t virgl;          /* non-zero when the device supports VirGL     */
    uint32_t width, height;  /* scanout size                                */
    uint32_t max_resources;  /* per-process resource quota                  */
    uint32_t max_bytes;      /* per-process byte quota                      */
} gpu_info_t;

typedef struct {
    uint32_t ctx;            /* context handle                              */
    uint32_t target;         /* VirGL target (2D, 3D, ...)                  */
    uint32_t format;         /* VirGL pixel format                          */
    uint32_t bind;           /* VirGL bind flags                            */
    uint32_t width, height, depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t _pad;
} gpu_res_create_t;

typedef struct {
    uint32_t ctx;
    uint32_t res;            /* resource handle                             */
    uint32_t x, y, z;
    uint32_t w, h, d;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
    uint32_t size;           /* bytes at `data`                             */
    uint32_t _pad;
    uint64_t data;           /* user pointer to the pixel/vertex payload    */
} gpu_transfer_t;

typedef struct {
    uint32_t ctx;
    uint32_t size;           /* bytes at `cmd`                              */
    uint32_t fenced;         /* non-zero to wait for completion             */
    uint32_t _pad;
    uint64_t cmd;            /* user pointer to the VirGL command stream    */
    uint64_t fence_out;      /* user uint64_t* for the fence id, or 0       */
} gpu_submit_t;

typedef struct {
    uint32_t scanout;
    uint32_t res;
    uint32_t x, y, w, h;
} gpu_scanout_t;

typedef struct {
    uint32_t res;
    uint32_t x, y, w, h;
    uint32_t _pad;
} gpu_flush_t;

/* Dispatch entry point, called from the syscall table. */
uint64_t syscall_gpu_call(uint64_t op, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5);

/* Release every GPU resource owned by a process.  Called from thread_exit(),
 * next to gui_cleanup_process(). */
void gpu_cleanup_process(uint64_t owner_pid);

/* Validate a VirGL command stream that is already in kernel memory.
 * Returns 0 when every packet header fits inside `size` bytes, non-zero
 * otherwise.  Exposed for host unit testing — this is the function that stands
 * between a hostile process and the host GPU, so it is tested directly rather
 * than only through the syscall. */
int gpu_validate_cmd_stream(const uint32_t *cmd, uint32_t dwords);

#endif /* AURALITE_KERNEL_GPU_SYSCALLS_H */

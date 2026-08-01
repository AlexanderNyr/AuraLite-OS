/* kernel/gpu/gpu_syscalls.c — user-space access to the virtio-gpu 3D path.
 *
 * Phase K1 of GL_PLAN.md.  See gpu_syscalls.h for the security model; this
 * file implements it.
 *
 * The shape follows kernel/gui/gui_syscalls.c deliberately: one dispatch
 * switch, argument blocks copied in with copy_from_user(), and nothing from
 * user space dereferenced directly.  Doing something novel here would be a
 * mistake — the GUI path is the pattern this kernel has already reviewed.
 */

#include <stdint.h>

#include "kernel/gpu/gpu_syscalls.h"
#include "kernel/proc/usercopy.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/errno.h"
#include "drivers/gpu/virtio_gpu.h"

/* ============================================================================
 * Per-process resource tracking
 *
 * A process names its resources with SMALL HANDLES that are meaningless
 * outside its own table.  Every entry point translates a handle to the real
 * device id, so a process cannot reach another's resources even by guessing —
 * the id it would have to guess does not exist in its namespace at all.
 * ==========================================================================*/

typedef struct {
    int      used;
    uint64_t owner;          /* owning pid                                   */
    uint32_t dev_id;         /* real id handed to the driver                 */
    uint32_t ctx_handle;     /* owning context handle, 0 if none             */
    uint32_t bytes;          /* charged against the process quota            */
} gpu_res_slot_t;

typedef struct {
    int      used;
    uint64_t owner;
    uint32_t dev_id;
} gpu_ctx_slot_t;

/* Global tables rather than per-TCB allocations: the counts are small and
 * fixed, and this keeps process teardown a simple scan with no ownership
 * question about who frees what. */
#define GPU_CTX_SLOTS (GPU_MAX_CTX_PER_PROC * 8)
#define GPU_RES_SLOTS (GPU_MAX_RES_PER_PROC * 4)

static gpu_ctx_slot_t gpu_ctxs[GPU_CTX_SLOTS];
static gpu_res_slot_t gpu_reses[GPU_RES_SLOTS];

/* Device ids are allocated monotonically and never reused within a boot, so a
 * stale id from a dead process can never collide with a live one. */
static uint32_t next_dev_ctx_id = 1;
static uint32_t next_dev_res_id = 1;

static uint64_t current_pid(void) {
    tcb_t *cur = sched_current();
    return cur ? cur->id : 0;
}

/* Handles are 1-based slot indices: 0 is always invalid, which makes a
 * zero-initialised or forgotten field fail closed. */
static gpu_ctx_slot_t *ctx_from_handle(uint32_t handle, uint64_t pid) {
    if (handle == 0 || handle > GPU_CTX_SLOTS) return (gpu_ctx_slot_t *)0;
    gpu_ctx_slot_t *c = &gpu_ctxs[handle - 1];
    if (!c->used || c->owner != pid) return (gpu_ctx_slot_t *)0;
    return c;
}

static gpu_res_slot_t *res_from_handle(uint32_t handle, uint64_t pid) {
    if (handle == 0 || handle > GPU_RES_SLOTS) return (gpu_res_slot_t *)0;
    gpu_res_slot_t *r = &gpu_reses[handle - 1];
    if (!r->used || r->owner != pid) return (gpu_res_slot_t *)0;
    return r;
}

static uint32_t proc_res_count(uint64_t pid) {
    uint32_t n = 0;
    for (int i = 0; i < GPU_RES_SLOTS; i++) {
        if (gpu_reses[i].used && gpu_reses[i].owner == pid) n++;
    }
    return n;
}

static uint32_t proc_res_bytes(uint64_t pid) {
    uint32_t total = 0;
    for (int i = 0; i < GPU_RES_SLOTS; i++) {
        if (gpu_reses[i].used && gpu_reses[i].owner == pid) {
            total += gpu_reses[i].bytes;
        }
    }
    return total;
}

static uint32_t proc_ctx_count(uint64_t pid) {
    uint32_t n = 0;
    for (int i = 0; i < GPU_CTX_SLOTS; i++) {
        if (gpu_ctxs[i].used && gpu_ctxs[i].owner == pid) n++;
    }
    return n;
}

/* The command-stream validator lives in gpu_cmdcheck.c so that host unit tests
 * can link it without the rest of the kernel; see that file for why. */

/* ============================================================================
 * Dispatch helpers
 * ==========================================================================*/

/* Copy a user argument block into kernel memory.  Returns 0 on success. */
static int fetch_args(void *dst, uint64_t user_ptr, uint64_t size) {
    if (!user_ptr) return -1;
    return copy_from_user(dst, (const void *)(uintptr_t)user_ptr, size);
}

/* ============================================================================
 * Operations
 * ==========================================================================*/

static uint64_t op_info(uint64_t a2) {
    gpu_info_t info;
    memset(&info, 0, sizeof info);

    info.present       = virtio_gpu_available() ? 1u : 0u;
    info.virgl         = virtio_gpu_virgl_supported() ? 1u : 0u;
    info.max_resources = GPU_MAX_RES_PER_PROC;
    info.max_bytes     = GPU_MAX_BYTES_PER_PROC;

    const virtio_gpu_info_t *gi = virtio_gpu_get_info();
    if (gi) {
        info.width  = gi->width;
        info.height = gi->height;
    }

    if (!a2) return (uint64_t)-EFAULT;
    if (copy_to_user((void *)(uintptr_t)a2, &info, sizeof info) != 0) {
        return (uint64_t)-EFAULT;
    }
    return 0;
}

static uint64_t op_ctx_create(void) {
    uint64_t pid = current_pid();
    if (!virtio_gpu_virgl_supported()) return (uint64_t)-ENODEV;
    if (proc_ctx_count(pid) >= GPU_MAX_CTX_PER_PROC) return (uint64_t)-EMFILE;

    for (int i = 0; i < GPU_CTX_SLOTS; i++) {
        if (gpu_ctxs[i].used) continue;
        uint32_t dev = next_dev_ctx_id++;
        if (virtio_gpu_ctx_create(dev, "userspace") != 0) {
            return (uint64_t)-EIO;
        }
        gpu_ctxs[i].used   = 1;
        gpu_ctxs[i].owner  = pid;
        gpu_ctxs[i].dev_id = dev;
        return (uint64_t)(i + 1);            /* 1-based handle */
    }
    return (uint64_t)-EMFILE;
}

static uint64_t op_ctx_destroy(uint64_t a2) {
    uint64_t pid = current_pid();
    gpu_ctx_slot_t *c = ctx_from_handle((uint32_t)a2, pid);
    if (!c) return (uint64_t)-EINVAL;

    /* Destroying a context orphans its resources, so drop them first rather
     * than leaving entries that translate to a dead device context. */
    for (int i = 0; i < GPU_RES_SLOTS; i++) {
        if (gpu_reses[i].used && gpu_reses[i].owner == pid &&
            gpu_reses[i].ctx_handle == (uint32_t)a2) {
            gpu_reses[i].used = 0;
        }
    }

    virtio_gpu_ctx_destroy(c->dev_id);
    c->used = 0;
    return 0;
}

static uint64_t op_res_create(uint64_t a2) {
    uint64_t pid = current_pid();
    gpu_res_create_t rc;
    if (fetch_args(&rc, a2, sizeof rc) != 0) return (uint64_t)-EFAULT;

    gpu_ctx_slot_t *c = ctx_from_handle(rc.ctx, pid);
    if (!c) return (uint64_t)-EINVAL;

    /* Bound the dimensions before multiplying, so the byte estimate below
     * cannot overflow. */
    if (rc.width  > 16384u || rc.height > 16384u || rc.depth > 4096u) {
        return (uint64_t)-EINVAL;
    }
    if (rc.width == 0) rc.width = 1;
    if (rc.height == 0) rc.height = 1;
    if (rc.depth == 0) rc.depth = 1;

    /* Charge four bytes per texel.  This is an estimate, not the device's real
     * allocation, but a quota only has to be a consistent upper bound. */
    uint64_t bytes = (uint64_t)rc.width * rc.height * rc.depth * 4ull;
    if (bytes > GPU_MAX_BYTES_PER_PROC) return (uint64_t)-ENOMEM;
    if (proc_res_bytes(pid) + (uint32_t)bytes > GPU_MAX_BYTES_PER_PROC) {
        return (uint64_t)-ENOMEM;
    }
    if (proc_res_count(pid) >= GPU_MAX_RES_PER_PROC) return (uint64_t)-EMFILE;

    for (int i = 0; i < GPU_RES_SLOTS; i++) {
        if (gpu_reses[i].used) continue;
        uint32_t dev = next_dev_res_id++;
        if (virtio_gpu_resource_create_3d(dev, c->dev_id, rc.target, rc.format,
                                          rc.bind, rc.width, rc.height,
                                          rc.depth, rc.array_size,
                                          rc.last_level, rc.nr_samples,
                                          rc.flags) != 0) {
            return (uint64_t)-EIO;
        }
        virtio_gpu_ctx_attach_resource(c->dev_id, dev);

        /* Give it guest-side backing so a later GPU_OP_TRANSFER has somewhere
         * to write.  A resource without backing exists only on the host and
         * silently swallows every upload -- the K1 defect this closes.
         *
         * Failing to attach is not fatal: a render target the application
         * never uploads into does not need backing, and refusing to create it
         * would break that case for no benefit.  A transfer to an unbacked
         * resource then fails loudly with -EIO. */
        virtio_gpu_resource_attach_memory(dev, (uint32_t)bytes);

        gpu_reses[i].used       = 1;
        gpu_reses[i].owner      = pid;
        gpu_reses[i].dev_id     = dev;
        gpu_reses[i].ctx_handle = rc.ctx;
        gpu_reses[i].bytes      = (uint32_t)bytes;
        return (uint64_t)(i + 1);
    }
    return (uint64_t)-EMFILE;
}

static uint64_t op_res_destroy(uint64_t a2) {
    uint64_t pid = current_pid();
    gpu_res_slot_t *r = res_from_handle((uint32_t)a2, pid);
    if (!r) return (uint64_t)-EINVAL;

    gpu_ctx_slot_t *c = ctx_from_handle(r->ctx_handle, pid);
    if (c) virtio_gpu_ctx_detach_resource(c->dev_id, r->dev_id);
    virtio_gpu_resource_release_memory(r->dev_id);
    r->used = 0;
    return 0;
}

static uint64_t op_transfer(uint64_t a2) {
    uint64_t pid = current_pid();
    gpu_transfer_t tr;
    if (fetch_args(&tr, a2, sizeof tr) != 0) return (uint64_t)-EFAULT;

    gpu_ctx_slot_t *c = ctx_from_handle(tr.ctx, pid);
    gpu_res_slot_t *r = res_from_handle(tr.res, pid);
    if (!c || !r) return (uint64_t)-EINVAL;

    if (tr.size == 0 || tr.size > GPU_MAX_XFER_BYTES) return (uint64_t)-EINVAL;
    if (!tr.data) return (uint64_t)-EFAULT;

    /* Validate the whole payload before copying any of it, so a partially
     * unmapped buffer cannot leave a half-written resource behind. */
    if (!validate_user_range((const void *)(uintptr_t)tr.data, tr.size, 0)) {
        return (uint64_t)-EFAULT;
    }

    void *bounce = kmalloc(tr.size);
    if (!bounce) return (uint64_t)-ENOMEM;
    if (copy_from_user(bounce, (const void *)(uintptr_t)tr.data, tr.size) != 0) {
        kfree(bounce);
        return (uint64_t)-EFAULT;
    }

    /* Copy into the resource's backing store and pull it across in one step.
     *
     * K1 shipped with this line calling virtio_gpu_transfer_to_host_3d()
     * directly and then freeing the bounce buffer UNUSED: the driver entry
     * point takes an offset into a resource it already owns, not a pointer to
     * fresh data, so the payload never reached the GPU and nothing said so.
     * The resource had no guest-side backing to reach in the first place --
     * see virtio_gpu_resource_attach_memory(). */
    int rc = virtio_gpu_resource_upload(c->dev_id, r->dev_id,
                                        bounce, (uint32_t)tr.size,
                                        tr.x, tr.y, tr.z,
                                        tr.w, tr.h, tr.d,
                                        tr.level, tr.stride, tr.layer_stride);
    kfree(bounce);
    return (rc == 0) ? 0 : (uint64_t)-EIO;
}

static uint64_t op_submit(uint64_t a2) {
    uint64_t pid = current_pid();
    gpu_submit_t sb;
    if (fetch_args(&sb, a2, sizeof sb) != 0) return (uint64_t)-EFAULT;

    gpu_ctx_slot_t *c = ctx_from_handle(sb.ctx, pid);
    if (!c) return (uint64_t)-EINVAL;

    if (sb.size == 0 || sb.size > GPU_MAX_CMD_BYTES) return (uint64_t)-EINVAL;
    if (sb.size & 3u) return (uint64_t)-EINVAL;   /* streams are dword arrays */
    if (!sb.cmd) return (uint64_t)-EFAULT;

    if (!validate_user_range((const void *)(uintptr_t)sb.cmd, sb.size, 0)) {
        return (uint64_t)-EFAULT;
    }

    uint32_t *stream = (uint32_t *)kmalloc(sb.size);
    if (!stream) return (uint64_t)-ENOMEM;
    if (copy_from_user(stream, (const void *)(uintptr_t)sb.cmd, sb.size) != 0) {
        kfree(stream);
        return (uint64_t)-EFAULT;
    }

    /* Validate the COPY, never the user buffer: checking user memory and then
     * forwarding it would be a time-of-check/time-of-use hole, since the
     * process could rewrite it in between. */
    if (gpu_validate_cmd_stream(stream, sb.size / 4u) != 0) {
        kfree(stream);
        return (uint64_t)-EINVAL;
    }

    int rc;
    uint64_t fence = 0;
    if (sb.fenced) {
        rc = virtio_gpu_submit_3d_fenced(c->dev_id, stream, sb.size, &fence);
    } else {
        rc = virtio_gpu_submit_3d(c->dev_id, stream, sb.size);
    }
    kfree(stream);
    if (rc != 0) return (uint64_t)-EIO;

    if (sb.fence_out) {
        if (copy_to_user((void *)(uintptr_t)sb.fence_out, &fence,
                         sizeof fence) != 0) {
            return (uint64_t)-EFAULT;
        }
    }
    return 0;
}

static uint64_t op_set_scanout(uint64_t a2) {
    uint64_t pid = current_pid();
    gpu_scanout_t sc;
    if (fetch_args(&sc, a2, sizeof sc) != 0) return (uint64_t)-EFAULT;

    gpu_res_slot_t *r = res_from_handle(sc.res, pid);
    if (!r) return (uint64_t)-EINVAL;

    /* Binding a resource to a scanout puts it on the physical display, which
     * is a shared resource.  Restrict it the way GUI_OP_RENDER is restricted:
     * only the early system processes may take over the screen. */
    if (pid > 2) return (uint64_t)-EPERM;

    int rc = virtio_gpu_set_scanout_resource(sc.scanout, r->dev_id,
                                             sc.x, sc.y, sc.w, sc.h);
    return (rc == 0) ? 0 : (uint64_t)-EIO;
}

static uint64_t op_flush(uint64_t a2) {
    uint64_t pid = current_pid();
    gpu_flush_t fl;
    if (fetch_args(&fl, a2, sizeof fl) != 0) return (uint64_t)-EFAULT;

    gpu_res_slot_t *r = res_from_handle(fl.res, pid);
    if (!r) return (uint64_t)-EINVAL;

    int rc = virtio_gpu_flush_resource(r->dev_id, fl.x, fl.y, fl.w, fl.h);
    return (rc == 0) ? 0 : (uint64_t)-EIO;
}

/* ============================================================================
 * Dispatch
 * ==========================================================================*/

uint64_t syscall_gpu_call(uint64_t op, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    /* Every operation except INFO needs a working device.  Answering INFO on a
     * machine with no GPU is the point: that is how a caller discovers there
     * is nothing to talk to. */
    if (op != GPU_OP_INFO && !virtio_gpu_available()) {
        return (uint64_t)-ENODEV;
    }

    switch (op) {
    case GPU_OP_INFO:        return op_info(a2);
    case GPU_OP_CTX_CREATE:  return op_ctx_create();
    case GPU_OP_CTX_DESTROY: return op_ctx_destroy(a2);
    case GPU_OP_RES_CREATE:  return op_res_create(a2);
    case GPU_OP_RES_DESTROY: return op_res_destroy(a2);
    case GPU_OP_TRANSFER:    return op_transfer(a2);
    case GPU_OP_SUBMIT:      return op_submit(a2);
    case GPU_OP_SET_SCANOUT: return op_set_scanout(a2);
    case GPU_OP_FLUSH:       return op_flush(a2);
    default:                 return (uint64_t)-EINVAL;
    }
}

/* ============================================================================
 * Process teardown
 * ==========================================================================*/

void gpu_cleanup_process(uint64_t owner_pid) {
    if (owner_pid == 0) return;

    int freed_res = 0, freed_ctx = 0;

    for (int i = 0; i < GPU_RES_SLOTS; i++) {
        if (!gpu_reses[i].used || gpu_reses[i].owner != owner_pid) continue;
        gpu_ctx_slot_t *c = ctx_from_handle(gpu_reses[i].ctx_handle, owner_pid);
        if (c) virtio_gpu_ctx_detach_resource(c->dev_id, gpu_reses[i].dev_id);
        /* Release the backing frames too, or a process that exits without
         * destroying its resources leaks physical memory permanently -- there
         * is nothing else that will ever free them. */
        virtio_gpu_resource_release_memory(gpu_reses[i].dev_id);
        gpu_reses[i].used = 0;
        freed_res++;
    }
    for (int i = 0; i < GPU_CTX_SLOTS; i++) {
        if (!gpu_ctxs[i].used || gpu_ctxs[i].owner != owner_pid) continue;
        virtio_gpu_ctx_destroy(gpu_ctxs[i].dev_id);
        gpu_ctxs[i].used = 0;
        freed_ctx++;
    }

    if (freed_res || freed_ctx) {
        kprintf("[gpu] reaped pid %lu: %d resource(s), %d context(s)\n",
                (unsigned long)owner_pid, freed_res, freed_ctx);
    }
}

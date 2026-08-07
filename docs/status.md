# AuraLite OS Status Matrix

This document describes the current state of the repository. It is more current
than the historical 14-phase roadmap in `PLAN.md`.

Legend:

- ✅ **Implemented / exercised** — built by default and has a boot-time or
  host-side test path.
- 🧪 **Experimental** — code exists and may work in constrained scenarios, but
  semantics or coverage are incomplete.
- 🚧 **WIP / partial** — scaffolding or protocol code exists; full data path is
  not complete.
- ❌ **Not implemented** — no working support yet.

## Boot and CPU

| Feature | Status | Notes |
|---|---:|---|
| Limine ISO boot | ✅ | BIOS path is best-tested; UEFI files are included in the ISO. |
| Higher-half kernel | ✅ | Linked at `0xFFFFFFFF80100000`. |
| GDT / IDT / PIC | ✅ | 256 IDT gates, PIC IRQ remap. |
| TSS | 🚧 | RSP0 works and is per-CPU. **IST is allocated but unused**: `tss_init()` fills `ist1` for every CPU, but `idt_set_gate()` hardcodes `ist = 0`, so no vector selects it — a fault taken on a bad stack (kernel stack overflow, #DF) triple-faults instead. See `TODO.md`. |
| SYSCALL/SYSRET | ✅ | Linux-like register ABI, custom syscall table. |
| SMP bring-up | 🧪 | APs online + scheduler lock; BSP-only scheduling remains the normal execution mode while APs enter `sched_idle()`. |
| LAPIC / IOAPIC | 🧪 | LAPIC enable + timer on each CPU are implemented; IOAPIC routing is still future work and legacy PIC/PIT paths remain available. |

## Memory management

| Feature | Status | Notes |
|---|---:|---|
| Physical memory manager | ✅ | Bitmap over 4 KiB frames from Limine memmap. |
| Virtual memory manager | ✅ | Extends Limine page tables through HHDM. |
| Kernel heap | ✅ | First-fit allocator with coalescing. |
| Per-process PML4 | 🧪 | Implemented for spawned user processes. |
| Copy-on-write | ✅ | `fork` via `paging_clone_user_space()` performs mark-and-share COW: page-table pages are copied, writable user frames are shared (write-protected in both parent and child) via `PAGE_FLAG_COW`; first write triggers `paging_handle_cow_fault()` to copy. PMM refcount (reference counting for shared frames) is implemented in `pmm.c`. |
| User pointer validation | 🧪 | `validate_user_range`, `copy_from_user`, `copy_to_user` use a #PF fixup path so TOCTOU/unmap during copy returns an error instead of panicking. |
| User ELF segment permissions / NX | ✅ | User linker emits page-aligned RX/R/NX/RW/NX `PT_LOAD` segments; `elf_load()` maps `PF_W` only as writable and applies `PAGE_FLAG_NO_EXEC` to non-`PF_X` segments. User stacks are writable+NX with guard pages left unmapped. `/elfperm` + `test_elf_permissions.sh` cover write-to-text and execute-from-data faults. |
| Stack guard pages + overflow diagnosis | ✅ | Kernel-thread stacks are bracketed by unmapped guard pages on both sides of each slot; user stacks have an unmapped guard page below them. `kernel/proc/guard.c` classifies a `#PF` on a known guard region and the `#PF` handler reports `[GUARD] kernel/user stack overflow`: a kernel-stack guard hit is fatal (`kernel_halt()`), a user-stack guard hit becomes SIGSEGV. `/stackguard` + `test_stack_guard.sh` and a host unit test cover the user overflow path and the classification boundaries. |
| Slab allocator | ✅ | `kernel/mm/slab.c` provides fixed-size caches; `tcb_cache`, `ofd_cache`, and `vnode_cache` are initialized during boot and used by thread/VFS allocation paths. |

## Scheduling and processes

| Feature | Status | Notes |
|---|---:|---|
| Kernel threads | ✅ | 16 KiB kernel stacks. |
| Preemptive round-robin | ✅ | PIT tick, quantum-based scheduling. |
| Blocking I/O / wait queues | ✅ | `kernel/proc/wait_queue.c` backs blocking pipes, futex waits, `select()`, and `nanosleep()` without yield-polling loops. |
| Ring 3 user mode | ✅ | ELF entry via `iretq`. |
| ELF loader | ✅ | Loads PT_LOAD segments at linked virtual addresses. |
| `spawn` | 🧪 | Used by shell `run <prog>` and integration-tested. |
| `fork` | 🧪 | COW fork via `paging_clone_user_space()` (O(page-tables) not O(address-space)); simplified PID semantics. |
| `execve` | 🧪 | Replaces current address space, simplified. |
| `wait4` / `wait` | 🧪 | Yield-polling, no precise child PID semantics. |
| SA_RESTART syscall restarting | 🧪 | Restartable blocking syscalls save restart metadata on `-EINTR` and are transparently re-dispatched from `sigreturn` when the handler was installed with `SA_RESTART`; signal frames preserve FPU/SSE state via `FXSAVE`/`FXRSTOR`. |
| Thread/process reaping | ✅ | Dead TCBs/stacks are deferred-reaped from a safe stack via `thread_reap_zombies()` called every PIT tick. User address spaces are fully freed by `paging_free_address_space()` (walk user PML4, free all data frames + page-table frames + PML4 itself). COW shared frames use PMM refcount — frame is only returned to the free pool when all sharers have released it. Boot log confirms: `[thread] reaped '/hello' (tid 6, 35 frames)`. |
| Per-process FD tables | 🧪 | Each TCB has its own FD table; `fork` shallow-copies entries. Lifetime/inheritance semantics remain simplified. |

## Filesystems

| Feature | Status | Notes |
|---|---:|---|
| VFS mount table | ✅ | Longest-prefix mount matching. |
| `readdir` / `mkdir` / `unlink` / `rename` / `stat` | ✅ | Generic VFS ops + matching syscalls (`100..105`); `stat()` exposes size/mode/link metadata plus seconds-resolution `mtime`/`ctime`/`atime` where supported. Baseline `mkfifo` named FIFOs, `symlink`/`readlink`, and `lstat`/`fstat` are wired (in-memory, `/tmp`-tested). |
| USTAR initrd | ✅ | Read-only root with user ELFs. |
| DevFS | ✅ | `/dev/null`, `/dev/zero`. |
| `tmpfs` | ✅ | Writable in-memory `/tmp`, supports `unlink`, `truncate`, and seconds-resolution `stat()` timestamps. |
| Tiny diskfs | ✅ | Persistent `/disk` (8 files × 4 KiB, AHCI port 0) with persistent stat timestamps in the on-disk table. |
| **FAT32 — full** | ✅ | `/fat`: subdirs, **LFN (UCS-2 read+write)**, mkdir/rmdir/unlink/rename/truncate, FSInfo, FAT date/time stamps decoded through `stat()`. |
| **ext2 — full** | ✅ | `/ext2`: mounts existing Linux-mkfs images **and** formats blank disks in-kernel. Direct + single/double/triple indirect blocks; mkdir/rmdir/unlink/rename; inode timestamps; cross-OS round-trip verified with `debugfs`. |
| buffer cache | 🧪 | Buffer cache layer for block I/O caching and synchronization. |
| exFAT | 🚧 | skeleton |
| NTFS | 🚧 | skeleton |
| ext4 | 🚧 | experimental ext4-like |
| Btrfs | 🚧 | experimental CoW prototype |
| F2FS | 🚧 | experimental log-structured prototype |

## Syscalls

| Area | Status | Notes |
|---|---:|---|
| Console/file I/O | ✅ | `read`, `write`, `open`, `close`. |
| Process basics | 🧪 | `getpid`, `exit`, `spawn`, `fork`, `execve`, `wait4`. |
| Directory/path ops | ✅/🧪 | `listdir`, `mkdir`, `rmdir`, `unlink`, `rename`, `truncate`, `stat`. |
| Networking | 🧪 | DNS, ping, legacy TCP calls and process-owned socket-style syscalls. |
| GUI | ✅/🧪 | `SYS_GUI_CALL` (200), `SYS_GUI_EVENT` (201), `SYS_GUI_THEME` (202). v2.0 theme engine, icons, notifications, snap, context menus. |
| Memory syscalls | 🧪 | `brk` implemented. `mmap`/`munmap` support eager private anonymous mappings and eager private file-backed reads. |
| Sockets | 🧪 | AF_INET/SOCK_STREAM handles include `socket`, `connect`, `send`, `recv`, `close`, plus server-side `bind`, `listen`, and `accept` (`305..307`). AF_INET/SOCK_DGRAM supports `sendto=44` and `recvfrom=45`. |
| Entropy (`getentropy`/`getrandom`) | ✅ | INTERNET_PLAN N0. ChaCha20 CSPRNG (`kernel/rng_core.h`, RFC 8439) seeded from RDSEED/RDRAND when present, else an interrupt-timing jitter pool stirred on every IRQ. Fails closed: `getentropy` returns `-ENOSYS` and `getrandom` blocks (or `EAGAIN` with `GRND_NONBLOCK`) until real entropy exists; estimated entropy logged at boot. Host RFC-vector + 1 MiB statistics tests, QEMU gate `test_rng.sh`. |
| Crypto primitives (`libatls`) | ✅ | INTERNET_PLAN N1/N2. Userspace static library `lib/libatls/`: SHA-256/512, HMAC-SHA256, HKDF, ChaCha20/Poly1305 AEAD, X25519, Ed25519 verify — all RFC-vector-verified by `tests/unit/test_atls_*` (155 checks) incl. the X25519 1000-iteration run and ten Wycheproof low-order triples. N2 adds zero-copy depth-bounded X.509 v3 parsing (`atls/x509.h`, `ATLS_DER_MAX_DEPTH` 32, iterative skipper): real-leaf field batteries, 10 000-deep refused, mutation corpus — the hostile half re-run in-guest by `/tests/x509test` on the 64 KiB stack. D7 enforced by a source grep: no `memcmp` on secrets, only `atls_ct_eq`. Shipped in the SDK (`AURALITE_LIBS_TLS`); gates `test_crypto.sh`, `test_x509.sh`. |

## Networking

| Feature | Status | Notes |
|---|---:|---|
| NIC abstraction (netdev) | ✅ | `kernel/net/netdev.{h,c}` selects an active NIC backend at boot; the IPv4/ARP/DHCP/UDP/TCP stack talks to it via `netdev_*` instead of a specific driver. e1000 is the default; virtio-net is the fallback when e1000 is absent. |
| PCI e1000 detection | ✅ | Supports common QEMU/VirtualBox/VMware 8254x IDs. |
| e1000 TX/RX | 🧪 | Legacy descriptor rings with INTx IRQ enable, IRQ cause handling, preallocated software RX queue, non-blocking compatibility receive, and blocking/timed receive helpers. TCP, ARP, DHCP, ICMP, and kernel UDP/DNS receive waits now use IRQ/wait-queue-backed bounded NIC waits; user UDP sockets (`sendto`/`recvfrom`) and basic fixed-RTO TCP retransmission are implemented; remaining N2 work is deeper socket blocking edge cases and production TCP features. |
| Ethernet / ARP | ✅ | Gateway routing support. |
| IPv4 / ICMP | ✅ | Ping self-test. |
| DHCP | ✅ | QEMU/VM NAT-oriented DORA flow. |
| UDP | ✅ | Used by DNS and exposed to userspace through AF_INET/SOCK_DGRAM `sendto`/`recvfrom`. |
| DNS resolver | ✅ | A-record lookup. |
| TCP client | 🧪 | Per-connection TCP state (up to 8 connections) with a one-segment retransmission slot and fixed RTO/retry handling for SYN, data ACK wait, and FIN close. Legacy `SYS_NET_*` are deprecated. |
| Socket API | 🧪 | AF_INET/SOCK_STREAM process-owned handles plus AF_INET/SOCK_DGRAM `sendto`/`recvfrom` exist. |
| TCP server (bind/listen/accept) | 🧪 | Server-side socket path is implemented with `tcp_listen()`/`tcp_accept()` and the `/tcpserver` minimal HTTP echo server. |
| virtio-net | 🧪 | Modern virtio PCI driver (`drivers/virtio_net/`) with a real data path: `VIRTIO_F_VERSION_1` negotiation, RX (queue 0) / TX (queue 1) split virtqueues, MAC read from device config, and a 12-byte `virtio_net_hdr` (no `MRG_RXBUF`). Registers as a netdev backend; selected when e1000 is absent. Validated end-to-end (DHCP + ICMP + DNS + TCP) under QEMU `-device virtio-net-pci`. Polling data path; no IRQ yet. |
| vmxnet3 / e1000e | 🚧 | Recognised by the virtual-driver probe, but no data path yet. Use e1000 or virtio-net. |

## Graphics and input

| Feature | Status | Notes |
|---|---:|---|
| Framebuffer console | ✅ | Limine-provided linear framebuffer. Double-buffered; note that `gfx_fill_rect()` omits the `back_fb` NULL guard its siblings have (`TODO.md`). |
| PSF/bitmap font rendering | ✅ | Embedded console font. |
| 2D graphics | ✅ | Double-buffered drawing. |
| Window manager demo | ✅ | Windows, widgets, taskbar, mouse interaction. |
| PS/2 keyboard | ✅ | Scan-code set 1, ASCII + rich key-event queues. **US layout only** — two fixed translation tables, no keymap selection, no dead keys. |
| PS/2 mouse | ✅ | IRQ 12, cursor/buttons and wheel-event support. |
| Kernel GUI/compositor | ✅ | **v2.0**: Theme engine (30+ params), desktop icons (32), notifications, window snapping (left/right/top/bottom/maximize), start menu, context menus, always-on-top windows, tool windows, edge/corner resize, double-click titlebar maximize, alpha blit, explicit event ABI (#define values), 64 windows, 128-event rings. Dirty-rect partial redraw is implemented via `compositor_render_dirty()` and `gfx_flip_rect()`, with idle frames skipping flips. Per-process window/icon cleanup on exit. |
| 3D software renderer | 🧪 | Demo renderer, CPU/SSE float math. |
| **OpenGL (libgl)** | 🚧 | User-space GL 1.1/1.3 stack with FBOs and an ES 2.0 shader path, software rasterizer — see `GL_PLAN.md`. **G0-G9 complete**: hardened `GUI_OP_BLIT` presentation path, libc float math, `glmath` layer, AuraGLX context, matrix stacks + immediate mode (all ten primitive modes), and a **filled edge-function rasterizer** with depth buffer (all 8 compare functions, `glDepthMask`), back-face culling, top-left fill rule, scissor test and `glPolygonMode`. 168 host unit tests + 38 QEMU assertions. `/glcube` renders a **solid depth-buffered cube**. Plus **frustum clipping** against all six planes (Sutherland–Hodgman for triangles, Liang–Barsky for lines, attributes interpolated at the cut) and `glPushAttrib`/`glPopAttrib`. 196 host unit tests + 46 QEMU assertions. The camera can fly through geometry without artefacts. Plus the full **GL 1.1 lighting model**: 8 lights (positional/directional/spot), Blinn–Phong specular, distance attenuation, front/back materials, `GL_COLOR_MATERIAL`, inverse-transpose normal matrix and `GL_NORMALIZE`. 228 host unit tests + 53 QEMU assertions. `/glcube` renders a lit, depth-buffered cube. Plus **texturing** (2D texture objects, all five base formats, nearest/bilinear filtering, repeat/clamp wrapping, `GL_MODULATE`/`REPLACE`/`DECAL`/`BLEND`, **perspective-correct** UV interpolation), **blending**, the **alpha test** and **fog** (linear/exp/exp2). 265 host unit tests + 63 QEMU assertions. `/glcube` renders a lit, textured, depth-buffered cube. Plus **vertex arrays** (all four attribute arrays, arbitrary stride, eight component types), **buffer objects** (GL 1.5 subset, both targets) and **display lists** (command-log based, so matrix and state commands replay correctly). 301 host unit tests + 71 QEMU assertions. `/glcube` compiles its cube into a display list and draws the grid from a vertex array. Note: arrays are an API-completeness feature, not a speed-up — the per-vertex transform dominates, so `glDrawArrays` measures the same as immediate mode. Plus the **GLU layer** (`gluPerspective`, `gluLookAt`, `gluOrtho2D`, `gluErrorString`, sphere/cylinder/disk quadrics) and two shipped demos: `/glcube` and `/glgears` — the latter ported from real OpenGL sources with the GL calls unchanged, which is the strongest available evidence the API behaves as applications expect. 322 host unit tests + 80 QEMU assertions. See [`docs/opengl.md`](opengl.md). Plus the **backend seam** (`gl_backend_t`, modelled on `netdev`): the software rasterizer is registered as an ordinary backend, any entry point may be NULL with software covering the rest, and `glGetString(GL_RENDERER)` reports the active one. A VirGL candidate is registered but declines until the kernel exposes a user-space 3D submission syscall. Plus **GL 1.2/1.3 texturing** (G10): mipmap chains with all four mipmap filters and `glGenerateMipmap`/`gluBuild2DMipmaps`, **two texture units** (`glActiveTexture`, `glClientActiveTexture`, `glMultiTexCoord2f`, per-unit environment, combined in fragment order), **3D textures** with trilinear sampling, **cube maps** with major-axis face selection, and `GL_CLAMP_TO_BORDER`. The mipmap level is chosen **per triangle** rather than per fragment — a scanline rasterizer has no `dFdx`/`dFdy`, so the level comes from the texture-space/screen-space area ratio; large receding surfaces must be tessellated, which `/glcube`'s new mipmapped floor demonstrates. Measured: `GL_NEAREST_MIPMAP_NEAREST` is *faster* than un-mipmapped `GL_LINEAR` (2.6 vs 3.2 ms/frame), trilinear costs 1.9×. This phase also uncovered and fixed a latent G1 bug — `aglxResize()` held a whole scratch context on the stack, which page-faulted once the context passed 130 KB. Plus **framebuffer objects and render-to-texture** (G12): `glGenFramebuffers`/`glBindFramebuffer`/`glFramebufferTexture2D`/`glFramebufferRenderbuffer`/`glCheckFramebufferStatus`, colour and depth renderbuffers, and `glReadPixels` in six formats reading whichever target is bound. The rasterizer needed **no changes at all** — an FBO re-points the four fields (`color`, `depth`, `width`, `height`) it has always written through, which is the return on having kept the render target abstract since G3. Rendering into an FBO measures the same as rendering into the window (3.72 vs 3.75 ms/frame); only the bind/unbind pair costs anything, and that scales with attachment area because unbinding forces the rendered texture opaque. Two real bugs surfaced and were fixed: `gl_fb_row()` flipped y unconditionally, correct for the window and upside-down for a texture; and a rendered texture sampled as fully transparent because the rasterizer writes no alpha byte. `/glcube` gained an inset render-to-texture panel showing a second view of the scene. Plus the **GLSL ES 1.0 front end** (G11a, the first of four shader sub-phases): a lexer, recursive-descent parser and type checker producing a typed AST, in 2400 lines. The full language — scalar/vector/matrix/sampler types, structs, arrays, functions with `in`/`out`/`inout`, the built-in library — with stage-aware rules (`attribute` refused in a fragment shader, `discard` in a vertex shader, a diagnostic when a vertex shader never writes `gl_Position`). Diagnostics are treated as the deliverable rather than an afterthought: every one carries a line number and names the rule, and most of the 167 host tests are negative cases asserting on the message text. One arena per compilation means a failed parse leaks nothing; every limit on untrusted input is a diagnostic rather than a fault. Not yet reachable from the GL API — `glCreateShader` arrives in G11c. Measured: 0.037 ms and 140 KB to compile a 22-line Blinn-Phong shader. Plus the **GLSL execution engine** (G11b): an AST-walking interpreter, 1500 lines, running the whole language — expressions, control flow, user functions with `in`/`out`/`inout`, structs, arrays, swizzled lvalues, matrix algebra and the built-in library. A shader reaches the outside world through three `glsl_env_t` callbacks, so the engine is testable with no GL context and G11c can attach the real pipeline without changing it. Semantics that differ from C are tested explicitly: `mod()` takes the sign of the divisor, integer division truncates towards zero, `matN(s)` is a diagonal, and undefined maths yields finite values rather than NaNs that would propagate through blending. Every limit on untrusted input (100k iterations, call depth 16, argument nesting 24) is a diagnostic rather than a fault. Three bugs were found only by running on the target, where the user stack is 64 KB against the host's 8 MB: two scratch arrays living on the C stack (5.9 KB and 1.2 KB per frame) and a shared argument buffer keyed on call depth rather than argument nesting, which made `max(dot(a,b), 0.0)` silently ignore its clamp. A kernel limit surfaced too: `spawn()` truncated executables over 256 KB without saying so. Measured 0.27 µs per trivial fragment shader invocation — 20 ms/frame at 320×240, against 0.07 ms for the entire fixed-function path, exactly the one-to-two orders of magnitude the plan predicted. The shader path buys API coverage, not frames per second. Plus the **shader pipeline** (G11c): shaders are reachable from the GL API and draw pixels — `glCreateShader` through `glUseProgram`, generic vertex attributes, uniforms including the matrix forms. The vertex shader replaces the transform, the fragment shader replaces texturing/lighting/fog, and varyings ride through the existing clipper and perspective-correct interpolator; clipping, culling, depth, scissor and blending are untouched because a shader changes neither the window coordinates nor the meaning of a colour. Linking builds the uniform/varying/attribute tables once so the interpreter's by-name lookups become index arithmetic, and a varying the fragment shader reads but the vertex shader never declares is a **link error** rather than a silent read of zeros. Assembling the pipeline exposed a 30× interpreter regression invisible in G11b's standalone benchmark: `glsl_run()` allocated and zeroed its 90 KB state on every invocation, which at one call per pixel cost 3.90 µs per fragment against 0.27 µs measured directly — caching it took a full-screen shaded frame from 306 ms to 12 ms. Measured at 320×240: fixed function 0.92 ms/frame, constant shader 12.1 ms, Lambert-lit 53.8 ms, vertex stage alone 1.2 µs/draw. Vertex shaders are affordable; full-screen fragment shaders are not. Plus **coexistence hardening** (G11d), the sub-phase the plan flagged as carrying most of the risk — correctly, since audit found four real defects. Shaded points and lines were **not shaded**: they wrote the vertex colour, which the shader path leaves at white, so a shaded `GL_LINE_LOOP` came out white; every G11c test drew triangles, so nothing exercised those rasterizers with a program bound. Immediate mode silently hybridised — the fixed-function matrices placed the geometry and the fragment shader coloured it, a combination no GL implementation produces and one that would render plausibly here and draw nothing on real hardware; now `GL_INVALID_OPERATION`, with the draw calls exempt. `glUseProgram` was accepted inside `glBegin`/`glEnd` and executed immediately inside a display list, the latter silently rebinding the current program as a side effect of compiling. Verified rather than assumed: scissor, culling, depth mask, blending and FBO rendering all apply to shaded fragments, while lighting, fog, the alpha test, the texture environment, `glShadeModel`, the fixed-function matrices and arrays all correctly fail to reach one. The audit was a probe program enumerating interactions rather than tests written from the specification — three of the four defects were combinations nobody would have thought to assert on. Plus the **VirGL hardware backend** (G13): `libgl/src/glvirgl.c` reaches a real virtio-gpu through `SYS_GPU_CALL` — probe, clear, and present via TRANSFER + SET_SCANOUT + RESOURCE_FLUSH. `DRAW_VBO` is deliberately absent: it needs shaders as TGSI, and G11's compiler produces an interpreted AST, so a TGSI back end is a compiler phase rather than a corner of this one. The phase also closed the K1 blocker — `op_transfer()` shipped copying its payload into a bounce buffer and freeing it **unused**, because a `RESOURCE_CREATE_3D` resource has no guest memory behind it; resources now get backing, released on destroy and on process teardown, the latter having been a permanent physical-memory leak. Three wire-format constants were wrong in the first draft and all were caught before reaching a device (two by a unit test running the kernel's own validator); the file now includes `drivers/gpu/virgl.h` rather than restating it, so there is one definition of the protocol in the tree. Attaching a real GPU also surfaced a **pre-existing driver hang** during virtio-gpu initialisation — bisected to before G11d, never exercised because no integration case attached a GPU until G13 added one; recorded in `TODO.md` with what has been ruled out. Honest framing: this buys a proved seam, not frames per second. **GL_PLAN.md G0–G13 complete** — the plan is finished: 967 host unit checks + 373 in-OS checks + 86 QEMU assertions. |
| GUI bulk pixel blit | ✅ | `GUI_OP_BLIT` / `GUI_OP_BLIT_ALPHA` + `ag_blit()`/`ag_blit_alpha()`. Source rect is fully validated with `validate_user_range()`, then copied row-by-row through a kernel bounce buffer, so no raw user pointer is ever dereferenced. Covered by `test_opengl.sh`. |
| Native VBox/VMware SVGA drivers | ❌ | Limine framebuffer is used instead. |
| virtio-gpu 2D scanout | 🧪 | Modern virtio-gpu PCI probe, control queue, RESOURCE_CREATE_2D, ATTACH_BACKING, SET_SCANOUT, TRANSFER_TO_HOST_2D and RESOURCE_FLUSH are implemented as an optional mirror path for `gfx_flip()`. The driver is initialised during graphics boot so mirroring is available before the GUI compositor starts. |
| GPU 3D submission syscall | 🧪 | `SYS_GPU_CALL` (203) exposes the kernel VirGL transport to user space: per-process resource handles, command-stream validation on a kernel-side copy, quotas (4 contexts / 64 resources / 64 MB), scanout restricted to PID ≤ 2, and reaping on process exit. 18 host unit checks against malformed streams. The final `transfer` hop still needs a driver entry point taking fresh data; see `GL_PLAN.md` phase K1. |
| virtio-gpu VirGL command transport | 🧪 | Feature negotiation, 3D context create/destroy, RESOURCE_CREATE_3D, context attach/detach, fenced SUBMIT_3D payload chains and TRANSFER_TO_HOST_3D are present. A tiny VirGL command-stream builder submits clear/framebuffer plus experimental vertex-buffer/triangle packet streams, and a full present pipeline now drives TRANSFER_TO_HOST_3D + SET_SCANOUT + RESOURCE_FLUSH to scan a 3D render target out to the display (with graceful software fallback when no virgl host is attached). Command-stream encoding is host-unit-tested (`test_virgl`). There is still no full OpenGL/Gallium state tracker. |

## Storage and USB

| Feature | Status | Notes |
|---|---:|---|
| POSIX.1-2024 compliance | 🧪 | Q1–Q16 all implemented (~410 functions covered, see `docs/posix2024_compliance.md`). Q12 added the runnable conformance harness (host gate in `make test-unit` — `tests/posix2024/` — plus the guest `conformtest` QEMU case); Q13 completed the AT-family; Q15 implemented `mq_notify`; Q16 added pselect/ppoll, getrandom and sig2str/str2sig (getrandom's backing was upgraded by INTERNET_PLAN N0 to a ChaCha20 CSPRNG seeded from RDSEED/RDRAND or interrupt jitter, failing closed with ENOSYS until real entropy exists); Q14 replaced the SysV IPC ENOSYS stubs with real kernel objects — semaphores (blocking semop, SEM_UNDO at exit), page-backed shared memory (destroy at last detach) and message queues (full mtype rules). Only the three named-semaphore functions remain argued 🔶 partials (need MAP_SHARED). `_POSIX_VERSION` = 202405L. |
| AHCI detection/init | ✅/🧪 | Controller/port setup works in QEMU AHCI. |
| AHCI sector read/write | ✅/🧪 | DMA READ/WRITE self-test passes on the QEMU AHCI test disk. |
| UHCI controller | ✅/🧪 | Controller + port + CONTROL/BULK TD/QH transfers used by MSC. |
| OHCI controller | 🧪 | Detection/init/root-port reset plus ED/TD control, bulk and interrupt scheduling works in QEMU for HID/MSC. |
| EHCI controller | 🧪 | Detection/init/root-port reset plus async qTD control/bulk scheduling works in QEMU for high-speed MSC. Periodic interrupt and split transactions are pending. |
| xHCI controller | 🧪 | QEMU xHCI slot/address/device-context setup, endpoint contexts, command/event rings, control/bulk/interrupt transfer rings are implemented for HID/MSC. |
| USB device enumeration | 🧪 | UHCI/OHCI/EHCI/xHCI devices are enumerated through standard requests/controller-specific address setup. |
| USB HID keyboard/mouse | 🧪 | Boot Protocol/generic keyboards and pointer devices work through UHCI, OHCI, xHCI and high-speed EHCI polling; generic HID parser handles keyboard reports and mouse/tablet pointer reports (QEMU `usb-kbd`/`usb-mouse`/`usb-tablet` tested). EHCI full/low-speed split transactions remain future work. |
| USB hubs | 🧪 | Standard hub descriptor/status, port power/reset and downstream child enumeration work in QEMU, including xHCI route-string addressing for devices behind a hub. |
| USB hotplug monitor | 🧪 | Polling monitor scans root ports and known hubs, marks removed records and dynamically attaches HID and MSC class drivers for newly enumerated devices. QEMU xHCI HID and MSC attach/detach are integration-tested. |
| USB Mass Storage | 🧪 | Bulk-Only/SCSI path works through UHCI, OHCI, high-speed EHCI and xHCI in QEMU, including runtime hotplug attach/read/detach. Active media is exposed through `/usb` via usbfs (`info`, `sector0.bin`, `disk.img`) and FAT32 superfloppy/partition root files are auto-detected read-only under `/usb/fat`. |

## Wireless and Bluetooth

| Feature | Status | Notes |
|---|---:|---|
| Bluetooth HCI protocol | 🚧 | HCI commands/events implemented; depends on USB transport. |
| Wi-Fi 802.11 MAC layer | 🚧 | Management frames/state machine; no chipset driver registered by default. |

## Userspace applications

| App | Status | Notes |
|---|---:|---|
| `/init` shell | ✅ | Interactive serial/keyboard shell. |
| `/hello` | ✅ | Smoke test app. |
| `/calc` | ✅ | Calculator. |
| `/sysinfo` | ✅ | Feature info display. |
| `/editor` | ✅ | Simple line editor. |
| `/clock` | ✅ | Uptime/countdown demo. |
| `/guess` | ✅ | Guessing game. |
| `/snake` | ✅ | Terminal snake. |
| `/http` | 🧪 | Uses DNS/TCP syscalls. |
| `/browser` | 🧪 | Text rendering of simple HTTP/HTML responses. |
| `/gcalc`, `/gedit`, `/gfiles`, `/gterm`, `/gsysmon`, `/gabout`, `/glaunch`, `/gusb` | 🧪 | GUI apps using `libauragui` v2.0; `/gusb` is the USB Manager for hotplug/storage status via `/usb`. `/gtheme` customizes window colors. |
| `/webview` | 🧪 | Web view (WEBVIEW_PLAN W0–W6): scaffold + blit budget + `/tmp/webview.frames`; W1 tokeniser (122 checks); W2 DOM (65; 10k-deep doc on the 64 KiB stack); W3 layout (79; 5 000 boxes in 1 064 µs); W4 painting (42; reference hash 0x4D394D5C host==guest; 144 µs scroll steps); W5 CSS (71; D4 subset); **W6 navigation** (`wv_url` + `wv_http` — 69 checks: HTTP/1.1, chunked==plain, growing 512 KB buffer; links in the display list, address bar + Back/Go, 8-entry history, https refusal page; QEMU gate `test_webview_net.sh` 10 asserts: fetch→link→back→https→chunked→100 KB). Canvas is W7. See `docs/webview.md`. |

## Known low-priority limitations

- **SMP scheduling is deliberately conservative.** APs are online, have CPU-local state and LAPIC timers, and enter the idle scheduler loop; normal user scheduling remains BSP-only until per-CPU run queues/TLB shootdown policy are completed.
- **TCP is intentionally minimal.** TCP receive uses timed waits over the IRQ-capable e1000 driver and has basic one-segment fixed-RTO retransmission for SYN/data/FIN, but the stack still supports a small fixed number of streams and lacks production features such as congestion control, sliding windows and richer packet queues.
- **Advanced filesystems are prototypes.** `ext4`, `btrfs`, `f2fs`, `ntfs` and `exfat` are scaffolding/experimental readers, not robust general-purpose filesystem implementations; timestamp support is currently best covered in VFS/tmpfs/diskfs/FAT32/ext2.
- **Hardware coverage is mostly virtualized.** Integration coverage is QEMU-first, with some VirtualBox/VMware-oriented device IDs; broad real-hardware validation is still pending.

## Highest-priority gaps

1. Harden address-space teardown for future SMP/TLB-shootdown support.
2. Audit remaining kernel-internal callers and expand fault-recovering uaccess tests.
3. Complete USB bulk/control transfer paths across OHCI/EHCI/xHCI.
4. Make scheduling SMP-aware or explicitly keep APs disabled in normal configs.
5. Grow `mmap` into lazy/shared VMAs and add broader file-backed sharing tests.
6. Tighten FD inheritance/lifetime semantics around `fork`, `execve` and process exit.
7. Expand shared/lazy VMAs and add guard pages around additional heap/stack regions.

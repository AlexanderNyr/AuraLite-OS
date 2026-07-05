# Phase BL1 — Boot Handoff Interface — Completed

Baseline commit: `6bad518` (AuraLite OS)
Result commit:   `b7816c7` — `boot: BL1: introduce boot_info_t handoff, remove Limine from kernel API`
Toolchain:       clang 19.1.7, ld.lld 19, NASM 2.16, gcc 14.2 (host tests)

---

## Definition of Done — all criteria met

| # | Criterion | Result |
|---|-----------|:------:|
| 1 | `grep -rn limine_get_ kernel/` returns zero results | **0 hits** |
| 2 | `grep -rn "limine/limine.h" kernel/` returns zero results | **0 hits** |
| 3 | `kernel.ld` has no `.limine_requests*` sections | **✓** |
| 4 | `make test-unit` green (including `test_boot_info`) | **36/36 pass** |
| 5 | `make kernel` compiles without new warnings | **clean** |
| 6 | New handoff test `test_boot_info` present and wired in | **✓** |

The three pre-existing `-Waddress-of-packed-member` warnings in `drivers/virtio_blk/virtio_blk.c`
are inherited from the baseline (commit 6bad518) and are unrelated to this phase.

---

## Files added

| Path | LOC | Purpose |
|------|----:|---------|
| `boot/shared/boot_info.h` | 111 | Shared bootloader/kernel handoff struct + BOOT_MEM_* / BOOT_MAGIC |
| `kernel/boot_info.h`      |  56 | Kernel-side accessor declarations |
| `kernel/boot_info.c`      | 106 | Accessor implementations + `boot_info_init()` latch |
| `tests/unit/test_boot_info.c` | 120 | Host-side coverage of every accessor + empty-struct fallback |
| `.gitignore`              |   3 | Ignore build artifacts (was missing upstream) |

## Files removed

| Path | Reason |
|------|--------|
| `kernel/limine_requests.h` | Replaced by `kernel/boot_info.h` |
| `kernel/limine_requests.c` | Replaced by `kernel/boot_info.c` |

## Files modified

Kernel (13):
`kernel.ld`, `kernel/arch/x86_64/boot.asm`, `kernel/arch/x86_64/lapic.c`,
`kernel/arch/x86_64/paging.c`, `kernel/arch/x86_64/smp.c`,
`kernel/arch/x86_64/syscall.c`, `kernel/fs/vfs.c`, `kernel/gui/gui.c`,
`kernel/kernel.c`, `kernel/lib/stack_protector.c`, `kernel/mm/page_cache.c`,
`kernel/mm/pmm.c`, `kernel/mm/vma.c`, `kernel/proc/elf.c`,
`kernel/proc/process.c`.

Drivers (12):
`drivers/ahci/ahci.c`, `drivers/bluetooth/bt.c`, `drivers/e1000/e1000.c`,
`drivers/framebuffer/fb.c`, `drivers/framebuffer/graphics.c`,
`drivers/gpu/virtio_gpu.c`, `drivers/usb/ehci.c`, `drivers/usb/msc.c`,
`drivers/usb/ohci.c`, `drivers/usb/uhci.c`, `drivers/usb/usb_core.c`,
`drivers/usb/xhci.c`, `drivers/virtio_blk/virtio_blk.c`,
`drivers/virtio_net/virtio_net.c`.

Build & tests (3):
`Makefile` (new `$(BUILD_DIR)/test_boot_info` target), `tests/unit/test_page_cache.c`,
`tests/unit/test_vma.c` (host stubs renamed).

---

## Deviations from the specification

The spec was mostly accurate but a few real facts about the baseline required
implementation to adapt.  All deviations are minor and self-contained.

1. **Extra migration sites the spec did not list.**  The plan's grep table
   only covered `kernel/`, but the following files also called
   `limine_get_hhdm_offset()` and had to be migrated:
   `drivers/ahci/ahci.c`, `drivers/bluetooth/bt.c`, `drivers/e1000/e1000.c`,
   `drivers/gpu/virtio_gpu.c`, `drivers/usb/{ehci,msc,ohci,uhci,usb_core,xhci}.c`,
   `drivers/virtio_blk/virtio_blk.c`, `drivers/virtio_net/virtio_net.c`,
   `kernel/proc/process.c`.  Additionally `kernel/gui/gui.c` carried a
   dead `extern struct limine_framebuffer *limine_get_framebuffer(void);`
   forward declaration that had to be removed.

2. **Memory-map API is now an array-of-structs**, not an array-of-pointers
   like Limine's original API.  `pmm.c` therefore uses `entries[i].base`
   instead of `entries[i]->base`.  This is a nicer API for our own loader
   because we own the memory layout; no indirection wasted.

3. **`boot_info_t` is NOT `__attribute__((packed))`.**  The spec draft
   marked it packed, but doing so makes any `&info->mmap` or `&info->cpus`
   return an "unaligned pointer" -- gcc `-Werror=address-of-packed-member`
   rightly rejects that.  All fields are already naturally aligned thanks
   to explicit `_pad` members, so the packed attribute would only strip
   alignment guarantees without changing the layout.  Documented as a
   comment on the struct.

4. **Framebuffer virtual-address translation moved into drivers.**  Limine
   pre-translated the framebuffer address to its HHDM view; our loader
   hands over a raw physical address (`boot_fb_t::phys_base`).
   `drivers/framebuffer/fb.c` and `graphics.c` now do
   `(uint32_t *)(uintptr_t)(boot_get_hhdm_offset() + fb->phys_base)`.

5. **`boot_cpu_t.goto_address` is a plain `uint64_t`** (function pointer
   value), not a `limine_goto_address` typedef.  `smp.c` casts
   `ap_entry` explicitly: `(uint64_t)(uintptr_t)ap_entry`.  `ap_entry`
   itself now takes `boot_cpu_t *` (identical struct layout for the
   fields APs need).

6. **iso-limine is intentionally broken until BL2-BL7 land.**  The
   spec explicitly permits this ("kernel is not yet bootable -- that
   is fine"); phase BL8 will remove the `iso-limine` target and the
   Limine bundle altogether.  The `-I limine` include flag is left in
   `CFLAGS` for now because no code depends on those headers anymore,
   but nothing else does either, so it is inert.

---

## Verification transcript

```console
$ grep -rn 'limine_get_'    kernel/  ; echo done
done
$ grep -rn 'limine/limine.h' kernel/  ; echo done
done
$ grep -rn 'limine_requests' kernel/  ; echo done
done
$ make kernel 2>&1 | tail -1
[link] build/kernel.elf
$ make test-unit 2>&1 | tail -3
[unit] running build/test_gdt_tss
[unit] running build/test_boot_info
test_boot_info: ALL PASS
```

---

## What is now unblocked

Phase BL2 (BIOS Stage 1 / MBR) can start.  The kernel is entirely loader-agnostic:
any code that fills a `boot_info_t` and jumps to `_start` with `RDI` pointing at
it will boot to shell (subject to actually mapping the higher half and setting
up the HHDM, which is BL3-BL4 territory).

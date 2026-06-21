# AuraLite OS TODO

All 14 phases are complete. This file tracks future enhancements and known
limitations. See [PLAN.md](PLAN.md) for the completed milestone history.

---

## Known Limitations

- **Single address space:** the user program shares the kernel's address space.
  Per-process page tables (`paging_new_address_space`) are implemented but
  unused. No `fork`/`execve`.
- **Global FD table:** file descriptors are a global pool, not per-process.
- **No thread reaping:** dead threads' TCBs + stacks are leaked (no GC).
- **APs idle:** application processors load the GDT/IDT and halt; they don't
  participate in the scheduler (no per-CPU run queues or work stealing).
- **Read-only initrd:** the VFS has no write-capable filesystem.
- **No errno:** syscalls return `-1` on error without a detailed errno code.
- **Polling I/O:** no interrupt-driven device I/O (e1000 and keyboard poll).
- **8×8 font:** no scalable font (PSF2) for higher-resolution text.

## Future Enhancements

### Memory management
- [ ] Per-process address spaces (switch CR3 on context switch)
- [ ] Copy-on-write `fork`
- [ ] Slab allocator for common fixed sizes (TCBs, page tables)
- [ ] Guard pages around the kernel heap for overflow detection
- [ ] Large-page (2 MiB) support for the HHDM and heap
- [ ] Remap `.rodata` read-only + NX (currently mapped RW by Limine)

### Scheduling
- [ ] Per-CPU run queues + work stealing (integrate APs into the scheduler)
- [ ] Thread reaping (free TCB + stack on exit)
- [ ] BLOCKED state + wait queues (for sleep / IO wait)
- [ ] `kthread_join(tid)`
- [ ] Migrate from round-robin to CFS

### File system
- [ ] Per-process FD tables (with a PCB)
- [ ] tmpfs (write-capable in-memory filesystem)
- [ ] ext2 read-only driver
- [ ] Directory vnodes + `readdir` syscall

### Networking
- [ ] UDP and TCP state machines
- [ ] BSD socket API (`socket`, `bind`, `listen`, `accept`, `connect`)
- [ ] Interrupt-driven e1000 RX (instead of polling)
- [ ] DHCP client for automatic IP configuration

### Userspace
- [ ] `fork` + `execve` for multi-process support
- [ ] User-space `mmap`/`brk` for heap growth
- [ ] Apply ELF segment `p_flags` (R/W/X) per segment
- [ ] Separate user programs as external binaries (not embedded)

### Graphics
- [ ] PSF2 8×16 font for sharper text
- [ ] Window manager (z-order, focus, drag)
- [ ] Mouse driver (PS/2)
- [ ] Widget toolkit (button, label, text input)

### Infrastructure
- [ ] LAPIC timer (per-CPU, calibrated against the PIT)
- [ ] HPET detection for higher-resolution timing
- [ ] GDB pretty-printers for kernel data structures
- [ ] GitHub Actions CI pipeline

# The residue ledger — the living copy (RESIDUE_PLAN.md §2; R0 rig)

Machine-checked by `tools/check_residue_claims.py`: row count, id
sequence, class totals, status arithmetic.  Statuses: `OPEN`,
`DONE@Rn`, `RE-AFFIRMED@Rn`, `HANDED-OFF@Rn`.  A W row flips to
DONE only in the commit whose exit gate runs green; N rows die
loudly or live loudly (plan D4); S rows leave with a measured
opener fact (D5).

Class key: W = work (QEMU-provable now) · M = metal-only ·
N = non-goal to re-affirm · S = sub-series hand-off.

| ID | Class | Status | Item (source) | Exit gate |
|----|-------|--------|---------------|-----------|
| RES-01 | W | DONE@R1 | UHCI TD waits iteration-bounded; 1 runner flake seen (PARITY §5) | uhci.c waits on PIT ticks; usb shard green ×1 local |
| RES-02 | W | DONE@T8 | `-cpu max` shell-banner oddity — R1 narrowed: shell STARTS, first SYS_WRITE never lands; ERMS and x2APIC exonerated by A/B boots. T8 ROOT-CAUSED AND FIXED: feature-bisecting `-cpu max` (67 candidates, QMP delta vs qemu64) pinned SMAP; the boot shell's hand-rolled initial user stack (user.c) wrote its argc frame from CPL0 with raw stores, fault-looped under CR4.SMAP (697k identical #PFs in a 30s boot, RIP=user_test_thread) and the shell never entered Ring 3 — the missing banner was the first absent receipt. Frame write now runs inside an explicit user-access window | smoke explains it or the line appears — receipts: x86_cpumax_smoke.sh 8/8 (banner, prompt, uname round-trip under -cpu max; FIFO-driven, banner-gated input) |
| RES-03 | W | DONE@R1 | pit.h ×2 + msc.h couplings in kernel/fs, pinned per-file (PARITY P1) | seam decision note written; pins updated or removed |
| RES-04 | W | DONE@R1 | no partition parse: raw mounts silently ignore GPT/MBR (PARITY §6) | probe prints named skip on partitioned media; smoke pins it |
| RES-05 | W | DONE@R1 | TSS IST1 filled but no gate uses it (status.md) | double-fault lane uses IST1 with a receipt, or ist fill removed |
| RES-06 | W | DONE@T8 | vfs.c off-x86 — R2 NARROWED: the mount/resolve core split into vfsmount.c (adopted on ALL FOUR widths); the fd/OFD/pipe remainder was the pipe wait's raw `sti`. T8 took the first receipt branch: `arch_irq_enable()` added to all four irqflags backends (sti / csrsi sstatus,2 / msr daifclr,#2), vfs.c migrated onto it, and the fd half now COMPILES at all four widths | the fd half compiles portable — receipts: test_width_sweep.sh lane 2b (vfs.c -c clean at x86_64/i386/rv64/a64; pre-fix the rv64 compile failed ON the sti); asm-file ratchet 29 → 27. The tcb fd tables stay x86-coupled BY ARCHITECTURE: the port kernels have no process layer to host them (re-affirmed, measured not waved through) |
| RES-07 | W | DONE@T3 | buffer_cache/tmpfs/devfs/cwd/symlink not adopted on ports (PARITY P2) — T3 NARROWED the close: buffer_cache/tmpfs/devfs join all three shared lists and mount in every bringup (cache init + /dev + /tmp + a per-tenant round-trip receipt; devfs split into a portable null/zero core + x86-only devfs_ext.c for tty0/audio); symlink/cwd stay x86-only under RES-06 precedent (the fd/OFD half, RES-06, remains their owner) | objects join the three shared lists; link green — receipts: `[fs32|rvfs|a64fs] RES-07: tmpfs /tmp round-trip OK`, fs/shell/parity/boot smokes green on i386/rv64/a64 |
| RES-08 | W | DONE@R2 | i386 shell fd layer initrd-only (PARITY P4/P7) | i386 shell cats /ext2 file through VFS; smoke pins it |
| RES-09 | W | DONE@R2 | no path-level VFS mounts on rv64/a64 (PARITY P2) | `[vfs] mounted /` printed on both tenants; smokes pin it |
| RES-10 | W | DONE@R3 | i386 TCP/sockets absent — net32 is miniproto only (I386 I8) | one TCP payload round-trips in an i386 smoke |
| RES-11 | S | HANDED-OFF@C3 | i386 compositor/GUI (I386 I8) | OPENER, measured 2026-11-04: the portable GUI surface is 4 669 lines (gui.c 3151 + wm.c 530 + fb.c 454 + psf.c 141 + graphics.c 393). The fb SEAM IS CLOSED — fb.c/psf.c/graphics.c are KERNEL32_SHARED rows and the i386 kernel draws 1024x768x32 (`[boot] framebuffer console online`). The console half is CLOSED at C3 (see RESIDUE3_PLAN §C3). What remains is the compositor LINK: gui.c/wm.c compile at 32 bits with 0 errors but need 8 x86_64 process/scheduler/mouse symbols (`sched_current`, `timer_get_ticks`, `timer_get_frequency`, `keyboard_get_event`, `keyboard_get_mods`, `mouse_get_event`, `mouse_get_position`, `process_spawn`). |
| RES-12 | S | DONE@C3 | i386 VBE graphics; text mode is the console (I386 I7) | CLOSED (RESIDUE3 C3): boot/bios/stage2/vbe.inc probes VBE (INT 10h AX=4F00/4F01), sets 1024x768x32 with LIN_FA (800x600x32 second, text mode last) via 4F02, and fills boot_info_t.fb through the generated offsets. Receipts: `[vbe] selecting 1024x768x32 (linear framebuffer)` on BOTH consoles, `[vbe] framebuffer live; boot_info.fb filled` on the UART, `fb: WC via PAT4 (768 pages` on the 64-bit BIOS lane (that lane used to print `fb: none present; WC remap skipped`), and a 1024x768 monitor screendump from the i386 lane. Gates: tests/integration/test_i386_vbe.sh (14/14, incl. the `-vga none` negative control) and test_i386_gui.sh. |
| RES-13 | W | DONE@R4 | a64 -smp 16 needs GICv3; v2 = 8 ifaces architectural (PARITY P6) | a64_smp_smoke -smp 16 lane: 15 online, IPI 15/15 |
| RES-14 | W | DONE@R5 | tenant SMP receipts-only — R5: init runs at U-mode/EL0 ON a secondary on BOTH tenants (strictly serialized one-job mailbox; per-hart stvec/VBAR, adopted final translation roots).  Preemptive multi-CPU runqueues on tenants remain the S-class successor, folded into RES-46's hand-off notes | a user thread RUNS on a secondary; receipt counted |
| RES-15 | W | DONE@R5 | x86 "BSP-only" was the THIRD stale doc row: per-CPU runqueues + least-loaded placement + stealing landed at SMP 3.2 — R5 added the once-printed receipt (`user thread pid=N on AP cpu=M`) and test_fpu_smp pins it | user thread observed scheduled on an AP; case pins it |
| RES-16 | W | DONE@T2 | device IRQ waking a hlt-ed AP unproven (MATURITY) | receipt line in an SMP case — CLOSED (RESIDUE2 T2): `[smpwake] PASS: 9 RTC(GSI8) device IRQs delivered to cpu1 (apic id 1); hlt looper woken 8 times` pinned by tests/integration/cases/test_irq_ap_wake.sh under -smp 4 |
| RES-17 | W | DONE@R6 | full libc floor on ports: no malloc/stdio/TLS (RISCV V8 / PARITY P8) | port program mallocs + stdio round-trip, three ports |
| RES-18 | W | DONE@T8 | PIE loading on ports (the wait on RES-17 ended at R6, which closed the libc floor). T8: the i386 tenant's elf32load accepts ET_DYN — static PIE seated at 0x10000000, .rel.dyn R_386_RELATIVE applied in-loader (the kernel as its own dynamic linker); /bin32/pie32 is built -fPIE + ld.lld -pie + pie32.ld (image base 0) | a PIE binary runs on one tenant; receipt — i386_pie_smoke.sh 6/6: loader line (base 0x10000000, 3 relocations), the program's own runtime address (only correct if relocation happened), pointer-table self-check, ET_EXEC no-regression |
| RES-19 | W | DONE@R6 | userspace dynamic allocation needs brk/mmap (TODO.md) | brk/mmap-lite syscalls exist; pins move 11→N |
| RES-20 | W | DONE@R7 | PCIe ECAM deferred on virt, both tenants (RISCV/ARM64 D7) — R7: shared pci_ecam.c walks bus 0 on both (a64's ECAM measured ABOVE 4 GiB; VA carve, not HHDM folklore), BAR placement ours (`-kernel` boots arrive all-zero) | `[pci] ECAM: N functions` on rv64+a64 |
| RES-21 | W | DONE@R7 | virtio-pci transport unused on tenants (TODO.md) — R7: shared virtio_pci.c, MODERN with VERSION_1 acked, same vrings/request/seam as mmio; vblk falls through when the windows are empty | vblk-over-PCI mounts ext2 in a lane |
| RES-22 | W | DONE@R8 | Rust row rv64: target exists, nothing built (RISCV close) — R8: same two sources, cfg'd ecall/rdtime, initrd `/binrv/rustes`; scounteren gate opened per hart | rustes/rsbr receipt line on rv64 |
| RES-23 | W | DONE@R8 | Rust row a64: same class (ARM64 close) — R8: cfg'd svc/cntvct, initrd `/bina64/rustes`; CNTKCTL_EL1 gate opened per core | same receipt on a64 |
| RES-24 | W | DONE@R9 | IPv6 SLAAC/sockets/dual-stack recorded at X7 — R9: SLAAC formed from the RA prefix (A-flag /64 + EUI-64), router learned, NS→NA responder added, off-link echo E2E in CI.  The "SLIRP filter" X7 blamed was FIVE of our own NDP bugs, pcap-named.  Sockets/dual-stack ride RES-26 | ping6 a SLAAC address (fec0::2 answered; case pins it) |
| RES-25 | W | DONE@R9 | TCP DNS fallback on truncated UDP (X3) — R9: RFC 1035 s4.2.2 length-prefixed retry against the SAME server; named one-shot knob (DNSCTL_FORCE_TC) drives the lane, the TCP wire and the 664-byte answer are real (guestfwd fixture) | >512B answer resolves via TCP; case pins it |
| RES-26 | W | DONE@Y4 | HTTPS-over-IPv6 fetch receipt — Y3 paid the TCP/AAAA/AF_INET6 debt; Y4: libahttp parses `[addr]`, resolves AAAA, dials v6 (serial v4 fallback), guest `[https6] PASS` against fec0::2:8446 | `[https6] PASS` in test_https6 |
| RES-27 | W | DONE@R9 | /apps/http still hand-rolled — the FOURTH stale doc row: http.c has been libahttp's keep-alive client since X2/X6 (header says so, Makefile links libahttp, http_get/x6 cases pin the behaviour); R9 recorded the catch, zero code needed | app links libahttp; behavior pins hold |
| RES-28 | W | DONE@R9 | virtio-net RX polls; IRQ RX pending — HALF-stale: the ISR + wq wake existed, but nothing ever slept (timed waits pause-spun), so the interrupt was decorative.  R9: wq_wait_deadline in the timed path + counted receipt (vmxnet3/e1000e data paths stay S → RES-46 class) | RX-via-IRQ receipt in net case (pinned) |
| RES-29 | W | DONE@R10 | atls_fe needed 32-bit limbs; -m32 crypto was blocked (I386/status) — closed: packed 8×uint32 radix-2^32 fe core + P256 limb parameterisation, same API | X25519/Ed25519/P-256 vectors green at -m32 ✔ (104 checks ×2 lanes: FORCE32 + real -m32) |
| RES-30 | M | PENDING-USER@R11 | PAT/WC framebuffer win measurable only on metal (OPT §7/HW H3) | R11 package slot 4 ships; user paste-back is the number's only source |
| RES-31 | W | DONE@R11 | PCID: D-PCID-5 gate FIRED (user WHPX log `pcid=1`) — D-PCID-1..4 implemented: pcid_policy.h (host-tested, 24 checks) + pcid.c, CR3-reload+generation fallback, no invpcid; named deviation: hash-slot 1..255, not the written bump-4095 | counters un-pinned (perf smoke self-selects by feature bit); WHPX receipt block = docs/metal_receipts.md slots 5/6 ✔ |
| RES-32 | M | PENDING-USER@R11 | ERMSB crossover tuning needs real silicon (HW H2) | R11 package slots 2/3 ship; user paste-back |
| RES-33 | M | PENDING-USER@R11 | O3 wall-clock, O8 ThinLTO bar, membench metal numbers (OPT §7/HW §6) | R11 package slots 3/8/9 ship |
| RES-34 | W | DONE@R11 | fast-boot knob unwired on i386; a64 fw-cfg AMEND-5 deferred — closed: fwcfg32.c (same port protocol) + fwcfg_a64.c (MMIO, BE selector at +8, DTB-discovered) feed the SHARED selftest.c, newly a KERNEL32/KERNELA64_SHARED row | knob toggles in a smoke on both ✔ (i386_shell: mode=off + 2 SKIPPED; a64_boot: mode=off + 2 SKIPPED) |
| RES-35 | W | DONE@R1 | O1/O6/O8 measured at R1: O8 REFUSED with numbers (naive --gc-sections on kernelrv "saves" 68% by deleting live boot/trap sections — all five smokes red; needs a KEEP() audit, folded into the R12 hand-off notes); O1 superseded (P7 linked word-wide string.c on i386); O6 deferred to R6 where port allocation actually grows | measured; done or refused with numbers |
| RES-36 | S | DONE@C4 | MSI/MSI-X for virtio + virtio-gpu (MATURITY) | OPENER, measured: virtio_pci.c's cap walk parses ONLY vendor-specific (0x09); zero mentions of caps 0x05/0x11 in virtio_pci.c or pci.c; the ISR register is read-to-clear INTx — the series starts at the cap walk it already owns. CLOSED at RESIDUE3 C4 (2026-11-04): the cap walk exists as a portable, host-tested module (drivers/pci/msi_caps.{h,c}, 12/12 unit cases), the x86_64 LAPIC side owns vectors 0x60-0x6F with dispatch from isr.c and EOI after the handler (kernel/arch/x86_64/msi.{h,c}), and both virtio drivers attach through the arch.h seam with the entry index written to the device and read back. The MSI (non-X) half of the walker is host-tested only — no virtio device in this QEMU offers cap 0x05 — and the request paths still poll the used ring (the message is delivered and counted; sleeping on it is a scheduler change, named not implied) | `[msi] virtio-blk: MSI-X vector 96 (1 msg, 3 entries)` → `queue 0 -> table entry 0 (readback 0), x86 vector 96` → `1 interrupt(s) delivered on vector 96` + `[virtio-blk] self-test: LBA 0 read OK (interrupt delivered)`; the same three lines for virtio-gpu; `opt/auralite.msi=off` keeps both on INTx with no msi line at all; test_msix.sh 28/28 |
| RES-37 | M | DONE@R11 | IOAPIC base was QEMU-hardcoded with no cross-check — closed: the kernel walks RSDP→RSDT/XSDT→MADT (both loaders publish rsdp_phys) and prints agree/disagree-by-name; QEMU NULL: `(MADT agree)` asserted. Interrupt Source Overrides remain the named residue in ioapic.c | `[ioapic] base ... (MADT agree)` pinned in test_metal_null; package slot 7 carries the metal line |
| RES-38 | S | DONE@T6 | OHCI/EHCI/xHCI full transfer scheduling; HID beyond UHCI; BOT short-packet line (USB/TODO) | OPENER, measured: uhci.c 555 lines IS the complete reference; ohci 709 / ehci 869 / xhci 2093 lines carry the shapes. CLOSED at T6: five of the named boxes were MEASURED STALE — the named stub markers are gone and the 13-gate USB sweep re-run 2026-09-05/06 pins the rings/queues (one xhci TD-timeout runner flake, re-run green). The narrowed survivor is frame-accurate isochronous STREAMING → RES-60 (owned by RESIDUE3 C5) |
| RES-39 | S | DONE@T6 | Bluetooth USB transport; Wi-Fi chipset backend (TODO/status) | OPENER, measured: bt.c 193 lines and wifi.c 333 + wifi_virt.c 136; the row's "the tree has ZERO wireless NIC" sentence had gone STALE IN ITS OWN FAVOUR. CLOSED at T6: Bluetooth's transport moved onto the controller-agnostic usb_core API with the protocol host-pinned (test_bt_hci 35/35); Wi-Fi gained RX ingestion (scan/auth/AID parsed from frames), host-pinned protocol (test_wifi_proto 37/37) and a virtual AP that proves the flow end to end (test_wifi_virtual_ap) — the plain `make iso` boot prints `[wifi] PASS: virtual AP: scan found 'AuraVirt' ch6` + auth/AID + 78 data bytes (re-run 2026-11-03). The narrowed survivor is a real controller/chip attach → RES-61 (RESIDUE3 C6) |
| RES-40 | W | DONE@R1 | virtio-gpu init hang found by G13, bisected pre-G11d (TODO) | repro'd + fixed, or narrowed with a new fact |
| RES-41 | S | DONE@L7 | TGSI backend for VirGL DRAW_VBO (GL G13) | OPENER, measured: the KERNEL side already emits the 12-dword DRAW_VBO packet (virgl.c:180); glvirgl.c:13 names its own gap — "does NOT implement DRAW_VBO … needs shaders" — the missing piece is a userspace TGSI assembler feeding CREATE_SHADER, nothing kernel-side.  CLOSED at GL2 L7: `gl_backend_t.draw` + a hand-written canned TGSI pipeline in libgl takes whole fixed-function glDrawArrays(GL_TRIANGLES) batches through CREATE_SHADER + SET_VERTEX_BUFFERS + DRAW_VBO (the seam — RES-41's userspace half). The AST→TGSI retarget itself was never claimed here and opens as RES-54 |
| RES-42 | W | DONE@R12 | POSIX leftovers triaged entry by entry: posix2024 known_partials measured EMPTY (0 active rows — the last one, sem_open, closed 2026-08-21 by conformtest's own CI catch); readline/scanf/jobs-fg-bg re-affirmed as the POSIX_PLAN's OWN named deferrals (§P9 text, lines 593/622/637); epoll re-affirmed a non-goal — poll() rides select(), recorded in TODO's map | each entry triaged: scheduled or re-affirmed ✔ |
| RES-43 | N | RE-AFFIRMED@R12 | W32 deferred W-surface, BitBlt, LoadLibraryW, kernel import binding — re-affirmed with their WIN32_PLAN D-numbers: SEH stays a setjmp shim (D-SEH), TLS per-process, no registry/COM/DirectX/WinSock (D8 class); the boundary text lives in docs/win32.md and status.md's w32 row | re-affirmed with D-numbers ✔ |
| RES-44 | N | RE-AFFIRMED@R12 | W32 registry (D8), MSG_COPY/SHM_LOCK/proc-sysvipc, DOOM §6, slab single-caller rule — all four re-affirmed as the decisions their plans recorded; none silently reversed | re-affirmed ✔ |
| RES-45 | W | DONE@R12 | TODO.md kept IN FULL and ANNOTATED (the user's call — the first draft rewrote it down to a map and the review caught the loss): headline pointer names the ledger as the machine-checked index; THIRTEEN stale entries receipted inline in the file's own ~~strikethrough~~+Done style (six from the R12 sweep: IST/#DF — closed R1; virtio-net polling — closed R9; brk/mmap; virtio-blk ×3 transports; symlinks; CI artifacts ×6 upload steps; seven from the follow-up audit: uaccess #PF fixup — M3; MAP_SHARED — shmem.c; auxv — M5; posix_spawn.c; tmpfs mkdir — Q12; mkdir(mode); tan/fmod/atan); live boxes carry their ledger row or class | every line dispositioned ✔; the file GREW by receipts (576→639 lines) ✔; harvest TODO count 26→20, baseline moved same-commit |
| RES-46 | S | DONE@F7 | FS data paths + modern NICs — F7 (FSFULL_PLAN.md) is RES-46's exit gate: the five filesystems (ext4/f2fs/btrfs/exFAT/NTFS) now have real, self-contained CI cases registered in `tests/integration/run_all.sh` ALL_CASES and run as the `fsfull` shard (each builds its own disks — no shared state), the five kernel self-tests run in the `full` selftest lane when mounted, and the ledger row proves coverage machine-checkably via `tools/check_residue_claims.py`.  F7 opened this row by MEASURING the FS harnesses against the tree: f2fs/btrfs/exFAT/NTFS are green, and the coverage surfaced a pre-existing F3 defect — the ext4 driver cannot interoperate with a real `mkfs.ext4` image (its non-standard `base_lba=128` layout is not host-e2fsck-verifiable), tracked in FSFULL_PLAN.md F7.  The NIC side (vmxnet3/e1000e/NVMe) stayed open as RES-46's named successor, and RESIDUE3 closed it in two measured steps: C7 re-derived vmxnet3/e1000e (14/14 each, T6's receipts still hold) and C7b built the NVMe driver this row's successor named — a polled queue-pair driver whose namespace mounts ext2 (test_nvme 23/23). | OPENER, measured: the five-filesystem line count is 3889 with 0 CI cases — F7 delivered 5 registered cases + the fsfull shard + full-lane self-tests; ext4's red interop lane is a discovered pre-existing driver defect, not a coverage gap |
| RES-47 | S | DONE@T7 | GUI isolation/permissions, clipboard/focus, settings (TODO) — part-stale too: gui_syscalls.c ALREADY gates 36 syscall cases behind require_owner/require_icon_owner | OPENER, measured: ownership gates exist (27 permission/owner sites); clipboard had 5 references and no cross-process model. CLOSED at RESIDUE2 T7 (2026-09-06): all four boxes landed with receipts — the owner/permission model extended to GUI-GLOBAL mutations (clipboard set+get deny-then-grant round trip), persisted settings/theme, richer editor/terminal, and the `gclip` clipboard manager dogfooding the ACL (7-check gate); T7's Result is the receipt and this row was the last one it forgot to flip |
| RES-48 | M | PENDING-USER@R11 | HW §6 metal receipts never exercised on user's machine | R11 package ships (tools/metal_receipts.sh, NULL test CI-pinned); pending-user is a status, not a failure |
| RES-49 | M | PENDING-USER@Y7 | Live-web PQ-hybrid fetch against a real host (the X9 PEER_EOF sentence) | paste `[tls] group=X25519MLKEM768` from `run http https://www.ietf.org/` into docs/live_web.md |
| RES-50 | N | RE-AFFIRMED@Y7 | Happy-eyeballs connection racing — dual-stack still picks then falls back serially (REALINTERNET2 §4) | re-affirmed; no racing |
| RES-51 | N | RE-AFFIRMED@Y7 | No DTLS, no QUIC, no TLS client certificates, no standalone ML-KEM TLS group, no kernel-side TLS (D4 + §4) | re-affirmed |
| RES-52 | S | DONE@C7 | TCP window scaling — CLOSED at RESIDUE3 C7 (2026-11-04), and the 64240 window is gone | OPENER, measured: `TCP_WINDOW` was 64240 and grep `wscale`/`TCPOPT`/`shift_cnt` across tcp.c+tcp.h was 0. What C7 found underneath the opener: (a) `ssthresh = TCP_WINDOW` — commented "arbitrarily high" — became the real ceiling the moment a peer could grant gigabytes (measured: peak in-flight stopped at exactly 64240 against a 16 MiB peer window); (b) the established path reset `snd_shift = 0` right after the SYN-ACK parser learned it, so a correctly scaled connection silently reverted to 16 bits at handshake completion. Both fixed. What landed: `kernel/net/tcp_wscale.h` (parse/emit/apply/encode/shift-for-buffer, pure, 56/56 host checks including shift 255 clamped to 14, wrong-length WSCALE ignored, truncated block, zero-length option that must not spin); a WSCALE in every SYN (option block 8 -> 12 bytes, header 28 -> 32); the peer's shift parsed from the SYN-ACK and applied as `snd_wnd = field << shift`; our own window encoded as `rcv_wnd >> shift` with rcv shift 0, derived from the receive buffer we actually hold and printed with the reason. | `[tcp] wscale: peer shift=8 -> snd_wnd=16776960 (field 65535); our shift 0 (64240-byte receive buffer)`; `[tcp] wscale lane (big): PASS 395660 bytes were in flight at once — above the 65535-byte unscaled ceiling`; `[tcp] wscale lane (clamped): PASS peak in-flight 14600 stayed inside the 65535-byte window the peer actually granted`; test_tcp_wscale 19/19 |
| RES-53 | W | DONE | TLS CertificateVerify: `rsa_pss_rsae_sha256` — RSASSA-PSS SHA-256 / MGF1 / saltLen=32; host `test_atls_tls` 41/41 (openssl vector + RSA s_server) | PSS verify lands; rust-lang.org CV is no longer the refusal |
| RES-54 | S | OPEN | GLSL AST → TGSI retarget (GL2 D7) | OPENER, measured: the canned triangle GL2 L6 shipped is hand-written TGSI dword arrays in glvirgl.c, and G11's compiler still produces an interpreted AST with no back end — a real TGSI retarget is compiler work (a successor plan, not a GL2 phase). The seam it would hang on is live: gl_backend_t.draw is wired, the eligibility screen and the whole-draw fallback are tested host-side (test_glvirgl 73) |
| RES-55 | W | DONE@RT3 | Realtek 8169/8168 gigabit NIC — REALTEK_PLAN.md (RT0-RT3). The 8169 is the one catalogued NIC QEMU cannot emulate, so the gate is INVERTED: the register/descriptor surface is host-pinned (r8169_desc.h, test_r8169_desc 74 checks) and the driver core is proved end-to-end against a register-level chip model on the host (test_r8169_driver 1635 checks: probe → MAC → TX → RX → IRQ; negative controls proven), then wired into net_init after e1000e and catalogued/docs flipped. Real silicon is the M-class successor (RES-56) | host gates + check_realtek_claims.py green in make test-unit; net_init chain includes r8169 |
| RES-56 | M | PENDING-USER@RT3 | 8169 real-silicon data path — the chip is the only place the true data path can be observed (QEMU has no 8169), and the model proves the driver, not the chip (REALTEK_PLAN §1.2) | RT3 package slot 10 ships; user paste-back is the number's only source (the RES-30/32/33/48 discipline) |
| RES-57 | S | DONE@C8 | 1694 `static-only` symbols in the W32 app ledgers are bound but never called at runtime — CLOSED at RESIDUE3 C8 (2026-11-05): all 820 distinct named pairs are now resolved in-guest through LoadLibrary + GetProcAddress, and a slice is called | OPENER, measured: 1694 markers across five ledgers = 1670 by name + 24 by ordinal = 820 distinct named pairs (7z 86, 7zFM 298, notepad++ 590, npp-plugins 372, putty 348). What C8 landed: `tools/w32_static_slice.py` generates the pair table from the ledgers and BOTH export tables (`w32/src/w32_bind.c` D7 table + `w32/src/w32_stubs_gen.c`), so the guest cannot check a different list than the host printed; `w32/tests/w32c8_static_slice.c` LoadLibraryA's all 20 named DLLs, resolves all 820 pairs, names every NULL, and calls 9 argument-free stubs + 4 real implementations through hand-written prototypes; `test_w32_static_slice.sh` is 38/38. Findings, all three of them the kind this box exists for: (a) `LoadLibraryA("imm32.dll")`, `("ole32.dll")`, `("uxtheme.dll")` answered NULL with MOD_NOT_FOUND — three implemented modules exported by the static tables and never registered as loadable, so IAT binding worked while LoadLibrary + GetProcAddress failed (the A-8 lesson recurring); fixed in `w32/src/w32_module.c`. (b) A name-only parser of the D7 table reports 9 exported symbols as absent because the generated stub table exports them — the generator reads both. (c) A naive "call every argument-free stub" slice is not safe: `msvcrt!__C_specific_handler` faulted (RIP 0x40007bf8, CR2 0x42) and `user32!CreateIconIndirect` faulted (CR2=0xa4) because it is a real implementation shadowing its stub; the slice is now stubs nothing shadows, plus 4 hand-written real prototypes. | `[w32] STATIC-ONLY modules: 20/20 loaded`; `[w32] STATIC-ONLY kernel32.dll: 267/267 resolved via GetProcAddress`; `[w32] STATIC-ONLY uxtheme.dll: 15/15 resolved via GetProcAddress`; `[w32] STATIC-ONLY 820/820 resolved over 20 modules (24 further markers are ordinal imports: no name to resolve)`; `[w32] STATIC-ONLY calls: 9/9 made`; `[w32] STATIC-ONLY call mpr.dll!WNetAddConnection2W -> 0x00000000000004c6 (called, not merely bound)`; `[w32] STATIC-ONLY real calls: 4/4`; `[w32] STATIC-ONLY call kernel32.dll!GetACP() -> 65001`; `C8-STATIC-OK`; `/tests/w32c8_static_slice.exe (tid 8) exited (code=78)`; test_w32_static_slice 38/38 |
| RES-58 | S | DONE@C8 | FAIL-CLEAN functions asserted by ledger class but never observed returning their honest failure in-guest (W32APP_PLAN D9) — CLOSED at RESIDUE3 C8 (2026-11-05): all 35 are now called by a real PE, by name, through w32run's binder | OPENER, measured: `docs/win32.md` generated 35 FAIL-CLEAN functions and no in-guest receipt had watched one return its documented failure. What C8 landed: `w32/tests/w32c8_failclean.c` imports all 35 by NAME and calls each with the smallest arguments that reach the refusal; `tests/integration/cases/test_w32_failclean_live.sh` asserts every one of the 35 receipts by name (42/42 assertions, exit 78 = clean). 32 refuse exactly as the page says; 3 are documented degradations and are asserted for the behaviour they have instead of being flattened into "failed" (`AdjustTokenPrivileges` TRUE + ERROR_NOT_ALL_ASSIGNED, `DwmGetColorizationColor` S_OK + opaque black, the SENSAPI pair reporting a real probe). What the measurement FOUND: `W32_ERROR_NO_SUCH_PRIVILEGE` was defined as **1530**, a code Windows never defines — `ERROR_NO_SUCH_PRIVILEGE` is **1313** — so an app comparing `GetLastError()` against the documented constant took the wrong branch on the `LookupPrivilegeValueW` refusal. Fixed in `w32/include/w32/advapi32.h`, found only because the box existed. A second finding was in the fixture itself: `CertFindCertificateInStore(NULL, ...)` reports ERROR_INVALID_HANDLE (6), not CRYPT_E_NOT_FOUND (0x80092009) — the CRYPT_E branch needs a non-NULL store, and both branches are now asserted. | `[w32] FAIL-CLEAN LookupPrivilegeValueW -> FALSE (as documented: ERROR_NO_SUCH_PRIVILEGE)`; `[w32] FAIL-CLEAN LsaOpenPolicy -> 0xC0000022 (as documented: STATUS_ACCESS_DENIED, no handle)`; `[w32] FAIL-CLEAN PrintDlgW -> FALSE (as documented: PDERR_NODEFAULTPRN, no printers)`; `[w32] FAIL-CLEAN WinVerifyTrust -> 0x800B0100 (as documented: TRUST_E_NOSIGNATURE)`; `C8-FAILCLEAN-OK`; `/tests/w32c8_failclean.exe (tid 8) exited (code=78)`; test_w32_failclean_live 42/42 |
| RES-59 | S | DONE@C8 | the Audacity Outcome-B walls — the §6 scale/shape/runtime risk materialised (W32APP_PLAN W32A-17) — CLOSED at RESIDUE3 C8 (2026-11-05): wall 1 measured shut at the same scale, wall 2 half-closed and stated as half, wall 3 now reported instead of hidden | OPENER, measured: `w32/app_ledger/audacity-3.7.5.gap` recorded 3 fatal walls (90932 relocations vs w32run's 16384-entry buffer; a 117-DLL bundled graph; a 19-module/419-symbol gap = UCRT api-set 302 + winmm 74). What C8 landed: one relocation path shared by the kernel loader and the personality (`w32/src/w32_reloc.c` + `pe_relocations_each()`, streaming, no fixed cap — the buffer and its 16384 ceiling are gone), so the 90932-relocation image applies every fixup with delta 0x3ffec0000000; the module table is 256 slots with 128 keepalives and depth 32, so the 117-node graph with a cycle guard loads; and a symbol gap is now a NAMED list (`w32_bind_imports_gap()` → `w32_gap_at()` → `w32_gap_describe()`), which turned Audacity's 419 and DOOM's 52 from numbers into work items. Wall 2 is HALF-closed and the ledger says so: the graph loads, but the UCRT api-set forwarders behind 302 of those symbols are not implemented, so Audacity still refuses — at a gap report, not at a relocation overflow. Measured receipts: `[w32] relocations: 90932 applied (DIR64), delta 0x3ffec0000000, no fixed cap` (test_w32_scale 17/17), plus the live FAIL-CLEAN and static-only gates that came out of the same rebuild (42/42 and 38/38). | `w32run: [w32] relocations: 90932 applied (DIR64), delta 0x3ffec0000000, no fixed cap`; `w32run: gap  winmm.dll                2 symbol(s)`; `w32run: gap  wsock32.dll              1 symbol(s)`; `w32run: [w32] gap: 2 module(s), 3 symbol(s) missing`; `w32run:   missing GDI32.dll!StretchDIBits` (DOOM: 52 named); `[w32] gap: 4 module(s), 52 symbol(s) missing`; test_w32_scale 17/17 |
| RES-60 | W | DONE@C5 | Frame-accurate isochronous STREAMING — xHCI ISOCH TRBs (type 5) plus a device that consumes them (RES-38's narrowed survivor; TODO.md's isoc box) | OPENER, measured: test_usb_isoc printed `[isoc] SKIP: no isochronous endpoint exercised`; xhci.c carried rings and no ISOCH TRB type. CLOSED at RESIDUE3 C5 (2026-11-04): the TRB path is wired (usb_isochronous_transfer routed xHCI isoc to a BULK transfer and never called xhci_isochronous_transfer), the endpoint is configured as ISOCH (1/5) instead of INTERRUPT (3/7), a 24-packet burst of a generated 440 Hz sine through QEMU `usb-audio` completes 24/24 with 1152 frames of audio and 0 lost, and the case flips SKIP→PASS with the count. Measured on the way, and recorded rather than hidden: pinning packets to frames (base+i) delivered 1/24 because QEMU's isoclock runs far ahead of the guest, so the burst is issued SIA with MFINDEX-based accounting. The narrowed survivors — HID report-ID breadth, EHCI split-transaction series, a short-CSW case — move to RES-62 / C5b | `[isoc] PASS: 24 packet(s) streamed on xHCI ep 0x01 (192 B/packet, 440 Hz sine, 1152 frames of audio)` + `frames: scheduled 24, completed 24, lost(overrun) 0, empty bursts 0, timeouts 0`; test_usb_isoc 11/11 |
| RES-61 | M | PENDING-USER@C6 | Bluetooth / Wi-Fi silicon attach — the narrowed survivor of RES-39; RESIDUE3 C6 proved both controller paths as far as a machine can without the chip | OPENER, measured at C1: bt_init() printed `[bt] PASS: HCI protocol layer ready (no controller detected)` — the transport was proved, the controller absent. C6 measured something sharper and had to change plan because of it: **QEMU removed its Bluetooth subsystem in 9.0** — `-bt` answers `invalid option` and `-device help` lists no `usb-bt-dongle`/`bt-*` device (asserted by test_bt_hci_transport.sh case 1), so no emulated dongle can ever be attached here. What C6 DID prove: the HCI command/event round trip lives in `drivers/bluetooth/bt_transport.{h,c}` with the transport injected, and Reset + Read Local Version + Read BD_ADDR + Inquiry run against a scripted CSR-class controller — 10/10 on the host (including a controller that answers nothing and one whose BD_ADDR is all zeroes) and a kernel self-test quoting the controller's own BD_ADDR `bc:9a:12:34:56:78`; the two hardcoded USB endpoints (`0x82`/`0x02`) now come from enumeration; pairing is a named refusal (SMP/SSP key exchange and a link-key store do not exist). Wi-Fi went further and reached the IP stack: `wifi_virt` answers ARP and ICMP for its address from the received bytes and `wifi_netdev.c` exposes the link as a `struct netdev`, so with `-nic none` the kernel's own `net_ping(10.0.2.2)` succeeds over 802.11. What remains is metal only, and it is the user's: a real dongle (class 0xE0/0x01/0x01) and a real 802.11 chipset still have never been touched by this tree | `[bt] selftest: PASS HCI transport round trip (no controller attached; QEMU has no Bluetooth device)` + `[bt] selftest: BD_ADDR bc:9a:12:34:56:78 read from a Command Complete`; `[net] PASS: ping 10.0.2.2 successful (ICMP echo reply received)` + `[wifi] PASS: virtual AP answered 1 ARP request(s) and 1 ICMP echo request(s) with real replies`; test_bt_hci_transport 12/12, test_wifi_virtual_ap 18/18, unit bt_transport 10/10 |
| RES-62 | S | DONE@C5b | USB survivors C5 did not take: HID report-ID breadth (full report descriptors, multi-report-ID devices), the EHCI split-transaction series for full/low-speed devices behind a high-speed hub, and a case for the short-CSW line | OPENER, measured 2026-11-04: hid.c's parser kept ONE `report_id` global and tagged no field, so a descriptor with report IDs 1 and 2 collapsed every field onto the last ID; `test_usb_hub*` attached its hub on xHCI and no case put an FS/LS device behind a high-speed hub, so ehci.c's TT fields were unexercised; msc.c's short-CSW line had no case. CLOSED at RESIDUE3 C5b (2026-11-04): the parser is `drivers/usb/hid_report.{h,c}` (per-field `report_id`, per-report bit cursor, `hid_report_body()` prefix split) with 8/8 host cases and a kernel selftest that passes over a two-ID descriptor; writing the wire-length test found that output-report bits were being counted as input bits (QEMU's 8-byte keyboard computed as 10 bytes), which is fixed and pinned; EHCI now decodes PORTSC.PSPD instead of returning "high-speed" for every port, and the QH capability words come from `drivers/usb/ehci_qh.{h,c}` (9/9 host cases) whose C-mask test found an empty mask for a micro-frame-7 start; the CSW check is `drivers/usb/msc_csw.{h,c}` (10/10 host cases) with short and over-long as separate results. Two plan bullets changed shape after measurement and say so in §C5b: no QEMU HID device declares a Report ID at all, and QEMU has no high-speed hub (its `usb-hub` is full-speed and refuses a high-speed bus), so the TT path is verified as an encoding plus a runtime log line, never as a transfer | `[hid] selftest: two-ID descriptor -> ids=2 (1,2) input bits: r1=64 r2=4 wire=9` + `[hid] selftest: PASS report-ID split and per-report decoding`; `[ehci] port 0: full-speed device (PSPD=0)` + `(interval=8 frames, maxpkt=8, eps=0, split=no, S-mask=0x01 C-mask=0x00)`; test_usb_hid_report_ids 13/13, test_usb_ehci_split 11/11, test_usb_hid_input 9/9; unit hid_report 8/8, ehci_qh 9/9, msc_csw 10/10, negcontrol catches the pre-C5b parser |

## Arithmetic (checker-enforced)

Rows: 62.  Classes: **W 36 · M 8 · N 4 · S 14**.  The R0 recount
**W 33 · M 5 · N 2 · S 8** (the plan §2 draft hand-summed 27/6/3/12
and was WRONG — amended same-commit, catch recorded) is the
historical pin; Y7 appended RES-49..53.  Statuses at CLOSE: OPEN 6 (RES-02/06/07/16/18/54 — named survivors, none hidden: two narrowed oddities, two port-adoption rows, one IOAPIC-routing row, and GL2's D7 hand-off; RES-26's DONE@Y4 flip is recorded below and was mistakenly still counted here), DONE@R1 6, DONE@R2 2, DONE@R3 1, DONE@R4 1, DONE@R5 2, DONE@R6 2 (RES-17/19; RES-18 PIE stays OPEN — it wants real relocation work, not a floor), DONE@R7 2 (RES-20/21), DONE@R8 2 (RES-22/23), DONE@R9 4 (RES-24/25/27/28; RES-26 stays OPEN, narrowed to the TCP layer), DONE@R10 1 (RES-29), DONE@R11 3 (RES-31/34/37), PENDING-USER@R11 4 (RES-30/32/33/48 — the package ships and NULL-tests green; the numbers are the user's move, and pending-user is a status, not a failure), DONE@R12 2 (RES-42/45), RE-AFFIRMED@R12 2 (RES-43/44), HANDED-OFF@R12 7 (RES-11/12/36/38/39/46/47 — every one leaves with a MEASURED opener fact in its row), DONE@L7 1 (RES-41 — the canned DRAW_VBO seam landed in GL2 L6; its compiler half opens as RES-54).
Y4: RES-26 → DONE@Y4; OPEN then 5 (RES-02/06/07/16/18).
Y7: +5 rows (RES-49..53); OPEN was 6 (the five R-series survivors
plus RES-53).  PSS verify flipped RES-53 → DONE; OPEN back to 5
(RES-02/06/07/16/18).  PENDING-USER@Y7 1, RE-AFFIRMED@Y7 2,
HANDED-OFF@Y7 1.
RESIDUE2 T2: RES-16 → DONE@T2; OPEN then 5 (RES-02/06/07/18/54).  TODO.md unchecked 40 → 38 (the T2 ISO box and the T1 errno box the T1 commit forgot to flip); baseline moved same-commit.
RESIDUE2 T3: RES-07 → DONE@T3 (narrowed close recorded in the row); OPEN then 4 (RES-02/06/18/54).  TODO.md unchecked 38 → 34 (the four T3 storage boxes); baseline moved same-commit.

RESIDUE3 C5: RES-60 → DONE@C5 (isoc streaming proved on QEMU usb-audio);
+1 row RES-62 (HID report-ID breadth / EHCI split series / short-CSW case,
HANDED-OFF@C5 → C5b), which is where the S pin goes 13 → 14.
RESIDUE3 C7b: RES-46's NVMe successor CLOSED (the row itself stays DONE@F7 —
it never had an NVMe box of its own).  What landed: drivers/nvme/{nvme.h,
nvme.c,nvme_proto.h} — PCI probe (class 01/08 prog-if 02), a controller reset
that waits for CSTS.RDY both ways, an 8-entry admin queue pair created before
CC.EN=1, Identify Controller/Namespace, one I/O queue pair created CQ-then-SQ,
and a namespace registered through blkdev.  Polled completion, PRP1-only via a
4 KiB bounce buffer, 512-byte LBAs only (a 4096-byte namespace is refused by
name).  Receipts: the controller's own Identify strings, `[ext2] blkdev 1
mounted at /ext2`, and a host debugfs read of the file the guest wrote onto the
namespace.  Table arithmetic unchanged: 62 rows, W 36 · M 8 · N 4 · S 14.
The plan harvest moves 52 → 53 for the same reason every phase row moves it:
the C7b table row names `patches/RESIDUE3_C7b_nvme.patch`, and the harvester
counts the `RESIDUE3` token in that path.  Baseline and ledger move together;
no new debt.
RESIDUE3 C8 addendum: the plan's harvest moves 53 → 54 because the header table
gained one row naming `patches/RESIDUE3_ALL_C0-C8.patch` — the cumulative patch
from `AlexanderNyr/AuraLite-OS@26cb7fe` (177 files, +34034/−755, replayed onto a
fresh clone with no differences).  Same reason every phase row moves it: the
harvester counts the `RESIDUE3` token in the path.  Baseline and ledger move
together; no new debt.  Table arithmetic unchanged: 62 rows, W 36 · M 8 · N 4 ·
S 14.
RESIDUE3 C8: RES-57, RES-58 and RES-59 → DONE@C8 (all three were
HANDED-OFF@W32U).  The 35 generated
FAIL-CLEAN functions are now watched, in-guest, by a real PE that imports them
by name and calls each one; the measurement found an error constant that was
not the documented one (`W32_ERROR_NO_SUCH_PRIVILEGE` 1530 → 1313), which is
the whole argument for the box.  RES-57's 1694 markers are now resolved live
(820 distinct named pairs, 20/20 modules, a called slice) and the measurement
found three modules the personality implemented but could not load by name
(imm32, ole32, uxtheme).  RES-59's three Audacity walls close with it: wall 1
(90932 relocations) measured shut at the same scale by the streaming loader,
wall 3 (the 419-symbol gap) now a named list rather than a count, and wall 2
(the 117-DLL graph) half-closed and stated as half — the graph loads, the UCRT
api-set forwarders behind 302 of its symbols do not exist, so Audacity refuses
at a gap report instead of a relocation overflow.  Table arithmetic
unchanged: 62 rows, W 36 · M 8 · N 4 · S 14.
RESIDUE3 C7: RES-52 → DONE@C7 (was HANDED-OFF@Y7).  Table arithmetic
unchanged: 62 rows, W 36 · M 8 · N 4 · S 14.  RES-46's NVMe successor did NOT
close — it was carried out of C7 into C7b with a measured opener (QEMU's
`-device nvme` exists, the tree has no NVMe code at all, and the blkdev seam it
must land on does).
RESIDUE3 C6: RES-61 stays PENDING-USER@C6 (moved from @C1) — the metal half
is a real dongle and a real 802.11 chip; the protocol half is now proved on
both stacks (HCI transport injected and scripted-controller tested; Wi-Fi
reaching the IP stack over the virtual AP).  Table arithmetic unchanged: 62
rows, W 36 · M 8 · N 4 · S 14.
RESIDUE3 C5b: RES-62 → DONE@C5b (HID report-ID parser extracted and host
tested, PORTSC.PSPD decoded, QH capability words host tested, CSW validator
host tested).  No new rows: the one part of C5b that did NOT get a device
receipt — the EHCI split-transaction path behind a TT — is named inside the
row and in §C5b rather than opened as fresh debt, because it is verified as an
encoding plus a runtime log and QEMU cannot build the topology (its `usb-hub`
is full-speed and refuses a high-speed bus).  Table arithmetic unchanged:
62 rows, W 36 · M 8 · N 4 · S 14.  Same commit: the row regex in
`tools/check_residue_claims.py` learned the lowercase phase suffix
(`DONE@C5b`); without it the row silently fell out of the count — 61 of 62 and
S 13 of 14 — instead of failing.
Harvest baseline lives in `tools/residue_baseline.txt` (the
harvester's own regex is the metric; the §2 draft quoted counts
from a wider grep that included "blocked" — superseded).  A marker
count that drifts from the baseline fails CI until the ledger and
baseline move in the same commit: new debt REGISTERS or the build
is red.

RESIDUE2 T1: +2 marker lines in RESIDUE2_PLAN.md (the T0 Result cites
`check_residue2_claims.py` / `test_residue2_claims.sh` — the rig's own
receipts); baseline moved 18 → 20 in the same commit. No new debt rows:
the plan's own ledger references are unchanged.

RESIDUE2 T8: +1 marker line in HW_PLAN.md (the RES-02 fix addendum to
the H2 result — SMAP root cause, user.c frame write, smoke receipt);
baseline moved 15 → 16 in the same commit.  No new debt rows: T8
CLOSES three (RES-02/06/18 → DONE@T8; rows updated above).

RESIDUE2 T9 (close-out): +1 marker line in RESIDUE2_PLAN.md (the T9
Result); baseline moved 20 → 21 same-commit.  The plan is COMPLETE:
TODO 40 → 7 (the seven are future-work outside its scope), ledger
OPEN 6 → 1 (RES-54), phases 10/10, checkers flipped with the last
phase (rinet2 Y4 pin moved with the T8 row flips).

RESIDUE2 CI fix (second wave, 2026-09-06): not a plan phase — the
post-close CI runs 92232963170/92244125363 reddened four jobs, all
four roots closed in one commit. ext4: private dirent dialect
(9-byte header) → the on-disk 8-byte u8+u8 header + the missing
mkfs.ext4 formatter facts (cluster fields, FILETYPE, bitmap padding,
reserved inodes, extent-derived i_blocks); ext4 13/13. stopped:
ISIG dispatch unconditional again in the fd-0 path (T4 had gated it
off the console tty); 17/17. posix2024: mq_notify fires on
delivered_size changes too; 95/95. selfhost-tcc: the "hang" was a
silent triple-fault reset loop — the BIOS stage2 ELF loader walked
the phdr table INSIDE the 2 MiB staging buffer, and once the tcc
kernel's physical extent passed the staging base (the device phase
pushed it over), the PT_LOAD #2 copy zeroed the live phdr table and
PT_LOAD #3 (.data/.got) was silently dropped; elf_load now spools
the headers locally before any copy (phentsize!=56 or >8 headers
refuses loudly); 19/19. No ledger rows opened or closed; the
baseline is unchanged (no new debt markers).
Same commit also syncs the sh5c unit gate's kernel C-file count
(127 -> 135): a local-only latent (CI's unit job builds no host tcc,
so the gate skips there), moved with the run's fix because any local
test-unit with the selfhost deps present was red; the assertion stays
exact (every C source on the kernel link line must compile).

RESIDUE2 CI fix (third wave, 2026-09-07, run 92442558788): five red
jobs, five roots -- three fresh, two of them queued fixes the second
wave's patch failed to carry (audited this wave by diffing the
worktree against fefccf6 file-by-file; everything found is in this
commit). aulink: symbol-relative merge element offsets (negative
addends against local anchors) -- reproduced on Ubuntu noble clang
18.1.3 exactly, regression-tested in test_aulink.sh. feeder v2:
EPIPE abort + il_send_wait content gates (test_stopped, already
delivered, called the missing function and passed vacuously in run
3's core shard). shell_all 180 s. gui: the brightness gate is
LFB-conditioned (BIOS text lane is honestly skipped -- fb.c's own
comment; the run-2 "capture race" theory was wrong, reproduced
black-on-every-capture with vncdotool installed locally). posix2024
on the prompt driver. usb_hub RES-01 refinement. SH5d/SH8 counts
127 -> 135 (the sh5c drift class). kernel_guest sends sync before
the host-side mcopy (buffer-cache flush; 26/26). sync(2) userland
(wrapper + shell command; kernel side shipped in wave 2). xhci
bulk data-phase timeout 1 s -> 10 s (vCPU starvation vs virtual
clock skew on overcommitted runners). No ledger rows opened or
closed; baseline unchanged.

RESIDUE2 CI fix (fourth wave, 2026-09-07, run 92482275773): one red
job (posix2024_conf, one conformtest subcheck -- the pselect-mask
flake, previously observed ~2 of 5 local runs). Root: a KERNEL race,
not test timing -- every child exit posts SIGCHLD, a default-ignore
signal is dropped at the next delivery boundary, but the
interruptible sleeps' EINTR gates tested raw sig_pending&~sig_mask,
and signal_deliver_iret clears one signal per boundary (the SIGKILLed
sender's last in-flight SIGUSR2 occupies the boundary slot ahead of
its own SIGCHLD; the microsecond gap to the next pselect has no irq
boundary to drop it; the 300 ms timeout wake then returns EINTR from
a signal nobody would ever feel). Fix: signal_actionable() /
signal_actionable_pending() predicates; signal_send wakes only for
actionable signals; the five EINTR gates (select.c, time.c, sysvipc.c
x3) use the predicate; test_select_stack.c covers both gate sides.
posix2024_conf 12/12 consecutive green at 95/95. No ledger rows opened
or closed; baseline unchanged.


OTA_PLAN O0 (2026-09-07): the OTA feature plan lands as
`docs/plans/OTA_PLAN.md` with 6 marker lines (its named-parking section 2
plus the close-out checklist line); the baseline gains the `OTA_PLAN.md 6`
row in the same commit. No new debt rows: the plan defers its ledger
coverage rows to its O5 close-out, exactly like FSFULL deferred to F7.

OTA_PLAN O1 (2026-09-07): boot-volume correctness landed — partition-aware
FAT32 mount (GPT, then MBR, then the unchanged raw-LBA-64 scratch leg),
plus the same partition-table respect in diskfs and the AHCI self-test
(non-destructive DMA verify on partitioned disks). No new debt rows and no
marker drift: `OTA_PLAN.md` stays at 6, the new `test_ota_bootvol.sh`
carries none, and the existing suites (kernel_guest, fat32_full, ahci
matrix incl. its q35 lane) stay green unmodified.

OTA_PLAN O2 (2026-09-08): reboot and identity landed — SYS_REBOOT (612,
flush + 8042 pulse, loud-halt fallback), the `reboot` shell command, and
AURALITE_VERSION as one build knob across kernel and userspace (five
identity prints unified: boot receipt, shell banner, uname, /proc/version,
gfx banner). No new debt rows and no marker drift: `OTA_PLAN.md` stays at
6, the new `test_ota_reboot.sh` carries none, and the touched suites
(syscalls, shell_commands, boot_to_shell, selfhost_kernel_guest,
ota_bootvol) stay green unmodified — the guest tcc build rides the
#ifndef fallback identity.

OTA_PLAN O3 (2026-09-08): the stage2 KERNEL.OLD fallback landed — one
bounded retry on any KERNEL.ELF load failure (missing/unreadable/bad
ELF), receipts on both sides, happy path unchanged, 32-bit leg
untouched, 6656/64512 B inside the 126-sector budget. The pass also
fixed the O2-shipped parity break (procfs.c pulling kernel.h's
ARCH_X86_64 #error into the rv64/a64/i386 syntax lanes; the version
macro moved to the arch-free kernel/version.h, parity back to 25/25).
No new debt rows and no marker drift: OTA_PLAN.md stays at 6, the new
test_ota_fallback.sh carries none.

OTA_PLAN O4 (2026-09-08): the ota tool landed — check/apply/rollback/status
with a streamed 4 KiB-chunk payload download into /fat/KERNEL.NEW, on-the-fly
sha256 via libatls, rename-based A/B swap and rollback, plus the plan's
whole-flow exit gate (three boots in one log: 0.0.1 -> applied 0.0.2-ota ->
rolled-back 0.0.1). The flow also found and fixed a real kernel TCP defect:
timer-derived ephemeral ports reused a port within a tick window and the
third connect of one boot met SLIRP's FIN-ACK; ports now come from a
monotonic wheel. No new debt rows and no marker drift: OTA_PLAN.md stays
at 6 (two Result phrasings reworded to stay inside the pin), and the new
test_ota_apply.sh / test_ota_manifest.c carry no markers.

OTA_PLAN O5 (2026-09-08, close-out): the OTA series is COMPLETE (O0-O5).
The four harnesses run as their own `ota` CI shard; tools/check_ota_claims.py
pins every done phase to its artefacts and greppable receipts (negative
control included) in the workflow's claim-check step and make test-unit;
docs/status.md, README.md and TODO.md carry the feature rows. The plan's
parked items, restated with their section-2 reasons:
- Streaming TLS payload downloads — needs an incremental ahttp API; the
  manifest sha256 already buys integrity and the manifest itself can be
  HTTPS (today https:// payload URLs are refused with a clear message).
- Signed manifests — atls carries ECDSA/RSA; pinned-key verification is
  clean work once the flow exists, but it adds key management this plan
  did not need to be honest about.
- initrd / userspace image updates — the manifest format gains an
  artifacts= list later; kernel-first is where the risk and payoff are.
- Boot-success watchdog (stage2 consulting a boot-attempt counter for
  auto-rollback of a kernel that faults mid-boot) — stage2 can read
  files, so it is possible; the A/B fallback plus `ota rollback` covers
  the corrupt/unloadable class first.
- Delta updates, and a real statvfs(3) (today's is a hardcoded stub;
  ota guards with a size sanity bound + write-time ENOSPC abort).
No new debt rows and no marker drift: OTA_PLAN.md stays at 6 and
TODO.md stays at 7 — the parked items above are restated in TODO.md as
bullets, not open boxes, because this file's open boxes are owned by
RESIDUE2 phases (pinned by tools/check_residue2_claims.py).

RESIDUE2 CI fix (post-OTA wave, 2026-09-08, run 92815985778): the O5
run confirms the OTA series (15/16 green, `ota` shard ALL PASSED,
check_ota_claims OK); the one red is a pre-existing xHCI flake, not an
OTA regression (e927a76..5a6e4df touches no driver under
drivers/usb/). Root: xhci_wait_transfer_cc matched Transfer Events by
TYPE only, so a leftover enumeration completion satisfied the first
blocking bulk wait instantly and every later event shifted by one
transfer -- the CSW read memcpy'd a zero bounce buffer ("bad CSW
signature 0x0" x3 after "PASS: ready", then "FAIL: READ(10) sector
0"; ~1 boot in 8 locally, once in CI). Fix: static
xhci_wait_transfer_event() matching (slot, ep) the way the interrupt
path always has -- stash take-what-is-ours, ring drain with parking,
stash re-scan per iteration. No assertion weakened, no test modified,
no suite touched; 15/15 test_usb_xhci (was 7/8), the whole usb shard
(21 cases) ALL PASSED, make iso EXIT 0, -Werror clean (zero
compiler warnings; the ld.lld .bss-alignment linker notes are
pre-existing -- CI 92815985778 on the BASE commit prints them too). No ledger rows
opened or closed; baseline unchanged (OTA_PLAN.md 6, TODO.md 7).

LX_COMPAT L0-L4 (2026-09-09 .. 2026-09-10, upstream as the L0..L4
updates): the plan lands measured against fb39fe2 (L0, baseline gains
the LX_COMPAT_PLAN.md row), the personality + translation table + lxrun
ship (L1), the stat/getdents64 marshals + upstream busybox (L2), the
signal-frame + shell rung (L3), and the dynamic loader rung (L4).  Each
phase added its own CI case in the lx shard; no ledger rows opened or
closed, and coverage registers at L5 per the plan.

LX_COMPAT L5 (2026-09-11, close-out): the Linux-application series is
COMPLETE (L0-L5).  The flagship rung ships an UNMODIFIED stock lua 5.4
interpreter (lua.org, SHA-256 pinned, upstream `make linux`, dynamic PIE
glibc + libm) running a real five-section script, gated as
test_lx_lua.sh (5/5: `LX5-LUA-OK`, exit 0).  The first in-guest run
named one missing arm -- glibc time() issues Linux nr 201, unmapped --
fixed by the measured alias row 201 -> 520 (native SYS_TIME is
Linux-exact).  tools/check_lx_claims.py pins all six phases to their
artefacts and greppable receipts (negative control included) in
make test-unit and the workflow's claim-check step; the workflow's
presence assert now also requires ./linux/tests/lua and
./linux/tests/lua_script.lua in the initrd (the w32hello.exe
precedent).  docs/status.md, README.md and TODO.md carry the feature
rows.  The plan's parked items, restated with their section-2 reasons:
- The full Linux syscall surface -- the ladder names each rung's calls;
  unmapped numbers fail loudly (-ENOSYS), so a new application names
  its next gap on first run.
- Full siginfo payloads, FPU save/restore across lx signals, vfork-class
  clone -- each measured out of scope for the terminal-program ladder
  and documented at the boundary in kernel/lx/.
- faccessat2 / pselect6 / ppoll / the socket-family numbers -- no
  ladder rung has asked for them yet; they stay deliberately unmapped
  until one does.
No new debt rows: the ledger's own table (RES-01..RES-54) is untouched,
the parked items above are restated in TODO.md as bullets, not open
boxes, because this file's open boxes are owned by RESIDUE2 phases
(pinned by tools/check_residue2_claims.py).  Baseline moved same-commit
(LX_COMPAT_PLAN.md 5 -> 6).

REALTEK RT3 (2026-09-11, close-out): the Realtek NIC series is COMPLETE
(RT0-RT3).  +2 rows: RES-55 (W, coverage) → DONE@RT3 and RES-56 (M, real
silicon) ships as PENDING-USER@RT3 — the 8169's data path is proved against
a register-level host model because QEMU has no 8169, and only the user's
machine can prove the chip.  tools/check_realtek_claims.py pins every ✅
phase to its artefacts and greppable receipts (negative control included)
in make test-unit and the workflow's claim-check step.  Baseline moved
same-commit (status-wip 7 → 6; the RTL8169 status row flips 🚧 → ✅).

W32RUN WR-5 (2026-10-23, live-matrix close-out): +1 marker line in
`docs/plans/W32RUN_PLAN.md` (WR-4's "orphan-on-close lifecycle
follow-up", the PuTTY session-window teardown gap); baseline moved
W32RUN_PLAN.md 6 → 7 same-commit.  No new debt rows: the gap is a
named WR-4 PARTIAL gap owned by the plan (the live matrix in
docs/win32_run.md carries it), not a ledger-class residue — the
table's 59 rows and class totals are untouched.

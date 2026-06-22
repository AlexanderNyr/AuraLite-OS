# AuraLite OS — изучение проекта

Дата изучения: 2026-06-22  
Репозиторий: https://github.com/AlexanderNyr/AuraLite-OS.git  
Локальный путь: `/home/user/AuraLite-OS`  
Ветка/commit: `main`, `502f180f92b2b5fd735c18f9bdf156b398d80149` (`Boot update`)

## 1. Краткий вывод

AuraLite OS — учебно-исследовательская x86_64 ОС с собственным ядром, загрузкой через Limine, Ring 3 userspace, initrd/VFS, сетевым стеком, GUI/оконным менеджером, USB/AHCI/Bluetooth/Wi‑Fi протокольными слоями и набором пользовательских программ.

Проект существенно шире, чем базовое описание в README: README/PLAN фиксируют «14 фаз», а текущий исходный код уже содержит расширения после них: DHCP/DNS/TCP, сетевые syscall’ы, `spawn/fork/execve/wait`, полный GUI/window manager, 3D renderer, AHCI, USB UHCI/OHCI/EHCI/xHCI, MSC-протокол, Bluetooth HCI и Wi‑Fi MAC layer.

## 2. Размер и структура

Без `.git` и `build`: 176 файлов, примерно 6.9 MiB.

Основные каталоги:

- `kernel/` — ядро, arch-код, MM, FS, PROC, NET, libc-like utils.
- `drivers/` — UART, framebuffer/GUI, keyboard/mouse, PIT, PCI, e1000, AHCI, USB, Bluetooth, Wi‑Fi.
- `libc/` — минимальная userspace libc, crt0, syscall wrapper, linker script.
- `userspace/` — init shell и приложения.
- `tests/unit/` — host-side unit tests.
- `tools/` — сборка ISO/initrd, QEMU launch/debug, генерация embedded binary.
- `docs/` — архитектура, memory map, syscall ABI, driver guide.
- `limine/` — vendored Limine binaries/tool/header.

Примерная разбивка по исходникам:

- `kernel/arch/x86_64`: 22 файла / ~1716 строк
- `kernel/mm`: 6 файлов / ~848 строк
- `kernel/proc`: 13 файлов / ~1217 строк
- `kernel/net`: 4 файла / ~1561 строк
- `drivers/usb`: 12 файлов / ~2940 строк
- `drivers/framebuffer`: 16 файлов / ~1977 строк
- `tests/unit`: 10 файлов / ~2375 строк

## 3. Загрузка и порядок инициализации

Фактический entry path:

1. Limine читает `/boot/limine/limine.conf`.
2. Загружает `/boot/kernel.elf` и модуль `/boot/initrd.tar`.
3. `_start` в `kernel/arch/x86_64/boot.asm` выставляет стек, чистит BSS и вызывает `kmain()`.
4. `kernel/kernel.c` инициализирует подсистемы в таком порядке:
   - UART
   - framebuffer console
   - GDT
   - IDT
   - PIC
   - TSS
   - SYSCALL/SYSRET
   - PMM
   - paging/VMM
   - kernel heap
   - SMP
   - PIT timer
   - scheduler
   - VFS + initrd + devfs
   - network stack + DNS/TCP self-tests
   - AHCI
   - USB UHCI/OHCI/EHCI/xHCI + USB core + MSC
   - Bluetooth
   - Wi‑Fi
   - graphics/keyboard/mouse/window manager
   - 3D demo
   - process address-space self-test
   - Ring 3 init shell

## 4. Kernel architecture

### 4.1 x86_64 platform layer

В `kernel/arch/x86_64/` реализованы:

- GDT с kernel/user code/data и TSS descriptor.
- TSS с RSP0 и IST1.
- IDT на 256 gates.
- ISR stubs в NASM.
- PIC remap IRQ 0–15 → vectors 32–47.
- Paging helpers, CR/MSR access, port I/O.
- SYSCALL/SYSRET setup.
- SMP bring-up через Limine MP request.

Важная деталь ABI: user data selector находится перед user code selector, чтобы SYSRET с `STAR[63:48]=0x10` дал `SS=0x1B`, `CS=0x23`.

### 4.2 Memory management

- PMM (`kernel/mm/pmm.c`): bitmap по 4 KiB frames, строится из Limine memmap, bitmap размещается преимущественно в bootloader-reclaimable памяти.
- VMM (`kernel/arch/x86_64/paging.c`): использует Limine page tables, расширяет их через HHDM, поддерживает map/unmap/get_phys, NX, новые PML4, clone user-space pages.
- Kernel heap (`kernel/mm/heap.c`, `kheap.c`): first-fit allocator, boundary tags, coalescing, `kmalloc/kfree/krealloc`, heap region `0xFFFFFFFF88000000`, on-demand mapping.

### 4.3 Processes/scheduler

- Preemptive round-robin scheduler, quantum 5 ticks.
- Kernel threads с 16 KiB kernel stack.
- Context switch сохраняет callee-saved registers + RFLAGS.
- TCB одновременно используется как PCB.
- Есть per-process PML4 и переключение CR3 при scheduling user-process thread.
- Реализованы `fork`, `execve`, `wait4`, `spawn`, но с упрощениями.

### 4.4 Userspace/Ring 3/syscalls

Минимальная libc предоставляет:

- `read/write/open/close/getpid/_exit`
- `fork/execve/wait/spawn`
- `listdir`, `dns_resolve`
- network wrappers: `net_connect/send/recv/close/ping`
- `printf`, string/memory functions, `atoi/strtol/rand/srand`

Реальные syscall numbers в коде:

- 0 `read`
- 1 `write`
- 2 `open`
- 3 `close`
- 39 `getpid`
- 57 `fork`
- 59 `execve`
- 60 `exit`
- 61 `wait4`
- 80 `listdir`
- 81 `spawn`
- 82 `dns`
- 83 `net_connect`
- 84 `net_send`
- 85 `net_recv`
- 86 `net_close`
- 87 `net_ping`

## 5. Filesystem

- VFS с mount table и longest-prefix matching.
- Initrd — USTAR tar parser, readonly files at `/`.
- DevFS — `/dev/null`, `/dev/zero`.
- FD table сейчас глобальная, не per-process.

## 6. Networking

Сеть завязана на QEMU e1000:

- PCI scan bus 0.
- e1000 legacy TX/RX descriptors, DMA buffers из PMM, MMIO explicitly mapped.
- Ethernet, ARP, IPv4, ICMP ping.
- DHCP DORA для IP/gateway/subnet/DNS.
- UDP для DNS.
- DNS resolver через QEMU DNS proxy `10.0.2.3`.
- Minimal TCP client: single connection, active open, send/recv, FIN close, no retransmission/sliding window.
- Userspace HTTP client/browser используют DNS + TCP syscalls.

Ограничения: polling I/O, single TCP connection, нет socket API, нет полноценной TCP state machine.

## 7. Graphics/UI

- Framebuffer console использует Limine framebuffer.
- 2D graphics layer double-buffered через `kmalloc` back buffer.
- PSF/font support, bitmap font rendering.
- PS/2 keyboard IRQ 1, ring buffer.
- PS/2 mouse IRQ 12.
- Window manager:
  - desktop gradient,
  - windows with title bars/borders/shadows/close buttons,
  - widgets: buttons, labels, progress bars, rectangles,
  - mouse drag/focus/close,
  - taskbar with uptime clock.
- 3D software renderer:
  - vec3/mat4 math,
  - wireframe meshes,
  - filled flat-shaded triangles,
  - painter’s algorithm,
  - cube/pyramid demo.

## 8. Drivers

Implemented or partially implemented:

- UART 16550 COM1.
- PIT 8254.
- PCI config access.
- e1000 NIC.
- AHCI SATA: controller/port setup, but sector I/O is known broken around PxCI command issue.
- USB:
  - UHCI: controller init, ports, TD/QH transfer primitives.
  - OHCI/EHCI/xHCI: controller detection/init/port enumeration structures.
  - USB core: enumeration framework, descriptors, device table; actual full transfers only UHCI-connected, others WIP.
  - MSC: CBW/CSW/SCSI layer ready, bulk transport WIP.
- Bluetooth HCI protocol over USB, depends on USB transfers/device availability.
- Wi‑Fi 802.11 MAC management layer, no real hardware driver registered by default.

## 9. Userspace programs

Built into initrd:

- `/init` — interactive shell.
- `/hello` — hello world.
- `/calc` — calculator.
- `/sysinfo` — OS/features info.
- `/editor` — line editor.
- `/clock` — clock/uptime demo.
- `/guess` — number guessing game.
- `/snake` — terminal snake.
- `/http` — HTTP client.
- `/browser` — text browser with simple HTML stripping/rendering.

Shell commands include: `ls`, `cat`, `echo`, `run`, `pwd`, `uname`, `free`, `nslookup`, `ping`, `ps` stub, `help`, `exit`.

## 10. Build/test status in current sandbox

### Unit tests

`make test-unit` completed successfully.

All 10 unit targets passed:

- PMM bitmap
- heap allocator
- string/memory
- bitmap primitives
- network utilities
- kprintf formatting
- libc pieces
- 3D math
- USB protocol structures
- window manager logic

### ISO build

`make iso` currently cannot be completed in this sandbox because `clang` is not installed:

```text
make: clang: No such file or directory
make: *** [Makefile:52: build/kernel/arch/x86_64/gdt.o] Error 127
```

This is an environment/toolchain issue, not necessarily a source-code failure. README expects Clang/LLD/NASM/QEMU/xorriso.

## 11. Important inconsistencies and risks found

1. **Документация частично устарела.** README/PLAN говорят о 14 фазах и версии `v0.1.0`; код уже `AURALITE_VERSION "1.0.0"` и содержит post-phase расширения.
2. **`make run` может требовать `build/disk.img`.** `tools/run_qemu.sh` подключает AHCI disk image, но обычный `make iso` его не создаёт. CI script создаёт disk image сам.
3. **AHCI sector read/write known broken.** Самотест отключён; TODO описывает triple fault при PxCI write.
4. **USB stack неоднороден.** UHCI transfer layer есть, но USB core прямо помечает OHCI/EHCI/xHCI transfer dispatch как not connected/WIP. MSC bulk transport тоже stub.
5. **Wi‑Fi/Bluetooth — больше протокольные слои, чем полноценные рабочие драйверы.** Wi‑Fi требует registered chipset driver; BT зависит от USB enumeration/transfer.
6. **FD table глобальная.** Нет per-process FD table.
7. **Нет thread reaping.** Dead TCBs/stacks не освобождаются.
8. **AP CPUs idle.** SMP bring-up есть, но scheduler не SMP-safe и AP не участвуют в scheduling.
9. **Syscalls не валидируют user pointers.** Пользовательские указатели напрямую разыменовываются в kernel context.
10. **ELF segments мапятся writable/user без применения p_flags.** RO/NX protection для userspace сегментов помечена TODO.
11. **`wait4` сильно упрощён.** Не ждёт конкретного child PID, не возвращает child PID, exit code фактически не устанавливается из `exit(code)`.
12. **TCP single-connection model.** Нет retransmit timer, windows, listen/accept, sockets.
13. **PCI scan только bus 0.** Для сложной топологии не хватит.
14. **Есть мелкие комментарные/документальные ошибки.** Например в syscall_entry comment у `o64 sysret` указано `CS = ... = 0x1B`, хотя CS должен быть `0x23`, SS — `0x1B`.

## 12. Что можно делать дальше

Наиболее полезные следующие шаги:

1. Установить toolchain и проверить полный `make iso` + QEMU boot.
2. Синхронизировать README/docs с текущей версией кода.
3. Починить `make run`, чтобы он создавал `build/disk.img` или не требовал его без AHCI.
4. Довести USB core transfers для OHCI/EHCI/xHCI или честно обозначить статус в README.
5. Разобрать AHCI PxCI triple fault через `make debug` + GDB.
6. Добавить user pointer validation для syscalls.
7. Сделать per-process FD table и thread reaper.
8. Привести wait/fork/exec semantics ближе к POSIX.
9. Разделить network stack на более чистые слои и убрать single global TCP connection.
10. Настроить CI/GitHub Actions с unit tests + QEMU integration.

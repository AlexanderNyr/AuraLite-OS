# Build and Run Guide

This guide covers the supported build outputs and emulator/hypervisor setups.

## Required tools

Debian/Ubuntu:

```bash
sudo apt update
sudo apt install clang lld nasm qemu-system-x86 mtools git make gcc python3

# Rust is REQUIRED, not optional: `make deps-check` lists rustc in
# REQUIRED_TOOLS and the build stops without it (lib/rsbr is a no_std Rust
# static library linked into the `rustes` userspace program).  The kernel
# ports each need their own bare-metal target.
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
rustup target add x86_64-unknown-none riscv64gc-unknown-none-elf aarch64-unknown-none

# Optional: `llvm-objcopy` (from the `llvm` package) packages the aarch64
# ELF kernel into the raw Image protocol (`make kernela64-img` /
# `run-a64-img` — the path that carries `-initrd`).  The target soft-skips
# without it.
sudo apt install llvm

# Optional, but needed for the complete integration suite:
sudo apt install e2fsprogs vncdotool python3-pil ovmf socat dosfstools

# Optional: the fsfull shard's external-interop lanes — exFAT needs
# exfatprogs (mkfs.exfat/fsck.exfat), NTFS needs ntfs-3g
# (mkntfs/ntfscp/ntfsls).  Those cases soft-skip without them.
sudo apt install exfatprogs ntfs-3g

# Optional: the Win32 personality's examples and its end-to-end gate.
sudo apt install mingw-w64
```

A plain `git clone` is enough: there are no submodules and no third-party
bootloader to fetch. `make iso` builds the whole boot chain — MBR, Stage 2 and
`BOOTX64.EFI` — from the sources in `boot/`.

Tool purposes:

| Tool | Used for |
|---|---|
| `clang` | Kernel and userspace C compilation. |
| `ld.lld` | Static ELF linking with custom linker scripts. |
| `nasm` | x86_64 assembly files. |
| `xorriso` | **Not used by any current target.** No Makefile rule or tool script invokes it; CI may still install it harmlessly. |
| `qemu-system-x86_64` | Local booting and integration testing. |
| `mtools` (`mformat`, `mcopy`) | **Required** (`make deps-check` enforces it): the legacy `mkisoimage_bios.sh`/`mkisoimage_dual.sh` paths (`make iso-bios`), the DOOM case's `fatpart`, and any script-built image. The **default** `make iso` no longer uses mtools — since SH7d the dual image is assembled by the in-tree C tool `tools/selfhost/mkiso.c`, which writes the FAT ESP directly. |
| `lld-link` | **Required.** Links `BOOTX64.EFI` as PE32+ (`make efi`). Ships with the `lld` package. |
| `rustc` + bare-metal targets | **Required.** `x86_64-unknown-none` builds `lib/rsbr` for the `rustes` userspace program; `riscv64gc-unknown-none-elf` and `aarch64-unknown-none` build the rsbr twin for the rv64/a64 kernels. |
| `llvm-objcopy` | Optional: `make kernela64-img` (raw aarch64 Image; soft-skips without it). |
| `git`, `make`, `gcc` | Source checkout and host helper builds. |
| `e2fsprogs` | Optional: `mkfs.ext2`/`debugfs` for ext2 tests; `mkfs.ext4`/`fsck.ext4` for the fsfull ext4 lane. |
| `exfatprogs`, `ntfs-3g` | Optional: the fsfull shard's external-interop lanes (`mkfs.exfat`/`fsck.exfat`; `mkntfs`/`ntfscp`/`ntfsls`). |
| `vncdotool` + Pillow | Optional: GUI/VNC screenshot assertions. |
| `mingw-w64` | Optional: builds `w32/examples/` and enables `test_w32_integration`. Not in `REQUIRED_TOOLS` — `make iso` skips the examples without it. |

**A note on `mingw-w64`.** Without it the build still succeeds and
`test_w32_integration` *skips*, printing why. That is deliberate — the OS
must not need a Windows cross-compiler to build — but it does mean a
contributor can see a green run in which the end-to-end Win32 gate tested
nothing. CI installs it and asserts the examples reached the initrd, so the
gate cannot go quiet there. If you are changing anything under `w32/`,
install it locally too.

## Build ISO

```bash
make deps-check
make iso
```

Main output:

```text
build/auralite.iso
```

This is a raw hybrid GPT + MBR disk image with AuraLite's custom BIOS and UEFI
loaders. The `.iso` suffix is retained for compatibility with existing tooling;
attach the image as a hard disk, not as an El Torito CD-ROM. The same bytes are
also available as `build/auralite-dual.iso` and `release/auralite.iso`.

## Build user programs only

```bash
make user
```

Outputs are under:

```text
build/user/*.elf
```

The initrd is built from these ELFs and mounted at `/` during boot.

## Build kernel only

```bash
make kernel
```

Output:

```text
build/kernel.elf
```

## Run in QEMU

```bash
make run
```

This uses `tools/run_qemu.sh`. The script creates AHCI test disks as needed and
attaches them through a QEMU AHCI controller. The kernel AHCI self-test reads
sector 0 and verifies write/readback on scratch sector 1. If AHCI is present,
AuraLite mounts `/disk` and a FAT32 volume at `/fat`; with a second AHCI disk it
also mounts ext2 at `/ext2`. Kernel logs are appended to `/fat/AURALOG.TXT`.

If you want a simpler command without the AHCI test disk:

```bash
qemu-system-x86_64 \
  -drive file=build/auralite.iso,format=raw,if=ide,snapshot=on \
  -boot order=c \
  -m 512M \
  -smp 4 \
  -vga std \
  -display none \
  -serial stdio \
  -no-reboot \
  -cpu qemu64 \
  -netdev user,id=net0 \
  -device e1000,netdev=net0
```

## Run in QEMU on Windows 10 (run.bat)

When running native QEMU for Windows 10 (`qemu-system-x86_64.exe`), place `auralite.iso` in a dedicated directory and create `run.bat` with the following robust script. It automatically verifies QEMU's installation path, creates necessary AHCI/ext2 virtual disks (`disk.img`, `ext2.img`) if missing, and launches QEMU in a single reliable command line:

```batch
@echo off
echo ========================================================
echo       Checking files and launching AuraLite OS...
echo ========================================================

:: 1. Check QEMU installation
echo [Step 1] Checking for QEMU installation...
SET QEMU_PATH="C:\Program Files\qemu\qemu-system-x86_64.exe"
IF NOT EXIST %QEMU_PATH% GOTO NO_QEMU
echo          - QEMU found at %QEMU_PATH%

:: 2. Check ISO image
echo [Step 2] Checking for auralite.iso...
IF NOT EXIST "auralite.iso" GOTO NO_ISO
echo          - auralite.iso found.

:: 3. Check and create disk.img
echo [Step 3] Checking for disk.img...
IF EXIST "disk.img" GOTO DISK1_OK
echo          - Creating disk.img (16MB)...
fsutil file createnew disk.img 16777216 > nul
:DISK1_OK
echo          - disk.img ready.

:: 4. Check and create ext2.img
echo [Step 4] Checking for ext2.img...
IF EXIST "ext2.img" GOTO DISK2_OK
echo          - Creating ext2.img (8MB)...
fsutil file createnew ext2.img 8388608 > nul
:DISK2_OK
echo          - ext2.img ready.

echo ========================================================
echo [Step 5] All files ready! Starting QEMU...
echo ========================================================

%QEMU_PATH% -cdrom auralite.iso -m 512M -smp 4 -vga std -serial stdio -no-reboot -no-shutdown -cpu qemu64 -netdev user,id=net0 -device e1000,netdev=net0 -device piix3-usb-uhci,id=uhci -device usb-kbd,bus=uhci.0,port=1 -device usb-mouse,bus=uhci.0,port=2 -drive file=disk.img,format=raw,if=none,id=ahcidisk -device ahci,id=ahci0 -device ide-hd,drive=ahcidisk,bus=ahci0.0 -drive file=ext2.img,format=raw,if=none,id=ext2disk -device ide-hd,drive=ext2disk,bus=ahci0.1

echo.
echo QEMU finished.
pause
exit /b

:NO_QEMU
echo.
echo [ERROR] QEMU emulator NOT FOUND at: %QEMU_PATH%
echo         Please install QEMU from https://qemu.weilnetz.de/w64/
echo         (Or edit run.bat if QEMU is installed in a different folder).
echo.
pause
exit /b

:NO_ISO
echo.
echo [ERROR] File 'auralite.iso' NOT FOUND in the current folder!
echo         Please put 'auralite.iso' into the same folder as run.bat.
echo.
pause
exit /b
```

### Windows 10 GUI Anti-Freeze Architecture
The underlying kernel incorporates a dedicated anti-freeze mechanism for Windows 10 hypervisors. The `gui_compositor_thread` guarantees 100 FPS updates via cooperative scheduling (`sched_yield`). This completely prevents Windows/QEMU display throttling and UI freezing.

## Run QEMU with USB Mass Storage

```bash
make run-usb-msc
```

This creates `build/usb-stick.img` if needed and attaches it as a UHCI
`usb-storage` device. During boot the USB core should enumerate the device, the
MSC layer should run SCSI `INQUIRY` / `READ CAPACITY`, and the MSC self-test
should read sector 0.

## Debug with GDB

```bash
make debug
```

Then attach from another terminal:

```bash
gdb build/kernel.elf -ex 'target remote :1234'
```

## Tests

### Unit tests

```bash
make test-unit
```

The host-side unit tests cover:

- PMM bitmap allocation helpers.
- Generic heap allocator.
- String/memory functions.
- Bitmap primitives.
- Network utility functions.
- Formatting helpers.
- libc helpers.
- 3D math helpers.
- USB protocol structures.
- Window-manager hit-testing/layout helpers.

### QEMU integration tests

```bash
make test-integration-fast   # faster subset, skips slow cases
make test-integration        # full black-box suite
make test                    # unit tests + full integration suite
```

The integration harness boots the real ISO in QEMU and asserts on serial output
and, for the GUI case, VNC screenshots. The full suite covers boot-to-shell,
shell commands, syscalls, process spawning/address spaces, AHCI `/disk` +
`/fat`, FAT32 persistence, FAT32 subdirs/LFN, ext2 cross-OS round-trips, USB
Mass Storage through UHCI, e1000 networking, HTTP path exercise, graphics, SMP
and GUI.

Logs are written to:

```text
build/integration-logs/
```

Optional packages for all assertions:

- `e2fsprogs` — ext2 tests use `mkfs.ext2` and `debugfs`.
- `vncdotool` — GUI test captures VNC screenshots; without it, the test keeps
  serial-level checks and soft-skips visual assertions.

## USB image

```bash
make usb
```

Output:

```text
build/usb.img
```

The ISO is already a raw hybrid GPT+MBR disk image, so this target simply
copies it to a `.img` name. It can be booted as a disk image or written to a USB stick:

```bash
sudo dd if=build/usb.img of=/dev/sdX bs=4M status=progress
```

Replace `/dev/sdX` carefully.

## VirtualBox

```bash
make vbox
```

If `VBoxManage` is installed, this creates or updates a VM named `AuraLite-OS`.
If not, it writes manual setup notes to:

```text
vm/virtualbox/README-VirtualBox.txt
```

Use the bundled ISO:

```text
vm/virtualbox/auralite.iso
```

Required network adapter for current networking support:

```text
Intel PRO/1000 MT Desktop (82540EM)
```

## VMware

```bash
make vmware
```

Open the generated VMX:

```text
vm/vmware/AuraLite-OS.vmwarevm/AuraLite-OS.vmx
```

The VM directory contains its own ISO copy:

```text
vm/vmware/AuraLite-OS.vmwarevm/auralite.iso
```

Use legacy VMware `e1000` networking. Do not switch to `vmxnet3` or `e1000e`
unless matching AuraLite drivers are added.

## Common build problems

### `clang: No such file or directory`

Install the compiler:

```bash
sudo apt install clang
```

### `ld.lld: No such file or directory`

Install LLD:

```bash
sudo apt install lld
```

### `nasm: No such file or directory`

```bash
sudo apt install nasm
```

### `mformat: command not found` or `mcopy: command not found`

Install mtools. `make deps-check` refuses to proceed without it, and the
legacy script paths (`make iso-bios`) and the DOOM case's `fatpart` still
call it — though the default `make iso` has been mtools-free since SH7d (the
in-tree `tools/selfhost/mkiso.c` writes the ESP).

```bash
sudo apt install mtools
```

### `[deps] missing required tool: rustc`

Rust is a required dependency, not an optional one. Install it and add the
bare-metal targets:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
rustup target add x86_64-unknown-none riscv64gc-unknown-none-elf aarch64-unknown-none
```

### `lld-link: command not found`

`BOOTX64.EFI` is a PE32+ binary, so the UEFI half of the image needs the
PE-mode driver of LLD. It ships in the same package as `ld.lld`:

```bash
sudo apt install lld
```

### `mkfs.ext2` or `debugfs` missing during integration tests

Install e2fsprogs:

```bash
sudo apt install e2fsprogs
```

### GUI integration test skips visual assertions

Install VNC tooling:

```bash
sudo apt install vncdotool
```

### `psf_font.inc file not found`

The current source tree expects:

```text
drivers/framebuffer/psf_font.inc
```

If it is missing, regenerate or restore it before building. It contains the
embedded PSF-style console font data used by `psf_font.h`.

### VirtualBox boots but networking fails

Check the adapter type. Use:

```text
Intel PRO/1000 MT Desktop (82540EM)
```

Do not use PCnet or virtio-net.

### VMware boots but networking fails

Check that the VMX contains:

```text
ethernet0.virtualDev = "e1000"
```

Do not use `vmxnet3` or `e1000e`.

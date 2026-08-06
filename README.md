# AuraLite OS

AuraLite OS is a from-scratch **x86_64 hobby operating system** with its own
custom BIOS and UEFI bootloader written in NASM assembly + freestanding C.  It
includes a higher-half kernel, preemptive multitasking, Ring 3 ELF user
programs, a small libc, an initrd-backed VFS, writable tmpfs/FAT32/ext2 storage,
e1000 networking, AHCI, USB Mass Storage through UHCI, framebuffer graphics, a
kernel GUI/window compositor, a small user-space GUI toolkit, and several
experimental device/protocol layers.

The project is intentionally incremental and educational: most subsystems have
small self-tests, host-side unit tests, and documentation explaining the design
trade-offs.

---

## Current status

The original 14-phase roadmap is complete, and the repository now contains
additional post-phase extensions.

> **Reality check:** AuraLite is still a hobby/educational OS. QEMU is the
> primary supported target; real-hardware and many non-QEMU virtual-device paths
> are experimental unless explicitly listed as stable below.

### Stable / exercised in normal builds

- Custom BIOS + UEFI dual-boot ISO (`make iso` / `make iso-dual`), plus
  optional Limine fallback (`make iso-limine`).  See `docs/BL{1..8}_REPORT.md`
  for the design of every boot phase.
- x86_64 long mode, higher-half kernel.
- GDT, IDT, PIC IRQ dispatch, TSS, SYSCALL/SYSRET.
- Physical memory manager, virtual memory manager, kernel heap.
- Preemptive round-robin scheduler and kernel threads.
- Ring 3 ELF loading and minimal libc.
- Initrd-backed VFS plus `/dev/null`, `/dev/zero`, writable `/tmp`, `/disk`,
  full FAT32 at `/fat`, and ext2 at `/ext2` when a second AHCI disk is present.
- AHCI SATA sector read/write on QEMU-style AHCI disks.
- e1000 networking with ARP, IPv4, ICMP, DHCP/fallback addressing, UDP DNS and
  a minimal single-connection TCP client.
- Framebuffer console, 2D graphics, PS/2 keyboard/mouse, window-manager demo,
  kernel GUI compositor v2.0 (theme engine, desktop icons, notifications, window snapping, start menu, context menus, 100 FPS guaranteed refresh rate), GUI syscalls and bundled GUI applications.
- Host-side unit tests and QEMU integration tests for the main subsystems.

### Experimental / partial

- **Advanced Storage / Filesystems**:
  - `buffer_cache`: Synchronized block cache layer.
  - `ext4`: Experimental ext4-like driver with extent tree parsing (`/ext4`).
  - `btrfs`: Experimental Copy-on-Write B-tree filesystem prototype (`/btrfs`).
  - `f2fs`: Experimental Flash-Friendly File System log-structured prototype (`/f2fs`).
  - `exfat` & `ntfs`: Skeleton/scaffolding drivers (`/exfat`, `/ntfs`).
- Per-process address spaces, `spawn`, `fork`, `execve`, `wait4` are present but
  simplified.
- USB host-controller support is uneven: UHCI has working control/bulk
  transfers and can drive USB Mass Storage; OHCI, EHCI and xHCI currently focus
  on controller/port bring-up and detection.
- AHCI detects/initialises ports and DMA read/write passes the QEMU AHCI test
  disk self-test; broader real-hardware coverage remains experimental.
- USB Mass Storage is ready through UHCI. MSC devices behind OHCI/EHCI/xHCI
  remain future work until those transfer backends are completed.
- Bluetooth HCI and Wi-Fi 802.11 layers are protocol frameworks that require
  working lower-level USB/chipset drivers.
- GUI v2.0 adds a theme engine, desktop icons, notifications, window snapping, start menu, and context menus, but the dirty-rect compositor currently forces full redraws each frame (partial redraw pending integration testing).

See [`docs/status.md`](docs/status.md) for a detailed support matrix.

---

## Quickstart

### Install dependencies

Debian/Ubuntu:

```bash
sudo apt update
sudo apt install clang lld nasm xorriso qemu-system-x86 mtools autoconf automake libtool git make gcc
# Optional but needed for the full integration suite:
sudo apt install e2fsprogs vncdotool python3-pil
```

```bash
git clone https://github.com/AlexanderNyr/AuraLite-OS.git
```

The default `make iso` needs no submodules and no Limine binaries -- it uses
the custom BL2..BL7 bootloader chain shipped in this repository.  Only the
optional `make iso-limine` fallback requires the vendored Limine binary bundle
(or the `third_party/limine` git submodule).

### Build the bootable ISO

```bash
make deps-check
make iso
```

`make iso` chains through `make iso-dual`: it assembles the BL2 MBR, the BL3+BL4
Stage 2, the BL6 UEFI application (`BOOTX64.EFI`), and packages them together
with the kernel into a hybrid GPT + MBR disk image that boots on BOTH firmware
types from the same file.  Output:

```text
build/auralite-dual.iso
build/auralite.iso
release/auralite.iso
```

All three files contain identical bytes. `build/auralite.iso` is the canonical
path consumed by local run and test tooling, while `release/auralite.iso` is
staged for distribution.

### Boot paths

| Path                       | Firmware  | How to invoke it in QEMU                                                                 |
|----------------------------|-----------|------------------------------------------------------------------------------------------|
| Legacy BIOS                | SeaBIOS   | `qemu-system-x86_64 -drive format=raw,file=release/auralite.iso,if=ide`                  |
| UEFI                       | OVMF      | `qemu-system-x86_64 -bios /usr/share/OVMF/OVMF_CODE_4M.fd -drive format=raw,file=release/auralite.iso,if=ide` |
| Real hardware (USB stick)  | either    | `dd if=release/auralite.iso of=/dev/sdX bs=4M`                                           |

### Limine fallback

If a distribution or downstream user needs the historic Limine-based ISO -- for
example to boot on a firmware known to reject our custom loader chain -- run:

```bash
make iso-limine
```

This produces `release/auralite-limine.iso` using `tools/mkisoimage_limine.sh`
and the vendored Limine binaries.  Because BL1 removed every `limine_get_*`
accessor from the kernel, this path uses Limine as a pure chain-loader that
fills a `boot_info_t` shim -- the kernel itself no longer knows about Limine.

### Run in QEMU

```bash
make run
```

Manual equivalent (the `.iso` file is a raw hybrid disk image, so attach it as
an IDE hard disk rather than with QEMU's `-cdrom` option):

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

> Note: `tools/run_qemu.sh` creates/attaches AHCI test disks automatically so
> `/disk`, `/fat` and `/ext2` can be exercised. The simpler manual command above
> boots without those writable/persistent mounts.

### Run in QEMU on Windows 10 (run.bat)

For Windows 10 users running native QEMU without Linux, place `auralite.iso` in a folder and create `run.bat` with the following robust script. It automatically verifies QEMU's installation path, creates necessary AHCI/ext2 virtual disks (`disk.img`, `ext2.img`) if missing, and launches QEMU in a single reliable command line:

```batch
@echo off
title AuraLite OS - QEMU Launcher

rem ------------------------------------------------------------------
rem [0] Switch this console to UTF-8 (code page 65001).
rem
rem     WHY THIS MATTERS: Windows user/folder names containing non-ASCII
rem     characters (Cyrillic, accented Latin, CJK, ...) -- for example
rem     "C:\Users\<CyrillicUserName>\Downloads\..." -- get mangled by cmd.exe's
rem     legacy single-byte OEM code page (866 on Russian Windows, and
rem     similar issues on other non-Latin locales) when such paths are
rem     expanded via %~dp0/%CD% and then handed to commands like COPY
rem     or passed as arguments to a child process such as QEMU. The
rem     visible symptoms are garbled ("mojibake") text in this window,
rem     and -- more seriously -- QEMU/COPY failing to find a file at a
rem     path that genuinely exists, because the two programs ended up
rem     disagreeing on how those non-ASCII bytes should be encoded.
rem     Switching to UTF-8 (chcp 65001) makes this consistent end to
rem     end on Windows 10 1903+ / Windows 11. The original code page is
rem     restored right before every exit point in this script.
rem ------------------------------------------------------------------
for /f "tokens=2 delims=:" %%P in ('chcp') do set "ORIG_CODEPAGE=%%P"
set "ORIG_CODEPAGE=%ORIG_CODEPAGE: =%"
chcp 65001 >nul

echo ============================================================
echo   AuraLite OS - QEMU Launcher for Windows 10
echo ============================================================
echo.

rem ------------------------------------------------------------------
rem [1] Locate qemu-system-x86_64.exe
rem
rem     Checked in order: PATH, then a short list of common install
rem     folders. Every check below is a single, unblocked line on
rem     purpose: paths like "C:\Program Files (x86)\qemu" contain
rem     parentheses, and cmd.exe mis-parses parentheses that appear
rem     inside a multi-line IF/FOR ( ... ) block (it balances them
rem     against the block's own braces before variables/paths are
rem     even expanded). Plain one-line "if exist ... set ..." commands
rem     are not parsed as a block, so this sidesteps that gotcha.
rem
rem     If QEMU lives somewhere else entirely, edit EXTRA_QEMU_DIR.
rem ------------------------------------------------------------------
echo [1/7] Looking for QEMU...

set "EXTRA_QEMU_DIR="
rem set "EXTRA_QEMU_DIR=D:\Tools\qemu"

set "QEMU_EXE="

for /f "delims=" %%Q in ('where qemu-system-x86_64.exe 2^>nul') do if not defined QEMU_EXE set "QEMU_EXE=%%Q"

if not defined QEMU_EXE if defined EXTRA_QEMU_DIR if exist "%EXTRA_QEMU_DIR%\qemu-system-x86_64.exe" set "QEMU_EXE=%EXTRA_QEMU_DIR%\qemu-system-x86_64.exe"
if not defined QEMU_EXE if exist "C:\Program Files\qemu\qemu-system-x86_64.exe" set "QEMU_EXE=C:\Program Files\qemu\qemu-system-x86_64.exe"
if not defined QEMU_EXE if exist "C:\Program Files (x86)\qemu\qemu-system-x86_64.exe" set "QEMU_EXE=C:\Program Files (x86)\qemu\qemu-system-x86_64.exe"
if not defined QEMU_EXE if exist "C:\qemu\qemu-system-x86_64.exe" set "QEMU_EXE=C:\qemu\qemu-system-x86_64.exe"

if not defined QEMU_EXE goto NO_QEMU
echo       - Found: %QEMU_EXE%

rem QEMU_DIR = the folder containing qemu-system-x86_64.exe (with trailing \).
for %%D in ("%QEMU_EXE%") do set "QEMU_DIR=%%~dpD"

rem ------------------------------------------------------------------
rem [2] Locate the ISO: auralite.iso next to this script, or (failing
rem     that) any *.iso file found in the same folder.
rem ------------------------------------------------------------------
echo [2/7] Looking for the AuraLite ISO...
set "ISO_FILE=%~dp0auralite.iso"

if not exist "%ISO_FILE%" (
    for %%F in ("%~dp0*.iso") do set "ISO_FILE=%%~fF"
)

if not exist "%ISO_FILE%" goto NO_ISO
echo       - Using: "%ISO_FILE%"

rem ------------------------------------------------------------------
rem [3] Locate UEFI (OVMF) firmware so the OS boots through its GOP
rem     framebuffer path instead of the legacy BIOS path.
rem
rem     WHY THIS MATTERS: auralite.iso is a dual-boot hybrid image with
rem     BOTH a legacy BIOS loader and a UEFI (BOOTX64.EFI) loader baked
rem     in, but only the UEFI path currently programs a linear
rem     graphics framebuffer. Booted via plain BIOS (SeaBIOS, no OVMF),
rem     the QEMU window gets stuck showing a static
rem     "Booting from Hard Disk..." text screen with a blinking cursor
rem     FOREVER -- that is not a hang, the OS is actually running fine
rem     underneath (see it alive on the serial console mirrored into
rem     THIS window), it simply has no framebuffer to draw its own
rem     console/GUI onto over BIOS. Only UEFI gives it one.
rem
rem     Firmware search order:
rem       1. OVMF_CODE.fd + OVMF_VARS.fd dropped directly next to this
rem          script (manual override -- copy your own OVMF build here
rem          under exactly these two names if the checks below fail).
rem       2. <qemu install dir>\share\edk2-x86_64-code.fd +
rem          edk2-i386-vars.fd -- the standard qemu-w64-setup.exe
rem          installer from https://qemu.weilnetz.de/w64/ ships these
rem          automatically, so most Windows QEMU installs already have
rem          them and nothing extra needs to be downloaded.
rem       3. <qemu install dir>\share\OVMF_CODE_4M.fd + OVMF_VARS_4M.fd
rem          -- alternate naming used by some QEMU/OVMF packages.
rem
rem     The VARS file is writable NVRAM storage, so it is copied to a
rem     private working copy under %PUBLIC% (see the comment further
rem     down, right before the copy happens, for why that specific
rem     location was chosen) instead of being used -- and mutated --
rem     directly from the QEMU install folder. If UEFI firmware cannot
rem     be found at all, the script falls back to the BIOS path with a
rem     clear warning explained at the end of this section.
rem ------------------------------------------------------------------
echo [3/7] Looking for UEFI (OVMF) firmware...

set "OVMF_CODE="
set "OVMF_VARS_SRC="

if exist "%~dp0OVMF_CODE.fd" if exist "%~dp0OVMF_VARS.fd" (
    set "OVMF_CODE=%~dp0OVMF_CODE.fd"
    set "OVMF_VARS_SRC=%~dp0OVMF_VARS.fd"
)

if not defined OVMF_CODE if exist "%QEMU_DIR%share\edk2-x86_64-code.fd" if exist "%QEMU_DIR%share\edk2-i386-vars.fd" (
    set "OVMF_CODE=%QEMU_DIR%share\edk2-x86_64-code.fd"
    set "OVMF_VARS_SRC=%QEMU_DIR%share\edk2-i386-vars.fd"
)

if not defined OVMF_CODE if exist "%QEMU_DIR%share\OVMF_CODE_4M.fd" if exist "%QEMU_DIR%share\OVMF_VARS_4M.fd" (
    set "OVMF_CODE=%QEMU_DIR%share\OVMF_CODE_4M.fd"
    set "OVMF_VARS_SRC=%QEMU_DIR%share\OVMF_VARS_4M.fd"
)

if defined OVMF_CODE (
    echo       - Found: "%OVMF_CODE%"
)
rem The writable VARS copy is deliberately placed under %PUBLIC%
rem (normally "C:\Users\Public"), NOT next to this script. %PUBLIC% is
rem a fixed, always-ASCII, always-writable system folder, so this
rem sidesteps any interaction between cmd.exe's COPY command and a
rem script/ISO location that (as is entirely normal and fine for
rem everything else) may contain Cyrillic/accented characters, spaces,
rem or parentheses -- e.g. "C:\Users\<name>\Downloads\my folder (1)".
rem Those characters are fully supported for the ISO/script location
rem itself; the extra caution here is only because copying a brand
rem new file into such a path from a batch script has proven unreliable
rem in the field. %TEMP% is used as a fallback if %PUBLIC% is unset.
set "OVMF_VARS="
set "OVMF_WORKDIR=%PUBLIC%\AuraLiteOS-QEMU"
if not defined PUBLIC set "OVMF_WORKDIR=%TEMP%\AuraLiteOS-QEMU"
if defined OVMF_CODE if not exist "%OVMF_WORKDIR%" mkdir "%OVMF_WORKDIR%" >nul 2>&1
if defined OVMF_CODE if exist "%OVMF_WORKDIR%" set "OVMF_VARS=%OVMF_WORKDIR%\ovmf_vars.fd"

if defined OVMF_VARS if not exist "%OVMF_VARS%" (
    copy /y "%OVMF_VARS_SRC%" "%OVMF_VARS%" >nul
    if errorlevel 1 (
        echo       [WARN] Could not copy the UEFI VARS file to:
        echo         "%OVMF_VARS%"
        set "OVMF_CODE="
        set "OVMF_VARS="
    )
)
if defined OVMF_VARS if not exist "%OVMF_VARS%" (
    echo       [WARN] UEFI VARS file is still missing after the copy attempt.
    set "OVMF_CODE="
    set "OVMF_VARS="
)

if defined OVMF_CODE (
    echo       - UEFI boot enabled: the OS will show its graphical
    echo         console/GUI directly in the QEMU window.
) else (
    echo       - No UEFI firmware found next to this script or under
    echo         "%QEMU_DIR%share".
    echo       - Falling back to legacy BIOS boot. The QEMU window will
    echo         show a static "Booting from Hard Disk..." screen with
    echo         a blinking cursor and appear frozen -- THIS IS EXPECTED
    echo         over plain BIOS: the OS has no graphics output there,
    echo         it only prints to the serial console mirrored into
    echo         THIS cmd window. To see the real GUI, copy an OVMF
    echo         build's OVMF_CODE.fd and OVMF_VARS.fd next to this
    echo         script, or reinstall QEMU from
    echo         https://qemu.weilnetz.de/w64/ ^(recent installers
    echo         bundle UEFI firmware automatically^).
)

rem ------------------------------------------------------------------
rem [4] Pick a CPU accelerator.
rem
rem     WHY THIS MATTERS: AuraLite is a real x86_64 kernel with a GUI
rem     compositor that targets 100 FPS and redraws the whole screen
rem     every frame. Without hardware-assisted virtualization, QEMU
rem     falls back to TCG (pure software instruction-by-instruction
rem     emulation), which is easily 10-50x slower -- that is exactly
rem     what makes the cursor jerky and the screen update only once
rem     every 1-2 seconds instead of smoothly.
rem
rem     On Windows 10/11, "whpx" (Windows Hypervisor Platform) is the
rem     hardware accelerator QEMU can use. Passing BOTH "-accel whpx"
rem     and "-accel tcg" is safe and always correct: per QEMU's own
rem     documented behaviour, it tries whpx first and silently falls
rem     back to tcg if whpx isn't available, so this line works whether
rem     or not Hyper-V/WHPX is enabled on this machine.
rem
rem     kernel-irqchip=off: WHPX's default in-kernel interrupt
rem     controller has a well-known bug on Windows 10 where timer/PIC
rem     interrupts fail to wake the virtual CPU from HLT. AuraLite's
rem     8259 PIC + Local APIC "virtual wire" timer path hits this
rem     directly, and the symptom looks exactly like a freeze: the
rem     clock stops advancing, the mouse cursor stops moving and the
rem     screen stops repainting, even though the OS is not actually
rem     hung (WHPX just silently drops the interrupts that would tell
rem     it to keep going). Passing kernel-irqchip=off makes WHPX
rem     emulate interrupt delivery in software instead of through that
rem     broken fast path -- CPU instruction execution still runs under
rem     hardware acceleration, only interrupt routing becomes emulated,
rem     so this is still far faster than plain TCG. This flag is
rem     specific to whpx and is ignored by (harmless to have present
rem     for) the tcg fallback.
rem
rem     If the GUI is still slow after this, WHPX is most likely not
rem     enabled at all. Turn it on with (as Administrator, then
rem     reboot):
rem         dism /online /enable-feature /featurename:HypervisorPlatform /all
rem     or via "Turn Windows features on or off" -> check
rem     "Windows Hypervisor Platform".
rem ------------------------------------------------------------------
echo [4/7] Selecting CPU accelerator (WHPX if available, else software)...
set "ACCEL_ARGS=-accel whpx,kernel-irqchip=off -accel tcg,thread=multi"

rem ------------------------------------------------------------------
rem [5] Create the optional AHCI/ext2 test disks next to this script
rem     if they don't already exist. These back /disk, /fat and /ext2
rem     inside AuraLite; the OS boots fine without them too, so any
rem     failure here is a warning, not a hard error.
rem ------------------------------------------------------------------
echo [5/7] Checking test disk images...
set "DISK0=%~dp0disk.img"
set "DISK1=%~dp0ext2.img"

if exist "%DISK0%" (
    echo       - disk.img already present.
) else (
    echo       - Creating disk.img [16 MiB, AHCI test disk for /fat and /disk]...
    fsutil file createnew "%DISK0%" 16777216 >nul 2>&1
    if not exist "%DISK0%" (
        echo         [WARN] Could not create disk.img -- continuing without it.
        set "DISK0="
    )
)

if exist "%DISK1%" (
    echo       - ext2.img already present.
) else (
    echo       - Creating ext2.img [8 MiB, ext2 test disk for /ext2]...
    fsutil file createnew "%DISK1%" 8388608 >nul 2>&1
    if not exist "%DISK1%" (
        echo         [WARN] Could not create ext2.img -- continuing without it.
        set "DISK1="
    )
)

rem ------------------------------------------------------------------
rem [6] Build the QEMU command line.
rem
rem   -accel whpx / tcg       : hardware acceleration when available,
rem                             see step [4] above.
rem   -drive if=pflash (x2)   : the OVMF CODE (read-only) and VARS
rem                             (writable NVRAM) firmware images, only
rem                             added when UEFI firmware was found in
rem                             step [3]. Omitted entirely -> QEMU boots
rem                             its built-in SeaBIOS instead.
rem   -drive ...,if=ide       : auralite.iso is a raw hybrid disk image
rem                             (BIOS MBR + UEFI ESP together), NOT an
rem                             optical disc -- it must be attached as
rem                             a plain hard disk. Do NOT use -cdrom;
rem                             the BIOS/UEFI loader chain expects to
rem                             see it on an IDE/AHCI disk interface.
rem   -smp 4                  : real SMP is on -- the boot CPU detects
rem                             every core via ACPI MADT and wakes the
rem                             application processors with
rem                             INIT-SIPI-SIPI.  Since SMP step 3.2 the
rem                             scheduler REALLY runs threads on every
rem                             core: per-CPU syscall state, per-CPU
rem                             LAPIC-calibrated timer ticks, run-queue
rem                             load balancing and work stealing, so 4
rem                             vCPUs give genuine parallel speedup.
rem   -vga std                : linear framebuffer for the GUI/console
rem                             (only actually driven when booting via
rem                             UEFI/OVMF -- see step [3] above).
rem   -serial stdio           : mirrors the kernel/shell serial console
rem                             into this cmd window either way.
rem   -netdev/-device e1000   : Intel 8254x NIC AuraLite's driver
rem                             recognises out of the box (DHCP/DNS/TCP).
rem   -device piix3-usb-uhci  : UHCI controller for the usb-tablet
rem                             device below (PS/2 keyboard from the
rem                             standard QEMU PC machine is still used
rem                             for typing; only the pointer moves to
rem                             USB).
rem   -device usb-tablet      : an ABSOLUTE-positioning USB pointer
rem                             device -- this is what gives "full
rem                             mouse capture": with a normal (relative)
rem                             PS/2 mouse, QEMU must literally grab the
rem                             cursor (Ctrl+Alt to release it) because
rem                             it only ever sees deltas and the guest
rem                             and host cursors would otherwise drift
rem                             apart. usb-tablet instead reports the
rem                             cursor's exact position every time, so
rem                             the guest cursor always matches the
rem                             host cursor 1:1 with no grab/release
rem                             step at all. AuraLite's generic USB HID
rem                             report parser already handles this
rem                             device and is exercised by this exact
rem                             configuration in the project's own
rem                             integration test suite
rem                             (test_usb_generic_hid.sh).
rem   -device ahci / ide-hd   : the two optional test disks from [5].
rem ------------------------------------------------------------------
echo [6/7] Assembling QEMU command line...

set "QEMU_ARGS=%ACCEL_ARGS%"
if defined OVMF_CODE set "QEMU_ARGS=%QEMU_ARGS% -drive if=pflash,unit=0,format=raw,readonly=on,file="%OVMF_CODE%" -drive if=pflash,unit=1,format=raw,file="%OVMF_VARS%""

set "QEMU_ARGS=%QEMU_ARGS% -drive file="%ISO_FILE%",format=raw,if=ide -boot order=c -m 256M -smp 4 -vga std -serial stdio -no-reboot -no-shutdown -cpu qemu64 -netdev user,id=net0 -device e1000,netdev=net0 -device piix3-usb-uhci,id=uhci -device usb-tablet,bus=uhci.0"

if defined DISK0 set "QEMU_ARGS=%QEMU_ARGS% -drive file="%DISK0%",format=raw,if=none,id=ahcidisk -device ahci,id=ahci0 -device ide-hd,drive=ahcidisk,bus=ahci0.0"
if defined DISK1 set "QEMU_ARGS=%QEMU_ARGS% -drive file="%DISK1%",format=raw,if=none,id=ext2disk -device ide-hd,drive=ext2disk,bus=ahci0.1"

echo.
echo [7/7] Starting QEMU...
echo ============================================================
echo   A QEMU window should appear shortly. Serial/console output
echo   is also mirrored into THIS window. Close the QEMU window
echo   (or press Ctrl+C here) to stop.
echo.
echo   If the GUI feels slow/jerky (cursor updating only once every
echo   1-2 seconds), WHPX hardware acceleration is likely disabled on
echo   this PC and QEMU is falling back to slow software emulation.
echo   Enable it (as Administrator, then reboot) with:
echo     dism /online /enable-feature /featurename:HypervisorPlatform /all
echo   or via "Turn Windows features on or off" -^> check
echo   "Windows Hypervisor Platform".
echo.
echo   If instead the clock is stuck, the mouse cursor does not move
echo   and the screen never repaints (looks frozen even though QEMU is
echo   clearly running), that is a known WHPX/Windows 10 bug with
echo   interrupt delivery -- already worked around in this script via
echo   kernel-irqchip=off. If it still happens, try updating QEMU to
echo   the latest version from https://qemu.weilnetz.de/w64/, since
echo   this WHPX bug has been actively fixed upstream.
echo ============================================================
echo.

"%QEMU_EXE%" %QEMU_ARGS%
set "QEMU_EXIT=%ERRORLEVEL%"

echo.
echo ============================================================
echo   QEMU exited with code %QEMU_EXIT%.
echo ============================================================
pause
if defined ORIG_CODEPAGE chcp %ORIG_CODEPAGE% >nul
exit /b %QEMU_EXIT%

:NO_QEMU
echo.
echo [ERROR] Could not find qemu-system-x86_64.exe
echo         Install QEMU for Windows from: https://qemu.weilnetz.de/w64/
echo         Then either:
echo           - add its install folder to your PATH, or
echo           - edit this run.bat and set EXTRA_QEMU_DIR to that folder.
echo.
pause
if defined ORIG_CODEPAGE chcp %ORIG_CODEPAGE% >nul
exit /b 1

:NO_ISO
echo.
echo [ERROR] No .iso file found next to run.bat
echo         Copy auralite.iso into this folder:
echo           %~dp0
echo.
pause
if defined ORIG_CODEPAGE chcp %ORIG_CODEPAGE% >nul
exit /b 1
```

### Run tests

```bash
make test-unit              # host-side unit tests
make test-integration-fast  # QEMU smoke/integration subset
make test-integration       # full QEMU integration suite
```

The full suite currently boots QEMU for 14 black-box cases, including AHCI,
FAT32 persistence, ext2 cross-OS round-trips, USB MSC, networking, SMP, graphics
and GUI/VNC checks.

---

## VirtualBox and VMware

AuraLite can boot in desktop hypervisors as long as the virtual hardware matches
currently implemented drivers.

### VirtualBox

```bash
make vbox
```

If `VBoxManage` is installed, this creates/updates a VM named `AuraLite-OS`.
Otherwise it writes manual setup notes to:

```text
vm/virtualbox/README-VirtualBox.txt
```

Recommended NIC: **Intel PRO/1000 MT Desktop (82540EM)**.

### VMware Workstation / Fusion / Player

```bash
make vmware
```

Open:

```text
vm/vmware/AuraLite-OS.vmwarevm/AuraLite-OS.vmx
```

Recommended NIC: **legacy `e1000`**, not `vmxnet3` or `e1000e`.

More details: [`docs/virtual_machines.md`](docs/virtual_machines.md).

---

## Make targets

| Target | Description |
|---|---|
| `make iso` | Build the dual-boot BIOS+UEFI ISO (`release/auralite.iso`) using the custom loader chain. |
| `make iso-bios` | BIOS-only hybrid MBR image (`build/auralite-bios.iso`). |
| `make iso-dual` | Same as `make iso` but without the release copy step (`build/auralite-dual.iso`). |
| `make iso-limine` | Legacy Limine-based ISO for firmware-compat fallback. |
| `make mbr` | BL2 512-byte MBR (`build/boot/mbr.bin`). |
| `make mbr-dual` | BL7 MBR variant reading Stage 2 from LBA 34 (`build/boot/mbr_dual.bin`). |
| `make stage2` | BL3+BL4 Stage 2 flat binary (`build/boot/stage2.bin`). |
| `make efi` | BL6 UEFI application (`build/boot/BOOTX64.EFI`). |
| `make kernel` | Build `build/kernel.elf` only. |
| `make user` | Build user-space ELF programs. |
| `make run` | Boot the ISO in QEMU with serial output and e1000 networking. |
| `make run-usb-msc` | Boot QEMU with a UHCI USB mass-storage test disk attached. |
| `make debug` | Boot QEMU paused and wait for GDB on port `1234`. |
| `make usb` | Copy the hybrid ISO to `build/usb.img` for USB/HDD-style booting. |
| `make vbox` | Build ISO and create/update VirtualBox configuration. |
| `make vmware` | Build ISO and generate a VMware `.vmx`. |
| `make vm-configs` | Generate both VirtualBox and VMware configs. |
| `make test-unit` | Build and run host-side unit tests. |
| `make test-integration-fast` | Run the faster QEMU integration subset. |
| `make test-integration` | Run the full QEMU black-box integration suite. |
| `make test` | Run unit tests and then full integration tests. |
| `make clean` | Remove `build/`. |

---

## Repository layout

```text
AuraLite-OS/
├── boot/limine/              # Limine boot configs
├── docs/                     # Architecture, ABI, drivers, VM setup, status
├── drivers/
│   ├── ahci/                 # AHCI SATA detection and DMA sector I/O
│   ├── bluetooth/            # Bluetooth HCI protocol layer
│   ├── e1000/                # Intel 8254x/e1000 NIC driver
│   ├── framebuffer/          # Console, 2D graphics, PSF font, WM, 3D demo
│   ├── keyboard/             # PS/2 keyboard
│   ├── mouse/                # PS/2 mouse
│   ├── pci/                  # PCI config-space access
│   ├── timer/                # PIT timer
│   ├── uart/                 # COM1 serial
│   ├── usb/                  # UHCI/OHCI/EHCI/xHCI + USB core + MSC layer
│   └── wifi/                 # 802.11 MAC management layer
├── kernel/
│   ├── arch/x86_64/          # CPU, GDT, IDT, IRQ, paging, syscall, SMP, TSS
│   ├── fs/                   # VFS, initrd, devfs, tmpfs, diskfs, FAT32, ext2
│   ├── gui/                  # Kernel GUI, compositor and GUI syscalls
│   ├── lib/                  # kprintf, string, bitmap, spinlock, assert
│   ├── mm/                   # PMM, heap core, kernel heap wrapper
│   ├── net/                  # Ethernet/ARP/IPv4/ICMP/UDP/DNS/TCP
│   ├── proc/                 # Threads, scheduler, ELF loader, processes
│   └── kernel.c              # kmain() orchestration
├── libauragui/               # User-space GUI toolkit wrappers/widgets
├── libc/                     # Minimal user-space libc and crt0
├── limine/                   # Vendored Limine binaries and header
├── scripts/                  # CI/integration helper
├── tests/unit/               # Host-side unit tests
├── tests/integration/        # QEMU black-box integration tests
├── tools/                    # ISO/initrd/VM/QEMU helper scripts
├── userspace/                # user programs, grouped (see each README.md)
│   ├── system/               #   init — started by the kernel itself
│   ├── apps/                 #   applications
│   ├── demos/                #   demonstrations
│   └── tests/                #   in-OS test programs
├── kernel.ld                 # Kernel linker script
├── Makefile                  # Build system
└── README.md
```

---

## User-space programs

The initrd currently packages:

Programs live in directories by kind, one location each — `/calc` does not
exist, but `calc`, `run calc` and `/apps/calc` all work. See
`docs/filesystem.md`.

| Path | Purpose |
|---|---|
| `/bin/init` | Interactive shell. |
| `/bin/hello` | Hello-world test program. |
| `/bin/apm` | Package manager; installs into `/opt`. |
| `/bin/play` | CLI audio player. |
| `/bin/sysinfo` | System information. |
| `/apps/calc` | Calculator. |
| `/apps/editor` | Simple line editor. |
| `/apps/clock` | Clock/uptime demo. |
| `/apps/http` | HTTP client. |
| `/apps/browser` | Text web browser with simple HTML rendering. |
| `/apps/gcalc` | Graphical calculator. |
| `/apps/gedit` | Graphical text editor. |
| `/apps/gfiles` | Graphical file manager. |
| `/apps/gterm` | Graphical terminal-style demo. |
| `/apps/gsysmon` | Graphical system monitor demo. |
| `/apps/gabout` | Graphical about dialog. |
| `/apps/gtaskmgr` | Graphical task manager. |
| `/apps/glaunch` | GUI application launcher. |
| `/apps/gaudio` | GUI music player. |
| `/apps/gbrowser` | GUI web browser. |
| `/apps/gusb` | GUI USB manager. |
| `/demos/guess` | Number guessing game. |
| `/demos/snake` | Terminal snake game. |
| `/demos/glcube` | OpenGL demo: lit, textured, depth-buffered rotating cube over a mipmapped floor, with a render-to-texture inset panel. |
| `/demos/glgears` | OpenGL demo: the classic three-gear benchmark. |
| `/tests/selftest` | Userspace regression checks for usercopy, FD and socket syscalls. |
| `/tests/gltest` | OpenGL regression checks (prints PASS/FAIL to serial). |
| `/tests/insttest` | Installation-policy checks. |
| `/pkg/*.pkg` | Package archives `apm` installs from. |

A command can be given by name — `run calc` — and is looked for in
`/bin:/apps:/demos:/tests:/opt:/`.

Common shell commands:

```text
help
ls /
cat /hello
echo hello
write /tmp/note hello
cat /tmp/note
run calc
run /apps/calc
nslookup example.com
ping example.com
gui
exit
```

---

## Documentation map

Start here:

- [`docs/README.md`](docs/README.md) — documentation index.
- [`docs/build_and_run.md`](docs/build_and_run.md) — build/run/troubleshooting.
- [`docs/status.md`](docs/status.md) — current feature and limitation matrix.
- [`docs/architecture.md`](docs/architecture.md) — kernel architecture.
- [`docs/memory_map.md`](docs/memory_map.md) — virtual/physical memory layout.
- [`docs/syscall_abi.md`](docs/syscall_abi.md) — syscall ABI and numbers.
- [`docs/driver_guide.md`](docs/driver_guide.md) — driver inventory and notes.
- [`docs/virtual_machines.md`](docs/virtual_machines.md) — VirtualBox/VMware setup.
- [`docs/virtual_driver_matrix.md`](docs/virtual_driver_matrix.md) — QEMU/VirtualBox/VMware device compatibility matrix.
- [`docs/opengl.md`](docs/opengl.md) — the software OpenGL 1.1/1.3 stack: supported subset, mipmapping, multitexturing, framebuffer objects, behaviour notes, performance.
- [`PLAN.md`](PLAN.md) — historical phase plan.
- [`GL_PLAN.md`](GL_PLAN.md) — the OpenGL stack (complete).
- [`FSLAYOUT_PLAN.md`](FSLAYOUT_PLAN.md) — filesystem layout and enforced install directories (complete).
- [`SDK_PLAN.md`](SDK_PLAN.md) — third-party application support (complete).
- [`WEBVIEW_PLAN.md`](WEBVIEW_PLAN.md) — a box-model web view (planned). Measured: a 2D renderer, with OpenGL used only for `<canvas>`.
- [`INTERNET_PLAN.md`](INTERNET_PLAN.md) — TLS 1.3 and real internet access (planned). The prerequisite for HTTPS anywhere.
- [`FIXES_PLAN.md`](FIXES_PLAN.md) — repair plan for known defects (planned), ranked by danger rather than by ease. Adds nothing; fixes what is broken.
- [`TODO.md`](TODO.md) — known limitations and future work.
- [`CHANGELOG.md`](CHANGELOG.md) — chronological changes.

---

## Known limitations

Short version:

- AHCI sector read/write is enabled and self-tested on QEMU AHCI disks; broader
  physical-hardware coverage is still experimental.
- Scheduler state is not SMP-safe; APs are brought online and idle rather than
  participating in general scheduling.
- File descriptors are now per-process, but descriptor inheritance/lifetime semantics are still simplified.
- User pointers passed to syscalls now go through basic range/permission validation and copy helpers, but there is not yet a fault-recovering uaccess layer.
- `fork`/`execve`/`wait4` are simplified and not POSIX-complete.
- Dead TCBs and kernel stacks are deferred-reaped, but full user address-space/page-table reaping is not implemented yet.
- Networking is polling-based. User space has process-owned socket-style handles, and the TCP transport supports per-connection state up to 8 streams.
- `/disk` is intentionally tiny: flat namespace, 8 files maximum, 4 KiB per file.
- FAT32 and ext2 are featureful enough for integration tests, but their hardware
  coverage is primarily QEMU/AHCI and they should still be treated as hobby OS
  filesystems rather than production-grade implementations.
- USB MSC currently uses the UHCI backend; OHCI/EHCI/xHCI transfer engines are
  not wired to class drivers yet.
- **The keyboard layout is hardcoded US.** Two fixed scancode tables, no keymap
  selection and no dead keys, so a non-US keyboard produces the wrong
  characters outside the shared ASCII subset.
- **Crypto primitives and X.509 parsing exist, but no TLS and therefore no
  HTTPS yet.** `INTERNET_PLAN.md` N0 gave a real entropy source (a ChaCha20
  CSPRNG seeded from RDSEED/RDRAND or interrupt-timing jitter;
  `getentropy()` fails closed with `-ENOSYS` until entropy exists); N1
  shipped `lib/libatls/` — userspace SHA-256/512, HMAC, HKDF,
  ChaCha20-Poly1305 AEAD, X25519 and Ed25519 verification; N2 added
  zero-copy, depth-bounded X.509 certificate parsing. All verified against
  RFC test vectors. What is still missing is everything protocol-shaped:
  the TLS 1.3 handshake/record layer, certificate validation, and an HTTPS
  client (phases N3–N6). See [`INTERNET_PLAN.md`](INTERNET_PLAN.md).
- **A kernel fault taken on a bad stack triple-faults.** The IST is allocated
  but no interrupt gate selects it, so a kernel stack overflow or double fault
  resets the machine with no diagnostic.
- `SIGSTOP`/`SIGTSTP` terminate rather than stop; there is no stopped state.

See [`docs/status.md`](docs/status.md) and [`TODO.md`](TODO.md).

---

## License notes

This repository vendors Limine binaries and `limine.h`; see `limine/LICENSE` for
Limine licensing. Font assets and third-party snippets are documented in their
respective source files where applicable.

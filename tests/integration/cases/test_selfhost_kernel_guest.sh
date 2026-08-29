#!/usr/bin/env bash
# test_selfhost_kernel_guest.sh -- SELFHOST_PLAN.md SH5d terminal gate.
#
# Boot #1 is the actual self-host build.  The host only starts QEMU and waits
# for a new `auralite#` prompt before feeding each command; inside the guest,
# /bin/tcc compiles every x86_64 kernel C source, guest-built mini-asm emits
# every kernel .asm object, and guest-built aulink links the result to the
# writable FAT volume as /fat/KERNEL.ELF.  There is deliberately no clang,
# ld.lld, nasm, Python, host-built tree kernel C/asm object, or host linker in
# that build command set.  The bootstrap tcc/libtcc1.a and pre-existing init.elf
# user-program byte stream are explicit seeds; closing them is SH8 scope.
#
# Boot #2 extracts that exact FAT artifact with mtools, packs it through the
# existing host image writer, and proves it reaches the ordinary Ring 3 shell.
# SH6 is where an in-guest shell script replaces this host-side prompt-aware
# transport loop; SH5d's claim is the compiler/linker/assembler closure.
set -u
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$(dirname "$0")/.."
. lib/lib.sh

# The guest tcc is deliberately optional for ordinary builds, matching the
# earlier selfhost cases.  Do not fetch/build a compiler from an integration
# test: a missing bootstrap is a loud skip, not a hidden network side effect.
if [ ! -f "$ROOT/build/selfhost/tcc.elf" ]; then
    echo "${C_YELLOW}[selfhost] guest tcc not built -- skipping (run 'make selfhost-deps selfhost-tcc' and rebuild the ISO)${C_RESET}"
    il_skip "guest tcc absent from build/selfhost (selfhost-deps not run)"
    exit 0
fi

# Always ask make to refresh the bootstrap image.  The SH5d source closure is
# an initrd prerequisite, so this prevents a stale ISO from compiling sources
# older than the tree the test is reporting on.
PREP_LOG="$ROOT/build/selfhost/sh5d-bootstrap-image.log"
mkdir -p "$(dirname "$PREP_LOG")"
if ! (cd "$ROOT" && make iso) >"$PREP_LOG" 2>&1; then
    echo "${C_RED}[selfhost] SH5d bootstrap ISO build failed${C_RESET}"
    tail -30 "$PREP_LOG" | sed 's/^/    /'
    exit 2
fi

il_init
il_have qemu-system-x86_64 mcopy readelf || exit 2

il_section "self-host kernel (SH5d): build in AuraLite, extract FAT ELF, boot it"

# The build itself uses /tmp: the stock FAT formatter intentionally creates a
# small 4 MiB volume, enough for the final ~1 MiB ELF but not a full object
# directory.  /tmp has 256 slots, and this job has 126 C + 9 asm objects plus
# fewer than 20 tools/generated files.  Only the completed kernel crosses the
# durability boundary, exactly what this phase gates.
DISK="$IL_BUILD/selfhost-sh5d-fat.img"
GUEST_ELF="$IL_BUILD/selfhost/kernel-guest.elf"
GUEST_ISO="$IL_BUILD/selfhost/kernel-guest.iso"
rm -f "$DISK" "$GUEST_ELF" "$GUEST_ISO"
il_make_disk "$DISK" 16 "AURSH5D!"

# Mirror KERNEL_SRCS/KERNEL_ASMS in the Makefile rather than hand-maintaining
# a second list.  aulink sorts each object directory, so host filesystem
# enumeration order cannot perturb the linked image.
mapfile -t C_SRCS < <(
    cd "$ROOT" || exit 1
    {
        find kernel drivers -name '*.c' \
            -not -path 'kernel/arch/i386/*' \
            -not -path 'kernel/arch/riscv64/*' \
            -not -path 'kernel/arch/aarch64/*' \
            -not -path 'kernel/dt/*' \
            -not -path 'kernel/drivers/*'
        printf '%s\n' w32/src/w32_pe.c
    } | LC_ALL=C sort | sed 's#^#/src/#'
)
mapfile -t ASM_SRCS < <(
    cd "$ROOT" || exit 1
    find kernel drivers -name '*.asm' \
        -not -path 'kernel/arch/i386/*' \
        -not -path 'kernel/arch/riscv64/*' \
        -not -path 'kernel/arch/aarch64/*' | LC_ALL=C sort | sed 's#^#/src/#'
)
C_COUNT=${#C_SRCS[@]}
ASM_COUNT=${#ASM_SRCS[@]}
if [ "$C_COUNT" -ne 126 ] || [ "$ASM_COUNT" -ne 9 ]; then
    echo "${C_RED}[selfhost] SH5d source closure drifted: $C_COUNT C, $ASM_COUNT asm (expected 126 / 9)${C_RESET}"
    exit 2
fi

# ---- Boot #1: build the next kernel entirely inside AuraLite ---------------
BUILD_LOG="$IL_LOGDIR/selfhost_kernel_guest_build.log"
IL_LAST_LOG="$BUILD_LOG"
trap il_dump_on_error EXIT

il_send_prompt "mkdir /tmp/sh5d"
il_send_prompt "mkdir /tmp/sh5d/lib"
il_send_prompt "mkdir /tmp/sh5d/tools"
il_send_prompt "mkdir /tmp/sh5d/build"
il_send_prompt "mkdir /tmp/sh5d/cobj"
il_send_prompt "mkdir /tmp/sh5d/aobj"

# The tiny tool libc is intentionally explicit.  It has the file/stdio and
# directory APIs aulink needs, but it is compiled by the guest tcc here rather
# than borrowed as a host-built archive.
il_send_prompt "run /bin/tcc -c -o /tmp/sh5d/lib/crt0.o /src/libc/tcc_crt0.s"
for spec in \
    'c libc.c' \
    'm malloc.c' \
    'e env.c' \
    's string_extra.c' \
    'u stdlib_extra.c' \
    'io stdio_extra.c' \
    'd dirent.c'; do
    set -- $spec
    il_send_prompt "run /bin/tcc -c -I/src/libc/include -o /tmp/sh5d/lib/$1.o /src/libc/src/$2"
done
# tcc_builtins.c is the guest compiler runtime glue, staged beside tcc_crt0.s
# rather than in libc/src (it is not a public libc translation unit).
il_send_prompt "run /bin/tcc -c -I/src/libc/include -o /tmp/sh5d/lib/bi.o /src/libc/tcc_builtins.c"

# Compile and link every build tool with tcc's own linker first.  The C
# generators have explicit output paths because the init shell intentionally
# has no redirection grammar.
il_send_prompt "run /bin/tcc -c -I/src/libc/include -o /tmp/sh5d/tools/aulink.o /src/aulink.c"
il_send_prompt "run /bin/tcc -c -I/src/libc/include -o /tmp/sh5d/tools/mini.o /src/mini-asm.c"
il_send_prompt "run /bin/tcc -c -DARCH_X86_64 -I/src -I/src/libc/include -o /tmp/sh5d/tools/offsets.o /src/selfhost/gen_asm_offsets.c"
il_send_prompt "run /bin/tcc -c -I/src/libc/include -o /tmp/sh5d/tools/userbin.o /src/selfhost/gen_user_binary.c"
il_send_prompt "run /bin/tcc -c -I/src/libc/include -o /tmp/sh5d/tools/apinc.o /src/selfhost/gen_ap_trampoline_inc.c"

TOOL_LIB='/tmp/sh5d/lib/crt0.o /tmp/sh5d/lib/c.o /tmp/sh5d/lib/bi.o /tmp/sh5d/lib/m.o /tmp/sh5d/lib/e.o /tmp/sh5d/lib/s.o /tmp/sh5d/lib/u.o /tmp/sh5d/lib/io.o /tmp/sh5d/lib/d.o'
il_send_prompt "run /bin/tcc -nostdlib -static -o /tmp/sh5d/aulink $TOOL_LIB /tmp/sh5d/tools/aulink.o /apps/tcc/libtcc1.a"
il_send_prompt "run /bin/tcc -nostdlib -static -o /tmp/sh5d/mini-asm $TOOL_LIB /tmp/sh5d/tools/mini.o /apps/tcc/libtcc1.a"
il_send_prompt "run /bin/tcc -nostdlib -static -o /tmp/sh5d/gen_asm_offsets $TOOL_LIB /tmp/sh5d/tools/offsets.o /apps/tcc/libtcc1.a"
il_send_prompt "run /bin/tcc -nostdlib -static -o /tmp/sh5d/gen_user_binary $TOOL_LIB /tmp/sh5d/tools/userbin.o /apps/tcc/libtcc1.a"
il_send_prompt "run /bin/tcc -nostdlib -static -o /tmp/sh5d/gen_ap_trampoline_inc $TOOL_LIB /tmp/sh5d/tools/apinc.o /apps/tcc/libtcc1.a"

# Generated inputs consumed by the kernel sources / handwritten assembly.
il_send_prompt "run /tmp/sh5d/gen_asm_offsets /tmp/sh5d/build/asm_offsets.inc"
il_send_prompt "run /tmp/sh5d/mini-asm -f bin -I/src -o /tmp/sh5d/build/ap_trampoline.bin /src/boot/smp/ap_trampoline.asm"
il_send_prompt "run /tmp/sh5d/gen_ap_trampoline_inc /tmp/sh5d/build/ap_trampoline.bin /tmp/sh5d/build/ap_trampoline.inc"
il_send_prompt "run /tmp/sh5d/gen_user_binary /src/selfhost/init.elf /tmp/sh5d/build/init_bin.h init_bin"

for src in "${C_SRCS[@]}"; do
    rel=${src#/src/}
    rel=${rel%.c}
    obj="/tmp/sh5d/cobj/${rel//\//_}.o"
    il_send_prompt "run /bin/tcc -c -ffreestanding -fno-pic -DARCH_X86_64 -I/src -I/tmp/sh5d -I/tmp/sh5d/build -I/src/w32/include -o $obj $src"
done
for src in "${ASM_SRCS[@]}"; do
    rel=${src#/src/}
    rel=${rel%.asm}
    obj="/tmp/sh5d/aobj/${rel//\//_}.o"
    il_send_prompt "run /tmp/sh5d/mini-asm -f elf64 -I/tmp/sh5d/build -I/src -o $obj $src"
done

# Directory arguments are lexical object sets.  This is the crucial SH5d
# aulink mode: one short command replaces an impossible 135-argument line.
il_send_prompt "run /tmp/sh5d/aulink -T /src/kernel.ld -o /fat/KERNEL.ELF /tmp/sh5d/cobj /apps/tcc/libtcc1.a /tmp/sh5d/aobj"
il_send_prompt "stat /fat/KERNEL.ELF"
il_send_prompt "exit"

echo "[selfhost] SH5d dispatch: $C_COUNT C + $ASM_COUNT asm sources through prompt-aware guest serial"
# il_run_qemu_prompt clears the queue as it drains it, so snapshot the count
# first.  Asserting it back from the transport's own log proves every queued
# command was actually delivered to a fresh prompt -- a queue that stalled
# halfway would otherwise first show up as a missing object count below.
QUEUED=$(printf '%s' "$IL_PROMPT_QUEUE" | grep -c .)

IL_SMP=1 IL_SELFTEST=fast il_run_qemu_prompt "$BUILD_LOG" 1200 \
    -drive "file=$DISK,format=raw,if=none,id=sh5ddisk" \
    -device "ahci,id=ahci0" \
    -device "ide-hd,drive=sh5ddisk,bus=ahci0.0"

il_assert_grep "$IL_PROMPT_DRIVER_LOG" \
    "\[prompt-qemu\] complete \($QUEUED/$QUEUED commands sent" \
    "transport dispatched all $QUEUED guest commands, each behind a fresh prompt"
il_assert_grep_fixed "$BUILD_LOG" "aulink: added $C_COUNT object(s) from directory /tmp/sh5d/cobj" \
    "guest aulink consumed all $C_COUNT sorted C objects from its directory"
il_assert_grep_fixed "$BUILD_LOG" "aulink: added $ASM_COUNT object(s) from directory /tmp/sh5d/aobj" \
    "guest mini-asm outputs entered aulink through its sorted directory mode"
il_assert_grep "$BUILD_LOG" "Size:[[:space:]]+[1-9][0-9]*" \
    "guest wrote /fat/KERNEL.ELF before leaving the first boot"
il_assert_no_grep "$BUILD_LOG" "undefined reference|aulink: [1-9][0-9]* error|tcc: error|\[proc\] spawn: .*not found" \
    "guest build had no compiler, linker, or guest-tool launch error"

if ! mcopy -i "$DISK@@32768" ::/KERNEL.ELF "$GUEST_ELF"; then
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    il_fail "host could not extract guest /fat/KERNEL.ELF at FAT LBA 64"
    il_summary
    exit 1
fi
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
il_pass "host extracted the guest-produced KERNEL.ELF from FAT LBA 64"

if [ -s "$GUEST_ELF" ] && readelf -h "$GUEST_ELF" | grep -q 'Machine:.*X86-64'; then
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    il_pass "extracted artifact is a non-empty x86_64 ELF"
else
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    il_fail "extracted guest artifact is missing or not an x86_64 ELF"
    il_summary
    exit 1
fi

# The default formatter exposes 8,095 usable 512-byte clusters in its
# 8,192-sector (4 MiB) volume.  The successful guest write already proves
# physical fit; retain this explicit host-side measurement so a future kernel
# growth names the point where SH5d needs a larger-volume design rather than
# silently relying on the current formatter's fixed geometry.
FAT_USABLE_BYTES=$((8095 * 512))
GUEST_ELF_BYTES=$(wc -c < "$GUEST_ELF")
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ "$GUEST_ELF_BYTES" -le "$FAT_USABLE_BYTES" ]; then
    il_pass "guest kernel is $GUEST_ELF_BYTES B, within the stock ${FAT_USABLE_BYTES} B FAT payload"
else
    il_fail "guest kernel is $GUEST_ELF_BYTES B, above the stock ${FAT_USABLE_BYTES} B FAT payload"
    il_summary
    exit 1
fi

# The image writer remains a host tool in SH5d (SH7 moves it in-guest), but it
# takes only the extracted guest ELF.  Rebuild its boot components incrementally
# in case this case was launched without a preceding normal `make iso`.
if ! (cd "$ROOT" && make mbr-dual stage2 efi) >"$ROOT/build/selfhost/sh5d-pack-deps.log" 2>&1 || \
   ! bash "$ROOT/tools/mkisoimage_dual.sh" "$GUEST_ELF" "$ROOT/build/boot/BOOTX64.EFI" "$GUEST_ISO" \
        >"$ROOT/build/selfhost/sh5d-pack.log" 2>&1; then
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    il_fail "could not pack the extracted guest kernel into its second-boot image"
    il_summary
    exit 1
fi
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
il_pass "packed the extracted guest kernel into a second-boot image"

# ---- Boot #2: only the FAT-produced ELF may supply the kernel -------------
BOOT_LOG="$IL_LOGDIR/selfhost_kernel_guest_boot.log"
IL_LAST_LOG="$BOOT_LOG"
IL_ISO="$GUEST_ISO"
il_send_prompt "uname -a"
il_send_prompt "run /bin/sysinfo"
# This is deliberately emitted by the second guest's shell, not by this host
# script: reaching and executing it is the terminal serial receipt promised by
# the plan after the FAT-produced kernel has actually booted.
il_send_prompt "echo [selfhost] kernel PASS: tcc-built kernel booted to shell"
il_send_prompt "exit"
IL_SMP=2 IL_SELFTEST=full il_run_qemu_prompt "$BOOT_LOG" 130

# Standard boot-to-shell receipts, intentionally matching the SH5c boot lane
# rather than inventing a self-host-only success signal.
il_assert_grep "$BOOT_LOG" "Hello from AuraLite OS kernel!"              "guest-built kernel kmain banner"
il_assert_grep "$BOOT_LOG" "IDT installed: 256 gates"                    "guest-built kernel IDT"
il_assert_grep "$BOOT_LOG" "TSS loaded"                                  "guest-built kernel TSS"
il_assert_grep "$BOOT_LOG" "SYSCALL/SYSRET configured"                   "guest-built kernel MSR setup"
il_assert_grep "$BOOT_LOG" "HHDM offset: 0xffff800000000000"             "guest-built kernel HHDM"
il_assert_grep "$BOOT_LOG" "\[pmm\] PASS:"                               "guest-built kernel PMM self-test"
il_assert_grep "$BOOT_LOG" "\[vmm\] PASS:"                               "guest-built kernel paging self-test"
il_assert_grep "$BOOT_LOG" "\[sched\] PASS:"                             "guest-built kernel scheduler self-test"
il_assert_grep "$BOOT_LOG" "\[vfs\] PASS:"                               "guest-built kernel VFS self-test"
il_assert_grep "$BOOT_LOG" "\[smp\] PASS:"                               "guest-built kernel SMP bring-up"
il_assert_grep "$BOOT_LOG" "\[boot\] starting init shell \(Ring 3\)"     "guest-built kernel reached Ring 3 init"
il_assert_grep "$BOOT_LOG" "auralite#"                                   "guest-built kernel reached shell prompt"
il_assert_grep "$BOOT_LOG" "AuraLite OS 0.0.1 x86_64"                    "guest-built kernel answers uname"
il_assert_grep "$BOOT_LOG" "\[perf\] boot-to-shell:"                     "guest-built kernel emits boot-to-shell receipt"
il_assert_grep "$BOOT_LOG" "'/bin/sysinfo'.*exited \(code=0\)"            "guest-built kernel runs a Ring 3 child"
# Anchor the guest's output line so the serial echo of the *input command*
# (`echo [selfhost] ...`) cannot impersonate the terminal receipt.
il_assert_grep "$BOOT_LOG" "^\\[selfhost\\] kernel PASS: tcc-built kernel booted to shell$" \
    "second guest emitted the terminal SH5d serial receipt from its shell"
il_assert_no_grep "$BOOT_LOG" "PANIC|TRIPLE FAULT|UNHANDLED EXCEPTION|STOP=" \
    "guest-built kernel has no fatal boot receipt"

if il_summary; then
    echo "[selfhost] SH5d host gate PASS: guest serial receipt verified"
else
    exit 1
fi

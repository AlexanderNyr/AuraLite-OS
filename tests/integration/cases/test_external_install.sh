#!/usr/bin/env bash
# test_external_install.sh — installing a package from outside the OS
# (SDK_PLAN phase S6).
#
# THIS CASE IS THE PROOF OF THE WHOLE SDK PLAN.
#
# Everything else shows a part working. This shows the whole route: an
# application that exists nowhere in the OS image is compiled against the
# staged SDK, wrapped into a package by the host tool, written onto a FAT32
# volume by the host, and then — with no rebuild of the OS — found, verified,
# installed and run by a booted AuraLite.
#
# If this passes, a third party can ship software for this OS.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "installing a package built outside the OS"

ROOT="$(cd ../.. && pwd)"
SDK="$ROOT/build/sdk"
MKAPKG="$ROOT/build/mkapkg"
DISK="$IL_BUILD/external-pkg.img"

# The SDK and the package tool are what a third party would be given.
if [ ! -d "$SDK" ] || [ ! -x "$MKAPKG" ]; then
    echo "  SKIP: run 'make sdk' and 'make build/mkapkg' first"
    exit 0
fi
command -v mformat >/dev/null 2>&1 || { echo "  SKIP: mtools not installed"; exit 0; }

LOG="$IL_LOGDIR/external_install.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# ---- build an application that is NOT part of this OS ----------------------
WORK="$(mktemp -d)"
cleanup() { rm -rf "$WORK"; }
trap 'cleanup; il_dump_on_error' EXIT

cat > "$WORK/thirdparty.c" <<'CEOF'
#include "stdio.h"
int main(void) {
    printf("THIRDPARTY: installed from outside the OS image\n");
    fflush(stdout);
    return 0;
}
CEOF

# Compiled and linked using ONLY the staged SDK.
# shellcheck disable=SC2086
cc -ffreestanding -fno-stack-protector -fno-pie -fno-pic -O2 \
   -I "$SDK/include" -c "$WORK/thirdparty.c" -o "$WORK/thirdparty.o" \
   >"$WORK/build.log" 2>&1 || { echo "  FAIL: compile"; cat "$WORK/build.log"; exit 1; }

ld.lld -nostdlib -static -T "$SDK/user.ld" -z max-page-size=4096 \
       "$WORK/thirdparty.o" "$SDK/lib/crt0.o" \
       --whole-archive "$SDK/lib/libaurac.a" --no-whole-archive \
       -o "$WORK/thirdparty.elf" >>"$WORK/build.log" 2>&1 \
       || { echo "  FAIL: link"; cat "$WORK/build.log"; exit 1; }

"$MKAPKG" -n thirdparty -v 0.1 -d "not part of the OS image" \
          -o "$WORK/thirdparty.apkg" "$WORK/thirdparty.elf" >/dev/null

# ---- write it to a FAT32 volume the way the host would ---------------------
#
# The volume must start at LBA 64: kernel/fs/fat32.c looks for a signature
# there and FORMATS the disk if it does not find one. A plain `mformat -i
# disk.img` puts the boot sector at LBA 0, and the kernel silently wipes it —
# which is exactly what happened the first time this was tried.
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=32 status=none
mformat -i "$DISK@@32768" -F -v AURALHCI :: 2>/dev/null
mcopy -i "$DISK@@32768" "$WORK/thirdparty.apkg" ::THIRDPTY.APKG

# ---- boot, install, run ----------------------------------------------------
il_send_delay 7
il_send "ls /fat"
il_send_delay 2
il_send "apm install /fat/THIRDPTY.APKG"
il_send_delay 4
il_send "ls /opt"
il_send_delay 1
il_send "run /opt/thirdparty"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 45 \
    -drive "file=$DISK,format=raw,if=none,id=ahcidisk" \
    -device "ahci,id=ahci0" \
    -device "ide-hd,drive=ahcidisk,bus=ahci0.0"

il_assert_grep "$LOG" "mounted FAT32"                      "the external volume mounted"
il_assert_no_grep "$LOG" "formatting default FAT32"        "the volume was NOT reformatted"
il_assert_grep "$LOG" "THIRDPTY.APKG"                      "the package is visible on it"
il_assert_grep "$LOG" "Installing thirdparty"              "apm read its header"
il_assert_grep "$LOG" "Unpacked .* to /opt/thirdparty"     "apm verified and installed it"
il_assert_grep "$LOG" "THIRDPARTY: installed from outside" "the installed program RAN"
il_assert_no_grep "$LOG" "PANIC"                           "no kernel panic"

il_summary

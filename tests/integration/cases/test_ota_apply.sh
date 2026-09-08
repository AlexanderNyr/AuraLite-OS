#!/usr/bin/env bash
# test_ota_apply.sh — OTA_PLAN O4: the exit-quality gate of the whole plan.
#
# Lane A (three boots, ONE serial log, no -no-reboot):
#   boot 0.0.1 -> `ota apply` (streamed download of a 0.0.2-ota kernel,
#   sha256-verified, A/B swap on the ESP) -> `reboot` -> boot 0.0.2-ota ->
#   `ota rollback` -> `reboot` -> boot 0.0.1 again.  Every step asserted
#   by receipt, including that stage2 never needed its KERNEL.OLD
#   fallback (the swapped KERNEL.ELF is a perfectly valid kernel).
#
# Lane B (negative): a manifest with a tampered sha256 aborts with
# `[ota] sha256 MISMATCH`, leaves no KERNEL.NEW, no KERNEL.OLD, and the
# active KERNEL.ELF untouched (rollback finds nothing to roll back to).
#
# The payload kernel is a COLD version-override build in a throwaway
# BUILD_DIR (object files do not depend on the Makefile — a warm-tree
# override can silently keep stock objects, the trap O2 recorded).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3 make

il_section "OTA O4: apply / activate / rollback over the network"

# A random port dodges stale fixture servers from aborted earlier runs
# (measured the hard way: a leaked server whose doc root was already
# deleted happily served 404s to a fresh lane for four minutes).
OTA_PORT="${OTA_PORT:-$((18000 + RANDOM % 2000))}"
DOCD="$(mktemp -d /tmp/ota-doc.XXXXXX)"
OVBUILD="$(mktemp -d /tmp/ota-kbuild.XXXXXX)"
trap 'rm -rf "$DOCD" "$OVBUILD"' EXIT

# ---- build the 0.0.2-ota payload kernel (cold, throwaway BUILD_DIR) ------
make -C "$IL_ROOT" kernel BUILD_DIR="$OVBUILD" AURALITE_VERSION=0.0.2-ota \
    > "$IL_LOGDIR/ota_apply_kernel_build.log" 2>&1
if [ $? -ne 0 ] || [ ! -s "$OVBUILD/kernel.elf" ]; then
    echo "FATAL: override kernel build failed (ota_apply_kernel_build.log)"
    exit 2
fi
cp "$OVBUILD/kernel.elf" "$DOCD/kernel-0.0.2.elf"
OV_SIZE=$(wc -c < "$DOCD/kernel-0.0.2.elf")
OV_SHA=$(sha256sum "$DOCD/kernel-0.0.2.elf" | awk '{print $1}')
printf 'version=0.0.2-ota\nurl=http://10.0.2.2:%s/kernel-0.0.2.elf\nsize=%s\nsha256=%s\n' \
    "$OTA_PORT" "$OV_SIZE" "$OV_SHA" > "$DOCD/manifest.txt"
printf 'version=0.0.2-ota\nurl=http://10.0.2.2:%s/kernel-0.0.2.elf\nsize=%s\nsha256=%s\n' \
    "$OTA_PORT" "$OV_SIZE" \
    "0000000000000000000000000000000000000000000000000000000000000000" \
    > "$DOCD/manifest-bad.txt"
echo "  [ota] payload kernel: $OV_SIZE bytes, sha256 $OV_SHA"

# ---- the fixture server (SLIRP reaches it as 10.0.2.2:$OTA_PORT) ---------
# `exec` so $HTTP_PID IS the python process (killing a subshell wrapper
# leaves the python child serving).
( cd "$DOCD" && exec python3 -m http.server "$OTA_PORT" --bind 0.0.0.0 \
      > "$IL_LOGDIR/ota_apply_http.log" 2>&1 ) &
HTTP_PID=$!
cleanup_http() {
    kill "$HTTP_PID" 2>/dev/null || true
    wait "$HTTP_PID" 2>/dev/null || true
    pkill -f "http.server $OTA_PORT" 2>/dev/null || true
}
trap 'cleanup_http; rm -rf "$DOCD" "$OVBUILD"' EXIT
sleep 0.7

# Pre-flight: prove OUR manifest is servable BEFORE booting a guest that
# would otherwise chase a stale server's 404s for four minutes.
python3 - "$OTA_PORT" "$OV_SHA" <<'PY'
import sys, urllib.request
port, sha = sys.argv[1], sys.argv[2]
try:
    body = urllib.request.urlopen(
        "http://127.0.0.1:%s/manifest.txt" % port, timeout=5).read().decode()
except Exception as e:
    print("FATAL: fixture server pre-flight failed: %s" % e)
    sys.exit(1)
if sha not in body:
    print("FATAL: fixture server served the wrong manifest")
    sys.exit(1)
PY
if [ $? -ne 0 ]; then exit 2; fi

# ---- Lane A: apply -> reboot -> 0.0.2-ota -> rollback -> reboot -> 0.0.1 --
LOGA="$IL_LOGDIR/ota_apply_a.log"
IL_LAST_LOG="$LOGA"
rm -f "$LOGA"

# Timed pipe (the O2/O3 pattern; a still-empty file would hand QEMU an
# immediate EOF).  Windows measured on the reference host: boot ~14 s,
# streamed 2.9 MiB download well inside 90 s, reboot-to-shell ~25 s.
( { sleep 14; printf 'run ota check http://10.0.2.2:%s/manifest.txt\n' "$OTA_PORT"
    sleep 8
    printf 'run ota apply http://10.0.2.2:%s/manifest.txt\n' "$OTA_PORT"
    sleep 90
    printf 'reboot\n'; sleep 26
    printf 'uname\n';  sleep 4
    printf 'run ota rollback\n'; sleep 8
    printf 'reboot\n'; sleep 26
    printf 'uname\n';  sleep 4
    printf 'exit\n'; } \
  | timeout 260 qemu-system-x86_64 \
    -drive "file=$IL_ISO,format=raw,if=none,id=btndisk,snapshot=on" \
    -device ahci,id=ahci0 -device ide-hd,drive=btndisk,bus=ahci0.0 \
    -m 512M -smp 2 -display none -serial stdio -cpu qemu64 -boot order=c \
    -netdev "user,id=net0" -device e1000,netdev=net0 \
    -fw_cfg "name=opt/auralite.selftest,string=fast" \
    > "$LOGA" 2>&1 ) || true

il_assert_grep "$LOGA" "\[ota\] manifest: version=0.0.2-ota" \
    "ota check reads the manifest over the network"
il_assert_grep "$LOGA" "\[ota\] downloading $OV_SIZE bytes into /fat/KERNEL.NEW" \
    "apply streams the payload into KERNEL.NEW"
il_assert_grep "$LOGA" "\[ota\] payload verified \(sha256 ok\)" \
    "the streamed digest matches the manifest"
il_assert_grep "$LOGA" "\[ota\] A/B swap done: KERNEL.OLD <- current, KERNEL.ELF <- new" \
    "the A/B swap happened"
il_assert_grep "$LOGA" "\[ota\] synced; reboot to activate" \
    "and the ESP is synced before the reboot"
il_assert_count "$LOGA" "Hello from AuraLite" 3 \
    "three boots live in the one serial log"
il_assert_count "$LOGA" "\[kernel\] AuraLite OS version" 3 \
    "each boot printed its kernel identity receipt"
il_assert_grep "$LOGA" "\[kernel\] AuraLite OS version 0.0.2-ota" \
    "the SECOND boot runs the downloaded kernel"
il_assert_grep "$LOGA" "AuraLite OS 0.0.2-ota x86_64" \
    "and its shell answers uname with 0.0.2-ota"
il_assert_grep "$LOGA" "\[ota\] rollback done: KERNEL.ELF <- KERNEL.OLD" \
    "rollback promotes the OLD slot"
il_assert_grep "$LOGA" "\[kernel\] AuraLite OS version 0.0.1" \
    "the THIRD boot is back on the original kernel"

# Order: the 0.0.2-ota identity must sit BETWEEN the two 0.0.1 boots.
V22=$(grep -n "AuraLite OS version 0.0.2-ota" "$LOGA" | head -1 | cut -d: -f1)
V11_FIRST=$(grep -n "AuraLite OS version 0.0.1" "$LOGA" | head -1 | cut -d: -f1)
V11_LAST=$(grep -n "AuraLite OS version 0.0.1" "$LOGA" | tail -1 | cut -d: -f1)
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ -n "$V22" ] && [ -n "$V11_FIRST" ] && [ -n "$V11_LAST" ] && \
   [ "$V11_FIRST" -lt "$V22" ] && [ "$V22" -lt "$V11_LAST" ]; then
    il_pass "version order in the log: 0.0.1 -> 0.0.2-ota -> 0.0.1"
else
    il_fail "version order in the log: 0.0.1 -> 0.0.2-ota -> 0.0.1 (got first-0.0.1=$V11_FIRST ota=$V22 last-0.0.1=$V11_LAST)"
fi

il_assert_no_grep "$LOGA" "falling back to KERNEL.OLD" \
    "stage2 never needed its fallback (the swapped kernel is valid)"
il_assert_no_grep "$LOGA" "connection silent|connection cut|sha256 MISMATCH" \
    "no transfer or digest failure anywhere in lane A"
il_assert_no_grep "$LOGA" "formatting" \
    "no formatter ran in any of the three boots (the O1 invariant)"

# ---- Lane B: tampered sha256 -----------------------------------------------
LOGB="$IL_LOGDIR/ota_apply_b.log"
IL_LAST_LOG="$LOGB"
rm -f "$LOGB"

( { sleep 14; printf 'run ota status\n'; sleep 4
    printf 'run ota apply http://10.0.2.2:%s/manifest-bad.txt\n' "$OTA_PORT"
    sleep 55
    printf 'run ota status\n'; sleep 4
    printf 'run ota rollback\n'; sleep 4
    printf 'exit\n'; } \
  | timeout 140 qemu-system-x86_64 \
    -drive "file=$IL_ISO,format=raw,if=none,id=btndisk,snapshot=on" \
    -device ahci,id=ahci0 -device ide-hd,drive=btndisk,bus=ahci0.0 \
    -m 512M -smp 2 -display none -serial stdio -no-reboot -cpu qemu64 \
    -boot order=c \
    -netdev "user,id=net0" -device e1000,netdev=net0 \
    -fw_cfg "name=opt/auralite.selftest,string=fast" \
    > "$LOGB" 2>&1 ) || true

il_assert_grep "$LOGB" "\[ota\] sha256 MISMATCH" \
    "the tampered manifest is rejected by the digest check"
il_assert_grep "$LOGB" "\[ota\] aborted; KERNEL.ELF is untouched" \
    "the abort receipt prints"
il_assert_no_grep "$LOGB" "A/B swap done" \
    "no swap happened on the negative lane"
il_assert_grep "$LOGB" "\[ota\] no KERNEL.OLD to roll back to" \
    "rollback confirms no OLD slot was ever created"

# The active kernel's digest is identical before and after the failed apply:
# ota status prints it twice; the two lines must match.
STATS=$(grep -o "active  /fat/KERNEL.ELF: [0-9]* bytes, sha256 [0-9a-f]*" "$LOGB")
N_STAT=$(printf '%s\n' "$STATS" | grep -c . )
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ "$N_STAT" -eq 2 ] && [ "$(printf '%s\n' "$STATS" | head -1)" = "$(printf '%s\n' "$STATS" | tail -1)" ]; then
    il_pass "the active KERNEL.ELF digest is byte-identical before and after the failed apply"
else
    il_fail "the active KERNEL.ELF digest changed across the failed apply (or status printed $N_STAT lines)"
fi

il_summary

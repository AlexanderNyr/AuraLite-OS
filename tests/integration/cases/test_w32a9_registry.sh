#!/usr/bin/env bash
# test_w32a9_registry.sh — W32APP_PLAN.md phase W32A-9 gate.
#
# The phase's claim, in guest: advapi32.dll is a real registry engine
# (the W32HIVE1 hive format) plus the security/CryptoAPI remainder, each
# honestly classed.  One NASM PE (w32a9_registry.asm) drives the whole
# surface: the six value types round-tripped, MORE_DATA, case-insensitive
# opens, RegEnumKey insertion order, QueryInfoKey counts, the delete
# contracts (values normal, keys-with-subkeys refused), the HKLM
# seed + the writable-only-under-\Software policy, the HKCR HKCU-wins
# merge with writes landing in the HKCU half, the single-user SID model
# (AllocateAndInitializeSid/CopySid/EqualSid/CheckTokenMembership), the
# SECURITY_DESCRIPTOR builder set at the x64 field offsets, the hash-only
# CryptoAPI (SHA-256/512 + SHA3-256 against the public "abc" vectors,
# the ragged-feed == one-shot buffering contract, NTE_BAD_ALGID
# refusals, provider lifetime), RtlGenRandom, GetUserName/IsTextUnicode,
# and the nine FAIL-CLEAN finals with their exact codes.
#
# This case runs TWO boots against one persistent AHCI scratch disk:
#   boot 1  the fixture writes the cross-boot marker
#           (A9-PERSIST-WRITE-OK) and passes every section;
#   boot 2  the same fixture reads the marker back with the exact
#           bytes (A9-PERSIST-READ-OK) -- the hive survived the power
#           cycle -- then the shell tears the hive (`write /disk/w32hive
#           GARBAGE`, an O_TRUNC text write) and a SECOND fixture run
#           must refuse loudly: A9-CORRUPT-OK, every Reg claim void,
#           still exit 78.
#
# Markers the fixture prints (12 sections):
#   A9-HIVE-OK        CRUD, types, MORE_DATA, enum, QueryInfoKey,
#                     deletes, flush
#   A9-PERSIST-...    the cross-boot marker (write in boot 1, read in
#                     boot 2)
#   A9-HKLM-OK        seed answers; \Software writable; else refused
#   A9-HKCR-OK        merged view; HKCU wins; writes land in HKCU
#   A9-SID-OK         build/copy/equal/length/free + membership model
#   A9-SD-OK          descriptor builders; control bits; field offsets
#   A9-CRYPTO-OK      vectors, ragged feed, refusals, lifetime
#   A9-RND-OK         two draws differ
#   A9-IDENT-OK       GetUserName A+W; IsTextUnicode both ways + BOM
#   A9-FAILCLEAN-OK   the nine documented refusals, exact codes
#   A9-CORRUPT-OK     the torn-write latch (second boot, second run)
#   W32A9-REGISTRY-OK final marker
#
# Exit code 78 is the fixture's own success path; 79 any failed check; 1
# the loader's refusal.  The host side (tests/unit/test_w32_a9.c, 360
# checks) pins the engine in isolation; this gate pins it through the
# loader, the bind table, the guest libc and diskfs.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32A-9 integration: registry hive + ADVAPI32"

a9_pass() { IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1)); il_pass "$@"; }
a9_fail() { IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1)); il_fail "$@"; }

# One scratch disk shared by both boots: no snapshot=, so what boot 1
# writes is what boot 2 reads.  Blank and unpartitioned -- diskfs
# auto-formats its AUFS superblock on first mount.
DISK="$IL_BUILD/w32a9_hive.img"
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none

# ---- Boot 1: full pass + the marker write ------------------------------------
LOG1="$IL_LOGDIR/w32a9_registry_boot1.log"
IL_LAST_LOG="$LOG1"
trap il_dump_on_error EXIT

il_send_delay 10
il_send "run /apps/w32run /tests/w32a9_registry.exe"
il_send_delay 16
il_send "exit"

IL_SELFTEST=fast il_run_qemu "$LOG1" 240 \
    -drive "file=$DISK,format=raw,if=none,id=hivedisk" \
    -device ahci,id=ahci0 \
    -device ide-hd,drive=hivedisk,bus=ahci0.0

il_assert_grep "$LOG1" "w32run: .*w32a9_registry\.exe mapped at 0x[0-9a-f]+, [0-9][0-9]* import\(s\) bound" \
    "boot1: fixture PE mapped and imports resolved"

for m in HIVE HKLM HKCR SID SD CRYPTO RND IDENT FAILCLEAN; do
    il_assert_grep "$LOG1" "A9-$m-OK" \
        "boot1: the fixture passed its $m section"
done
il_assert_grep "$LOG1" "A9-PERSIST-WRITE-OK" \
    "boot1: the cross-boot marker was written"
il_assert_grep "$LOG1" "W32A9-REGISTRY-OK" \
    "boot1: the fixture reported the full sequence succeeded"
il_assert_grep "$LOG1" "'/apps/w32run' \(tid [0-9]+\) exited \(code=78\)" \
    "boot1: the PE exited with its success status (78)"

# The hive must have landed on the persistent disk, not /tmp.
il_assert_no_grep "$LOG1" "using /tmp/w32hive" \
    "boot1: the hive chose /disk (persistent), not /tmp"
il_assert_no_grep "$LOG1" "hive .*cannot open for write|hive .*write failed" \
    "boot1: every hive save succeeded"
il_assert_no_grep "$LOG1" "A9-[A-Z-]*-FAIL" \
    "boot1: no section reported a failure"
il_assert_no_grep "$LOG1" "unresolved import" \
    "boot1: no import was left unbound"
il_assert_no_grep "$LOG1" "UNHANDLED EXCEPTION|kernel panic|Page Fault" \
    "boot1: no kernel fault"
il_assert_grep "$LOG1" "Goodbye!" \
    "boot1: shell survived and exited cleanly"

# ---- Boot 2: marker read-back, then the torn write ----------------------------
LOG2="$IL_LOGDIR/w32a9_registry_boot2.log"
IL_LAST_LOG="$LOG2"

il_send_delay 10
il_send "run /apps/w32run /tests/w32a9_registry.exe"
il_send_delay 16
il_send "write /disk/w32hive GARBAGE"
il_send_delay 2
il_send "run /apps/w32run /tests/w32a9_registry.exe"
il_send_delay 10
il_send "exit"

IL_SELFTEST=fast il_run_qemu "$LOG2" 300 \
    -drive "file=$DISK,format=raw,if=none,id=hivedisk" \
    -device ahci,id=ahci0 \
    -device ide-hd,drive=hivedisk,bus=ahci0.0

il_assert_grep "$LOG2" "w32run: .*w32a9_registry\.exe mapped at 0x[0-9a-f]+, [0-9][0-9]* import\(s\) bound" \
    "boot2: fixture PE mapped again"

# Run 1 of boot 2: the settings survived the power cycle.
il_assert_grep "$LOG2" "A9-PERSIST-READ-OK" \
    "boot2: the marker survived the reboot (hive persisted)"
il_assert_no_grep "$LOG2" "A9-PERSIST-WRITE-OK" \
    "boot2: run 1 read the marker, it did not rewrite a lost hive"
for m in HIVE HKLM HKCR SID SD CRYPTO RND IDENT FAILCLEAN; do
    il_assert_grep "$LOG2" "A9-$m-OK" \
        "boot2: the fixture passed its $m section again"
done
il_assert_grep "$LOG2" "W32A9-REGISTRY-OK" \
    "boot2: run 1 reported the full sequence succeeded"

# Run 2 of boot 2: the torn write must fail loud, not reset silently.
il_assert_grep "$LOG2" "A9-CORRUPT-OK" \
    "boot2: the torn hive was detected and refused"
il_assert_grep "$LOG2" "bad magic \(torn write\)" \
    "boot2: the engine named the corruption"
# Two successful fixture exits in this boot (run 1 + the corrupt run).
il_assert_count "$LOG2" "'/apps/w32run' \(tid [0-9]+\) exited \(code=78\)" 2 \
    "boot2: both fixture runs exited 78 (the corrupt path is a controlled exit)"
il_assert_no_grep "$LOG2" "exited \(code=79\)" \
    "boot2: no fixture run hit a failed check"
il_assert_no_grep "$LOG2" "A9-[A-Z-]*-FAIL" \
    "boot2: no section reported a failure"
il_assert_no_grep "$LOG2" "UNHANDLED EXCEPTION|kernel panic|Page Fault" \
    "boot2: no kernel fault"
il_assert_grep "$LOG2" "Goodbye!" \
    "boot2: shell survived and exited cleanly"

il_summary

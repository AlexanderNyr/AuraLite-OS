#!/usr/bin/env bash
# test_posix2024_conf.sh — POSIX.1-2024 conformance suite, guest layer (Q12).
#
# POSIX2024_PLAN.md phase Q12.  Boots AuraLite, runs /tests/conformtest
# (userspace/tests/conformtest/conformtest.c), and asserts the suite's
# syscall-backed markers end to end on the real kernel:
#   - AT-family on tmpfs: openat/mkdirat/fstatat (S_IFREG type bits and
#     AT_SYMLINK_NOFOLLOW)/faccessat/renameat/unlinkat (file + AT_REMOVEDIR)
#     /readlinkat + a cwd-relative openat via AT_FDCWD (Q5 gate hole closed);
#   - posix_spawn with explicit argv/envp — argv_echo prints ARGV_ECHO
#     markers proving the child received argc=3, argv[1]=q12, argv[2]="sp
#     ace", env[0]=A=Q12, and argv_terminated=1;
#   - mqueue send/receive round-trip; clock_nanosleep TIMER_ABSTIME;
#     getentropy bounds; scandir alphasort/versionsort ordering;
#   - named semaphores fail with the documented partial errno (ENOSYS) and
#     process-private unnamed semaphores still work.
#
# This case is the guest half of the Q12 gate: the host half
# (tests/posix2024/run_host.sh) runs as part of `make test-unit`.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "POSIX.1-2024 conformance suite (Q12)"

LOG="$IL_LOGDIR/posix2024_conf.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run conformtest"
il_send_delay 60
il_send "exit"

il_run_qemu "$LOG" 120

# ---- AT-family on tmpfs ----
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS at-tmpfs: mkdir" \
    "mkdir() creates a directory on tmpfs"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS at-tmpfs: openat(O_CREAT) creates" \
    "openat(O_CREAT) creates a file on tmpfs"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS at-tmpfs: write to openat fd" \
    "write() through an openat descriptor works"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS at-tmpfs: read back payload" \
    "read() back through an openat descriptor works"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS at-tmpfs: cwd-relative openat via AT_FDCWD" \
    "openat(AT_FDCWD, relative) resolves against the cwd"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS at-tmpfs: fstatat st_mode has S_IFREG (S_ISREG)" \
    "fstatat st_mode carries POSIX type bits (S_ISREG works)"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS at-tmpfs: mkdirat creates subdir" \
    "mkdirat creates a subdirectory"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS at-tmpfs: faccessat(F_OK) on dir" \
    "faccessat(F_OK) works"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS at-tmpfs: renameat file" \
    "renameat renames a file"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS at-tmpfs: old name gone after rename" \
    "old path is ENOENT after renameat"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS at-tmpfs: symlink to renamed.txt" \
    "symlink() works on tmpfs"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS at-tmpfs: readlinkat returns target" \
    "readlinkat returns the symlink target"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS at-tmpfs: fstatat AT_SYMLINK_NOFOLLOW sees the link itself" \
    "fstatat(AT_SYMLINK_NOFOLLOW) reports the link, not the target"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS at-tmpfs: unlinkat removes file" \
    "unlinkat removes a file"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS at-tmpfs: unlinkat AT_REMOVEDIR removes empty dir" \
    "unlinkat(AT_REMOVEDIR) removes an empty directory"

# ---- AT-family on FAT32 (skipped honestly when /fat is not mounted) ----
il_assert_grep "$LOG" "CONFORMTEST PASS at-fat|CONFORMTEST SKIP at-fat" \
    "AT-family on FAT32 passes or is skipped with /fat unmounted"

# ---- posix_spawn argv/envp ----
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS spawn: posix_spawn returns 0" \
    "posix_spawn() succeeds"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS spawn: child exit status is 0" \
    "posix_spawn child exits cleanly"
il_assert_grep_fixed "$LOG" "ARGV_ECHO argc=3" \
    "spawned child sees argc=3"
il_assert_grep_fixed "$LOG" "ARGV_ECHO argv[1]=q12" \
    "spawned child sees argv[1]=q12"
il_assert_grep_fixed "$LOG" "ARGV_ECHO argv[2]=sp ace" \
    "spawned child sees argv[2]=\"sp ace\""
il_assert_grep_fixed "$LOG" "ARGV_ECHO argv_terminated=1" \
    "spawned child argv is NULL-terminated"
il_assert_grep_fixed "$LOG" "ARGV_ECHO env[0]=A=Q12" \
    "spawned child sees the custom envp entry A=Q12"

# ---- mqueue round-trip ----
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS mqueue: mq_open(O_CREAT|O_EXCL)" \
    "mq_open(O_CREAT|O_EXCL) works"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS mqueue: mq_receive round-trips the payload" \
    "mq_send/mq_receive round-trip on one descriptor"

# ---- semaphores ----
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS sem: named sem_open is the documented partial (ENOSYS)" \
    "named sem_open fails with the documented partial errno ENOSYS"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS sem: sem_trywait EAGAIN when empty" \
    "sem_trywait reports EAGAIN on an empty semaphore"

# ---- clock_nanosleep ----
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS clockns: clock_nanosleep(TIMER_ABSTIME) returns 0" \
    "clock_nanosleep(TIMER_ABSTIME) returns 0"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS clockns: slept >= ~200 ms (abstime honoured)" \
    "clock_nanosleep actually sleeps until the absolute deadline"

# ---- getentropy ----
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS entropy: getentropy(16) succeeds" \
    "getentropy(16) succeeds"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS entropy: length > 256 fails with EIO" \
    "getentropy length > 256 fails with EIO"

# ---- scandir ordering ----
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS scandir: alphasort orders fA,fB,fc10,fc2" \
    "scandir(alphasort) orders fA,fB,fc10,fc2"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS scandir: versionsort orders fA,fB,fc2,fc10" \
    "scandir(versionsort) orders fA,fB,fc2,fc10"

# ---- suite summary and negatives ----
il_assert_grep_fixed "$LOG" "CONFORMTEST ALL PASS" \
    "conformtest reports ALL PASS"
il_assert_no_grep_fixed "$LOG" "CONFORMTEST FAIL" \
    "no conformtest check failed"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"

il_summary

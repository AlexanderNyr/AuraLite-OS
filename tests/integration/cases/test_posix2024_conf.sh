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
#   - named semaphores WORK (the ENOSYS documented-partial closed when
#     MAP_SHARED landed -- this suite caught the stale expectation on
#     its first CI run) and process-private unnamed ones still work.
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
il_send_delay 320
il_send "exit"

il_run_qemu "$LOG" 480

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

# ---- Q15: mq_notify + sigevent delivery ----
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS mq_notify: register SIGEV_SIGNAL" \
    "mq_notify(SIGEV_SIGNAL) registers"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS mq_notify: second registration gives EBUSY" \
    "a second registration on the same queue gives EBUSY"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS mq_notify: SIGEV_SIGNAL delivered on empty->non-empty" \
    "SIGEV_SIGNAL is delivered on the empty->non-empty transition"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS mq_notify: re-armed after drain (second delivery)" \
    "the notification re-arms after the queue is drained"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS mq_notify: deregister with NULL" \
    "mq_notify(mq, NULL) deregisters"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS mq_notify: no delivery after deregistration" \
    "no delivery after deregistration"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS mq_notify: register SIGEV_THREAD" \
    "mq_notify(SIGEV_THREAD) registers"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS mq_notify: SIGEV_THREAD runs the notification function" \
    "SIGEV_THREAD runs the notification function on a fresh pthread"

# ---- semaphores ----
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS sem: named sem_open works (former documented partial, closed)" \
    "named sem_open works (the former partial, closed by measurement)"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS sem: named trywait on empty fails EAGAIN" \
    "named semaphore counts correctly (EAGAIN when drained)"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS sem: named close + unlink" \
    "named semaphore lifecycle completes"
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

# ---- Q13: AT-family completion ----
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS link: link() creates a second name" \
    "link() creates a hard link on tmpfs"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS link: write via one name visible via the other" \
    "hard-linked names share the data block"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS link: st_nlink == 2" \
    "hard link reports st_nlink == 2"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS link: both names share the inode" \
    "hard-linked names share the inode"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS link: cross-device gives EXDEV" \
    "cross-device link gives EXDEV"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS linkat: creates via AT_FDCWD" \
    "linkat(AT_FDCWD, ...) works"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS link: unlink of one name keeps the other alive" \
    "unlinking one hard link keeps the other name alive"
il_assert_grep "$LOG" "CONFORMTEST PASS link: FAT32 gives EPERM|CONFORMTEST SKIP at-fat" \
    "FAT32 link gives EPERM, or /fat is unmounted (skip)"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS symlinkat: creates a link" \
    "symlinkat() creates a symbolic link"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS symlinkat: readlinkat sees the target" \
    "symlinkat target is readable via readlinkat"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS mknod: mkfifoat creates a FIFO" \
    "mkfifoat() creates a FIFO"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS mknod: FIFO type in st_mode" \
    "FIFO type is visible in st_mode"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS mknod: mknodat creates a regular file" \
    "mknodat() creates a regular file"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS mknod: device node gives ENOSYS (no devfs backing)" \
    "device nodes honestly report ENOSYS"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS utimens: utimensat sets explicit times" \
    "utimensat() sets explicit times"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS utimens: atime read back (within 1s)" \
    "utimensat atime reads back through stat"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS utimens: UTIME_NOW/UTIME_OMIT accepted" \
    "utimensat handles UTIME_NOW/UTIME_OMIT"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS utimens: mtime kept (UTIME_OMIT)" \
    "UTIME_OMIT leaves mtime untouched"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS utimens: futimens on an fd" \
    "futimens() works on an fd"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS utimens: futimens mtime read back via fstat" \
    "futimens mtime reads back via fstat"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS fdopendir: dirfd() returns the fd" \
    "fdopendir dirfd() matches the original fd"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS fdopendir: readdir lists the entries" \
    "fdopendir readdir lists directory entries"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS fdopendir: opendir yields a real dirfd" \
    "opendir stream carries a real fd for dirfd()"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS fdopendir: non-directory fd gives ENOTDIR" \
    "fdopendir on a non-directory fd gives ENOTDIR"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS fexecve: child exit status is 0" \
    "fexecve() execs a binary by fd"
il_assert_grep_fixed "$LOG" "ARGV_ECHO argv[1]=fex" \
    "fexecve child sees argv[1]=fex"
il_assert_grep_fixed "$LOG" "ARGV_ECHO env[0]=B=fex" \
    "fexecve child sees env[0]=B=fex"

# ---- Q16: Issue-8 tail ----
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS q16: pselect wakes on signal with EINTR (20/20)" \
    "pselect installs the mask atomically and a pending signal wakes it (EINTR)"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS q16: ppoll reports POLLIN" \
    "ppoll reports POLLIN on a readable pipe"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS q16: ppoll interrupted by signal (EINTR)" \
    "ppoll with a mask is interrupted by a signal (EINTR)"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS q16: getrandom(32) returns 32" \
    "getrandom fills the buffer"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS q16: two getrandom streams differ" \
    "two getrandom streams differ"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS q16: getrandom unknown flags -> EINVAL" \
    "getrandom rejects unknown flags with EINVAL"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS q16: sig2str/str2sig round-trips every named signal" \
    "sig2str/str2sig round-trip every named signal"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS q16: str2sig(\"NOSUCH\") -> EINVAL" \
    "str2sig rejects an unknown name with EINVAL"

# ---- Q14: System V IPC ----
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS sysv: semget(IPC_PRIVATE, 1) creates" \
    "semget(IPC_PRIVATE) creates a semaphore set"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS sysv: semctl(GETVAL) == 1" \
    "semctl(SETVAL/GETVAL) round-trips"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS sysv: semop(P) decrements" \
    "semop(P) decrements the semaphore"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS sysv: semop(IPC_NOWAIT) on insufficient -> EAGAIN" \
    "semop with IPC_NOWAIT fails with EAGAIN instead of blocking"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS sysv: shmget(4096) creates" \
    "shmget(IPC_PRIVATE, 4096) creates a segment"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS sysv: shm write/read round-trip" \
    "attached shm is readable/writable"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS sysv: shmctl(IPC_STAT) reads metadata" \
    "shmctl(IPC_STAT) returns segment metadata"
# The kernel's own serial output ([thread] reaped ...) can interleave with the
# guest's stdout mid-line, so a full fixed line is flaky to grep for.  The
# marker substring "counter == 400 (no lost increments)" always survives the
# interleave as a contiguous run, so assert on that instead.
il_assert_grep_fixed "$LOG" "counter == 400 (no lost increments)" \
    "forked pair reaches exactly 400 protected increments (sem+shm)"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS sysv: msgget(IPC_PRIVATE) creates" \
    "msgget(IPC_PRIVATE) creates a queue"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS sysv: msgrcv(typ=0) FIFO returns mtype 1" \
    "msgrcv with msgtyp=0 returns the FIFO message"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS sysv: msgrcv(typ=5) returns mtype 5 'five-a'" \
    "msgrcv with msgtyp>0 returns an exact mtype match"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS sysv: msgrcv(typ=-5) returns mtype 5 'five-b'" \
    "msgrcv with msgtyp<0 returns first mtype <= -msgtyp"
il_assert_grep_fixed "$LOG" "CONFORMTEST PASS sysv: msgrcv on empty queue -> ENOMSG" \
    "msgrcv on an empty queue fails with ENOMSG"

# ---- suite summary and negatives ----
il_assert_grep_fixed "$LOG" "CONFORMTEST ALL PASS" \
    "conformtest reports ALL PASS"
il_assert_no_grep_fixed "$LOG" "CONFORMTEST FAIL" \
    "no conformtest check failed"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"

il_summary

#!/usr/bin/env bash
# test_aspace_refs.sh — RESIDUE3 C2: refcounted address spaces, and every
# zombie reaped.
#
# The gate is a real thread group: /tests/fpustress creates four CLONE_VM
# threads inside one process.  Before this phase the tree
#   - freed an address space when the NON-pthread TCB was reaped (the
#     `!z->is_pthread` guard), whichever thread actually died last, and
#   - left the pthread zombies queued forever (no waiter: clone() threads are
#     not in the parent/child table and pthread_join() synchronises on the
#     clear_tid futex, not on wait()), so a TCB + a 32 KiB kernel stack leaked
#     per thread.
#
# After it, the boot log carries the receipt for both halves:
#   [thread] reaped 'pthread' (tid 9, 0 frames, aspace refs 4)   <- not freed
#   ...
#   [thread] reaped 'pthread' (tid 12, 0 frames, aspace refs 1)
#   [thread] reaped '/tests/fpustress' (tid 8, 1046 frames, aspace refs 0)
# i.e. the LAST owner frees the page tables, and it is not the one the old
# code would have picked.
#
# Run on -smp 4 so the reaping is genuinely cross-CPU.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "RESIDUE3 C2: address-space refcounts and total zombie reaping"

IL_SMP=4
LOG="$IL_LOGDIR/aspace_refs.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 12
il_send "fpustress"
il_send_wait "FPSTRESS PASS" 40
il_send "exit"

il_run_qemu "$LOG" 80

# 1. the boot census names the table's state (a receipt, not an inference)
il_assert_grep "$LOG" "\[aspace\] live [0-9]+, shared [0-9]+, last-puts [0-9]+" \
    "the address-space table prints its census at boot"

# 2. the thread group ran and the address space was SHARED
il_assert_grep "$LOG" "FPSTRESS PASS" \
    "the 4-thread FPU stress ran to completion"

# 3. every pthread zombie was reaped, and its space was NOT freed (refs > 0)
il_assert_count "$LOG" "\[thread\] reaped 'pthread' \(tid [0-9]+, 0 frames, aspace refs [1-9][0-9]*\)" 4 \
    "each of the 4 pthreads is reaped while the space is still shared"

# 4. the leader, reaped LAST, is the one that frees: frames > 0, refs == 0.
#    This is the line the old code could not produce in this order.
il_assert_grep "$LOG" "\[thread\] reaped '/tests/fpustress' \(tid [0-9]+, [1-9][0-9]* frames, aspace refs 0\)" \
    "the last owner of the shared space frees it (frames > 0, refs 0)"

# 5. no double free, no underflow: the table never authorises a second release
il_assert_no_grep "$LOG" "double-puts [1-9]" \
    "no address space was released twice"

# 6. and the invariants of the phase hold
il_assert_no_grep "$LOG" "PANIC"                         "no kernel panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"           "no unhandled exception"
il_assert_no_grep "$LOG" "refs 4294967"                  "no refcount underflow"

il_summary

#!/usr/bin/env bash
# tests/integration/lib/lib.sh — shared helpers for QEMU integration tests.
#
# Public API:
#   il_init                            — set up ROOT, ISO, BUILD, LOG, etc.
#                                        env knobs: IL_NIC (default e1000),
#                                        IL_SMP (default 2), IL_CPU (default
#                                        qemu64; e.g. "qemu64,+rdrand,+rdseed")
#   il_have <bin>...                   — assert helper binaries exist
#   il_make_disk <path> <MiB> <magic>  — create raw image with magic in sector 0
#   il_run_qemu <log> <timeout_s> [-- <extra qemu args>]
#                                        boot the ISO; serial → <log>;
#                                        stdin gets shell commands via il_send().
#   il_run_qemu_prompt <log> <timeout> [extra qemu args]
#                                        run il_send_prompt() commands only
#                                        after a fresh auralite# prompt, and
#                                        require every `run` command to exit 0
#                                        (IL_PROMPT_CHECK_EXIT=0 disables).
#                                        Sets IL_PROMPT_DRIVER_LOG to the
#                                        transport's own progress/diagnostic
#                                        log; <log> stays pure serial output.
#   il_send "<text>"                   — queue text to be sent into serial
#   il_send_prompt "<text>"            — queue text for prompt-aware serial
#                                        (call BEFORE il_run_qemu).
#   il_send_delay <secs>               — queue a sleep between commands.
#   il_send_wait <fixed-str> [secs]    — queue a content gate: hold further
#                                        lines until the string hits the log.
#   il_assert_grep <log> "<pattern>" "<human description>"
#   il_assert_no_grep <log> "<pattern>" "<human description>"
#   il_assert_count <log> "<pattern>" <min>  — at least N matches.
#   il_pass <msg>  / il_fail <msg>     — report result.
#   il_summary                         — print pass/fail counters; exit code.
#
# This file is intentionally Bash-only (no exotic deps) and POSIX-friendly
# where possible so the same tests run on Debian, Ubuntu, Arch, and macOS
# (with `brew install qemu xorriso`).

set -u
# NOTE: do NOT `set -e` in the library — individual asserts must keep going.

# -------- colors --------
if [ -t 1 ] && [ "${NO_COLOR:-}" = "" ]; then
    C_RED=$'\033[31m'
    C_GREEN=$'\033[32m'
    C_YELLOW=$'\033[33m'
    C_BLUE=$'\033[34m'
    C_BOLD=$'\033[1m'
    C_DIM=$'\033[2m'
    C_RESET=$'\033[0m'
else
    C_RED=""; C_GREEN=""; C_YELLOW=""; C_BLUE=""; C_BOLD=""; C_DIM=""; C_RESET=""
fi

# -------- counters --------
IL_PASS_COUNT=0
IL_FAIL_COUNT=0
IL_ASSERT_COUNT=0
IL_FAILED_ASSERTS=()
IL_INPUT_QUEUE=""    # accumulated shell input; flushed via il_run_qemu
IL_PROMPT_QUEUE=""   # commands gated by a fresh prompt via il_run_qemu_prompt

# -------- init --------
il_init() {
    IL_ROOT="${IL_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
    IL_BUILD="${IL_BUILD:-$IL_ROOT/build}"
    IL_ISO="${IL_ISO:-$IL_BUILD/auralite.iso}"
    IL_LOGDIR="${IL_LOGDIR:-$IL_BUILD/integration-logs}"
    mkdir -p "$IL_LOGDIR"

    # QEMU binary (overridable for cross / non-x86_64 hosts).
    IL_QEMU="${IL_QEMU:-qemu-system-x86_64}"

    # NIC model on the user/SLIRP NAT (overridable so a test can exercise an
    # alternative backend, e.g. IL_NIC=virtio-net-pci for the virtio-net path).
    IL_NIC="${IL_NIC:-e1000}"

    # RESIDUE2 T5: full netdev replacement for the L2-lab cases.  When set,
    # it replaces the default SLIRP line entirely (the -device still pairs
    # with netdev id net0, so keep id=net0 in the replacement); unset means
    # the historical user/SLIRP NAT plus any IL_NETDEV_OPTS additions.
    IL_NETDEV="${IL_NETDEV:-}"

    # CPU model (overridable so a test can toggle CPU features, e.g.
    # IL_CPU="qemu64,+rdrand,+rdseed" for the N0 hardware-entropy path).
    IL_CPU="${IL_CPU:-qemu64}"

    # Build the boot image on demand.  `make iso` must leave the canonical
    # build/auralite.iso artefact behind; fail immediately instead of running
    # every test against a missing file and producing misleading assertions.
    if [ ! -f "$IL_ISO" ]; then
        echo "${C_YELLOW}[lib] boot image missing — running 'make iso'…${C_RESET}"
        if ! ( cd "$IL_ROOT" && make iso >/dev/null ); then
            echo "${C_RED}[lib] failed to build boot image: $IL_ISO${C_RESET}" >&2
            exit 2
        fi
    fi
    if [ ! -f "$IL_ISO" ]; then
        echo "${C_RED}[lib] make iso did not create boot image: $IL_ISO${C_RESET}" >&2
        exit 2
    fi
}

# -------- prereq check --------
il_have() {
    local missing=0 b
    for b in "$@"; do
        if ! command -v "$b" >/dev/null 2>&1; then
            echo "${C_RED}[lib] missing required binary: $b${C_RESET}" >&2
            missing=1
        fi
    done
    [ "$missing" -eq 0 ]
}

# -------- disk image helpers --------
# il_make_disk <path> <size_MiB> <magic_string_8B>
il_make_disk() {
    local path="$1" size_mb="$2" magic="$3"
    [ -f "$path" ] && return 0
    mkdir -p "$(dirname "$path")"
    dd if=/dev/zero of="$path" bs=1M count="$size_mb" status=none
    python3 - "$path" "$magic" <<'PY'
import sys
path, magic = sys.argv[1], sys.argv[2].encode()
sector = bytearray(512)
sector[0:len(magic)] = magic
sector[510] = 0x55
sector[511] = 0xAA
with open(path, "r+b") as f:
    f.write(sector)
PY
}

# -------- input queue (sent to QEMU's serial stdin) --------
il_send() { IL_INPUT_QUEUE+="$1"$'\n'; }
il_send_raw() { IL_INPUT_QUEUE+="$1"; }
# Long compiler jobs cannot be driven with guessed sleeps: while tcc owns the
# polling serial path, a later line can vanish before the shell reads it.
# Keep this queue separate from the legacy sleep-based one so existing cases
# retain their exact timing semantics.
il_send_prompt() { IL_PROMPT_QUEUE+="$1"$'\n'; }
il_send_delay() {
    # Emit a literal sleep marker. We turn it into a real sleep at run time.
    IL_INPUT_QUEUE+=$'\x1b__SLEEP__'"$1"$'\n'
}
# RESIDUE2 CI fix: content-gated wait.  Emits a marker the feeder turns
# into "poll the live serial log until this FIXED string shows up (at
# most N seconds)".  Use this instead of a guessed il_send_delay when
# the guest prints a receipt for what you are waiting on — under a
# loaded CI runner the constant sleep is either wasted time (too long)
# or a race (too short); CI run 92244125363 lost test_stopped's whole
# Ctrl+Z leg to a 25 s guess that local hardware met in 18 s.
il_send_wait() {
    local pat="$1" secs="${2:-60}"
    IL_INPUT_QUEUE+=$'\x1b__WAIT__'"$secs"$'\x1f'"$pat"$'\n'
}

# Internal: feed the queue into a process, honouring sleep markers.
#
# RESIDUE2 T9 (flakiness box): the feed is now CONSUMPTION-gated, not
# clock-gated.  The legacy driver sent each line after a blind 0.20 s
# gap, which races two measured guest quirks: the polling UART drops
# input typed while a spawned child still owns the CPU, and a line
# arriving mid-prompt can lose its first character to the echo.  The
# fix paces by the ONLY signal that proves the shell is ready for
# more: the "auralite#" prompt itself.  Before each line the feeder
# waits (polling the live serial log) until a NEW prompt appeared
# after the previous one -- i.e. the previous command finished and
# the shell re-entered its read loop.  The first line waits for the
# shell's FIRST prompt (up to 90 s: TCG through the full selftest),
# which also retires the "guess the boot time" class of races.
# Capped per line (15 s) so a command that legitimately never returns
# to the prompt (GUI apps, `exit`) degrades to exactly the legacy
# behaviour.  IL_FEED_SYNC=0 restores the old driver wholesale.
#
# RESIDUE2 CI fix (run 92244125363): two hardening changes.
# (1) A broken output pipe (QEMU died or was killed by the budget)
#     aborts the feed loop immediately.  Before, every remaining
#     queued line first burned its full 15 s gate cap and then
#     printed "write error: Broken pipe" — test_shell_all spent 164 s
#     feeding a dead guest after its 50 s budget expired, and the
#     shard looked like an assertion failure instead of a budget one.
# (2) il_send_wait() content markers: a case can wait for a RECEIPT
#     in the serial log (fixed string) instead of guessing seconds —
#     the same principle as the prompt gate, for flows the prompt
#     cannot pace (a foreground Ctrl+Z handshake, a long child that
#     prints progress markers).
_il_prompt_count() {
    local n
    n=$(grep -ac -- "auralite#" "$1" 2>/dev/null) || n=0
    [ -n "$n" ] || n=0
    echo "$n"
}

_il_feed_queue() {
    local line log="${IL_FEED_LOG:-}" synced=0 base=0 n
    if [ "${IL_FEED_SYNC:-1}" = "1" ] && [ -n "$log" ]; then
        synced=1
        base=$(_il_prompt_count "$log")
        if [ "$base" -eq 0 ]; then
            # Boot not shell-ready yet: wait for the first prompt.
            for _ in $(seq 1 900); do
                n=$(_il_prompt_count "$log")
                [ "$n" -gt 0 ] && break
                sleep 0.1
            done
            # The prompt JUST printed authorises the FIRST line (the
            # per-line gate below demands a prompt NEWER than the last
            # one it released; without this decrement the first line
            # would wait out its whole cap for a second prompt that
            # only its own consumption can produce -- found the hard
            # way by the first T9 regression run: every case lost its
            # first command and 15 s).
            base=$(( $(_il_prompt_count "$log") - 1 ))
            [ "$base" -lt 0 ] && base=0
        fi
    fi
    # Read input queue line-by-line; treat sleep markers specially.
    while IFS= read -r line; do
        if [[ "$line" == *$'\x1b__SLEEP__'* ]]; then
            local secs="${line#*$'\x1b__SLEEP__'}"
            sleep "$secs"
        elif [[ "$line" == *$'\x1b__WAIT__'* ]]; then
            # Content gate: poll the live serial log until the fixed
            # string appears (or the per-marker cap expires).  il_send_wait
            # emits these; see its comment.
            local wmark="${line#*$'\x1b__WAIT__'}"
            local wsecs="${wmark%%$'\x1f'*}"
            local wpat="${wmark#*$'\x1f'}"
            local seen=0
            for _ in $(seq 1 $(( wsecs * 10 ))); do
                if grep -Fq -- "$wpat" "$log" 2>/dev/null; then seen=1; break; fi
                sleep 0.1
            done
            [ "$seen" -eq 1 ] || printf 'il_send_wait: pattern not seen within %ss: %s\n' \
                "$wsecs" "$wpat" >&2
        else
            if [ "$synced" -eq 1 ]; then
                # Wait for a prompt NEWER than the last one we fed
                # (the previous command finished) before typing again.
                for _ in $(seq 1 150); do
                    n=$(_il_prompt_count "$log")
                    [ "$n" -gt "$base" ] && break
                    sleep 0.1
                done
                base=$(_il_prompt_count "$log")
            fi
            if ! printf '%s\n' "$line" 2>/dev/null; then
                printf 'il_feed: output pipe closed (guest gone) — dropping %d queued line(s)\n' \
                    "$(grep -c '' <<< "$IL_INPUT_QUEUE")" >&2
                break
            fi
            # Small per-line gap: AuraLite's serial input is polling-based,
            # so we mustn't blast characters faster than it consumes them.
            sleep 0.20
        fi
    done <<< "$IL_INPUT_QUEUE"
}

# -------- QEMU launcher --------
# il_run_qemu <log_path> <timeout_secs> [extra qemu args...]
il_run_qemu() {
    local log="$1"; shift
    local timeout_s="$1"; shift
    local extra=( "$@" )

    # Ensure the log directory exists
    mkdir -p "$(dirname "$log")"
    : > "$log"
    export IL_LAST_LOG="$log"

    # Compose the QEMU command line.  The default custom-loader artefact is a
    # raw hybrid disk image (despite its .iso suffix), not an El Torito CD.
    # Attach it as a snapshot-backed IDE hard disk so SeaBIOS executes its MBR
    # without allowing a test to modify the base image.
    # We always include:
    #   - serial → stdio (we read from it, write to it)
    #   - e1000 NIC on user/SLIRP NAT
    # The caller can override or add more via $extra.
    # CPU count is overridable via IL_SMP.  It defaults to 2, but a case that
    # runs long enough to hit the kernel's known BSP-only scheduling window
    # (see TODO.md) can pin itself to 1 rather than being intermittently red.
    local smp="${IL_SMP:-2}"

    # OPT_PLAN.md O2: pin the boot self-test intensity.  The kernel's
    # BUILD default is `fast`; CI must grep the full historical self-test
    # output, so the lib passes `full` through fw_cfg unless a case asks
    # otherwise via IL_SELFTEST (test_selftest_modes.sh and the perf
    # smoke exercise `fast`/`off` deliberately).
    local selftest="${IL_SELFTEST:-full}"

    # RESIDUE2 T5: IL_NETDEV (set by the L2-lab cases) replaces the whole
    # netdev line; unset keeps the historical user/SLIRP NAT (+opts).
    local netdev_args=(
        -netdev "user,id=net0${IL_NETDEV_OPTS:-}"
        -device "${IL_NIC},netdev=net0"
    )
    if [ -n "$IL_NETDEV" ]; then
        # shellcheck disable=SC2206   # deliberate word split: QEMU tokens
        netdev_args=( ${IL_NETDEV} -device "${IL_NIC},netdev=net0" )
    fi

    local base_args=(
        -drive "file=$IL_ISO,format=raw,if=ide,snapshot=on"
        -m 512M
        -smp "$smp"
        -display none
        -serial stdio
        -no-reboot
        -cpu "$IL_CPU"
        -boot order=c
        "${netdev_args[@]}"
        -fw_cfg "name=opt/auralite.selftest,string=${selftest}"
    )

    # Stream queued input → QEMU stdin, capture serial output → log.
    # Use `timeout --foreground` so signals propagate cleanly.  The log
    # path travels to the feeder through the environment (T9: the
    # consumption-gated feed polls the live log for the shell prompt).
    set +e
    IL_FEED_LOG="$log" _il_feed_queue \
      | timeout --foreground "${timeout_s}" "$IL_QEMU" "${base_args[@]}" "${extra[@]}" \
            >"$log" 2>&1
    local rc=$?
    set -e

    # QEMU returns 124 on timeout (we expect this — the OS doesn't shut down).
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ] && [ "$rc" -ne 143 ]; then
        echo "${C_YELLOW}[lib] qemu exited rc=$rc (timeout/SIGTERM ok; others may be real failures)${C_RESET}"
    fi

    # Reset queue for next sub-test in the same script.
    IL_INPUT_QUEUE=""
    return 0
}

# Prompt-aware sibling of il_run_qemu.  SH5d queues more than one hundred
# tcc/mini-asm invocations; sleep-delayed input is unsafe because AuraLite's
# serial driver is polling based and drops a command sent while the compiler
# still owns the CPU.  prompt_qemu.py is deliberately only a serial transport:
# all compilation, assembly and linking still happen inside the guest.
#
# Usage: il_send_prompt "..."; ...; il_run_qemu_prompt LOG TIMEOUT [qemu args]
il_run_qemu_prompt() {
    local log="$1"; shift
    local timeout_s="$1"; shift
    local extra=( "$@" )
    local smp="${IL_SMP:-2}"
    local selftest="${IL_SELFTEST:-full}"
    # RESIDUE2 T5: IL_NETDEV (set by the L2-lab cases) replaces the whole
    # netdev line; unset keeps the historical user/SLIRP NAT (+opts).
    local netdev_args=(
        -netdev "user,id=net0${IL_NETDEV_OPTS:-}"
        -device "${IL_NIC},netdev=net0"
    )
    if [ -n "$IL_NETDEV" ]; then
        # shellcheck disable=SC2206   # deliberate word split: QEMU tokens
        netdev_args=( ${IL_NETDEV} -device "${IL_NIC},netdev=net0" )
    fi

    local base_args=(
        -drive "file=$IL_ISO,format=raw,if=ide,snapshot=on"
        -m 512M
        -smp "$smp"
        -display none
        -serial stdio
        -no-reboot
        -cpu "$IL_CPU"
        -boot order=c
        "${netdev_args[@]}"
        -fw_cfg "name=opt/auralite.selftest,string=${selftest}"
    )
    local queue rc had_errexit=0
    [[ $- == *e* ]] && had_errexit=1
    mkdir -p "$(dirname "$log")"
    queue=$(mktemp "${IL_LOGDIR:-$(dirname "$log")}/prompt-queue.XXXXXX") || return 1
    printf '%s' "$IL_PROMPT_QUEUE" > "$queue"
    export IL_LAST_LOG="$log"
    # prompt_qemu.py's own progress/diagnostic lines are NOT serial output, so
    # they do not belong in the serial log -- but they are the first thing to
    # read when a long guest build stalls, so they get their own artefact.
    IL_PROMPT_DRIVER_LOG="${log%.log}-driver.log"
    export IL_PROMPT_DRIVER_LOG

    # prompt_qemu.py requires every `run` command to reach exit code 0 before
    # it sends the next one (the kernel prints a thread-exit receipt for each).
    # That is what keeps a single failing tcc invocation from being followed by
    # a hundred more compiles and an unrelated-looking link error.  Callers
    # queueing commands that are *meant* to fail set IL_PROMPT_CHECK_EXIT=0.
    local prompt_args=()
    [ "${IL_PROMPT_CHECK_EXIT:-1}" -eq 0 ] && prompt_args+=(--no-check-run-exit)

    set +e
    python3 "$IL_ROOT/tests/integration/lib/prompt_qemu.py" \
        --log "$log" --timeout "$timeout_s" --commands "$queue" \
        "${prompt_args[@]}" -- \
        "$IL_QEMU" "${base_args[@]}" "${extra[@]}" 2>&1 \
        | tee "$IL_PROMPT_DRIVER_LOG"
    rc=${PIPESTATUS[0]}
    [ "$had_errexit" -eq 1 ] && set -e
    rm -f "$queue"
    IL_PROMPT_QUEUE=""

    if [ "$rc" -ne 0 ]; then
        echo "${C_YELLOW}[lib] prompt-aware QEMU driver exited rc=$rc; inspect $log${C_RESET}"
    fi
    # Preserve il_run_qemu's assertion-first contract: a bad QEMU process
    # still leaves a serial log that gives every diagnostic its own assertion.
    return 0
}

# -------- assertions --------
il_assert_grep() {
    local log="$1" pat="$2" desc="$3"
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    if grep -qE "$pat" "$log"; then
        il_pass "$desc"
    else
        il_fail "$desc  (pattern: ${C_DIM}$pat${C_RESET})"
    fi
}

# Fixed-string variant (no regex, avoids '(' ')' '?' issues in SELFTEST output)
il_assert_grep_fixed() {
    local log="$1" pat="$2" desc="$3"
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    if grep -Fq -- "$pat" "$log"; then
        il_pass "$desc"
    else
        il_fail "$desc  (fixed: ${C_DIM}$pat${C_RESET})"
    fi
}

il_assert_no_grep() {
    local log="$1" pat="$2" desc="$3"
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    if grep -qE "$pat" "$log"; then
        il_fail "$desc  (unexpected pattern: ${C_DIM}$pat${C_RESET})"
    else
        il_pass "$desc"
    fi
}

il_assert_no_grep_fixed() {
    local log="$1" pat="$2" desc="$3"
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    if grep -Fq -- "$pat" "$log"; then
        il_fail "$desc  (unexpected fixed: ${C_DIM}$pat${C_RESET})"
    else
        il_pass "$desc"
    fi
}

# Assert only when the environment actually provided working DNS.
#
# Some cases (X3 cache, X4 fragment reassembly) resolve a public name through
# QEMU's SLIRP resolver.  That depends on the *host* having working DNS, which
# is not a property of the kernel under test: on a sandboxed/offline CI box the
# guest correctly reports the failure and the case would go red for a reason
# unrelated to what it is gating.  The deterministic part of those cases (the
# kernel self-test through the real glue) stays a hard assertion; the online
# part degrades to a reported SKIP.
il_assert_grep_if_dns() {
    local log="$1" pat="$2" desc="$3"
    if grep -qE "\[dns\] (PASS|cache (HIT|MISS))" "$log"; then
        il_assert_grep "$log" "$pat" "$desc"
    else
        il_skip "$desc (no DNS answer in log; host resolver unavailable)"
    fi
}

il_skip() {
    IL_SKIP_COUNT=$((${IL_SKIP_COUNT:-0} + 1))
    echo "  ${C_YELLOW}⊘${C_RESET} SKIP: $*"
}

il_assert_count() {
    # AUDIT_A2: this called il_pass/il_fail without incrementing
    # IL_ASSERT_COUNT, so every case using it under-reported its own total
    # ("4/2 assertions passed").  Seven cases are affected.  The other
    # il_assert_* helpers bump the counter; this one was missed.
    local log="$1" pat="$2" min="$3" desc="${4:-count of '$pat' >= $min}"
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    local n
    n="$(grep -cE "$pat" "$log" || true)"
    if [ "$n" -ge "$min" ]; then
        il_pass "$desc (got $n)"
    else
        il_fail "$desc (got $n)"
    fi
}

# -------- reporting --------
il_pass() {
    IL_PASS_COUNT=$((IL_PASS_COUNT + 1))
    echo "  ${C_GREEN}✔${C_RESET} $*"
}

il_fail() {
    IL_FAIL_COUNT=$((IL_FAIL_COUNT + 1))
    IL_FAILED_ASSERTS+=("$*")
    echo "  ${C_RED}✘${C_RESET} $*"
}

il_section() {
    echo
    echo "${C_BOLD}${C_BLUE}== $* ==${C_RESET}"
}

# USB_PLAN U0: refuse to pass a case whose guest log contains fabricated data.
#
# The xHCI driver answers control/bulk/interrupt transfers from invented
# buffers (see USB_PLAN.md §1).  test_usb_xhci.sh consequently asserted eight
# properties of data the OS made up -- including "READ(10) works" against a
# sector reading "AURALUSB" -- and reported 8/8 green.  A suite that cannot
# tell a working driver from no driver is worse than no suite, so any log
# carrying a SYNTHETIC marker fails here, loudly, naming the marker.
#
# This is deliberately a *global* guard rather than a per-case assertion: it
# means a synthesis added anywhere later cannot quietly ride into a green run.
# The count reaches zero at U5, at which point this guard becomes inert.
#
# Escape hatch for the phases that must run while synthesis still exists:
# IL_ALLOW_SYNTHETIC=1 downgrades it to a warning.
il_check_synthetic() {
    local log="${IL_LAST_LOG:-}"
    [ -n "$log" ] && [ -f "$log" ] || return 0
    grep -Fq 'SYNTHETIC' "$log" || return 0

    local n
    n=$(grep -Fc 'SYNTHETIC' "$log")
    if [ "${IL_ALLOW_SYNTHETIC:-0}" = "1" ]; then
        echo "${C_YELLOW:-}  ! ${n} SYNTHETIC marker(s) in the log (allowed by IL_ALLOW_SYNTHETIC=1)${C_RESET:-}"
        return 0
    fi
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    il_fail "guest log contains ${n} SYNTHETIC marker(s): the OS fabricated data (USB_PLAN.md)"
    grep -F 'SYNTHETIC' "$log" | sed 's/^/      /' | head -8
}

il_summary() {
    il_check_synthetic
    local total=${IL_ASSERT_COUNT:-$((IL_PASS_COUNT + IL_FAIL_COUNT))}
    echo
    if [ "$IL_FAIL_COUNT" -eq 0 ]; then
        echo "${C_BOLD}${C_GREEN}── ${IL_PASS_COUNT}/${total} assertions passed ──${C_RESET}"
        return 0
    else
        echo "${C_BOLD}${C_RED}── ${IL_FAIL_COUNT} of ${total} assertions FAILED ──${C_RESET}"
        printf '  %s\n' "${IL_FAILED_ASSERTS[@]}"
        return 1
    fi
}

# Trap to dump the most-recent log on hard errors only.
# Usage:  trap il_dump_on_error EXIT
# It checks the script's exit status ($?) and only dumps on non-zero.
il_dump_on_error() {
    local ec=$?
    # Treat "we logged failed asserts" (IL_FAIL_COUNT>0) OR non-zero exit as failure.
    if [ "$ec" -eq 0 ] && [ "${IL_FAIL_COUNT:-0}" -eq 0 ]; then
        return 0
    fi
    local last_log="${IL_LAST_LOG:-}"
    [ -n "$last_log" ] && [ -f "$last_log" ] || return 0
    echo
    echo "${C_YELLOW}── last 40 lines of $last_log ──${C_RESET}"
    tail -40 "$last_log"
}

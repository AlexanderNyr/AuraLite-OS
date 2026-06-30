#!/usr/bin/env bash
# tests/integration/lib/lib.sh — shared helpers for QEMU integration tests.
#
# Public API:
#   il_init                            — set up ROOT, ISO, BUILD, LOG, etc.
#   il_have <bin>...                   — assert helper binaries exist
#   il_make_disk <path> <MiB> <magic>  — create raw image with magic in sector 0
#   il_run_qemu <log> <timeout_s> [-- <extra qemu args>]
#                                        boot the ISO; serial → <log>;
#                                        stdin gets shell commands via il_send().
#   il_send "<text>"                   — queue text to be sent into serial
#                                        (call BEFORE il_run_qemu).
#   il_send_delay <secs>               — queue a sleep between commands.
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

    # Build the ISO on demand.
    if [ ! -f "$IL_ISO" ]; then
        echo "${C_YELLOW}[lib] ISO missing — running 'make iso'…${C_RESET}"
        ( cd "$IL_ROOT" && make iso >/dev/null )
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
il_send_delay() {
    # Emit a literal sleep marker. We turn it into a real sleep at run time.
    IL_INPUT_QUEUE+=$'\x1b__SLEEP__'"$1"$'\n'
}

# Internal: feed the queue into a process, honouring sleep markers.
_il_feed_queue() {
    # Read input queue line-by-line; treat sleep markers specially.
    local line
    while IFS= read -r line; do
        if [[ "$line" == *$'\x1b__SLEEP__'* ]]; then
            local secs="${line#*$'\x1b__SLEEP__'}"
            sleep "$secs"
        else
            printf '%s\n' "$line"
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

    # Compose the QEMU command line.  We always include:
    # ...

    # Compose the QEMU command line.  We always include:
    #   - serial → stdio (we read from it, write to it)
    #   - e1000 NIC on user/SLIRP NAT
    #   - small AHCI disk
    # The caller can override or add more via $extra.
    local base_args=(
        -cdrom "$IL_ISO"
        -m 512M
        -smp 2
        -display none
        -serial stdio
        -no-reboot
        -cpu qemu64
        -boot order=d
        -netdev user,id=net0
        -device "${IL_NIC},netdev=net0"
    )

    # Stream queued input → QEMU stdin, capture serial output → log.
    # Use `timeout --foreground` so signals propagate cleanly.
    set +e
    _il_feed_queue \
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
    if grep -qE "$pat" "$log"; then
        il_fail "$desc  (unexpected pattern: ${C_DIM}$pat${C_RESET})"
    else
        il_pass "$desc"
    fi
}

il_assert_no_grep_fixed() {
    local log="$1" pat="$2" desc="$3"
    if grep -Fq -- "$pat" "$log"; then
        il_fail "$desc  (unexpected fixed: ${C_DIM}$pat${C_RESET})"
    else
        il_pass "$desc"
    fi
}

il_assert_count() {
    local log="$1" pat="$2" min="$3" desc="${4:-count of '$pat' >= $min}"
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

il_summary() {
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

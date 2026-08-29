#!/usr/bin/env bash
# test_prompt_qemu.sh -- unit test for the SH5d prompt-aware serial transport.
#
# tests/integration/lib/prompt_qemu.py drives ~170 guest compiler invocations
# one per fresh `auralite#` prompt, and refuses to continue past a `run`
# command that did not exit zero.  That refusal is the difference between a
# gate that names the failing compile and one that reports a link error five
# minutes later, so it gets tested directly rather than only as a side effect
# of a 76-second QEMU boot.
#
# The guest is replaced by a stub that speaks the same serial contract: a
# prompt, then the kernel's `[thread] ... exited (code=N)` receipt
# (kernel/proc/thread.c) for every spawned command, and nothing for builtins.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
mkdir -p "$ROOT/build"
TMP=$(mktemp -d "$ROOT/build/prompt-qemu.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
FAILED=0
pass(){ echo "PASS: $*"; }
fail(){ echo "FAIL: $*"; FAILED=1; }

cat > "$TMP/fake-guest.sh" <<'STUB'
#!/usr/bin/env bash
# Stand-in for QEMU + AuraLite's init shell over -serial stdio.
printf 'auralite#\n'
while IFS= read -r line; do
    case "$line" in
        "run ok")     printf "[thread] 't' (tid 5) exited (code=0)\n" ;;
        "run bad")    printf "[thread] 't' (tid 5) exited (code=3)\n" ;;
        "run silent") printf "tcc: output, but no thread-exit receipt\n" ;;
        "mkdir x")    : ;;                  # builtin: spawns no thread
        "exit")       printf "bye\n"; exit 0 ;;
    esac
    printf 'auralite#\n'
done
STUB
chmod +x "$TMP/fake-guest.sh"

PQ="$ROOT/tests/integration/lib/prompt_qemu.py"

# drive <expected-rc> <name> <command...>
drive() {
    local want="$1"; shift
    local name="$1"; shift
    printf '%s\n' "$@" > "$TMP/cmds.txt"
    python3 "$PQ" --log "$TMP/$name.log" --timeout 30 \
        --commands "$TMP/cmds.txt" ${EXTRA_ARGS:-} -- "$TMP/fake-guest.sh" \
        > "$TMP/$name.driver.log" 2>&1
    local rc=$?
    if [ "$rc" -eq "$want" ]; then
        return 0
    fi
    echo "    expected rc=$want, got rc=$rc"
    sed 's/^/    | /' "$TMP/$name.driver.log"
    return 1
}

# 1. A queue where every spawned command exits zero completes cleanly.
if drive 0 ok "mkdir x" "run ok" "run ok" "exit"; then
    pass "all-zero-exit queue completes"
else
    fail "all-zero-exit queue did not complete"; fi

# 2. A non-zero exit stops the queue at that command, naming it.
if drive 1 bad "run ok" "run bad" "run ok" "exit" && \
   grep -Fq "command 2/4 (run bad) reported exit code 3, not 0" \
        "$TMP/bad.driver.log"; then
    pass "non-zero exit halts the queue and names the command and code"
else
    fail "non-zero exit was not diagnosed"; fi

# 3. A `run` that leaves no thread-exit receipt at all is also a failure:
#    silently treating "no receipt" as success is how a hung spawn would
#    otherwise look like a passing build.
if drive 1 silent "run silent" "exit" && \
   grep -Fq "produced no thread-exit receipt at all" "$TMP/silent.driver.log"; then
    pass "missing exit receipt halts the queue"
else
    fail "missing exit receipt was not diagnosed"; fi

# 4. Builtins spawn no thread and print no receipt; requiring one from them
#    would make every mkdir/stat/echo command a false failure.
if drive 0 builtins "mkdir x" "mkdir x" "exit"; then
    pass "builtin commands are not held to a thread-exit receipt"
else
    fail "builtins were wrongly required to exit zero"; fi

# 5. The check is opt-out for callers that queue commands meant to fail.
EXTRA_ARGS="--no-check-run-exit"
if drive 0 optout "run ok" "run bad" "exit"; then
    pass "--no-check-run-exit disables the gate"
else
    fail "--no-check-run-exit did not disable the gate"; fi
unset EXTRA_ARGS

# 6. The completion line the integration case asserts on must report the full
#    queue length, so a truncated dispatch cannot read as success.
if grep -Eq '^\[prompt-qemu\] complete \(4/4 commands sent' \
        "$TMP/ok.driver.log"; then
    pass "completion receipt reports the full queue length"
else
    fail "completion receipt missing or under-reported"; fi

# 7. Every command really was delivered one-per-prompt: the stub only prints a
#    new prompt after reading a line, so a blind burst would desynchronise and
#    the queue would not drain.
SENT=$(grep -c '^\[prompt-qemu\] [0-9]*/4: ' "$TMP/ok.driver.log")
if [ "$SENT" -eq 4 ]; then
    pass "each queued command was sent behind its own prompt"
else
    fail "expected 4 prompt-gated sends, saw $SENT"; fi

if [ "$FAILED" -eq 0 ]; then
    echo "[selfhost] prompt-qemu PASS: transport gates each command on a fresh prompt and a zero exit"
    exit 0
fi
exit 1

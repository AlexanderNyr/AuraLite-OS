#!/usr/bin/env bash
# tests/posix2024/run_host.sh -- POSIX.1-2024 conformance harness, host layer.
# POSIX2024_PLAN.md phase Q12.  Four checks:
#
#   1. header self-containment sweep -- every public header must compile
#      standalone under -std=c11 -Wall -Wextra -Werror with
#      _POSIX_C_SOURCE=202405L (a header that only compiles when something
#      else was included first is a bug, and the gate rejects it);
#   2. matrix -> archive drift check -- every full row of
#      docs/posix2024_compliance.md must resolve to a defined symbol in
#      libaurac.a (matrix_check.py), partials must equal the allowlist,
#      and there must be no "missing" rows;
#   3. negative control -- the drift check must FAIL against a degraded
#      copy of the archive (one object module dropped), proving the gate
#      notices when a claimed implementation disappears;
#   4. Q-family unit sub-suites re-run as the harness's own sub-suites.
#
# Mirrors tests/unit/test_userlibs.sh: if the archives are not built, the
# harness skips with a loud hint instead of failing (run 'make iso' first).
#
# Exit status 0 = everything green, 1 = any check failed.

cd "$(dirname "$0")/../.." || exit 1

HOST_CC="${HOST_CC:-cc}"
LIBAURAC="build/lib/libaurac.a"
MATRIX="docs/posix2024_compliance.md"
INC_DIR="lib/libc/include"

fail=0
passes=0
fails=0

note()  { printf 'POSIX2024 host: %s\n' "$*"; }
pass()  { passes=$((passes + 1)); printf 'PASS: %s\n' "$*"; }
failm() { fails=$((fails + 1)); printf 'FAIL: %s\n' "$*"; }

# ---------------------------------------------------------------------------
# 0. Do the archives exist?  If not, skip like test_userlibs.sh does.
# ---------------------------------------------------------------------------
if [ ! -f "$LIBAURAC" ]; then
    note "libaurac.a not built -- skipping (run 'make iso' or 'make libs' first)"
    exit 0
fi

# ---------------------------------------------------------------------------
# 1. Header self-containment sweep
# ---------------------------------------------------------------------------
hdr_fail=0
hdr_count=0
while IFS= read -r hdr; do
    hdr_count=$((hdr_count + 1))
    rel="${hdr#$INC_DIR/}"
    printf '#include <%s>\nint main(void) { return 0; }\n' "$rel" > /tmp/posix2024_hdr.c
    if ! $HOST_CC -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=202405L \
            -I "$INC_DIR" -fsyntax-only /tmp/posix2024_hdr.c 2>/dev/null; then
        hdr_fail=1
        printf '  header %s does not compile standalone\n' "$rel"
    fi
done < <(find "$INC_DIR" -name '*.h' | sort)
rm -f /tmp/posix2024_hdr.c
if [ "$hdr_fail" -eq 0 ]; then
    pass "header sweep: $hdr_count/$hdr_count public headers self-contained"
else
    failm "header sweep: some of $hdr_count headers not self-contained (see above)"
fi

# ---------------------------------------------------------------------------
# 2. Matrix -> archive drift check
# ---------------------------------------------------------------------------
drift_out=$(python3 tests/posix2024/matrix_check.py \
    "$MATRIX" "$LIBAURAC" \
    tests/posix2024/known_partials.txt \
    tests/posix2024/known_macros.txt "$INC_DIR" 2>&1)
drift_rc=$?
if [ "$drift_rc" -eq 0 ]; then
    pass "matrix drift: ${drift_out#matrix: }"
else
    failm "matrix drift:"
    printf '%s\n' "$drift_out" | sed 's/^/  /'
fi

# ---------------------------------------------------------------------------
# 3. Negative control -- the drift check must catch a deleted implementation
# ---------------------------------------------------------------------------
# Drop the object that provides strlcpy from a scratch copy of the archive;
# the checker must report strlcpy missing.  (strlcpy is chosen only because
# it lives in a small object; any claimed symbol would do.)
obj=$(nm -A "$LIBAURAC" 2>/dev/null | awk '/ [TtDd] strlcpy$/ {split($1, a, ":"); print a[2]; exit}')
if [ -z "$obj" ]; then
    failm "negative control: strlcpy object not found in archive"
else
    tmpdir=$(mktemp -d)
    cp "$LIBAURAC" "$tmpdir/degraded.a"
    if ar d "$tmpdir/degraded.a" "$obj" 2>/dev/null; then
        nc_out=$(python3 tests/posix2024/matrix_check.py \
            "$MATRIX" "$tmpdir/degraded.a" \
            tests/posix2024/known_partials.txt \
            tests/posix2024/known_macros.txt "$INC_DIR" 2>&1)
        nc_rc=$?
        if [ "$nc_rc" -ne 0 ] && printf '%s' "$nc_out" | grep -q strlcpy; then
            pass "negative control: drift check failed on archive with $obj removed (as required)"
        else
            failm "negative control: checker did not catch strlcpy removal (rc=$nc_rc)"
        fi
    else
        failm "negative control: could not degrade archive (ar d $obj failed)"
    fi
    rm -rf "$tmpdir"
fi

# ---------------------------------------------------------------------------
# 4. Q-family unit sub-suites re-run
# ---------------------------------------------------------------------------
subs="test_q1_headers test_stdio_ext test_string_ext test_stdlib_ext \
test_pthread_ext test_ipc test_posix_spawn test_q10_stubs test_q11_new"
sub_fail=0
for t in $subs; do
    if make -s "build/$t" >/dev/null 2>&1; then
        if ! ./build/"$t" >/dev/null 2>&1; then
            sub_fail=1
            printf '  sub-suite %s FAILED\n' "$t"
        fi
    else
        sub_fail=1
        printf '  sub-suite %s could not be built\n' "$t"
    fi
done
if [ "$sub_fail" -eq 0 ]; then
    pass "sub-suites: all Q-family unit binaries passed"
else
    failm "sub-suites: one or more Q-family unit binaries failed (see above)"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
if [ "$fails" -eq 0 ]; then
    note "all checks passed ($passes PASS, 0 FAIL)"
    exit 0
else
    note "harness FAILED ($passes PASS, $fails FAIL)"
    exit 1
fi

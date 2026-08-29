#!/usr/bin/env bash
# test_shmake.sh -- SELFHOST_PLAN.md SH6e host gate: shmake semantics.
#
# Compiles tools/shmake/shmake.c with the host cc and drives a temp
# worktree.  The integration case is the in-guest proof; this file is
# the fast one: variables, .PHONY, timestamps, and "only dependents
# rebuild" without a QEMU boot.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
FAILED=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAILED=1; }

SHMAKE="$ROOT/build/shmake"
mkdir -p "$ROOT/build"
cc -std=c99 -Wall -Wextra -Werror -O2 -o "$SHMAKE" "$ROOT/tools/shmake/shmake.c" \
    || { echo "FAIL: shmake does not compile with host cc"; exit 1; }
pass "shmake compiles with host cc"

WORKDIR=$(mktemp -d "$ROOT/build/shmake-test.XXXXXX")
trap 'rm -rf "$WORKDIR"' EXIT
cd "$WORKDIR"

cat > stamp.sh << 'EOF'
#!/bin/sh
tag=$1
target=$2
shift 2
echo "stamp $target" > "$target"
echo "g$tag $target" >> rebuild.log
echo "g$tag rebuilt $target"
EOF
chmod +x stamp.sh

# Real tabs in the Makefile.
python3 - << 'PY'
from pathlib import Path
Path("Makefile").write_text(
    "CC = ./stamp.sh\n"
    "GEN = 0\n"
    "\n"
    "app: a.out b.out\n"
    "\t$(CC) $(GEN) app a.out b.out\n"
    "\n"
    "a.out: a.in\n"
    "\t$(CC) $(GEN) a.out a.in\n"
    "\n"
    "b.out: b.in\n"
    "\t$(CC) $(GEN) b.out b.in\n"
    "\n"
    ".PHONY: always\n"
    "always:\n"
    "\t$(CC) $(GEN) always.out\n"
)
PY
echo a > a.in
echo b > b.in

# ---- cold run ----
rm -f rebuild.log a.out b.out app always.out
"$SHMAKE" GEN=1 > cold.out 2> cold.err
if grep -q "g1 rebuilt a.out" cold.out \
   && grep -q "g1 rebuilt b.out" cold.out \
   && grep -q "g1 rebuilt app" cold.out; then
    pass "cold run rebuilds a.out, b.out, app"
else
    fail "cold run missed a rebuild"; cat cold.out; cat cold.err
fi
grep -q "\\[shmake\\] rebuilt=3 uptodate=0" cold.out \
    && pass "cold run summary is rebuilt=3 uptodate=0" \
    || fail "cold run summary (got $(grep '\\[shmake\\]' cold.out))"

# ---- $(CC) expanded ----
grep -q "./stamp.sh 1 a.out a.in" cold.out \
    && pass "\$(CC) and \$(GEN) expand in the recipe" \
    || fail "\$(CC) did not expand (recipe echo missing)"

# ---- command-line override beats the file ----
"$SHMAKE" -n GEN=9 a.out > ov.out 2>&1 || true
# a.out is up to date, so -n prints the up-to-date line, not a recipe.
# Force a rebuild by removing it.
rm -f a.out
"$SHMAKE" -n GEN=9 a.out > ov.out 2>&1
grep -q "./stamp.sh 9 a.out a.in" ov.out \
    && pass "command-line GEN=9 overrides the Makefile" \
    || fail "command-line override did not win: $(cat ov.out)"

# ---- incremental: only dependents of a.in ----
# Give a.in a strictly newer mtime without sleeping.
touch -t 202001010000 a.out b.out app a.in b.in
touch -t 202001010100 a.in
rm -f rebuild.log
"$SHMAKE" GEN=2 > inc.out 2> inc.err
if grep -q "g2 rebuilt a.out" inc.out \
   && grep -q "g2 rebuilt app" inc.out \
   && ! grep -q "g2 rebuilt b.out" inc.out \
   && grep -q "shmake: 'b.out' is up to date" inc.out; then
    pass "touching a.in rebuilds a.out and app, not b.out"
else
    fail "incremental rebuild graph"; cat inc.out; cat inc.err
fi

# ---- no-op ----
"$SHMAKE" GEN=3 > noop.out 2> noop.err
grep -q "\\[shmake\\] rebuilt=0 uptodate=3" noop.out \
    && pass "no-op run is rebuilt=0 uptodate=3" \
    || fail "no-op summary (got $(grep '\\[shmake\\]' noop.out))"

# ---- .PHONY: a file of the same name does not skip the recipe ----
touch -t 202001010000 always
touch -t 199901010000 always.out 2>/dev/null || true
rm -f always.out
touch always
"$SHMAKE" GEN=4 always > phony.out 2> phony.err
if grep -q "g4 rebuilt always.out" phony.out; then
    pass ".PHONY always runs even when a file named 'always' exists"
else
    fail ".PHONY was skipped"; cat phony.out; cat phony.err
fi

# ---- missing source is an error ----
if "$SHMAKE" GEN=5 missing.out > miss.out 2> miss.err; then
    fail "missing target exited 0"
else
    grep -q "no rule to make target 'missing.out'" miss.err \
        && pass "missing target is a diagnosed error" \
        || fail "missing target diagnostic: $(cat miss.err)"
fi

# ---- -C changes directory ----
mkdir -p sub
cp Makefile stamp.sh sub/
echo a > sub/a.in
echo b > sub/b.in
"$SHMAKE" -C sub -f Makefile GEN=6 app > cdir.out 2> cdir.err
if [ -f sub/app ] && [ -f sub/a.out ]; then
    pass "-C dir builds inside dir"
else
    fail "-C did not write outputs in sub/"; cat cdir.out; cat cdir.err
fi

# ---- circular dependency ----
python3 - << 'PY'
from pathlib import Path
Path("cycle.mk").write_text("a: b\n\techo a\nb: a\n\techo b\n".replace("echo", "\techo"))
# the replace above is wrong if there were no tabs; write explicitly:
Path("cycle.mk").write_text("a: b\n\techo a\nb: a\n\techo b\n")
PY
if "$SHMAKE" -f cycle.mk a > cycle.out 2> cycle.err; then
    fail "circular graph exited 0"
else
    grep -qi "circular" cycle.err \
        && pass "circular dependency is diagnosed" \
        || fail "circular diagnostic: $(cat cycle.err)"
fi

echo
if [ "$FAILED" -eq 0 ]; then
    echo "=== ALL SHMAKE TESTS PASSED ==="
    exit 0
fi
echo "=== SHMAKE TESTS FAILED ==="
exit 1

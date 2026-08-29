# sh6d_probe.sh -- SELFHOST SH6d happy-path gate script.
#
# Staged at /tests/sh6d_probe.sh and run in-guest as
#     sh /tests/sh6d_probe.sh
#
# What each construct has to prove:
#   C1  taken `if true` runs its then-body
#   C2  untaken `if false` skips then, runs else
#   C3  `elif` of a failing if is taken
#   C4  `for x in a b c` iterates three times with $x set
#   C5  `while true; do ...; break; done` runs the body once and stops
#
# Nested `if` inside the while body is the landmine (ledger SH-41): if the
# inner if collected from the script frame it would swallow the outer `done`
# and the break would never run.  Multi-line form is the one build.sh uses;
# a one-liner is C1 so both shapes are covered.
#
# `true`/`false` are status builtins: a condition must not have to be `echo`
# (prints) or `run nosuch` (127 and a diagnostic).  A failing *condition* is
# consumed by the construct, so `if false` does not abort the script the way
# a top-level `false` would (SH6a's runner stops on a nonzero line).
#
# The receipt counts the five constructs above.

# C1 (taken if, one-liner): then-body runs
if true; then echo [selfhost] sh6d: if-taken; fi

# C2 (untaken + else, multi-line)
if false
then
  echo [selfhost] sh6d: UNREACHABLE-IF
else
  echo [selfhost] sh6d: else-taken
fi

# C3 (elif)
if false; then
  echo [selfhost] sh6d: UNREACHABLE-ELIF1
elif true; then
  echo [selfhost] sh6d: elif-taken
else
  echo [selfhost] sh6d: UNREACHABLE-ELIF2
fi

# C4 (for, known iteration count)
for x in a b c
do
  echo [selfhost] sh6d: for-$x
done

# C5 (while + break) with a nested if inside the body -- SH-41
while true
do
  if true; then
    echo [selfhost] sh6d: nested-if
  fi
  echo [selfhost] sh6d: while-once
  break
  echo [selfhost] sh6d: UNREACHABLE-WHILE
done

# $? after a successful compound is 0
echo [selfhost] sh6d: status-after=$?

echo [selfhost] control PASS: 5 branches and loops ran

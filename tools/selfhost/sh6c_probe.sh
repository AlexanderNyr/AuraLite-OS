# sh6c_probe.sh -- SELFHOST SH6c happy-path gate script.
#
# Staged at /tests/sh6c_probe.sh and run in-guest as
#     sh /tests/sh6c_probe.sh
#
# What each line has to prove:
#   a | b               data flows stage-to-stage through SYS_PIPE
#   a | b | c           the chain holds for three stages
#   $?                  a successful pipeline leaves status 0
#   a ; b               the element after a FAILURE still runs
#   a && b              the element after a failure is SKIPPED -- the
#                       "first stage fails, failure propagates through &&"
#                       case of the gate
#   a || b              the element after a failure RUNS
#   ... | (failing) &&  a pipeline whose LAST stage fails still stops the
#                       &&: the pipeline's status is the last stage's
#                       (POSIX), so this is the shape a build script uses
#
# Failure source, measured not assumed: `run` of a name that is on no search
# path is 127 (the shell reports it and never spawns).  A `run` of a missing
# ABSOLUTE path exits 0 -- the kernel spawns the child and the load failure
# lands inside it; that shape must not be used as a failure source.
#
# SH6a's script runner stops on a non-zero line status, so every
# intentionally-failing list ends with a `; echo ...survived` whose status
# is 0.  The receipt counts the pipelines (the command lists containing |):
# P1, P2, P4 and P5.

set OUT=/tmp/sh6c.log

# P1 (2 stages): echo through cat, into a file whose name is a variable
echo [selfhost] sh6c: p1 | cat > $OUT

# P2 (3 stages): appended to the same file
echo [selfhost] sh6c: p2 | cat | cat >> $OUT

# P3: $? after two successful pipelines is 0
echo [selfhost] sh6c: status-after-p2=$?

# L1 (`;`): the element after a FAILURE still runs
run sh6c-no-such-program ; echo [selfhost] sh6c: semicolon-continued

# L2 (`&&`): the failing first element stops the chain.  Padded with `;`
# so the line's status is 0 and SH6a's runner does not abort the script.
run sh6c-no-such-program && echo [selfhost] sh6c: UNREACHABLE-AND ; echo [selfhost] sh6c: and-survived

# L3 (`||`): the element after a failure runs
run sh6c-no-such-program || echo [selfhost] sh6c: or-continued

# P4: a pipeline whose LAST stage fails; the && behind it must not run.
echo x | run sh6c-no-such-program && echo [selfhost] sh6c: UNREACHABLE-AND2 ; echo [selfhost] sh6c: pipe-and-survived

# P5: a pipeline writes the file, a redirect reads it back
echo roundtrip | cat > /tmp/sh6c_p5.txt
cat < /tmp/sh6c_p5.txt

# read back what P1/P2 wrote, to prove both pipelines landed
cat $OUT

echo [selfhost] pipe PASS: 4 pipelines ran

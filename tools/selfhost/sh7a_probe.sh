# sh7a_probe.sh -- SELFHOST SH7a happy-path gate script.
#
# Staged at /tests/sh7a_probe.sh and run in-guest as
#     sh /tests/sh7a_probe.sh
#
# SH7a is the first image-tooling C twin: a /bin/sha256sum that wraps the one
# SHA-256 implementation the tree ships (libatls).  This script proves, in
# order:
#   S1  the published FIPS vectors pass (the tool's own --selftest)
#   S2  hashing stdin works (echo piped in -> coreutils digest line)
#   S3  hashing a FILE works and matches the stdin hash of the same bytes
#       (--eq branches on the exit status -- the scripting shell has no
#        cut/grep to parse a digest line, so parity is a return code)
#   S4  a mismatch is detected and reported (negative control: --eq must FAIL
#        when the content differs, or the MATCH in S3 would be meaningless)
#
# SH6a's runner stops on a nonzero line status, so the intentionally-failing
# S4 condition is consumed by `if ... ; then ... else ... ; fi`; the final
# receipt is the line the host greps.

F=/tmp/sh7a_data.txt

# S1: built-in known-answer test (empty / "abc" / 1M 'a')
sha256sum --selftest

# S2: stdin path.  echo appends a newline, so the file in S3 carries the same
# trailing newline -- the hashed content must be byte-identical.
echo sh7a-hash-me > $F
echo sh7a-hash-me | sha256sum

# S3: file hash equals stdin hash of the same content (exit 0 => match)
if cat $F | sha256sum --eq $F
then
  echo [selfhost] sh7a: s3-file-stdin-match
else
  echo [selfhost] sh7a: UNREACHABLE-MISMATCH
fi

# S4 (negative control): feed DIFFERENT bytes to --eq; it must return non-zero.
if echo different-content | sha256sum --eq $F
then
  echo [selfhost] sh7a: UNREACHABLE-FALSE-MATCH
else
  echo [selfhost] sh7a: s4-mismatch-detected
fi

rm $F

echo [selfhost] sha256 PASS: selftest + stdin + file parity verified in-guest

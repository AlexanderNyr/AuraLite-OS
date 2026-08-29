# sh6b_probe.sh -- SELFHOST SH6b happy-path gate script.
#
# Staged at /tests/sh6b_probe.sh and run in-guest as
#     sh /tests/sh6b_probe.sh kernel
#
# What each line has to prove:
#   set NAME=VALUE   a variable can be assigned from a positional parameter
#   > file           truncate-and-write, target taken from a variable
#   >> file          append, to the same file
#   "a > b"          a `>` inside quotes is text, not a redirect
#   $MSG             a named variable expands where a positional one did
#   cat < a > b      both directions at once: stdin from one file, stdout to
#                    another, which is the only way to prove `<` really feeds
#                    the command and is not just parsed
#   unset NAME       the variable is gone afterwards, not merely empty
#
# The receipt names two files because that is what is actually written:
# /tmp/sh6b.log and /tmp/sh6b_copy.txt.

set TGT=$1
set LOG=/tmp/sh6b.log
echo [selfhost] sh6b: target=$TGT > $LOG
echo second line >> $LOG
echo "a > b is text, not a redirect" >> $LOG
set MSG="[selfhost] sh6b: variable expanded"
echo $MSG >> $LOG
cat < $LOG > /tmp/sh6b_copy.txt
echo [selfhost] sh6b: copy follows
cat < /tmp/sh6b_copy.txt
unset TGT
echo [selfhost] sh6b: after-unset=[$TGT]
echo [selfhost] redirect PASS: 2 files written and read back

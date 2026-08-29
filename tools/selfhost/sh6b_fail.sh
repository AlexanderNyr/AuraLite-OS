# sh6b_fail.sh -- SELFHOST SH6b error-path gate script.
#
# Line 8 has an unmatched double quote.  The old strtok tokenizer would have
# split it into `"unterminated` and run echo with that as an argument, so a
# script with a typo would produce output that looks plausible.  The parser
# refuses the line instead, and the script stops there with the line number.
echo [selfhost] sh6b: fail probe
echo "unterminated
echo [selfhost] sh6b: UNREACHABLE-AFTER-QUOTE-ERROR

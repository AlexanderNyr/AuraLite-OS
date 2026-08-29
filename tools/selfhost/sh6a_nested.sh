# sh6a_nested.sh -- proves `sh` nests.  Invoked from /tests/sh6a_probe.sh as
#     sh /tests/sh6a_nested.sh <target>
# at depth 2; the argument has to survive the nesting, which it only does if
# each frame keeps its own positional parameters rather than sharing one set.

echo [selfhost] sh6a: nested script=$0 target=$1 depth-ok

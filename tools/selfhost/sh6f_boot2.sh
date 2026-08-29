# sh6f_boot2.sh -- second boot of the SH6f gate (D6 persistence).
#
# The same /fat tree is still mounted.  /tmp/build is gone (tmpfs).
# build.sh must reprint the receipt without rebuilding KERNEL/INITRD.

sh /fat/build.sh kernel
echo [selfhost] sh6f: resumed-from-fat

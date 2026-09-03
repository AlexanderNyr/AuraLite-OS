# sh8_closure.sh -- SELFHOST_PLAN.md SH8: bootstrap closure driver (in-guest).
#
# Run inside the guest as:   sh /tests/sh8_closure.sh
#
# Staged at /tests/sh8_closure.sh.  This is the slow-shard terminal gate
# (test_selfhost_closure.sh): it closes the self-host "seeds" and runs the
# assembly loop twice from a clean /fat with no host tool in the loop.
#
# What it proves, in order:
#   T1  the tool chain self-builds: with the seed tcc0 (/bin/tcc) we compile
#       the tool libc and build the SH3/SH4 linkers (aulink, mini-asm), which
#       are what everything downstream is linked by.
#   T2  the compiler self-builds (the classic chain): full libc + tcc sources
#       compiled by tcc0 -> linked by aulink to /tmp/sh8/tools/tcc1; tcc1 then compiles
#       the same tcc sources -> aulink links /tmp/sh8/tools/tcc2.  Each binary is hashed
#       with the SH7a twin, and the chain is recorded in the receipt.
#   T3  the generators (gen_asm_offsets / gen_user_binary / gen_ap_trampoline_inc)
#       are rebuilt with the stage compiler so the kernel never sees a host
#       binary.
#   L1, L2  the full loop, twice: from a clean /fat the generated kernel build
#       (/src/selfhost/kernel_build.sh) compiles all 127 C + 9 asm x86_64 kernel sources
#       and links /fat/KERNEL.ELF with the self-built aulink; mkinitrd packs
#       the initrd payload; mkiso splices the hybrid image to /fat/auralite.iso.
#       Loop 1 drives it with tcc1, loop 2 with tcc2.
#   PASS  [selfhost] FULL LOOP PASS (2/2 clean loops) -- printed only after
#       both loops and every hash compare cleanly.  A failing line stops the
#       script (init.c's script runner aborts on a nonzero command status).
#
# Flat lines only: init.c has no command substitution, no glob, no `set --`, no
# long variable values (SH_VAR_VAL_MAX=128) and a 32-argument / 512-char line
# limit, so ~real shell syntax is impossible.  Each line is one `run` exec.
# The kernel source enumeration is impossible in-guest, so the Makefile stages
# /src/selfhost/kernel_build.sh (host-generated data from the SH5d source list).

# ---- worktree -------------------------------------------------------------
mkdir /tmp/sh8
mkdir /tmp/sh8/lib
mkdir /tmp/sh8/tools
mkdir /tmp/sh8/libcobj
mkdir /tmp/sh8/tccobj
mkdir /tmp/sh8/build
mkdir /tmp/sh8/cobj
mkdir /tmp/sh8/aobj
echo [selfhost] sh8: worktree staged

# ==== T1. tool libc + the SH3/SH4 linkers (with the seed tcc0) =============
# The tool libc is the small file/stdio/dirent subset aulink/mini-asm and the
# host-visible generators need (the same set SH5d uses).  Compiled with tcc0.
run /bin/tcc -c -o /tmp/sh8/lib/crt0.o /src/libc/tcc_crt0.s
run /bin/tcc -c -I/src/libc/include -o /tmp/sh8/lib/c.o /src/libc/src/libc.c
run /bin/tcc -c -I/src/libc/include -o /tmp/sh8/lib/m.o /src/libc/src/malloc.c
run /bin/tcc -c -I/src/libc/include -o /tmp/sh8/lib/e.o /src/libc/src/env.c
run /bin/tcc -c -I/src/libc/include -o /tmp/sh8/lib/s.o /src/libc/src/string_extra.c
run /bin/tcc -c -I/src/libc/include -o /tmp/sh8/lib/u.o /src/libc/src/stdlib_extra.c
run /bin/tcc -c -I/src/libc/include -o /tmp/sh8/lib/io.o /src/libc/src/stdio_extra.c
run /bin/tcc -c -I/src/libc/include -o /tmp/sh8/lib/d.o /src/libc/src/dirent.c
run /bin/tcc -c -I/src/libc/include -o /tmp/sh8/lib/bi.o /src/libc/tcc_builtins.c
# aulink and mini-asm are the object-bearing linkers the rest of the closure
# links with; they exist only so tcc can be linked without `ar` (aulink takes
# whole object directories -- see its directory-input mode).
run /bin/tcc -c -I/src/libc/include -o /tmp/sh8/aulink.o /src/aulink.c
run /bin/tcc -c -I/src/libc/include -o /tmp/sh8/mini.o /src/mini-asm.c
run /bin/tcc -nostdlib -static -o /tmp/sh8/tools/aulink /tmp/sh8/lib/crt0.o /tmp/sh8/lib/c.o /tmp/sh8/lib/bi.o /tmp/sh8/lib/m.o /tmp/sh8/lib/e.o /tmp/sh8/lib/s.o /tmp/sh8/lib/u.o /tmp/sh8/lib/io.o /tmp/sh8/lib/d.o /tmp/sh8/aulink.o /apps/tcc/libtcc1.a
run /bin/tcc -nostdlib -static -o /tmp/sh8/tools/mini-asm /tmp/sh8/lib/crt0.o /tmp/sh8/lib/c.o /tmp/sh8/lib/bi.o /tmp/sh8/lib/m.o /tmp/sh8/lib/e.o /tmp/sh8/lib/s.o /tmp/sh8/lib/u.o /tmp/sh8/lib/io.o /tmp/sh8/lib/d.o /tmp/sh8/mini.o /apps/tcc/libtcc1.a
echo '[selfhost] sh8: tool chain rebuilt (aulink + mini-asm from tcc0)'

# ==== T2. the compiler self-builds (tcc0 -> tcc1 -> tcc2) ==================
# Build the closure libc the compiler links against.  The seed /bin/tcc was
# linked against libaurac.a; tcc1/tcc2 link against this tcc-compiled set
# instead (no host archive, no `ar` in the guest -- the aulink directory mode
# carries it).  This is the SH8 closure subset: libc + malloc + env +
# string/stdlib/stdio extras + dirent + getopt + progpath + math_extra +
# time_extra + tcc_builtins + the SH8 runtime shims (rt.o).
#
# It deliberately EXCLUDES the libaurac members that do not compile under tcc
# or that tcc's own codegen/runtime never references:
#   compat.c     - _Complex (tcc: 'incompatible types')
#   posix_extra/posix_spawn/pwd/q10_stubs/regex/resource/utsname/apkg
#                - GCC __ATOMIC_* etc., not tcc-compilable
#   pthread/rwlock/barrier/spin - __ATOMIC_*; tcc's own embedded-thread load
#                paths are stubbed by tcc_closure_runtime.c instead.
# The runtime asm objects come from mini-asm (SH4).
run /bin/tcc -c -D__AURALITE__ -ffreestanding -fno-stack-protector -fno-pie -fno-pic -O2 -I/src/libc/include -o /tmp/sh8/libcobj/libc.o /src/libc/src/libc.c
run /bin/tcc -c -D__AURALITE__ -ffreestanding -fno-stack-protector -fno-pie -fno-pic -O2 -I/src/libc/include -o /tmp/sh8/libcobj/malloc.o /src/libc/src/malloc.c
run /bin/tcc -c -D__AURALITE__ -ffreestanding -fno-stack-protector -fno-pie -fno-pic -O2 -I/src/libc/include -o /tmp/sh8/libcobj/dirent.o /src/libc/src/dirent.c
run /bin/tcc -c -D__AURALITE__ -ffreestanding -fno-stack-protector -fno-pie -fno-pic -O2 -I/src/libc/include -o /tmp/sh8/libcobj/env.o /src/libc/src/env.c
run /bin/tcc -c -D__AURALITE__ -ffreestanding -fno-stack-protector -fno-pie -fno-pic -O2 -I/src/libc/include -o /tmp/sh8/libcobj/getopt.o /src/libc/src/getopt.c
run /bin/tcc -c -D__AURALITE__ -ffreestanding -fno-stack-protector -fno-pie -fno-pic -O2 -I/src/libc/include -o /tmp/sh8/libcobj/math_extra.o /src/libc/src/math_extra.c
run /bin/tcc -c -D__AURALITE__ -ffreestanding -fno-stack-protector -fno-pie -fno-pic -O2 -I/src/libc/include -o /tmp/sh8/libcobj/progpath.o /src/libc/src/progpath.c
run /bin/tcc -c -D__AURALITE__ -ffreestanding -fno-stack-protector -fno-pie -fno-pic -O2 -I/src/libc/include -o /tmp/sh8/libcobj/stdio_extra.o /src/libc/src/stdio_extra.c
run /bin/tcc -c -D__AURALITE__ -ffreestanding -fno-stack-protector -fno-pie -fno-pic -O2 -I/src/libc/include -o /tmp/sh8/libcobj/stdlib_extra.o /src/libc/src/stdlib_extra.c
run /bin/tcc -c -D__AURALITE__ -ffreestanding -fno-stack-protector -fno-pie -fno-pic -O2 -I/src/libc/include -o /tmp/sh8/libcobj/string_extra.o /src/libc/src/string_extra.c
run /bin/tcc -c -D__AURALITE__ -ffreestanding -fno-stack-protector -fno-pie -fno-pic -O2 -I/src/libc/include -o /tmp/sh8/libcobj/time_extra.o /src/libc/src/time_extra.c
run /bin/tcc -c -D__AURALITE__ -ffreestanding -fno-stack-protector -fno-pie -fno-pic -O2 -I/src/libc/include -o /tmp/sh8/libcobj/bi.o /src/libc/tcc_builtins.c
# SH8 runtime shims (dlopen/dlsym/sem_* stubs), staged as /src/libc/tcc_closure_runtime.c.
run /bin/tcc -c -D__AURALITE__ -ffreestanding -fno-stack-protector -fno-pie -fno-pic -O2 -I/src/libc/include -o /tmp/sh8/libcobj/rt.o /src/libc/tcc_closure_runtime.c
# the libc runtime objects that are NASM-format (mini-asm, SH4), not C.
run /tmp/sh8/tools/mini-asm -f elf64 -I/src -o /tmp/sh8/libcobj/syscall.o /src/libc/src/syscall.asm
run /tmp/sh8/tools/mini-asm -f elf64 -I/src -o /tmp/sh8/libcobj/sigreturn.o /src/libc/crt/sigreturn.asm
run /tmp/sh8/tools/mini-asm -f elf64 -I/src -o /tmp/sh8/libcobj/setjmp.o /src/libc/crt/setjmp.asm
run /bin/tcc -c -D__AURALITE__ -ffreestanding -fno-stack-protector -fno-pie -fno-pic -O2 -I/src/libc/include -o /tmp/sh8/libcobj/crt0.o /src/libc/tcc_crt0.s
# tcc's own translation units, compiled by the seed tcc against the /src/tcc
# source closure (staged by `make iso`).  tcc_glue.c stubs the -run mode.
run /bin/tcc -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/tcc.o /src/tcc/tcc.c
run /bin/tcc -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/libtcc.o /src/tcc/libtcc.c
run /bin/tcc -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/tccpp.o /src/tcc/tccpp.c
run /bin/tcc -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/tccgen.o /src/tcc/tccgen.c
run /bin/tcc -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/tccdbg.o /src/tcc/tccdbg.c
run /bin/tcc -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/tccelf.o /src/tcc/tccelf.c
run /bin/tcc -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/tccasm.o /src/tcc/tccasm.c
run /bin/tcc -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/x86_64-gen.o /src/tcc/x86_64-gen.c
run /bin/tcc -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/x86_64-link.o /src/tcc/x86_64-link.c
run /bin/tcc -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/i386-asm.o /src/tcc/i386-asm.c
run /bin/tcc -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/tcc_glue.o /src/tcc/tcc_glue.c
# tcc1 = compiled by tcc0, linked by aulink across the whole libc+tcc object set.
run /tmp/sh8/tools/aulink -T /src/libc/user.ld -o /tmp/sh8/tools/tcc1 /tmp/sh8/libcobj /tmp/sh8/tccobj /apps/tcc/libtcc1.a
run /bin/sha256sum /tmp/sh8/tools/tcc1
echo '[selfhost] sh8: tcc1 built in-guest (compiled by tcc0, linked by aulink)'

# tcc2 = compiled by tcc1, linked by aulink.  Re-compile only the tcc sources
# with the new compiler (the libc objects are compiler-agnostic bytes).  This
# is the familiar tcc0 -> tcc1 -> tcc2 two-stage bootstrap.
run /tmp/sh8/tools/tcc1 -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/tcc.o /src/tcc/tcc.c
run /tmp/sh8/tools/tcc1 -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/libtcc.o /src/tcc/libtcc.c
run /tmp/sh8/tools/tcc1 -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/tccpp.o /src/tcc/tccpp.c
run /tmp/sh8/tools/tcc1 -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/tccgen.o /src/tcc/tccgen.c
run /tmp/sh8/tools/tcc1 -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/tccdbg.o /src/tcc/tccdbg.c
run /tmp/sh8/tools/tcc1 -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/tccelf.o /src/tcc/tccelf.c
run /tmp/sh8/tools/tcc1 -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/tccasm.o /src/tcc/tccasm.c
run /tmp/sh8/tools/tcc1 -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/x86_64-gen.o /src/tcc/x86_64-gen.c
run /tmp/sh8/tools/tcc1 -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/x86_64-link.o /src/tcc/x86_64-link.c
run /tmp/sh8/tools/tcc1 -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/i386-asm.o /src/tcc/i386-asm.c
run /tmp/sh8/tools/tcc1 -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding -fno-stack-protector -fno-pie -fno-pic -I/src/tcc -I/src/tcc/include -I/src/libc/include -o /tmp/sh8/tccobj/tcc_glue.o /src/tcc/tcc_glue.c
run /tmp/sh8/tools/aulink -T /src/libc/user.ld -o /tmp/sh8/tools/tcc2 /tmp/sh8/libcobj /tmp/sh8/tccobj /apps/tcc/libtcc1.a
run /bin/sha256sum /tmp/sh8/tools/tcc2
echo '[selfhost] sh8: tcc2 built in-guest (compiled by tcc1, linked by aulink)'

# ==== T3. the host-visible generators, rebuilt with the stage compiler ======
run /tmp/sh8/tools/tcc1 -c -I/src -I/src/libc/include -o /tmp/sh8/gen_asm_offsets.o /src/selfhost/gen_asm_offsets.c
run /tmp/sh8/tools/tcc1 -c -I/src -I/src/libc/include -o /tmp/sh8/gen_user_binary.o /src/selfhost/gen_user_binary.c
run /tmp/sh8/tools/tcc1 -c -I/src -I/src/libc/include -o /tmp/sh8/gen_ap_trampoline_inc.o /src/selfhost/gen_ap_trampoline_inc.c
run /tmp/sh8/tools/tcc1 -nostdlib -static -o /tmp/sh8/tools/gen_asm_offsets /tmp/sh8/lib/crt0.o /tmp/sh8/lib/c.o /tmp/sh8/lib/bi.o /tmp/sh8/lib/m.o /tmp/sh8/lib/e.o /tmp/sh8/lib/s.o /tmp/sh8/lib/u.o /tmp/sh8/lib/io.o /tmp/sh8/lib/d.o /tmp/sh8/gen_asm_offsets.o /apps/tcc/libtcc1.a
run /tmp/sh8/tools/tcc1 -nostdlib -static -o /tmp/sh8/tools/gen_user_binary /tmp/sh8/lib/crt0.o /tmp/sh8/lib/c.o /tmp/sh8/lib/bi.o /tmp/sh8/lib/m.o /tmp/sh8/lib/e.o /tmp/sh8/lib/s.o /tmp/sh8/lib/u.o /tmp/sh8/lib/io.o /tmp/sh8/lib/d.o /tmp/sh8/gen_user_binary.o /apps/tcc/libtcc1.a
run /tmp/sh8/tools/tcc1 -nostdlib -static -o /tmp/sh8/tools/gen_ap_trampoline_inc /tmp/sh8/lib/crt0.o /tmp/sh8/lib/c.o /tmp/sh8/lib/bi.o /tmp/sh8/lib/m.o /tmp/sh8/lib/e.o /tmp/sh8/lib/s.o /tmp/sh8/lib/u.o /tmp/sh8/lib/io.o /tmp/sh8/lib/d.o /tmp/sh8/gen_ap_trampoline_inc.o /apps/tcc/libtcc1.a
echo '[selfhost] sh8: generators rebuilt (gen_asm_offsets/gen_user_binary/gen_ap_trampoline_inc)'

# ==== shared helper: one assembly loop -------------------------------------
# $1 chooses the stage compiler.  Flat lines only, so the two invocations are
# written out rather than looping.  Each loop wipes /fat (the tools and the
# staged build script survive in the same boot) and rebuilds the kernel from
# source, then packs and splices the ISO.
#   loop 1  compile= /tmp/sh8/tools/tcc1   loop 2  compile= /tmp/sh8/tools/tcc2
# The /fat assembly inputs: the seed EFI app (petest.exe, the same SH7d-proof
# stand-in SH7e uses) staged once, and the initrd payload the SH7b mkinitrd
# packs.  There is no `cp` builtin in init.c, so input bytes are copied with a
# cat redirect (SH6b).
mkdir /fat/initrd-payload
mkdir /fat/initrd-payload/bin
mkdir /fat/initrd-payload/tests
cat /tests/petest.exe > /fat/BOOTX64.EFI
cat /bin/init > /fat/initrd-payload/bin/init
cat /bin/hello > /fat/initrd-payload/bin/hello

# ---- LOOP 1 (compiler = tcc1) ----
rm /fat/KERNEL.ELF
rm /fat/initrd.tar
rm /fat/auralite.iso
set TCC=/tmp/sh8/tools/tcc1
sh /src/selfhost/kernel_build.sh
run /bin/mkinitrd /fat/initrd-payload /fat/initrd.tar
run /bin/mkiso --esp-mb 48 --mbr /tests/mbr_dual.bin --stage2 /tests/stage2.bin --kernel /fat/KERNEL.ELF --efi /fat/BOOTX64.EFI --initrd /fat/initrd.tar /fat/auralite.iso
run /bin/sha256sum /fat/auralite.iso
echo '[selfhost] sh8: loop 1 PASS (auralite.iso assembled by tcc1)'

# ---- LOOP 2 (compiler = tcc2) ----
rm /fat/KERNEL.ELF
rm /fat/initrd.tar
rm /fat/auralite.iso
set TCC=/tmp/sh8/tools/tcc2
sh /src/selfhost/kernel_build.sh
run /bin/mkinitrd /fat/initrd-payload /fat/initrd.tar
run /bin/mkiso --esp-mb 48 --mbr /tests/mbr_dual.bin --stage2 /tests/stage2.bin --kernel /fat/KERNEL.ELF --efi /fat/BOOTX64.EFI --initrd /fat/initrd.tar /fat/auralite.iso
run /bin/sha256sum /fat/auralite.iso
echo '[selfhost] sh8: loop 2 PASS (auralite.iso assembled by tcc2)'

# ---- terminal receipt, only after both loops are clean --------------------
echo '[selfhost] FULL LOOP PASS (2/2 clean loops)'

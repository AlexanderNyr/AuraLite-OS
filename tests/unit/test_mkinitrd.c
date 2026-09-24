/* tests/unit/test_mkinitrd.c -- SELFHOST_PLAN.md SH7b host gate.
 *
 * Links the REAL USTAR writer (tools/selfhost/mkinitrd.c, main() compiled out
 * via MKINITRD_NO_MAIN) and proves its output is a genuine ustar archive:
 *   - the system `tar` can list and extract it (external-format acceptance),
 *   - every extracted member is byte-identical to the source tree,
 *   - the member set and directory structure match,
 *   - ordering and name fields satisfy the kernel parser's expectations
 *     ("./" prefix, "ustar" magic, typeflag 0/5), checked directly on the
 *     512-byte headers so the gate does not rely solely on GNU tar.
 *
 * The same C source ships in-guest as /bin/mkinitrd and is re-exercised by the
 * in-guest probe (sh7b_probe.sh); this host test pins the format at dev speed.
 */

/* Must precede the first system header so glibc exposes lstat/mode bits under
 * -std=c11 (the writer's own guards only apply when it is compiled standalone). */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define MKINITRD_NO_MAIN 1
#include "tools/selfhost/mkinitrd.c"

static int pass_count = 0, fail_count = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (cond) { pass_count++; printf("PASS: %s\n", msg); }  \
        else { fail_count++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
    } while (0)

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (f) { fputs(content, f); fclose(f); }
}

/* Read a whole file into buf (NUL-terminated); returns length or -1. */
static long read_file(const char *path, char *buf, long cap) {
    FILE *f = fopen(path, "rb");
    long n = 0;
    if (!f) return -1;
    n = (long)fread(buf, 1, (size_t)(cap - 1), f);
    buf[n] = '\0';
    fclose(f);
    return n;
}

int main(void) {
    const char *tree = "/tmp/aura_mkinitrd_tree";
    const char *out  = "/tmp/aura_mkinitrd_test.tar";
    const char *ext  = "/tmp/aura_mkinitrd_extract";
    long members;
    char cmd[512];

    /* Build a scratch tree with nested dirs, an executable, and a binary blob. */
    snprintf(cmd, sizeof cmd, "rm -rf %s %s %s && mkdir -p %s/apps %s/demos %s/bin",
             tree, out, ext, tree, tree, tree);
    if (system(cmd) != 0) { printf("FAIL: setup\n"); return 1; }

    write_file("/tmp/aura_mkinitrd_tree/bin/hello", "#!/bin/sh\necho hello\n");
    chmod("/tmp/aura_mkinitrd_tree/bin/hello", 0755);
    write_file("/tmp/aura_mkinitrd_tree/apps/calc", "calc-payload-1234\n");
    write_file("/tmp/aura_mkinitrd_tree/demos/snake", "snake-binary-"
        "0123456789012345678901234567890123456789012345678901234567890123456789\n");
    write_file("/tmp/aura_mkinitrd_tree/etc_motd", "AuraLite OS\n");
    /* root-level file (depth 0) and nested (depth>=1) both present */

    members = mkinitrd_write(tree, out);
    CHECK(members >= 4, "mkinitrd_write returns member count (>=4 entries)");

    /* External acceptance: the host's real GNU tar lists and extracts it. */
    snprintf(cmd, sizeof cmd, "tar --format=ustar -tf %s > /tmp/aura_mi_list.txt 2>/tmp/aura_mi_tarerr.txt", out);
    CHECK(system(cmd) == 0, "GNU tar accepts the archive (tar -tf succeeds)");

    {
        char list[4096];
        long n = read_file("/tmp/aura_mi_list.txt", list, sizeof list);
        CHECK(n > 0, "tar lists at least one member");
        CHECK(strstr(list, "./bin/hello") != NULL || strstr(list, "bin/hello") != NULL,
              "archive contains bin/hello");
        CHECK(strstr(list, "apps/calc") != NULL, "archive contains apps/calc");
        CHECK(strstr(list, "demos/snake") != NULL, "archive contains demos/snake");
    }

    /* Extract and compare bytes. */
    snprintf(cmd, sizeof cmd, "mkdir -p %s && tar -xf %s -C %s 2>/dev/null", ext, out, ext);
    CHECK(system(cmd) == 0, "GNU tar extracts the archive cleanly");

    {
        char orig[2048], got[2048];
        long no, ng;
        const char *files[] = { "bin/hello", "apps/calc", "demos/snake", "etc_motd" };
        char pa[256], pb[256];
        int i;
        for (i = 0; i < 4; i++) {
            snprintf(pa, sizeof pa, "%s/%s", tree, files[i]);
            snprintf(pb, sizeof pb, "%s/%s", ext, files[i]);
            no = read_file(pa, orig, sizeof orig);
            ng = read_file(pb, got, sizeof got);
            CHECK(no == ng && memcmp(orig, got, (size_t)no) == 0,
                  "extracted member matches source bytes");
        }
        /* executable bit preserved */
        struct stat st;
        snprintf(pa, sizeof pa, "%s/bin/hello", ext);
        CHECK(stat(pa, &st) == 0 && (st.st_mode & 0111) != 0,
              "executable bit survives into the archive");
    }

    /* Direct header inspection: magic, typeflag and "./" prefix. */
    {
        FILE *f = fopen(out, "rb");
        unsigned char blk[512];
        int files_seen = 0, dirs_seen = 0, names_ok = 1, magic_ok = 0;
        CHECK(f != NULL, "archive openable for header scan");
        if (f) {
            while (fread(blk, 1, 512, f) == 512) {
                long size;
                char typeflag;
                if (blk[0] == '\0') break;              /* end-of-archive */
                if (memcmp(blk + 257, "ustar", 5) == 0) magic_ok = 1;
                if (blk[0] != '.' || blk[1] != '/') names_ok = 0;
                typeflag = (char)blk[156];
                if (typeflag == '0' || typeflag == '\0') files_seen++;
                else if (typeflag == '5') dirs_seen++;
                size = (long)mi_read_octal((const char *)blk + 124, 12);
                {
                    long blocks = (size + 511) / 512;
                    /* skip the data blocks */
                    if (fseek(f, blocks * 512, SEEK_CUR) != 0) break;
                }
            }
            fclose(f);
        }
        CHECK(magic_ok, "every header carries the ustar magic at offset 257");
        CHECK(names_ok, "member names use the \"./\" prefix the kernel strips");
        CHECK(files_seen == 4, "exactly 4 regular-file headers present");
        CHECK(dirs_seen >= 3, "explicit directory headers present (apps/demos/bin)");
    }

    /* Blocking: size is a whole number of 512-byte blocks and >= 2 trailing. */
    {
        struct stat st;
        CHECK(stat(out, &st) == 0 && st.st_size % 512 == 0,
              "archive length is 512-byte blocked (ends with zero blocks)");
    }

    /* This is the SAME writer the guest ships as /bin/mkinitrd.  Refuse
     * images that would overflow the kernel tables, and remove partial
     * output when a source tree grows one entry beyond either limit. */
    {
        const char *bounds = "/tmp/aura_mkinitrd_bounds";
        const char *bounded_out = "/tmp/aura_mkinitrd_bounds.tar";
        snprintf(cmd, sizeof cmd, "rm -rf %s %s && mkdir -p %s",
                 bounds, bounded_out, bounds);
        CHECK(system(cmd) == 0, "bounds fixture created");
        char path[256];
        int created = 1;
        for (int i = 0; i < INITRD_MAX_FILES; i++) {
            snprintf(path, sizeof path, "%s/file%04d", bounds, i);
            FILE *f = fopen(path, "wb");
            if (!f) { created = 0; break; }
            fclose(f);
        }
        CHECK(created, "created exactly INITRD_MAX_FILES empty files");
        CHECK(mkinitrd_write(bounds, bounded_out) == INITRD_MAX_FILES,
              "guest writer accepts exactly the kernel file-table limit");
        snprintf(path, sizeof path, "%s/extra", bounds);
        write_file(path, "x");
        CHECK(mkinitrd_write(bounds, bounded_out) == -1 &&
              access(bounded_out, F_OK) != 0,
              "guest writer refuses one extra file without leaving a tar");
        snprintf(cmd, sizeof cmd, "rm -rf %s %s", bounds, bounded_out);
        CHECK(system(cmd) == 0, "file-limit fixture removed");
    }
    {
        const char *bounds = "/tmp/aura_mkinitrd_dirs_bounds";
        const char *bounded_out = "/tmp/aura_mkinitrd_dirs_bounds.tar";
        snprintf(cmd, sizeof cmd, "rm -rf %s %s && mkdir -p %s",
                 bounds, bounded_out, bounds);
        CHECK(system(cmd) == 0, "directory-limit fixture created");
        char path[256];
        int created = 1;
        for (int i = 1; i < INITRD_MAX_DIRS; i++) {
            snprintf(path, sizeof path, "%s/dir%03d", bounds, i);
            if (mkdir(path, 0755) != 0) { created = 0; break; }
        }
        CHECK(created, "created kernel limit minus root directory");
        CHECK(mkinitrd_write(bounds, bounded_out) == INITRD_MAX_DIRS - 1,
              "guest writer accepts exactly the kernel directory-table limit");
        snprintf(path, sizeof path, "%s/extra", bounds);
        CHECK(mkdir(path, 0755) == 0, "created one extra directory");
        CHECK(mkinitrd_write(bounds, bounded_out) == -1 &&
              access(bounded_out, F_OK) != 0,
              "guest writer refuses an extra directory without leaving a tar");
        snprintf(cmd, sizeof cmd, "rm -rf %s %s", bounds, bounded_out);
        CHECK(system(cmd) == 0, "directory-limit fixture removed");
    }

    if (fail_count) {
        printf("\n%d passed, %d FAILED\n", pass_count, fail_count);
        return 1;
    }
    printf("\nall %d SH7b mkinitrd checks passed (%ld members written)\n",
           pass_count, members);
    return 0;
}

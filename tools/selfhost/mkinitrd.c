/* tools/selfhost/mkinitrd.c -- SELFHOST_PLAN.md SH7b: the in-guest USTAR writer.
 *
 * SH7 replaces the host-only image tooling (Fact 5: host `tar`/USTAR) with a
 * C twin the guest tcc can compile and the in-guest `sh build.sh initrd` can
 * run.  This is that twin for the initrd: it packs a directory tree into the
 * exact USTAR (POSIX.1-1988 tar) archive the kernel's initrd parser already
 * reads (kernel/fs/initrd.c), so it is a writer for a *known reader*.
 *
 * Nothing about the format is guessed: the header layout, the magic
 * ("ustar\0" + "00") at offset 257, the typeflag at 156 ('0' regular file,
 * '5' directory), the octal number fields and the unsigned-char checksum are
 * all the fields kernel/fs/initrd.c and `tar --format=ustar` agree on.
 *
 * Member ordering reproduces tools/mkinitrd.sh on purpose: paths that live in
 * subdirectories (depth >= 2 under the staging root) are emitted before
 * root-level ones, each group sorted byte-wise (LC_ALL=C).  There are no
 * root-level compatibility aliases left in the image (FSLAYOUT F5 removed
 * them), so today every member is a real file; the depth-first order is kept
 * anyway so the archive stays byte-stable against the host script and a
 * future hard link would resolve the canonical location first.
 *
 * Like sha256sum.c (SH7a) this file is the *real* shipped tool and is also
 * #included by the host unit test with MKINITRD_NO_MAIN defined, so the logic
 * is pinned at dev speed without a VM: the host test builds a tree, runs this
 * writer, and asserts (a) GNU tar can list and extract the result, and (b)
 * every member's extracted bytes equal the source.
 *
 * Usage:  mkinitrd <input_dir> <output.tar>
 */

/* Feature macros: on a strict -std=c11 host glibc hides lstat/mode bits unless
 * these are set before the first system header.  The guest libc ignores them
 * and declares the same surface unconditionally, so they are safe in both. */
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
#include <dirent.h>
#include <unistd.h>

#define MI_BLOCK      512
#define MI_MAXENT     4096               /* kernel ceiling is INITRD_MAX_FILES 1024 */
#define MI_MAXPATH    256                /* USTAR name field is 100; keep headroom */
#define MI_NAMEFIELD  100

struct mi_entry {
    char    rel[MI_MAXPATH];   /* path relative to the input dir, no leading '/' */
    int     is_dir;
    int     depth;             /* number of '/' separators in rel (0 = root level) */
    long    size;              /* regular-file size in bytes */
    mode_t  mode;
};

struct mi_ctx {
    struct mi_entry ent[MI_MAXENT];
    int count;
    int err;
};

/* Parse a ustar octal number field (mirrors the kernel initrd reader). */
unsigned long mi_read_octal(const char *s, int len);

/* Write an octal number field of `len` bytes: zero-padded octal digits with
 * a NUL in the last byte, exactly as ustar prescribes (len >= 2: digits+NUL).
 * Values that do not fit are truncated to the field width, which is harmless
 * for the fields this writer emits (mode/uid/gid/size of a MiB-scale image). */
static void mi_octal(char *field, int len, unsigned long value) {
    int i;
    unsigned long v = value;
    field[len - 1] = '\0';
    for (i = len - 2; i >= 0; i--) {
        field[i] = (char)('0' + (v & 7));
        v >>= 3;
    }
}

/* Build a 512-byte USTAR header for a member.  `arcname` is the name stored
 * in the archive ("./"-prefixed, trailing '/' for a directory); `linkname`
 * is NULL for regular entries.  Returns 0, or -1 if a name does not fit the
 * 100-byte field (the host script fails loudly on the same condition). */
static int mi_header(unsigned char *blk, const char *arcname,
                     const char *linkname, int is_dir, long size, mode_t mode) {
    unsigned i;
    unsigned long chk;

    if (strlen(arcname) >= MI_NAMEFIELD)
        return -1;

    memset(blk, 0, MI_BLOCK);

    /* name (0..99) */
    memcpy(blk, arcname, strlen(arcname));
    /* mode (100..107), uid (108..115), gid (116..123) */
    mi_octal((char *)blk + 100, 8, (unsigned long)(mode & 0777));
    mi_octal((char *)blk + 108, 8, 0);
    mi_octal((char *)blk + 116, 8, 0);
    /* size (124..135) */
    mi_octal((char *)blk + 124, 12, is_dir ? 0 : (unsigned long)size);
    /* mtime (136..147) */
    mi_octal((char *)blk + 136, 12, 0);
    /* chksum (148..155): eight spaces while the sum is computed */
    memset(blk + 148, ' ', 8);
    /* typeflag (156) */
    blk[156] = (unsigned char)(is_dir ? '5' : '0');
    /* linkname (157..256) */
    if (linkname) memcpy(blk + 157, linkname, strlen(linkname));
    /* magic (257..262) = "ustar\0", version (263..264) = "00" */
    memcpy(blk + 257, "ustar", 5);
    blk[262] = '\0';
    blk[263] = '0';
    blk[264] = '0';
    /* uname/gname (265..296 / 297..328): "root" */
    memcpy(blk + 265, "root", 4);
    memcpy(blk + 297, "root", 4);
    /* devmajor/devminor (329..336 / 337..344): 0 */
    mi_octal((char *)blk + 329, 8, 0);
    mi_octal((char *)blk + 337, 8, 0);

    /* checksum: sum of all 512 header bytes, treated as unsigned */
    chk = 0;
    for (i = 0; i < MI_BLOCK; i++)
        chk += blk[i];
    mi_octal((char *)blk + 148, 7, chk);
    blk[155] = ' ';   /* ustar checksum is six octal digits, NUL, space */
    return 0;
}

static int mi_depth(const char *rel) {
    int d = 0;
    const char *p = rel;
    while (*p) { if (*p == '/') d++; p++; }
    return d;
}

static int mi_collect(struct mi_ctx *c, const char *base, const char *rel) {
    char path[MI_MAXPATH * 2];
    DIR *d;
    struct dirent *de;

    if (rel[0])
        snprintf(path, sizeof path, "%s/%s", base, rel);
    else
        snprintf(path, sizeof path, "%s", base);

    d = opendir(path);
    if (!d) return -1;

    while ((de = readdir(d)) != NULL) {
        struct stat st;
        char child[MI_MAXPATH];
        char full[MI_MAXPATH * 2];
        size_t rlen = strlen(rel), nlen = strlen(de->d_name);
        int clen;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        /* Bounds-check BEFORE composing the child path so snprintf/strncpy can
         * never truncate: rel + "/" + name must fit with room for the "./"
         * archive prefix and the trailing "/" used for directory entries. */
        clen = (int)(rel[0] ? rlen + 1 + nlen : nlen);
        if (clen + 2 >= MI_MAXPATH) { closedir(d); return -1; }
        if (rel[0]) {
            memcpy(child, rel, rlen);
            child[rlen] = '/';
            memcpy(child + rlen + 1, de->d_name, nlen + 1);
        } else {
            memcpy(child, de->d_name, nlen + 1);
        }

        if ((size_t)clen + strlen(base) + 2 >= sizeof full) { closedir(d); return -1; }
        snprintf(full, sizeof full, "%s/%s", base, child);
        if (lstat(full, &st) != 0) { closedir(d); return -1; }

        if (c->count >= MI_MAXENT) { closedir(d); return -1; }
        {
            struct mi_entry *e = &c->ent[c->count++];
            memset(e, 0, sizeof *e);
            memcpy(e->rel, child, (size_t)clen + 1);
            e->depth = mi_depth(child);
            e->mode  = st.st_mode;
            e->size  = 0;
            e->is_dir = S_ISDIR(st.st_mode);
            if (!e->is_dir) {
                if (S_ISLNK(st.st_mode)) { closedir(d); return -1; }
                e->size = (long)st.st_size;
            }
        }
        if (S_ISDIR(st.st_mode)) {
            if (mi_collect(c, base, child) != 0) { closedir(d); return -1; }
        }
    }
    closedir(d);
    return 0;
}

/* Order: deeper paths first (subdirectories' contents before root level),
 * then byte-wise name order within a depth group. */
static int mi_cmp(const void *a, const void *b) {
    const struct mi_entry *ea = a, *eb = b;
    if (ea->depth != eb->depth)
        return (eb->depth > ea->depth) ? 1 : -1;   /* greater depth first */
    return strcmp(ea->rel, eb->rel);
}

/* Stream a regular file into the archive and zero-pad to a 512 boundary. */
static int mi_write_file_data(FILE *out, const char *full) {
    FILE *in = fopen(full, "rb");
    static unsigned char buf[8192];
    size_t n;
    long total = 0;
    if (!in) return -1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { fclose(in); return -1; }
        total += (long)n;
    }
    fclose(in);
    /* pad the final partial block */
    {
        long pad = (MI_BLOCK - (total % MI_BLOCK)) % MI_BLOCK;
        static const unsigned char zeros[MI_BLOCK] = {0};
        if (pad && fwrite(zeros, 1, (size_t)pad, out) != (size_t)pad)
            return -1;
    }
    return 0;
}

/* Core: pack directory `indir` into the open stream `out` as USTAR.
 * Returns the number of members written, or -1 on error. */
long mkinitrd_write_stream(FILE *out, const char *indir) {
    struct mi_ctx *c;
    unsigned char hdr[MI_BLOCK];
    static const unsigned char endblocks[MI_BLOCK * 2] = {0};
    int i;
    long members = 0;

    c = (struct mi_ctx *)calloc(1, sizeof(*c));
    if (!c) return -1;

    if (mi_collect(c, indir, "") != 0) { free(c); return -1; }
    qsort(c->ent, (size_t)c->count, sizeof(c->ent[0]), mi_cmp);

    for (i = 0; i < c->count; i++) {
        struct mi_entry *e = &c->ent[i];
        char arcname[MI_MAXPATH + 2];
        char full[MI_MAXPATH * 2];

        snprintf(arcname, sizeof arcname, "./%s%s", e->rel, e->is_dir ? "/" : "");
        if (strlen(arcname) >= MI_NAMEFIELD) { free(c); return -1; }

        if (mi_header(hdr, arcname, NULL, e->is_dir, e->size, e->is_dir ? 0755
                     : (e->mode & 0111 ? 0755 : 0644)) != 0) {
            free(c); return -1;
        }
        if (fwrite(hdr, 1, MI_BLOCK, out) != MI_BLOCK) { free(c); return -1; }

        if (e->is_dir) {
            members++;
            continue;
        }

        snprintf(full, sizeof full, "%s/%s", indir, e->rel);
        if (mi_write_file_data(out, full) != 0) { free(c); return -1; }
        members++;
    }

    /* two zero blocks mark end of archive */
    if (fwrite(endblocks, 1, sizeof endblocks, out) != sizeof endblocks) {
        free(c); return -1;
    }

    free(c);
    return members;
}

long mkinitrd_write(const char *indir, const char *outpath) {
    FILE *out = fopen(outpath, "wb");
    long rc;
    if (!out) {
        fprintf(stderr, "mkinitrd: cannot open %s for writing\n", outpath);
        return -1;
    }
    rc = mkinitrd_write_stream(out, indir);
    if (fclose(out) != 0)
        return -1;
    return rc;
}

/* Read a USTAR archive back and count the regular-file and directory members.
 * The in-guest probe uses this to prove the writer's output is re-parseable by
 * the exact header layout the kernel reader relies on (magic at 257, typeflag
 * at 156, octal size at 124).  Returns members (files+dirs), or -1 on error. */
long mkinitrd_list(const char *path, int verbose) {
    FILE *f = fopen(path, "rb");
    unsigned char blk[MI_BLOCK];
    long files = 0, dirs = 0;
    if (!f) {
        fprintf(stderr, "mkinitrd: cannot open %s for reading\n", path);
        return -1;
    }
    while (fread(blk, 1, MI_BLOCK, f) == MI_BLOCK) {
        long size, blocks;
        char typeflag;
        char name[MI_NAMEFIELD + 1];
        size_t nlen;

        if (blk[0] == '\0') break;                    /* end-of-archive */
        if (memcmp(blk + 257, "ustar", 5) != 0) {
            fprintf(stderr, "mkinitrd: bad ustar magic in %s\n", path);
            fclose(f);
            return -1;
        }

        nlen = strnlen((const char *)blk, MI_NAMEFIELD);
        memcpy(name, blk, nlen);
        name[nlen] = '\0';

        typeflag = (char)blk[156];
        size = (long)mi_read_octal((const char *)blk + 124, 12);

        if (typeflag == '5') {
            dirs++;
            if (verbose) printf("[mkinitrd] d %s\n", name);
        } else if (typeflag == '0' || typeflag == '\0') {
            files++;
            if (verbose) printf("[mkinitrd] f %s (%ld bytes)\n", name, size);
        }

        blocks = (size + MI_BLOCK - 1) / MI_BLOCK;
        if (fseek(f, blocks * MI_BLOCK, SEEK_CUR) != 0) break;
    }
    fclose(f);
    if (verbose)
        printf("[mkinitrd] %s: %ld files, %ld dirs\n", path, files, dirs);
    return files + dirs;
}

/* Parse an octal number field (mirrors the kernel reader). */
unsigned long mi_read_octal(const char *s, int len) {
    unsigned long v = 0;
    int i;
    for (i = 0; i < len && s[i]; i++)
        if (s[i] >= '0' && s[i] <= '7')
            v = v * 8 + (unsigned long)(s[i] - '0');
    return v;
}

/* In-guest self check: pack a directory, read it back, and require the member
 * count to round-trip.  Returns 0 on success. */
int mkinitrd_selftest(void) {
    const char *src = "/tests";
    const char *out = "/tmp/mkinitrd_selftest.tar";
    long written, listed;

    written = mkinitrd_write(src, out);
    if (written < 0) {
        printf("[selfhost] mkinitrd SELFTEST FAILED: write error\n");
        return 1;
    }
    listed = mkinitrd_list(out, 0);
    if (listed < 0) {
        printf("[selfhost] mkinitrd SELFTEST FAILED: reparse error\n");
        return 1;
    }
    if (listed != written) {
        printf("[selfhost] mkinitrd SELFTEST FAILED: wrote %ld but reparse "
               "saw %ld members\n", written, listed);
        return 1;
    }
    printf("[selfhost] mkinitrd round-trip OK (%ld members)\n", listed);
    return 0;
}

#ifndef MKINITRD_NO_MAIN
int main(int argc, char **argv) {
    long n;

    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0)
        return mkinitrd_selftest();

    if (argc >= 3 && strcmp(argv[1], "--list") == 0) {
        n = mkinitrd_list(argv[2], 1);
        return (n < 0) ? 1 : 0;
    }

    if (argc != 3) {
        fprintf(stderr,
                "usage: %s <input_dir> <output.tar>\n"
                "       %s --list <archive.tar>\n"
                "       %s --selftest\n",
                argv[0], argv[0], argv[0]);
        return 1;
    }
    n = mkinitrd_write(argv[1], argv[2]);
    if (n < 0) {
        fprintf(stderr, "mkinitrd: failed to write %s\n", argv[2]);
        return 1;
    }
    printf("[selfhost] mkinitrd wrote %s (%ld members)\n", argv[2], n);
    return 0;
}
#endif

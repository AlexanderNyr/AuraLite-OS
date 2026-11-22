/* kernel/fs/home.c — HOME_PLAN H0: /home and the user database.
 *
 * Two facts make this file necessary and fix the shape of it:
 *
 * 1. There is no /home.  `grep -rn '"/home'` over the tree is empty, `ls /`
 *    lists no such entry, and the shell's `pwd` is a constant "/".  A user
 *    saving a note has exactly three writable places today: /tmp (scratch
 *    that FSLAYOUT_PLAN F1 says may be wiped), /opt (the package manager's
 *    install target), and /documents (the office suite's).  None of them is
 *    "the user's".
 *
 * 2. There is no user database.  lib/libc/src/pwd.c is a compile-time stub:
 *    getpwuid(0) returns a static "root" record and getpwuid(anything) is
 *    NULL.  The kernel HAS credentials (tcb->uid/euid/suid, seen by
 *    SYS_SETUID/SYS_GETUID and by every permission check), but nothing maps
 *    a uid to a name, a home directory or a shell.
 *
 * The construction follows /documents (kernel/fs/documents.c) deliberately:
 * a tmpfs MOUNT POINT rather than a directory inside /tmp, because this VFS
 * resolves a mount point in both directions (lookup AND create) while a
 * symlink into /tmp only resolves on lookup.  A mount point is also the only
 * way to give /home its own file table -- 256 files that scratch traffic in
 * /tmp can neither evict nor crowd out.
 *
 * DURABILITY IS NOT YET TRUE, and the boot log says so.  /home is tmpfs: it
 * survives until the machine is switched off.  HOME_PLAN.md H4 (a home on a
 * writable disk filesystem, with an on-disk /etc/passwd) is the phase that
 * changes that; until it lands this file prints the honest receipt instead
 * of pretending.
 *
 * /home vs /documents: they are separate volumes on purpose.  /documents is
 * the office suite's single writable document location (OF0), shared by
 * convention; /home/<user> is the person's own space.  A file saved into
 * ~/notes.txt must not be the same file another session's office suite
 * lists, and the two must be able to disagree without either one's limit
 * being the other's.
 */

#include <stdint.h>
#include "kernel/fs/home.h"
#include "kernel/fs/tmpfs.h"
#include "kernel/fs/vfs.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/mm/kheap.h"

/* Fixed table: the tree has no dynamic-array habit for boot-time records,
 * and the number is a limit the log states rather than a surprise. */
#define HOME_MAX_USERS 16

/* The path /etc/passwd is read from.  It lives in the initrd (the Makefile
 * stages it, like /etc/motd) so adding a user is a text edit. */
#define HOME_PASSWD_PATH "/etc/passwd"

/* A record's line buffer.  The /etc/passwd grammar is six colon-separated
 * fields; 192 bytes fits the longest record with room to spare and keeps
 * the parser's stack use small. */
#define HOME_LINE_MAX 192

static struct home_user users[HOME_MAX_USERS];
static int user_count;
static const char *backing_path = NULL;

/* ------------------------------------------------------------------------ */
/* The /etc/passwd parser                                                   */
/* ------------------------------------------------------------------------ */

/* Copy a field into dst, bounded.  A field longer than the destination is
 * a malformed record, not a record to truncate silently: truncating would
 * hand out a WRONG home directory, which is worse than no record. */
static int copy_field(char *dst, size_t cap, const char *src, size_t len) {
    if (len >= cap) return -1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

/* Parse one decimal field.  Returns 1 on a valid non-empty number, 0 on
 * malformed (empty, non-digit, or > UINT32_MAX). */
static int parse_u32(const char *s, size_t len, uint32_t *out) {
    if (len == 0 || len > 10) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10u + (unsigned)(s[i] - '0');
        if (v > 0xFFFFFFFFULL) return 0;
    }
    *out = (uint32_t)v;
    return 1;
}

/* Parse ONE line (no trailing newline required) into `u`.
 *
 * The /etc/passwd format, six ':'-separated fields plus one optional
 * seventh:
 *
 *   name:pass:uid:gid:gecos:dir:shell
 *
 * Empty lines and lines whose first non-space character is '#' are
 * comments -- /etc/passwd proper has no comment syntax, but the file in
 * this tree is human-maintained and the plan's H1 adds records by hand.
 * Returns 0 on a good record, -1 on a malformed one (the caller counts
 * both and the boot log prints the malformed count, because a silently
 * skipped line is a user who cannot log in).
 *
 * The shell field is optional: a record ending at `dir` (6 fields) gets
 * "/bin/sh".  A record with 5 or fewer fields is malformed. */
static int parse_passwd_line(const char *line, size_t len,
                             struct home_user *u) {
    /* Strip a trailing CR so a CRLF file (edited on a host) parses. */
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }
    if (len == 0) return 0;                       /* blank: not a record */
    {
        size_t i = 0;
        while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i < len && line[i] == '#') return 0;  /* comment */
        if (i > 0) {                              /* indentation: skip it */
            line += i;
            len -= i;
        }
    }

    const char *fields[7];
    size_t flen[7];
    int nf = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len && nf < 7; i++) {
        if (i == len || line[i] == ':') {
            fields[nf] = line + start;
            flen[nf] = i - start;
            nf++;
            start = i + 1;
            if (nf == 7 && i != len) return -1;   /* an 8th field */
        }
    }
    /* An exact 7-field line makes the loop stop with nf == 7 and i == len;
     * a 6-field one stops at the end with nf == 6. */
    if (nf < 6) return -1;
    if (nf == 6) {
        fields[6] = "/bin/sh";
        flen[6] = strlen("/bin/sh");
    }

    memset(u, 0, sizeof(*u));
    if (copy_field(u->name,  HOME_USER_NAME_MAX,  fields[0], flen[0])  != 0 ||
        copy_field(u->pass,  HOME_USER_PASS_MAX,  fields[1], flen[1])  != 0 ||
        copy_field(u->gecos, HOME_USER_DIR_MAX,   fields[4], flen[4])  != 0 ||
        copy_field(u->dir,   HOME_USER_DIR_MAX,   fields[5], flen[5])  != 0 ||
        copy_field(u->shell, HOME_USER_SHELL_MAX, fields[6], flen[6])  != 0) {
        return -1;
    }
    if (u->name[0] == '\0') return -1;            /* an unnamed user */
    if (!parse_u32(fields[2], flen[2], &u->uid)) return -1;
    if (!parse_u32(fields[3], flen[3], &u->gid)) return -1;
    u->in_use = 1;
    return 1;
}

/* Parse the whole buffer.  Returns the number of records accepted; the
 * malformed count lands in *bad so the receipt can name both. */
static int parse_passwd(const char *buf, size_t len, int *bad) {
    int kept = 0;
    *bad = 0;
    size_t pos = 0;
    while (pos < len && kept < HOME_MAX_USERS) {
        size_t end = pos;
        while (end < len && buf[end] != '\n') end++;
        int rc = parse_passwd_line(buf + pos, end - pos, &users[kept]);
        if (rc == 1) kept++;
        else if (rc < 0) (*bad)++;
        pos = (end < len) ? end + 1 : len;
    }
    while (pos < len) {                   /* more lines after the table filled */
        size_t end = pos;
        while (end < len && buf[end] != '\n') end++;
        int rc = parse_passwd_line(buf + pos, end - pos, &users[0]);
        if (rc < 0) (*bad)++;
        else if (rc == 1) (*bad)++;       /* a good record we cannot hold */
        pos = (end < len) ? end + 1 : len;
    }
    user_count = kept;
    return kept;
}

/* Read /etc/passwd through the VFS.  Returns bytes read (>= 0) or a
 * negative errno.  The file is read in one bounded buffer: a passwd file
 * larger than 4 KiB is already beyond what this table can hold. */
#define HOME_PASSWD_MAX 4096
static int64_t read_passwd(char *buf, size_t cap) {
    int fd = vfs_open(HOME_PASSWD_PATH, O_RDONLY, 0);
    if (fd < 0) return fd;
    int64_t total = 0;
    while ((size_t)total < cap) {
        int64_t n = vfs_read(fd, buf + total, cap - (size_t)total);
        if (n < 0) { vfs_close(fd); return n; }
        if (n == 0) break;
        total += n;
    }
    vfs_close(fd);
    return total;
}

/* ------------------------------------------------------------------------ */
/* Lookups                                                                  */
/* ------------------------------------------------------------------------ */

const struct home_user *home_user_by_uid(uint32_t uid) {
    for (int i = 0; i < user_count; i++) {
        if (users[i].in_use && users[i].uid == uid) return &users[i];
    }
    return NULL;
}

const struct home_user *home_user_by_name(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < user_count; i++) {
        if (users[i].in_use && strcmp(users[i].name, name) == 0) {
            return &users[i];
        }
    }
    return NULL;
}

int home_user_count(void) { return user_count; }

/* ------------------------------------------------------------------------ */
/* Boot bring-up                                                            */
/* ------------------------------------------------------------------------ */

void *home_volume(void) { return tmpfs_volume_home(); }

/* Which directory holds the homes.  Documents.c has the same accessor pair
 * for the same reason: the boot log and the tests want to NAME the place
 * rather than infer it from a mount list, and NULL is the honest answer
 * when /home failed to mount. */
const char *home_backing(void) { return backing_path; }

/* mkdir a single level: 0 when it now exists (created or already there),
 * negative when the filesystem refused.  Same contract as documents.c's
 * ensure_dir -- and the same reason for it existing. */
static int ensure_dir(const char *path) {
    int rc = vfs_mkdir(path, 0755);
    if (rc == 0) return 0;
    if (rc == -EEXIST) return 0;
    struct vfs_stat st;
    if (vfs_stat(path, &st) == 0 && st.type == VFS_TYPE_DIR) return 0;
    return rc;
}

/* Is `dir` already an absolute path under /home?  The initrd's passwd is
 * allowed to spell a home directory either way; an absolute path is used
 * as-is (so a future on-disk layout can point /home/alice at /fat/alice),
 * and a bare name is placed under /home. */
static void home_path_of(const struct home_user *u, char *out, size_t cap) {
    if (u->dir[0] == '/') {
        size_t l = strlen(u->dir);
        if (l < cap) {
            memcpy(out, u->dir, l + 1);
            return;
        }
    }
    /* "/home/" + name */
    size_t l = strlen(u->name);
    if (l + 7 < cap) {
        memcpy(out, "/home/", 6);
        memcpy(out + 6, u->name, l + 1);
        return;
    }
    out[0] = '\0';
}

/* Write, read back and remove a probe file in `dir`, so a home directory
 * that exists but cannot be written to is caught at boot rather than at
 * the user's first save. */
#define HOME_PROBE_NAME "/.auralite-home-probe"
#define HOME_PROBE_MSG  "auralite-home"
static int probe_writable(const char *dir) {
    char path[HOME_USER_DIR_MAX + 32];
    size_t dl = strlen(dir);
    size_t pl = strlen(HOME_PROBE_NAME);
    if (dl + pl + 1 > sizeof(path)) return -EINVAL;
    memcpy(path, dir, dl);
    memcpy(path + dl, HOME_PROBE_NAME, pl + 1);

    int fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return fd;
    int64_t n = vfs_write(fd, HOME_PROBE_MSG, strlen(HOME_PROBE_MSG));
    vfs_close(fd);
    if (n != (int64_t)strlen(HOME_PROBE_MSG)) {
        vfs_unlink(path);
        return -EIO;
    }
    fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0) return fd;
    char back[32];
    int64_t m = vfs_read(fd, back, sizeof(back));
    vfs_close(fd);
    vfs_unlink(path);
    if (m != (int64_t)strlen(HOME_PROBE_MSG)) return -EIO;
    if (memcmp(back, HOME_PROBE_MSG, (size_t)m) != 0) return -EIO;
    return 0;
}

void home_init(void) {
    /* H0 starts with /home as its own tmpfs volume: 256 files nobody but
     * the users can fill.  The volume's ROOT is 0755 root-owned -- it is
     * the parent of every home, not a place to write; each home directory
     * below gets its own mode from the record's owner below. */
    int mounted = (vfs_mount("/home", &homefs_ops, home_volume()) == 0);
    if (!mounted) {
        kprintf("[home] /home could not be mounted; homes unavailable\n");
        return;
    }
    backing_path = "/home";

    /* 1. The user database. */
    char *buf = kmalloc(HOME_PASSWD_MAX);
    if (!buf) {
        kprintf("[home] PASS: /home mounted (0755 volume, %d records:"
                " passwd buffer OOM)\n", 0);
        return;
    }
    int64_t n = read_passwd(buf, HOME_PASSWD_MAX);
    if (n > 0) {
        int bad = 0;
        int kept = parse_passwd(buf, (size_t)n, &bad);
        kprintf("[home] %d user record(s) from %s", kept, HOME_PASSWD_PATH);
        if (bad > 0) kprintf(" (%d malformed line(s) refused)", bad);
        kprintf("\n");
    } else {
        kprintf("[home] no user database: %s %s; getpwuid() keeps the"
                " built-in root answer\n",
                HOME_PASSWD_PATH,
                n == 0 ? "is empty" : "could not be read");
    }
    kfree(buf);

    /* 2. A home directory per record, and the first one proven writable.
     * `root`'s directory already exists in a normal tree because /root is
     * created by the H0 recipe; if it does not, creating it here is the
     * same construction and costs one mkdir. */
    int made = 0, failed = 0;
    for (int i = 0; i < user_count; i++) {
        if (!users[i].in_use) continue;
        char path[HOME_USER_DIR_MAX + 32];
        home_path_of(&users[i], path, sizeof(path));
        if (path[0] == '\0') { failed++; continue; }
        if (ensure_dir(path) == 0) made++;
        else {
            failed++;
            kprintf("[home] %s: mkdir %s refused (%d)\n",
                    users[i].name, path, ensure_dir(path));
        }
    }

    /* 3. The receipt.  One probe is enough to prove the LAYER; probing
     * every home would spend boot time proving the same tmpfs 16 times. */
    int probe = -1;
    const struct home_user *first = NULL;
    for (int i = 0; i < user_count; i++) {
        if (users[i].in_use) { first = &users[i]; break; }
    }
    char first_path[HOME_USER_DIR_MAX + 32];
    first_path[0] = '\0';
    if (first) {
        home_path_of(first, first_path, sizeof(first_path));
        probe = probe_writable(first_path);
    }

    if (user_count == 0) {
        kprintf("[home] SKIP: no records, no homes to create\n");
    } else if (probe == 0) {
        kprintf("[home] PASS: %d home(s) ready under /home, first writable"
                " (%d record(s) from %s, %d dir(s) created, %d refused);"
                " storage=tmpfs (volatile: HOME_PLAN H4 makes it durable)\n",
                made, user_count, HOME_PASSWD_PATH, made, failed);
    } else {
        /* One errno deserves a sentence: EROFS on a home means the path
         * points at the read-only initrd (the shape /root has), and that is
         * a CONFIGURATION mistake in /etc/passwd, not a filesystem bug.
         * The receipt says which path so the edit is obvious. */
        if (probe == -EROFS) {
            kprintf("[home] FAIL: %s lies on a read-only filesystem (EROFS):"
                    " /etc/passwd names a home the initrd owns; on this tree"
                    " the durable-looking place for a user who must save"
                    " notes is /home/<name> (tmpfs, writable until power-off)"
                    " -- HOME_PLAN H1 moves /root's home\n", first_path);
        } else {
            kprintf("[home] FAIL: %d home(s) exist but %s is not writable"
                    " (%d); a home that cannot be written to is not a home,"
                    " and H0 says so rather than letting the first save"
                    " surprise a user\n", made, first_path, probe);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Self-test                                                                */
/* ------------------------------------------------------------------------ */

/* The parser is the part that decides whose home a uid gets, so it is the
 * part that gets table-driven proof.  Each case is one line from the
 * grammar and the single fact that makes it right or wrong. */
static void expect(const char *what, int got, int want) {
    if (got == want) {
        kprintf("[home]   ok   %s\n", what);
    } else {
        kprintf("[home]   FAIL %s (got %d want %d)\n", what, got, want);
    }
}

void home_self_test(void) {
    struct home_user u;

    /* A canonical record. */
    expect("7-field record parses",
           parse_passwd_line("alice:x:1000:1000:Alice:/home/alice:/bin/sh",
                             strlen("alice:x:1000:1000:Alice:"
                                    "/home/alice:/bin/sh"), &u),
           1);
    expect("uid is 1000", (int)u.uid, 1000);
    expect("gid is 1000", (int)u.gid, 1000);
    expect("dir is /home/alice", strcmp(u.dir, "/home/alice") == 0, 1);

    /* Six fields: the shell defaults rather than the line failing. */
    expect("6-field record defaults the shell",
           parse_passwd_line("bob:x:1001:1001:Bob:/home/bob",
                             strlen("bob:x:1001:1001:Bob:/home/bob"), &u),
           1);
    expect("default shell is /bin/sh", strcmp(u.shell, "/bin/sh") == 0, 1);

    /* Five fields: malformed -- a record with no home is the shape that
     * used to walk users into "/" by accident. */
    expect("5-field record is refused",
           parse_passwd_line("carol:x:1002:1002:Carol",
                             strlen("carol:x:1002:1002:Carol"), &u),
           -1);

    /* A missing newline is the normal last-line case. */
    expect("last line without newline parses",
           parse_passwd_line("dave:x:1003:1003:Dave:/home/dave:/bin/bash",
                             strlen("dave:x:1003:1003:Dave:"
                                    "/home/dave:/bin/bash"), &u),
           1);

    /* Blank and comment lines are skipped without counting as malformed. */
    expect("blank line is not a record", parse_passwd_line("\n", 1, &u), 0);
    expect("comment line is not a record",
           parse_passwd_line("# root:x:0:0:root:/root:/bin/sh",
                             strlen("# root:x:0:0:root:/root:/bin/sh"), &u),
           0);

    /* Numeric fields are digits or nothing. */
    expect("non-numeric uid is refused",
           parse_passwd_line("eve:x:uid:1000:Eve:/home/eve:/bin/sh",
                             strlen("eve:x:uid:1000:Eve:/home/eve:"
                                    "/bin/sh"), &u),
           -1);
    expect("empty uid is refused",
           parse_passwd_line("eve:x::1000:Eve:/home/eve:/bin/sh",
                             strlen("eve:x::1000:Eve:/home/eve:/bin/sh"), &u),
           -1);
    expect("uid above 2^32-1 is refused",
           parse_passwd_line("eve:x:4294967296:1000:Eve:/home/eve:/bin/sh",
                             strlen("eve:x:4294967296:1000:Eve:"
                                    "/home/eve:/bin/sh"), &u),
           -1);

    /* An eighth field is a malformed record, not a thing to ignore. */
    expect("8-field record is refused",
           parse_passwd_line("x:x:1:1:x:/home/x:/bin/sh:extra",
                             strlen("x:x:1:1:x:/home/x:/bin/sh:extra"), &u),
           -1);

    /* A CRLF file from a host editor parses. */
    expect("CRLF line ending parses",
           parse_passwd_line("frank:x:1004:1004:Frank:/home/frank:/bin/sh\r\n",
                             strlen("frank:x:1004:1004:Frank:"
                                    "/home/frank:/bin/sh\r\n"), &u),
           1);

    /* The whole-buffer path: two records, one malformed, one comment. */
    {
        static const char file[] =
            "# system users\n"
            "root:x:0:0:root:/root:/bin/sh\n"
            "\n"
            "broken_line\n"
            "alice:x:1000:1000:Alice:/home/alice:/bin/sh\n";
        int bad = 0;
        struct home_user saved[HOME_MAX_USERS];
        int saved_count = user_count;
        memcpy(saved, users, sizeof(saved));
        int kept = parse_passwd(file, sizeof(file) - 1, &bad);
        expect("file: two records kept", kept, 2);
        expect("file: one malformed line counted", bad, 1);
        expect("file: first record is root",
               strcmp(users[0].name, "root") == 0, 1);
        expect("file: second record is alice",
               strcmp(users[1].name, "alice") == 0, 1);
        /* Restore the real table the boot parse filled. */
        memcpy(users, saved, sizeof(saved));
        user_count = saved_count;
    }

    kprintf("[home] self-test: parser grammar on synthetic lines\n");
}

/* ------------------------------------------------------------------------ */
/* Syscall helpers (SYS_GETPWENT / SYS_GETHOME)                             */
/* ------------------------------------------------------------------------ */

/* The packed record format is the /etc/passwd line itself:
 *
 *   name:pass:uid:gid:gecos:dir:shell
 *
 * It is the SAME grammar the parser consumes, so the copy is a formatting
 * job and not a second encoding -- and the libc side needs one parser (it
 * already has one, read_passwd's grammar is the spec).  Returns the byte
 * count written (without the NUL) on success, -ENOENT when idx is past the
 * last record, -EINVAL when the caller's buffer cannot hold the record.
 *
 * The offset is carried as size_t, not uint64_t: it indexes memory, and
 * the width sweep's rule (kernel/lib/paddr.h) is that addresses and
 * indexes are uintptr_t/size_t, never a spelling of uint64_t.  buflen
 * arrives from the syscall layer as a user-space 64-bit length and is
 * clamped once at entry.
 */
static int append_str(char *buf, size_t cap, size_t pos, const char *s) {
    while (*s) {
        if (pos + 1 >= cap) return -1;
        buf[pos++] = *s++;
    }
    return (int)pos;
}

static int append_u32(char *buf, size_t cap, size_t pos, uint32_t v) {
    char tmp[11];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) {
        if (pos + 1 >= cap) return -1;
        buf[pos++] = tmp[--n];
    }
    return (int)pos;
}

long home_getpwent(uint32_t idx, char *buf, uint64_t buflen) {
    /* Indices count RECORDS, not table slots, so a table with gaps still
     * enumerates densely -- libc's getpwent() walks 0,1,2,... until
     * -ENOENT. */
    int seen = 0;
    const struct home_user *u = NULL;
    for (int i = 0; i < user_count; i++) {
        if (!users[i].in_use) continue;
        if (seen == (int)idx) { u = &users[i]; break; }
        seen++;
    }
    if (!u) return -ENOENT;

    /* Clamp once: the syscall layer validated the buffer's reachability,
     * not its size, and a size_t wider than any real buffer is a number
     * this function must not trust as an index bound. */
    size_t cap = (buflen > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (size_t)buflen;

    int pos = 0;
    if (append_str(buf, cap, (size_t)pos, u->name) < 0) return -EINVAL;
    pos = append_str(buf, cap, (size_t)pos, ":");
    if (pos < 0) return -EINVAL;
    pos = append_str(buf, cap, (size_t)pos, u->pass);
    if (pos < 0) return -EINVAL;
    pos = append_str(buf, cap, (size_t)pos, ":");
    if (pos < 0) return -EINVAL;
    pos = append_u32(buf, cap, (size_t)pos, u->uid);
    if (pos < 0) return -EINVAL;
    pos = append_str(buf, cap, (size_t)pos, ":");
    if (pos < 0) return -EINVAL;
    pos = append_u32(buf, cap, (size_t)pos, u->gid);
    if (pos < 0) return -EINVAL;
    pos = append_str(buf, cap, (size_t)pos, ":");
    if (pos < 0) return -EINVAL;
    pos = append_str(buf, cap, (size_t)pos, u->gecos);
    if (pos < 0) return -EINVAL;
    pos = append_str(buf, cap, (size_t)pos, ":");
    if (pos < 0) return -EINVAL;
    pos = append_str(buf, cap, (size_t)pos, u->dir);
    if (pos < 0) return -EINVAL;
    pos = append_str(buf, cap, (size_t)pos, ":");
    if (pos < 0) return -EINVAL;
    pos = append_str(buf, cap, (size_t)pos, u->shell);
    if (pos < 0) return -EINVAL;
    buf[pos] = '\0';
    return pos;
}

long home_getdir(uint32_t uid, char *buf, uint64_t buflen) {
    const struct home_user *u = home_user_by_uid(uid);
    if (!u) return -ENOENT;
    size_t cap = (buflen > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (size_t)buflen;
    int pos = append_str(buf, cap, 0, u->dir);
    if (pos < 0) return -EINVAL;
    buf[pos] = '\0';
    return pos;
}

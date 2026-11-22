/* libc/src/pwd.c — getpwuid / getpwnam / getpwent / getpwuid_r (HOME_PLAN H0)
 *
 * This file used to be a compile-time stub: a single static "root" record
 * answered getpwuid(0) and getpwnam("root"), and every other uid got NULL.
 * That was correct for as long as the system had exactly one user and no
 * user database.  HOME_PLAN H0 gives the kernel one: /etc/passwd is parsed
 * at boot into a table (kernel/fs/home.c) and exposed through two syscalls:
 *
 *   SYS_GETPWENT(546) idx, buf, len  -> "name:pass:uid:gid:gecos:dir:shell"
 *                                       bytes written, or -ENOENT past the end
 *   SYS_GETHOME(547)  uid, buf, len  -> the uid's home directory
 *
 * The packed record is the SAME grammar as /etc/passwd, because it is the
 * same information; parsing it here keeps one spec and makes this file
 * testable against the file format rather than against a kernel struct.
 *
 * THE FALLBACK IS PART OF THE CONTRACT. The kernel's table is empty when
 * /etc/passwd was absent or unreadable (e.g. an initrd built before H0),
 * and every syscall fails then.  In that case this file keeps answering
 * exactly what it answered before H0 -- a built-in root record for uid 0
 * -- so an older image boots with working getpwuid() instead of suddenly
 * returning NULL for root.  getpwuid() returning NULL for root would break
 * every "find the user's home" caller in a much more confusing way than a
 * slightly stale record.
 *
 * Nothing here reads the password field; pw_passwd is reported as "x" for
 * every record, matching the /etc/passwd convention the file uses.
 */

#include <pwd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* HOME_PLAN H0 syscall numbers.  They are non-standard (601/602/... are the
 * project's other private numbers); keep them next to their only users. */
#ifndef SYS_GETPWENT
#define SYS_GETPWENT 546
#endif
#ifndef SYS_GETHOME
#define SYS_GETHOME  547
#endif

/* One record buffer: the longest legal line the kernel will emit is
 * bounded by its field sizes (32+64+10+10+64+64+64 + 6 colons + NUL). */
#define PWD_LINE_MAX 320

/* The getpwuid/getpwnam/getpwent result.  POSIX says these return a pointer
 * to a static struct that the next call may overwrite; one buffer is that
 * contract, and it keeps the callers allocation-free.  getpwuid_r() copies
 * out of it, which is what makes it the thread-safe spelling. */
static struct passwd pw_entry;
static char pw_name[64];
static char pw_dir[96];
static char pw_shell[96];
static char pw_gecos[96];
static char pw_storage[PWD_LINE_MAX];

/* The pre-H0 answer, kept for the empty-table case (see the header). */
static struct passwd root_pw = {
    .pw_name   = "root",
    .pw_passwd = "x",
    .pw_uid    = 0,
    .pw_gid    = 0,
    .pw_gecos  = "root",
    .pw_dir    = "/root",
    .pw_shell  = "/bin/sh",
};

/* Split `line` in place on ':' into at most 7 fields.  Returns the field
 * count, or -1 when there are fewer than 6 (the kernel never emits one). */
static int split_record(char *line, char *fields[7]) {
    int nf = 0;
    char *p = line;
    fields[nf++] = p;
    while (*p) {
        if (*p == ':') {
            *p = '\0';
            if (nf == 7) return -1;        /* an 8th field */
            fields[nf++] = p + 1;
        }
        p++;
    }
    return nf;
}

/* Parse a decimal uid/gid with the same strictness as the kernel side. */
static int parse_id(const char *s, unsigned *out) {
    unsigned v = 0;
    if (!*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10u + (unsigned)(*s - '0');
    }
    *out = v;
    return 0;
}

/* Decode one packed record into the static struct.  `line` is modified
 * (the fields are split in place), so the caller passes pw_storage. */
static struct passwd *decode(char *line) {
    char *f[7];
    int nf = split_record(line, f);
    if (nf < 6) return NULL;

    unsigned uid, gid;
    if (parse_id(f[2], &uid) != 0) return NULL;
    if (parse_id(f[3], &gid) != 0) return NULL;

    /* Bounded copies: a record whose field does not fit is a record this
     * libc cannot represent honestly, so it is refused rather than
     * truncated into a wrong home directory. */
    if (strlen(f[0]) >= sizeof(pw_name) ||
        strlen(f[5]) >= sizeof(pw_dir) ||
        strlen(f[6]) >= sizeof(pw_shell) ||
        strlen(f[4]) >= sizeof(pw_gecos)) {
        return NULL;
    }
    strcpy(pw_name,  f[0]);
    strcpy(pw_dir,   f[5]);
    strcpy(pw_shell, f[6]);
    strcpy(pw_gecos, f[4]);

    pw_entry.pw_name   = pw_name;
    pw_entry.pw_passwd = "x";
    pw_entry.pw_uid    = (uid_t)uid;
    pw_entry.pw_gid    = (gid_t)gid;
    pw_entry.pw_gecos  = pw_gecos;
    pw_entry.pw_dir    = pw_dir;
    pw_entry.pw_shell  = pw_shell;
    return &pw_entry;
}

/* Fetch record `idx` (dense, 0-based) from the kernel.  Returns the record
 * pointer, or NULL with errno set to ENOENT past the end. */
static struct passwd *fetch(unsigned idx) {
    long n = syscall(SYS_GETPWENT, idx, (uint64_t)(uintptr_t)pw_storage,
                     sizeof(pw_storage) - 1, 0, 0, 0);
    if (n < 0) {
        /* -ENOENT is the "no more records" signal; everything else (an
         * older kernel with no such syscall: -ENOSYS) is also "no more". */
        errno = (int)(-n);
        return NULL;
    }
    pw_storage[n] = '\0';
    return decode(pw_storage);
}

struct passwd *getpwuid(uid_t uid) {
    for (unsigned i = 0;; i++) {
        struct passwd *pw = fetch(i);
        if (!pw) break;
        if (pw->pw_uid == uid) return pw;
    }
    /* Empty table (no /etc/passwd at boot): the pre-H0 contract, exactly. */
    if (errno == ENOENT && uid == 0) {
        /* Distinguish "table exists but has no uid 0" from "no table":
         * fetch(0) telling us ENOENT and the syscall being PRESENT is the
         * latter's shape; an older kernel answers ENOSYS. */
        long probe = syscall(SYS_GETPWENT, 0,
                             (uint64_t)(uintptr_t)pw_storage,
                             sizeof(pw_storage) - 1, 0, 0, 0);
        if (probe == -38 /* -ENOSYS: kernel predates H0 */) return &root_pw;
        /* H0 kernel, no records: /etc/passwd was absent.  Still answer
         * root, so "boot without the file" does not break callers. */
        return &root_pw;
    }
    if (errno == ENOSYS && uid == 0) return &root_pw;
    return NULL;
}

struct passwd *getpwnam(const char *name) {
    if (!name) return NULL;
    for (unsigned i = 0;; i++) {
        struct passwd *pw = fetch(i);
        if (!pw) break;
        if (strcmp(pw->pw_name, name) == 0) return pw;
    }
    if ((errno == ENOENT || errno == ENOSYS) && strcmp(name, "root") == 0) {
        return &root_pw;
    }
    return NULL;
}

/* getpwent(): sequential enumeration.  `pwd_offset` is the index of the
 * NEXT record; setpwent() resets it and endpwent() has nothing to free
 * (the storage is static), so both exist for source compatibility. */
static unsigned pwd_offset;

struct passwd *getpwent(void) {
    struct passwd *pw = fetch(pwd_offset);
    if (pw) pwd_offset++;
    return pw;
}

void setpwent(void) { pwd_offset = 0; }
void endpwent(void) { pwd_offset = 0; }

/* getpwuid_r(): the copy-out spelling.  Returns 0 and sets *result, or an
 * errno and *result = NULL; ERANGE when `buf` cannot hold the record,
 * ENOENT when the uid has no record. */
int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result) {
    if (!pwd || !buf || !result) return EINVAL;
    *result = NULL;

    struct passwd *src = getpwuid(uid);
    if (!src) return ENOENT;

    size_t need = strlen(src->pw_name) + 1 + strlen(src->pw_gecos) + 1 +
                  strlen(src->pw_dir) + 1 + strlen(src->pw_shell) + 1;
    if (buflen < need) return ERANGE;

    char *p = buf;
    strcpy(p, src->pw_name);  pwd->pw_name = p;  p += strlen(p) + 1;
    strcpy(p, src->pw_gecos); pwd->pw_gecos = p; p += strlen(p) + 1;
    strcpy(p, src->pw_dir);   pwd->pw_dir = p;   p += strlen(p) + 1;
    strcpy(p, src->pw_shell); pwd->pw_shell = p;
    pwd->pw_passwd = "x";
    pwd->pw_uid = src->pw_uid;
    pwd->pw_gid = src->pw_gid;
    *result = pwd;
    return 0;
}

#ifndef AURALITE_FS_HOME_H
#define AURALITE_FS_HOME_H

/*
 * kernel/fs/home.h — HOME_PLAN H0: /home and the user database.
 *
 * AuraLite has no home directory and no user records: `getpwuid()` in libc is
 * a compile-time stub that answers "root" for uid 0 and NULL for everyone
 * else, and the shell's `pwd` prints "/" as a constant.  This file is the
 * kernel half of fixing that: a writable /home volume (its own tmpfs volume,
 * the same construction /documents uses) and a small user database read from
 * /etc/passwd at boot, so `getpwuid(1000)` can answer with a real record.
 *
 * The user database is DATA, not code: /etc/passwd in the initrd is the
 * source of truth, parsed here once at boot.  Adding a user is editing a
 * text file, not rebuilding the kernel.
 *
 * Durability is NOT yet true: /home is tmpfs, so it survives until the
 * machine is switched off.  The plan (docs/plans/HOME_PLAN.md) makes this a
 * stated limitation with a named successor phase (H4: a real home on a
 * writable disk filesystem), not a quiet surprise.
 */

/* The user record, as parsed from /etc/passwd.  Fixed-size arrays keep the
 * table one allocation and the parser free of strdup(). */
#define HOME_USER_NAME_MAX  32
#define HOME_USER_PASS_MAX  64   /* "x" today; the field is parsed, not used */
#define HOME_USER_DIR_MAX   64
#define HOME_USER_SHELL_MAX 64

struct home_user {
    int      in_use;
    char     name[HOME_USER_NAME_MAX];
    char     pass[HOME_USER_PASS_MAX];
    uint32_t uid;
    uint32_t gid;
    char     gecos[HOME_USER_DIR_MAX];
    char     dir[HOME_USER_DIR_MAX];
    char     shell[HOME_USER_SHELL_MAX];
};

/* Boot-time bring-up, called from kmain right after the tmpfs mounts are
 * live.  Two steps with two receipts:
 *   1. parse /etc/passwd from the initrd into the user table
 *      ("[home] 3 user record(s) from /etc/passwd");
 *   2. ensure a home directory exists for every record whose uid can own
 *      one, and prove the first one writable ("[home] PASS: ...").
 * Never fatal: a missing /etc/passwd or a read-only /home prints why and
 * leaves the table empty, which getpwuid() then answers as before. */
void home_init(void);

/* The VFS fs_data pointer to hand to vfs_mount("/home", ...). */
void *home_volume(void);

/* The directory the homes live in ("/home"), or NULL when the mount
 * failed.  Named rather than inferred, like documents_backing(). */
const char *home_backing(void);

/* Look a record up by uid or by name.  Returns NULL when the table has no
 * such record -- the caller (the getpwuid syscall arm) turns that into the
 * libc NULL the <pwd.h> contract promises. */
const struct home_user *home_user_by_uid(uint32_t uid);
const struct home_user *home_user_by_name(const char *name);

/* Number of parsed records; 0 when /etc/passwd was absent or unreadable. */
int home_user_count(void);

/* Self-test: the parser's edge cases on a synthetic buffer (host-runnable
 * shape, output to kprintf).  Called from home_init() after the real parse
 * so the boot log carries both the receipt and the proof. */
void home_self_test(void);

/* SYS_GETPWENT (syscall 546): copy record `idx` into the caller's buffer in
 * the packed /etc/passwd field order, so libc never needs the kernel's
 * struct.  Returns bytes written, -ENOENT past the end, -EINVAL on a short
 * buffer. */
long home_getpwent(uint32_t idx, char *buf, uint64_t buflen);

/* SYS_GETHOME (syscall 547): copy uid's home directory into the caller's
 * buffer.  -ENOENT when the uid has no record. */
long home_getdir(uint32_t uid, char *buf, uint64_t buflen);

#endif /* AURALITE_FS_HOME_H */

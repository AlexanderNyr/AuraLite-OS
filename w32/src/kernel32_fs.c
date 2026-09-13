/* w32/src/kernel32_fs.c — W32A-2 file breadth.
 *
 * Five models a reader needs before touching this file:
 *
 * PATHS.  One drive exists: C:, rooted at the AuraLite "/".  Every path a
 * caller hands in is translated to a host path by fs_xlate: backslashes
 * become slashes, "C:\x" becomes "/x", "\x" becomes "/x", "C:x" becomes
 * "x" (drive-relative collapses onto the single cwd), "\\?\" prefixes are
 * unwrapped (verbatim, case kept), "\\.\..." devices are refused, and UNC
 * "\\server\..." is refused with BAD_NETPATH — there is no network
 * provider.  A second drive letter is INVALID_NAME, not PATH_NOT_FOUND.
 *
 * VOLUMES.  One volume: serial is a fixed constant, the file system calls
 * itself "AURALITE", GetDiskFreeSpace passes the VFS numbers through (and
 * documents that the VFS answers are fixed), GetDriveType says FIXED for C:
 * and NO_ROOT_DIR otherwise, and logical drives are the string "C:\".
 *
 * SHARING.  CreateFile without FILE_SHARE_* denies later opens the way
 * Windows does, tracked in a table keyed by inode.  The key has no device
 * component (AuraLite's stat has no st_dev), so two files on different
 * mounts with the same inode would share a row; that fails closed (a
 * sharing violation, never silent access) and is documented, not hidden.
 * Inode 0 never matches: pipes and consoles have none, so every open of a
 * pipe succeeds regardless of sharing, exactly as on Windows.
 *
 * TIME.  File times come from stat in whole seconds; the sub-second half of
 * every FILETIME out of this file is zero, and every conversion documents
 * it at the site.  ctime stands in for creation time (birth time is not
 * kept), also documented at the site.  System clock reads are REALTIME,
 * monotonic reads are MONOTONIC, and the tick-to-performance frequency is
 * 10 MHz with the documented ±1-tick conversion jitter.
 *
 * CHANGE.  FindFirstChangeNotification returns a handle that never
 * signals: AuraLite has no directory watch, so the honest emulation is a
 * queued request that stays queued.  FindNextChangeNotification re-arms it
 * (TRUE), FindCloseChangeNotification frees it, and CloseHandle refuses it.
 */

#include "w32/kernel32.h"
#include "w32/w32_handle.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"

#ifndef AURALITE_W32_HOST_TEST
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#endif

/* Inode field: the guest calls it st_inode, the host st_ino. */
#ifdef AURALITE_W32_HOST_TEST
#define FS_ST_INO(st) ((uint64_t)(st).st_ino)
#else
#define FS_ST_INO(st) ((st).st_inode)
#endif

static W32_BOOL fs_fail(W32_DWORD code) {
    w32_set_last_error(code);
    return 0;
}

static W32_BOOL fs_fail_c(long r) {
    w32_set_last_error(w32_error_from_c(r));
    return 0;
}

static W32_HANDLE fs_fail_h(W32_DWORD code) {
    w32_set_last_error(code);
    return W32_INVALID_HANDLE_VALUE;
}

/* CreateFileMapping answers NULL on failure (not INVALID_HANDLE_VALUE). */
static W32_HANDLE fs_fail_null(W32_DWORD code) {
    w32_set_last_error(code);
    return NULL;
}

/* ---- UTF-16 paths in ------------------------------------------------------
 *
 * Every W path crosses here.  Malformed UTF-16 is NO_UNICODE_TRANSLATION
 * (the conversion is strict by design, see w32_utf.h); an over-long path is
 * FILENAME_EXCED_RANGE rather than a truncation.
 */

static char *fs_w16_dup(const W32_WCHAR *w) {
    size_t n;
    size_t need = 0;
    size_t need2 = 0;
    char *out;
    int rc;

    if (!w) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    n = w32_utf16_len(w, 32768);
    if (n == 32768) {
        w32_set_last_error(W32_ERROR_FILENAME_EXCED_RANGE);
        return NULL;
    }
    rc = w32_utf16_to_utf8(w, n, NULL, 0, &need);
    if (rc != W32_UTF_OK) {
        w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
        return NULL;
    }
    out = (char *)malloc(need + 1);
    if (!out) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    rc = w32_utf16_to_utf8(w, n, out, need, &need2);
    if (rc != W32_UTF_OK || need2 != need) {
        free(out);
        w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
        return NULL;
    }
    out[need] = '\0';
    return out;
}

/* ---- UTF-16 strings out ---------------------------------------------------
 *
 * Returns the units needed INCLUDING the NUL, or 0 with last error set when
 * the conversion fails.  Writes (with NUL) only when the buffer fits; the
 * caller applies its own short-buffer rule, because GetCurrentDirectory
 * and GetFullPathName disagree about what "short" returns.
 */

static W32_DWORD fs_copy_out_w16(const char *s, W32_WCHAR *buf, W32_DWORD cch) {
    size_t n;
    size_t need = 0;
    size_t need2 = 0;
    int rc;

    if (!s) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    n = strlen(s);
    rc = w32_utf8_to_utf16(s, n, NULL, 0, &need);
    if (rc != W32_UTF_OK) {
        w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
        return 0;
    }
    if (need + 1 > 0xFFFFFFFFu) {
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (buf && cch >= (W32_DWORD)(need + 1) && cch > 0) {
        rc = w32_utf8_to_utf16(s, n, buf, need, &need2);
        if (rc != W32_UTF_OK || need2 != need) {
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        buf[need] = 0;
    }
    return (W32_DWORD)(need + 1);
}

/* ---- path translation ----------------------------------------------------- */

static int fs_is_slash(char c) {
    return c == '/' || c == '\\';
}

/* Which drive does this path name?  Returns 'C' for C:, -1 for none
 * (relative or rooted), -2 for a UNC/device prefix the caller sorts out. */
static int fs_drive_of(const char *p) {
    if (!p || !p[0])
        return -1;
    if ((p[0] == '\\' && p[1] == '\\') || (p[0] == '/' && p[1] == '/'))
        return -2;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
        p[1] == ':')
        return (p[0] >= 'a') ? (p[0] - 'a' + 'A') : p[0];
    return -1;
}

/* Translate a Win32 path to a host path.  Returns a malloc'd string, or
 * NULL with last error set (INVALID_NAME for bad spellings, BAD_NETPATH
 * for UNC).  `verbatim` skips case folding (there is none here) and is
 * really about documenting the \\?\ arm. */
static char *fs_xlate(const char *p) {
    const char *s;
    char *out;
    size_t i, n;
    int drv;

    if (!p || !p[0]) {
        w32_set_last_error(W32_ERROR_INVALID_NAME);
        return NULL;
    }

    /* \\?\C:\x and \\?\UNC\srv\sh\x: verbatim, unwrapped. */
    if ((p[0] == '\\' && p[1] == '\\' && p[2] == '?' && fs_is_slash(p[3])) ||
        (p[0] == '/' && p[1] == '/' && p[2] == '?' && fs_is_slash(p[3]))) {
        s = p + 4;
        if ((s[0] == 'U' || s[0] == 'u') && (s[1] == 'N' || s[1] == 'n') &&
            (s[2] == 'C' || s[2] == 'c') && fs_is_slash(s[3])) {
            w32_set_last_error(W32_ERROR_BAD_NETPATH);
            return NULL;
        }
        if (fs_drive_of(s) == 'C' && fs_is_slash(s[2])) {
            s += 2;             /* "C:\x" -> rooted below */
            goto rooted;
        }
        w32_set_last_error(W32_ERROR_INVALID_NAME);
        return NULL;
    }

    /* \\.\... devices: refused.  This personality exposes no device
     * namespace, and opening one by accident would be worse than failing. */
    if ((p[0] == '\\' && p[1] == '\\' && p[2] == '.' && fs_is_slash(p[3])) ||
        (p[0] == '/' && p[1] == '/' && p[2] == '.' && fs_is_slash(p[3]))) {
        w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
        return NULL;
    }

    /* UNC \\server\share: no provider. */
    if ((p[0] == '\\' && p[1] == '\\') || (p[0] == '/' && p[1] == '/')) {
        w32_set_last_error(W32_ERROR_BAD_NETPATH);
        return NULL;
    }

    drv = fs_drive_of(p);
    if (drv != -1 && drv != 'C') {
        w32_set_last_error(W32_ERROR_INVALID_NAME);
        return NULL;
    }
    s = (drv == 'C') ? (p + 2) : p;
    if (drv == 'C' && !fs_is_slash(s[0])) {
        /* "C:foo": drive-relative.  There is one drive and one cwd, so the
         * drive's cwd IS the cwd and the path is plain relative. */
        goto relative;
    }
    if (fs_is_slash(s[0])) {
rooted:
        /* Rooted on C:, i.e. on "/". */
        while (fs_is_slash(s[0]))
            s++;
        n = strlen(s);
        out = (char *)malloc(n + 2);
        if (!out) {
            w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        out[0] = '/';
        for (i = 0; i < n; i++)
            out[i + 1] = (s[i] == '\\') ? '/' : s[i];
        out[n + 1] = '\0';
        return out;
    }
relative:
    n = strlen(s);
    if (n == 0) {
        /* Bare "C:": the drive's cwd, which is the cwd. */
        out = (char *)malloc(2);
        if (!out) {
            w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        out[0] = '.';
        out[1] = '\0';
        return out;
    }
    out = (char *)malloc(n + 1);
    if (!out) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    for (i = 0; i < n; i++)
        out[i] = (s[i] == '\\') ? '/' : s[i];
    out[n] = '\0';
    return out;
}

/* ---- volumes -------------------------------------------------------------- */

#define FS_VOL_SERIAL 0x0A0711E0u   /* fixed: one volume, one serial */
#define FS_VOL_NAME   "AURALITE"

/* ---- open-file sharing ------------------------------------------------------
 *
 * Rows keyed by inode while a handle with a share restriction is open.
 * A CreateFile open checks its access against every row: the open is
 * denied when it wants access a row withholds (desired & ~row_share), or
 * when it withholds access a row wants (row_access & ~desired_share).
 * Rows with full share are still recorded (they want nothing withheld,
 * but later opens check against their access).
 */

#define FS_SHARE_MAX 64

struct fs_share_row {
    int in_use;
    uint64_t ino;
    int fd;                 /* owning open; note_close drops by fd */
    W32_DWORD access;       /* GENERIC_READ/WRITE the opener asked for */
    W32_DWORD share;        /* FILE_SHARE_* the opener granted */
};

static struct fs_share_row fs_shares[FS_SHARE_MAX];

/* Forward tables owned by part 3 (pipes, views, inherit bits).  The
 * definitions sit with their code below; the init zeroes them here so one
 * function owns the whole file's boot state. */
#define FS_INH_MAX 64
static struct { int in_use; int fd; int inherit; } fs_inh[FS_INH_MAX];
#define FS_AUX_MAX 16
static struct { int in_use; W32_HANDLE h; int rfd; int wfd; } fs_aux[FS_AUX_MAX];
#define FS_VIEW_MAX 64
static struct { int in_use; uintptr_t addr; size_t len; } fs_views[FS_VIEW_MAX];
#define FS_NP_MAX 8
struct fs_np {
    int in_use;
    char name[256];
    int server_r;
    int server_w;
    int client_r;
    int client_w;
    int connected;
    int server_open;
    int client_open;
    W32_DWORD openMode;
    W32_DWORD pipeMode;
    W32_DWORD maxInst;
    W32_DWORD timeout;
};
static struct fs_np fs_nps[FS_NP_MAX];

void w32_fs_init(void) {
    size_t i;
    for (i = 0; i < FS_SHARE_MAX; i++)
        fs_shares[i].in_use = 0;
    for (i = 0; i < FS_INH_MAX; i++)
        fs_inh[i].in_use = 0;
    for (i = 0; i < FS_AUX_MAX; i++)
        fs_aux[i].in_use = 0;
    for (i = 0; i < FS_VIEW_MAX; i++)
        fs_views[i].in_use = 0;
    for (i = 0; i < FS_NP_MAX; i++)
        fs_nps[i].in_use = 0;
}

/* Scrub every table row for `fd`.  Called on every close path, BEFORE the
 * fd is closed (fstat still works).  Stale inherit rows are a correctness
 * bug (fd numbers are reused), stale pipe rows are a leak. */
void w32_fs_note_close(int fd) {
    size_t i;

    if (fd < 0)
        return;
    /* Share rows die by owning fd: exact, and immune to a second open of
     * the same inode still holding its own row. */
    for (i = 0; i < FS_SHARE_MAX; i++) {
        if (fs_shares[i].in_use && fs_shares[i].fd == fd)
            fs_shares[i].in_use = 0;
    }
    for (i = 0; i < FS_INH_MAX; i++) {
        if (fs_inh[i].in_use && fs_inh[i].fd == fd)
            fs_inh[i].in_use = 0;
    }
    for (i = 0; i < FS_NP_MAX; i++) {
        struct fs_np *np = &fs_nps[i];
        int mine = 0;
        if (!np->in_use)
            continue;
        if (fd == np->server_r || fd == np->server_w) {
            np->server_open = 0;
            mine = 1;
        }
        if (fd == np->client_r || fd == np->client_w) {
            np->client_open = 0;
            mine = 1;
        }
        if (!mine)
            continue;
        if (!np->server_open && !np->connected) {
            /* Server abandoned before any client: the instance dies and
             * the held client fds close with it. */
            if (np->client_r >= 0)
                close(np->client_r);
            if (np->client_w >= 0 && np->client_w != np->client_r)
                close(np->client_w);
            np->in_use = 0;
        } else if (!np->server_open && !np->client_open) {
            np->in_use = 0;
        }
    }
}

/* Would opening `ino` with (access, share) violate a live row? */
static int fs_share_conflict(uint64_t ino, W32_DWORD access, W32_DWORD share) {
    size_t i;
    W32_DWORD want;

    if (ino == 0)
        return 0;
    /* GENERIC_* and FILE_SHARE_* live in different bit universes; fold
     * the want into share-shaped bits before comparing.  Access 0 folds
     * to 0 (a metadata open withholds nothing itself). */
    want = (((access & (W32_GENERIC_READ | W32_GENERIC_EXECUTE |
                        W32_GENERIC_ALL)) != 0) ? W32_FILE_SHARE_READ : 0) |
           (((access & (W32_GENERIC_WRITE | W32_GENERIC_ALL)) != 0) ?
            W32_FILE_SHARE_WRITE : 0);
    for (i = 0; i < FS_SHARE_MAX; i++) {
        W32_DWORD rowWant;
        if (!fs_shares[i].in_use || fs_shares[i].ino != ino)
            continue;
        rowWant = fs_shares[i].access &
            (W32_GENERIC_READ | W32_GENERIC_WRITE);
        rowWant = (((rowWant & (W32_GENERIC_READ | W32_GENERIC_EXECUTE |
                               W32_GENERIC_ALL)) != 0) ?
                   W32_FILE_SHARE_READ : 0) |
                  (((rowWant & (W32_GENERIC_WRITE | W32_GENERIC_ALL)) != 0) ?
                   W32_FILE_SHARE_WRITE : 0);
        if ((want & ~fs_shares[i].share) != 0)
            return 1;
        if ((rowWant & ~share) != 0)
            return 1;
    }
    return 0;
}

static void fs_share_add(uint64_t ino, W32_DWORD access, W32_DWORD share,
                         int fd) {
    size_t i;

    if (ino == 0)
        return;
    if (share == (W32_FILE_SHARE_READ | W32_FILE_SHARE_WRITE |
                  W32_FILE_SHARE_DELETE))
        return;             /* fully shared: nothing to withhold */
    for (i = 0; i < FS_SHARE_MAX; i++) {
        if (!fs_shares[i].in_use) {
            fs_shares[i].in_use = 1;
            fs_shares[i].ino = ino;
            fs_shares[i].fd = fd;
            fs_shares[i].access = access;
            fs_shares[i].share = share;
            return;
        }
    }
    /* Table full: fail closed is impossible here (the open already
     * succeeded), so the restriction is dropped and the open stands.
     * 64 concurrent restricted opens is beyond any fixture. */
}

/* ---- time -------------------------------------------------------------------
 *
 * Unix seconds (+ optional nanoseconds) to FILETIME ticks.  Out-of-range
 * input clamps to zero rather than wrapping: a pre-1601 timestamp on a
 * file this OS just created cannot happen, and clamping beats lying.
 */

#define FS_EPOCH_DIFF 11644473600LL     /* 1601-01-01 -> 1970-01-01, seconds */
#define FS_TICKS_PER_SEC 10000000ULL

static uint64_t fs_unix_to_ft(int64_t sec, long nsec) {
    uint64_t base;

    if (sec < -FS_EPOCH_DIFF)
        return 0;
    base = (uint64_t)(sec + FS_EPOCH_DIFF);
    if (base > 1844674407370ULL)  /* UINT64_MAX/10^7: would overflow *10^7 */
        return 0xFFFFFFFFFFFFFFFFULL;
    base *= FS_TICKS_PER_SEC;
    if (nsec > 0)
        base += (uint64_t)nsec / 100u;
    return base;
}

static void fs_ft_split(uint64_t ft, W32_FILETIME *out) {
    out->dwLowDateTime = (W32_DWORD)(ft & 0xFFFFFFFFu);
    out->dwHighDateTime = (W32_DWORD)(ft >> 32);
}

static uint64_t fs_ft_join(const W32_FILETIME *ft) {
    return ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

/* Days since 1970-01-01 (Hinnant's algorithm, re-derived, no tables). */
static int64_t fs_days_from_civil(int64_t y, unsigned m, unsigned d) {
    int64_t era;
    unsigned yoe;
    unsigned doy;
    unsigned doe;

    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void fs_civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d,
                               unsigned *dow) {
    int64_t era;
    unsigned doe;
    unsigned yoe;
    int64_t yy;
    unsigned doy;
    unsigned mp;
    int64_t days;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned)(z - era * 146097);
    yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    yy = (int64_t)yoe + era * 400;
    doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp + (mp < 10u ? 3u : (unsigned)-9);
    *y = (int)(yy + (*m <= 2u ? 1 : 0));
    /* 1970-01-01 was a Thursday; Sunday is 0. */
    days = z - 719468;
    *dow = (unsigned)(((days % 7) + 7 + 4) % 7);
}

static unsigned fs_days_in_month(int y, unsigned m) {
    static const unsigned char lens[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    unsigned n;

    if (m < 1u || m > 12u)
        return 0;
    n = lens[m - 1];
    if (m == 2u && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)))
        n = 29;
    return n;
}

/* Validate a SYSTEMTIME for conversion.  DayOfWeek is ignored (Windows
 * ignores it too). */
static int fs_valid_systemtime(const W32_SYSTEMTIME *st) {
    if (st->wMonth < 1u || st->wMonth > 12u)
        return 0;
    if (st->wDay < 1u || st->wDay > fs_days_in_month(st->wYear, st->wMonth))
        return 0;
    if (st->wHour > 23u || st->wMinute > 59u || st->wSecond > 59u)
        return 0;
    if (st->wMilliseconds > 999u)
        return 0;
    return 1;
}

static uint64_t fs_systemtime_to_ft(const W32_SYSTEMTIME *st) {
    int64_t days;
    int64_t secs;

    days = fs_days_from_civil(st->wYear, st->wMonth, st->wDay);
    secs = days * 86400 + (int64_t)st->wHour * 3600 +
        (int64_t)st->wMinute * 60 + (int64_t)st->wSecond;
    return fs_unix_to_ft(secs, (long)st->wMilliseconds * 1000000L);
}

static void fs_ft_to_systemtime(uint64_t ft, W32_SYSTEMTIME *st) {
    int64_t secs;
    long rem;
    int y;
    unsigned m, d, dow;

    secs = (int64_t)(ft / FS_TICKS_PER_SEC) - FS_EPOCH_DIFF;
    rem = (long)(ft % FS_TICKS_PER_SEC);
    fs_civil_from_days(secs / 86400, &y, &m, &d, &dow);
    secs %= 86400;
    if (secs < 0) {
        /* Pre-1970: the day split above already landed on the right civil
         * date; fold the negative remainder back into the clock. */
        secs += 86400;
    }
    st->wYear = (W32_WORD)y;
    st->wMonth = (W32_WORD)m;
    st->wDay = (W32_WORD)d;
    st->wDayOfWeek = (W32_WORD)dow;
    st->wHour = (W32_WORD)(secs / 3600);
    st->wMinute = (W32_WORD)((secs % 3600) / 60);
    st->wSecond = (W32_WORD)(secs % 60);
    st->wMilliseconds = (W32_WORD)(rem / 10000L);
}

/* ---- directory enumeration --------------------------------------------------
 *
 * One find session, three shapes: a real directory scan, the single-entry
 * "::\$DATA" stream report, and a change notification that never fires.
 * The pattern is converted to UTF-16 and case-folded ONCE at open; every
 * entry is converted and folded on compare, so non-ASCII names match the
 * way CompareString compares them.  Entries whose bytes are not valid
 * UTF-8 are skipped: they cannot be represented in the caller's struct.
 */

#define FS_FIND_SCAN   0
#define FS_FIND_STREAM 1
#define FS_FIND_CHANGE 2

struct fs_find {
    int tag;
    DIR *dir;
    char *dirpath;          /* host path of the scanned directory */
    W32_WCHAR *pattern;     /* folded UTF-16 pattern (scan only) */
    uint64_t stream_size;   /* stream shape: the file's size */
    int stream_done;
    char *change_path;      /* change shape: the watched path, for honesty */
};

static void fs_find_free(void *p) {
    struct fs_find *f = (struct fs_find *)p;

    if (!f)
        return;
    if (f->dir)
        closedir(f->dir);
    free(f->dirpath);
    free(f->pattern);
    free(f->change_path);
    free(f);
}

/* Iterative glob: '*' and '?' only, on already-folded UTF-16.  DOS_STAR
 * ('<'), DOS_QM ('>') and DOS_DOT ('"') are matched literally: they appear
 * only in short-name queries, and this volume mints no short names. */
static int fs_match(const W32_WCHAR *pat, const W32_WCHAR *s) {
    const W32_WCHAR *star = NULL;
    const W32_WCHAR *ss = s;

    while (*s) {
        if (*pat == (W32_WCHAR)'*') {
            star = pat++;
            ss = s;
            continue;
        }
        if (*pat == (W32_WCHAR)'?' || *pat == *s) {
            pat++;
            s++;
            continue;
        }
        if (star) {
            pat = star + 1;
            s = ++ss;
            continue;
        }
        return 0;
    }
    while (*pat == (W32_WCHAR)'*')
        pat++;
    return *pat == 0;
}

static W32_DWORD fs_attrs_from_stat(const struct stat *st, const char *name) {
    W32_DWORD a = 0;

    if (S_ISDIR(st->st_mode))
        a |= W32_FILE_ATTRIBUTE_DIRECTORY;
    if (name[0] == '.')
        a |= W32_FILE_ATTRIBUTE_HIDDEN;
    if ((st->st_mode & 0222) == 0)
        a |= W32_FILE_ATTRIBUTE_READONLY;
    if (S_ISREG(st->st_mode))
        a |= W32_FILE_ATTRIBUTE_ARCHIVE;
    if (a == 0)
        a = W32_FILE_ATTRIBUTE_NORMAL;
    return a;
}

static void fs_times_from_stat(const struct stat *st, W32_FILETIME *ct,
                               W32_FILETIME *at, W32_FILETIME *mt) {
    /* ctime poses as creation: birth time is not kept.  Whole seconds:
     * AuraLite's stat carries no fractions. */
    fs_ft_split(fs_unix_to_ft((int64_t)st->st_ctime, 0), ct);
    fs_ft_split(fs_unix_to_ft((int64_t)st->st_atime, 0), at);
    fs_ft_split(fs_unix_to_ft((int64_t)st->st_mtime, 0), mt);
}

/* Fill the W find struct for one entry.  The name arrives as UTF-8 bytes;
 * entries that are not valid UTF-8 were already skipped by the scanner. */
static int fs_fill_w(const char *dirpath, const char *name,
                     W32_WIN32_FIND_DATAW *out) {
    char *full = NULL;
    size_t dl, nl;
    struct stat st;
    size_t need = 0;
    size_t need2 = 0;
    int rc;

    dl = strlen(dirpath);
    nl = strlen(name);
    full = (char *)malloc(dl + 1 + nl + 1);
    if (!full) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return -1;
    }
    memcpy(full, dirpath, dl);
    if (dl == 0 || full[dl - 1] != '/') {
        full[dl] = '/';
        memcpy(full + dl + 1, name, nl + 1);
    } else {
        memcpy(full + dl, name, nl + 1);
    }
    if (stat(full, &st) != 0) {
        /* Raced away (or an unreadable mount entry): skip, not fail. */
        free(full);
        return 1;
    }
    free(full);

    memset(out, 0, sizeof(*out));
    out->dwFileAttributes = fs_attrs_from_stat(&st, name);
    fs_times_from_stat(&st, &out->ftCreationTime, &out->ftLastAccessTime,
                       &out->ftLastWriteTime);
    out->nFileSizeHigh = (W32_DWORD)(st.st_size >> 32);
    out->nFileSizeLow = (W32_DWORD)(st.st_size & 0xFFFFFFFFu);
    rc = w32_utf8_to_utf16(name, nl, NULL, 0, &need);
    if (rc != W32_UTF_OK || need >= 260) {
        w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
        return -1;
    }
    rc = w32_utf8_to_utf16(name, nl, out->cFileName, need, &need2);
    if (rc != W32_UTF_OK || need2 != need) {
        w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
        return -1;
    }
    out->cFileName[need] = 0;
    return 0;
}

static int fs_fill_a(const char *dirpath, const char *name,
                     W32_WIN32_FIND_DATAA *out) {
    char *full = NULL;
    size_t dl, nl;
    struct stat st;

    dl = strlen(dirpath);
    nl = strlen(name);
    if (nl >= 260) {
        w32_set_last_error(W32_ERROR_FILENAME_EXCED_RANGE);
        return -1;
    }
    full = (char *)malloc(dl + 1 + nl + 1);
    if (!full) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return -1;
    }
    memcpy(full, dirpath, dl);
    if (dl == 0 || full[dl - 1] != '/') {
        full[dl] = '/';
        memcpy(full + dl + 1, name, nl + 1);
    } else {
        memcpy(full + dl, name, nl + 1);
    }
    if (stat(full, &st) != 0) {
        free(full);
        return 1;
    }
    free(full);

    /* ACP is UTF-8 (see kernel32_loc.c), so the A struct carries the same
     * bytes the filesystem gave us — no conversion, no loss. */
    memset(out, 0, sizeof(*out));
    out->dwFileAttributes = fs_attrs_from_stat(&st, name);
    fs_times_from_stat(&st, &out->ftCreationTime, &out->ftLastAccessTime,
                       &out->ftLastWriteTime);
    out->nFileSizeHigh = (W32_DWORD)(st.st_size >> 32);
    out->nFileSizeLow = (W32_DWORD)(st.st_size & 0xFFFFFFFFu);
    memcpy(out->cFileName, name, nl + 1);
    return 0;
}

/* Advance the scan to the next matching entry and fill `outW`/`outA`
 * (exactly one is non-NULL).  Returns 0 on a match, 1 at end, -1 on error. */
static int fs_scan_next(struct fs_find *f, W32_WIN32_FIND_DATAW *outW,
                        W32_WIN32_FIND_DATAA *outA) {
    struct dirent *de;
    W32_WCHAR folded[264];
    size_t i;

    for (;;) {
        size_t need = 0;
        size_t need2 = 0;
        size_t nl;
        int rc;

        de = readdir(f->dir);
        if (!de)
            return 1;
        /* "." and ".." are enumerated, as on Windows. */
        nl = strlen(de->d_name);
        if (nl >= 260)
            continue;
        rc = w32_utf8_to_utf16(de->d_name, nl, NULL, 0, &need);
        if (rc != W32_UTF_OK || need > 260)
            continue;       /* unrepresentable bytes: skip, documented */
        rc = w32_utf8_to_utf16(de->d_name, nl, folded, need, &need2);
        if (rc != W32_UTF_OK || need2 != need)
            continue;
        folded[need] = 0;
        for (i = 0; i < need; i++)
            folded[i] = w32_fold_char(folded[i]);
        if (!fs_match(f->pattern, folded))
            continue;
        if (outW)
            rc = fs_fill_w(f->dirpath, de->d_name, outW);
        else
            rc = fs_fill_a(f->dirpath, de->d_name, outA);
        if (rc == 1)
            continue;       /* raced away: keep scanning */
        return rc;
    }
}

static struct fs_find *fs_find_open_scan(const char *pattern_utf8) {
    struct fs_find *f = NULL;
    char *dirpart = NULL;
    const char *pat;
    char *hostdir = NULL;
    size_t n, i;
    size_t need = 0;
    size_t need2 = 0;
    int rc;

    /* Split directory from pattern at the last slash of either kind. */
    n = strlen(pattern_utf8);
    pat = pattern_utf8;
    for (i = n; i > 0; i--) {
        if (fs_is_slash(pattern_utf8[i - 1])) {
            pat = pattern_utf8 + i;
            break;
        }
    }
    if (pat[0] == '\0') {
        /* A bare "C:\dir\": FindFirstFile matches nothing without a
         * pattern, and says so with FILE_NOT_FOUND. */
        w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
        return NULL;
    }
    if (pat == pattern_utf8) {
        dirpart = (char *)malloc(2);
        if (!dirpart) {
            w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        dirpart[0] = '.';
        dirpart[1] = '\0';
    } else {
        size_t dl = (size_t)(pat - pattern_utf8);
        while (dl > 0 && fs_is_slash(pattern_utf8[dl - 1]) &&
               !(dl == 3 && fs_drive_of(pattern_utf8) == 'C')) {
            dl--;           /* "C:\dir\" keeps its root; "dir\" does not */
            if (dl == 0)
                break;
        }
        if (dl == 0) {
            dirpart = (char *)malloc(2);
            if (!dirpart) {
                w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
                return NULL;
            }
            dirpart[0] = '.';
            dirpart[1] = '\0';
        } else {
            dirpart = (char *)malloc(dl + 1);
            if (!dirpart) {
                w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
                return NULL;
            }
            memcpy(dirpart, pattern_utf8, dl);
            dirpart[dl] = '\0';
        }
    }

    hostdir = fs_xlate(dirpart);
    free(dirpart);
    if (!hostdir)
        return NULL;

    f = (struct fs_find *)calloc(1, sizeof(*f));
    if (!f) {
        free(hostdir);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    f->tag = FS_FIND_SCAN;
    f->dir = opendir(hostdir);
    if (!f->dir) {
        W32_DWORD code = w32_error_from_c(-1);
        /* opendir on a missing path: FILE or PATH not found depending on
         * which half is missing.  The pattern half is a guess, so blame
         * the path — Windows leans that way too. */
        if (code == W32_ERROR_FILE_NOT_FOUND)
            code = W32_ERROR_PATH_NOT_FOUND;
        free(hostdir);
        free(f);
        w32_set_last_error(code);
        return NULL;
    }
    f->dirpath = hostdir;

    /* Fold the pattern once. */
    rc = w32_utf8_to_utf16(pat, strlen(pat), NULL, 0, &need);
    if (rc != W32_UTF_OK || need > 260) {
        fs_find_free(f);
        w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
        return NULL;
    }
    f->pattern = (W32_WCHAR *)malloc((need + 1) * sizeof(W32_WCHAR));
    if (!f->pattern) {
        fs_find_free(f);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    rc = w32_utf8_to_utf16(pat, strlen(pat), f->pattern, need, &need2);
    if (rc != W32_UTF_OK || need2 != need) {
        fs_find_free(f);
        w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
        return NULL;
    }
    f->pattern[need] = 0;
    for (i = 0; i < need; i++)
        f->pattern[i] = w32_fold_char(f->pattern[i]);
    return f;
}

W32ABI W32_HANDLE FindFirstFileW(W32_LPCWSTR name, W32_WIN32_FIND_DATAW *out) {
    char *utf8 = NULL;
    struct fs_find *f = NULL;
    W32_HANDLE h;
    int rc;

    if (!out)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    utf8 = fs_w16_dup(name);
    if (!utf8)
        return W32_INVALID_HANDLE_VALUE;
    f = fs_find_open_scan(utf8);
    free(utf8);
    if (!f)
        return W32_INVALID_HANDLE_VALUE;
    rc = fs_scan_next(f, out, NULL);
    if (rc != 0) {
        fs_find_free(f);
        if (rc == 1)
            w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
        return W32_INVALID_HANDLE_VALUE;
    }
    h = w32_handle_alloc_obj(W32_HANDLE_KIND_FIND, f, fs_find_free);
    if (!h) {
        fs_find_free(f);
        return fs_fail_h(W32_ERROR_TOO_MANY_OPEN_FILES);
    }
    return h;
}

W32ABI W32_HANDLE FindFirstFileA(W32_LPCSTR name, W32_WIN32_FIND_DATAA *out) {
    struct fs_find *f = NULL;
    W32_HANDLE h;
    int rc;

    if (!name || !out)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    /* ACP is UTF-8: the A path is already the bytes the fs wants. */
    f = fs_find_open_scan(name);
    if (!f)
        return W32_INVALID_HANDLE_VALUE;
    rc = fs_scan_next(f, NULL, out);
    if (rc != 0) {
        fs_find_free(f);
        if (rc == 1)
            w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
        return W32_INVALID_HANDLE_VALUE;
    }
    h = w32_handle_alloc_obj(W32_HANDLE_KIND_FIND, f, fs_find_free);
    if (!h) {
        fs_find_free(f);
        return fs_fail_h(W32_ERROR_TOO_MANY_OPEN_FILES);
    }
    return h;
}

W32ABI W32_BOOL FindNextFileW(W32_HANDLE h, W32_WIN32_FIND_DATAW *out) {
    struct fs_find *f;

    if (!out)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    f = (struct fs_find *)w32_handle_get_obj(h, W32_HANDLE_KIND_FIND);
    if (!f || f->tag != FS_FIND_SCAN)
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    if (fs_scan_next(f, out, NULL) != 0)
        return fs_fail(W32_ERROR_NO_MORE_FILES);
    return 1;
}

W32ABI W32_BOOL FindNextFileA(W32_HANDLE h, W32_WIN32_FIND_DATAA *out) {
    struct fs_find *f;

    if (!out)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    f = (struct fs_find *)w32_handle_get_obj(h, W32_HANDLE_KIND_FIND);
    if (!f || f->tag != FS_FIND_SCAN)
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    if (fs_scan_next(f, NULL, out) != 0)
        return fs_fail(W32_ERROR_NO_MORE_FILES);
    return 1;
}

W32ABI W32_BOOL FindClose(W32_HANDLE h) {
    struct fs_find *f = (struct fs_find *)w32_handle_release_obj(h,
        W32_HANDLE_KIND_FIND);
    if (!f)
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    if (f->tag != FS_FIND_SCAN && f->tag != FS_FIND_STREAM) {
        /* A change handle answers to FindCloseChangeNotification only. */
        fs_find_free(f);
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    }
    fs_find_free(f);
    return 1;
}

W32ABI W32_HANDLE FindFirstFileExW(W32_LPCWSTR name, W32_DWORD level,
                                  void *out, W32_DWORD searchOp,
                                  void *reserved, W32_DWORD flags) {
    /* BASIC and STANDARD differ only in the short name, which this volume
     * never mints, so both fill the same struct. */
    if (level > W32_FIND_EX_INFO_BASIC)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    if (searchOp > W32_FIND_EX_SEARCH_LIMIT_TO_DIRECTORIES)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    if (reserved != NULL)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    if (flags & ~(W32_FIND_FIRST_EX_CASE_SENSITIVE |
                  W32_FIND_FIRST_EX_LARGE_FETCH))
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    if (!out)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    if (searchOp == W32_FIND_EX_SEARCH_LIMIT_TO_DIRECTORIES) {
        /* No directory-only scan exists underneath; the filter would be a
         * lie by omission, so the op is refused rather than emulated. */
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    }
    if (flags & W32_FIND_FIRST_EX_CASE_SENSITIVE) {
        /* The matcher folds unconditionally; case-sensitive enumeration
         * cannot be honoured, so it is refused. */
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    }
    return FindFirstFileW(name, (W32_WIN32_FIND_DATAW *)out);
}

/* ---- streams: the one stream every file has ---------------------------------
 *
 * AuraLite keeps no alternate data streams, so the enumeration reports the
 * default stream and only it.  Directories are refused: streams attach to
 * files, and reporting "::$DATA" for a directory would invent data.
 */

static void fs_fill_stream(uint64_t size, W32_WIN32_FIND_STREAM_DATA *out) {
    static const char tail[] = "::$DATA";
    size_t i;

    memset(out, 0, sizeof(*out));
    out->StreamSize.QuadPart = (int64_t)size;
    for (i = 0; i < sizeof(tail) - 1; i++)
        out->cStreamName[i] = (W32_WCHAR)(unsigned char)tail[i];
    out->cStreamName[sizeof(tail) - 1] = 0;
}

W32ABI W32_HANDLE FindFirstStreamW(W32_LPCWSTR name, W32_DWORD infoLevel,
                                  W32_WIN32_FIND_STREAM_DATA *out,
                                  W32_DWORD flags) {
    char *utf8 = NULL;
    char *host = NULL;
    struct stat st;
    struct fs_find *f = NULL;
    W32_HANDLE h;

    if (!out)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    if (infoLevel != W32_FIND_STREAM_INFO_STANDARD || flags != 0)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    utf8 = fs_w16_dup(name);
    if (!utf8)
        return W32_INVALID_HANDLE_VALUE;
    host = fs_xlate(utf8);
    free(utf8);
    if (!host)
        return W32_INVALID_HANDLE_VALUE;
    if (stat(host, &st) != 0) {
        W32_DWORD code = w32_error_from_c(-1);
        free(host);
        return fs_fail_h(code);
    }
    free(host);
    if (!S_ISREG(st.st_mode))
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    f = (struct fs_find *)calloc(1, sizeof(*f));
    if (!f)
        return fs_fail_h(W32_ERROR_NOT_ENOUGH_MEMORY);
    f->tag = FS_FIND_STREAM;
    f->stream_size = st.st_size;
    f->stream_done = 1;         /* the single entry ships immediately */
    fs_fill_stream(f->stream_size, out);
    h = w32_handle_alloc_obj(W32_HANDLE_KIND_FIND, f, fs_find_free);
    if (!h) {
        fs_find_free(f);
        return fs_fail_h(W32_ERROR_TOO_MANY_OPEN_FILES);
    }
    return h;
}

W32ABI W32_BOOL FindNextStreamW(W32_HANDLE h, W32_WIN32_FIND_STREAM_DATA *out) {
    struct fs_find *f;

    if (!out)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    f = (struct fs_find *)w32_handle_get_obj(h, W32_HANDLE_KIND_FIND);
    if (!f || f->tag != FS_FIND_STREAM)
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    (void)f;
    /* One stream exists and it already shipped. */
    return fs_fail(W32_ERROR_NO_MORE_FILES);
}

/* ---- change notifications: the handle that never fires ---------------------- */

W32ABI W32_HANDLE FindFirstChangeNotificationW(W32_LPCWSTR path,
                                              W32_BOOL subtree,
                                              W32_DWORD filter) {
    char *utf8 = NULL;
    char *host = NULL;
    struct stat st;
    struct fs_find *f = NULL;
    W32_HANDLE h;

    (void)subtree;
    (void)filter;               /* stored nowhere: nothing is watched */
    utf8 = fs_w16_dup(path);
    if (!utf8)
        return W32_INVALID_HANDLE_VALUE;
    host = fs_xlate(utf8);
    free(utf8);
    if (!host)
        return W32_INVALID_HANDLE_VALUE;
    if (stat(host, &st) != 0) {
        W32_DWORD code = w32_error_from_c(-1);
        free(host);
        return fs_fail_h(code);
    }
    if (!S_ISDIR(st.st_mode)) {
        free(host);
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    }
    f = (struct fs_find *)calloc(1, sizeof(*f));
    if (!f) {
        free(host);
        return fs_fail_h(W32_ERROR_NOT_ENOUGH_MEMORY);
    }
    f->tag = FS_FIND_CHANGE;
    f->change_path = host;      /* kept so the handle means something */
    h = w32_handle_alloc_obj(W32_HANDLE_KIND_CHANGE, f, fs_find_free);
    if (!h) {
        fs_find_free(f);
        return fs_fail_h(W32_ERROR_TOO_MANY_OPEN_FILES);
    }
    return h;
}

W32ABI W32_BOOL FindNextChangeNotification(W32_HANDLE h) {
    struct fs_find *f = (struct fs_find *)w32_handle_get_obj(h,
        W32_HANDLE_KIND_CHANGE);
    if (!f || f->tag != FS_FIND_CHANGE)
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    /* Re-armed.  It will never fire — documented on the file, not here. */
    return 1;
}

W32ABI W32_BOOL FindCloseChangeNotification(W32_HANDLE h) {
    struct fs_find *f = (struct fs_find *)w32_handle_release_obj(h,
        W32_HANDLE_KIND_CHANGE);
    if (!f)
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    fs_find_free(f);
    return 1;
}

/* ---- attributes ----------------------------------------------------------- */

W32ABI W32_DWORD GetFileAttributesW(W32_LPCWSTR name) {
    char *utf8 = NULL;
    char *host = NULL;
    struct stat st;
    const char *base;
    W32_DWORD a;

    utf8 = fs_w16_dup(name);
    if (!utf8)
        return W32_INVALID_FILE_ATTRIBUTES;
    host = fs_xlate(utf8);
    free(utf8);
    if (!host)
        return W32_INVALID_FILE_ATTRIBUTES;
    /* stat, not lstat: links are transparent here (no reparse points), so
     * the caller learns about the target, which is what DIRECTORY tests
     * want to know. */
    if (stat(host, &st) != 0) {
        W32_DWORD code = w32_error_from_c(-1);
        free(host);
        w32_set_last_error(code);
        return W32_INVALID_FILE_ATTRIBUTES;
    }
    base = strrchr(host, '/');
    base = base ? base + 1 : host;
    a = fs_attrs_from_stat(&st, base);
    free(host);
    return a;
}

W32ABI W32_BOOL GetFileAttributesExW(W32_LPCWSTR name, W32_DWORD level,
                                    W32_WIN32_FILE_ATTRIBUTE_DATA *out) {
    char *utf8 = NULL;
    char *host = NULL;
    struct stat st;
    const char *base;

    if (level != 0)     /* GetFileExInfoStandard is the only level */
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    if (!out)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    utf8 = fs_w16_dup(name);
    if (!utf8)
        return 0;
    host = fs_xlate(utf8);
    free(utf8);
    if (!host)
        return 0;
    if (stat(host, &st) != 0)
        return fs_fail_c(-1);
    base = strrchr(host, '/');
    base = base ? base + 1 : host;
    out->dwFileAttributes = fs_attrs_from_stat(&st, base);
    fs_times_from_stat(&st, &out->ftCreationTime, &out->ftLastAccessTime,
                       &out->ftLastWriteTime);
    out->nFileSizeHigh = (W32_DWORD)((uint64_t)st.st_size >> 32);
    out->nFileSizeLow = (W32_DWORD)((uint64_t)st.st_size & 0xFFFFFFFFu);
    free(host);
    return 1;
}

W32ABI W32_BOOL SetFileAttributesW(W32_LPCWSTR name, W32_DWORD attrs) {
    char *utf8 = NULL;
    char *host = NULL;
    struct stat st;
    mode_t mode;
    int rc;

    /* READONLY is enforced via the mode bits; HIDDEN/SYSTEM/ARCHIVE have no
     * backing store and are accepted silently; TEMPORARY is accepted (every
     * file here is equally temporary); DIRECTORY is ignored, as on Windows
     * (attributes cannot turn a file into a directory). */
    if (attrs & ~(W32_FILE_ATTRIBUTE_READONLY | W32_FILE_ATTRIBUTE_HIDDEN |
                  W32_FILE_ATTRIBUTE_SYSTEM | W32_FILE_ATTRIBUTE_DIRECTORY |
                  W32_FILE_ATTRIBUTE_ARCHIVE | W32_FILE_ATTRIBUTE_NORMAL |
                  W32_FILE_ATTRIBUTE_TEMPORARY))
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    utf8 = fs_w16_dup(name);
    if (!utf8)
        return 0;
    host = fs_xlate(utf8);
    free(utf8);
    if (!host)
        return 0;
    if (stat(host, &st) != 0) {
        W32_DWORD code = w32_error_from_c(-1);
        free(host);
        w32_set_last_error(code);
        return 0;
    }
    mode = st.st_mode;
    if (attrs & W32_FILE_ATTRIBUTE_READONLY)
        mode &= (mode_t)~(S_IWUSR | S_IWGRP | S_IWOTH);
    else
        mode |= S_IWUSR;
    rc = chmod(host, mode);
    free(host);
    if (rc != 0)
        return fs_fail_c(-1);
    return 1;
}

/* ---- copy -------------------------------------------------------------------
 *
 * One core serves CopyFile, CopyFileEx and the cross-volume half of Move.
 * Same-file copies are refused (opening the destination O_TRUNC first would
 * eat the source); the source is checked against the share table the way an
 * opener asking READ with full share would be, so an exclusive open elsewhere
 * fails the copy with SHARING_VIOLATION instead of being bypassed.
 */

struct fs_copy_opts {
    W32_PROGRESS_CB progress;
    void *data;
    W32_BOOL *cancel;
    int copy_symlink;
};

/* Drive one progress callback.  Returns 0 to go on, 1 to abort (last error
 * already OPERATION_ABORTED).  QUIET detaches the callback via *pcb. */
static int fs_progress(W32_PROGRESS_CB *pcb, uint64_t total, uint64_t done,
                       W32_HANDLE srcH, W32_HANDLE dstH, void *data,
                       W32_DWORD reason, W32_BOOL *cancel) {
    W32_LARGE_INTEGER li_total;
    W32_LARGE_INTEGER li_done;
    W32_DWORD verdict;

    if (cancel && *cancel) {
        w32_set_last_error(W32_ERROR_OPERATION_ABORTED);
        return 1;
    }
    if (!pcb || !*pcb)
        return 0;
    li_total.QuadPart = (int64_t)total;
    li_done.QuadPart = (int64_t)done;
    verdict = (*pcb)(li_total, li_done, li_total, li_done, 1, reason,
                     srcH, dstH, data);
    if (verdict == 1 || verdict == 2) { /* CANCEL or STOP (no resume here) */
        w32_set_last_error(W32_ERROR_OPERATION_ABORTED);
        return 1;
    }
    if (verdict == 3)                   /* QUIET */
        *pcb = NULL;
    return 0;
}

static W32_BOOL fs_copy_bytes(int sfd, int dfd, uint64_t total,
                              W32_HANDLE srcH, W32_HANDLE dstH,
                              struct fs_copy_opts *o) {
    static char buf[65536];
    uint64_t done = 0;
    W32_PROGRESS_CB cb = o ? o->progress : NULL;

    /* STREAM_SWITCH first, then CHUNK_FINISHED per chunk. */
    if (fs_progress(&cb, total, 0, srcH, dstH, o ? o->data : NULL, 1,
                    o ? o->cancel : NULL))
        return 0;
    for (;;) {
        ssize_t n;
        size_t off = 0;

        if (o && o->cancel && *o->cancel) {
            w32_set_last_error(W32_ERROR_OPERATION_ABORTED);
            return 0;
        }
        do {
            n = read(sfd, buf, sizeof(buf));
        } while (n < 0 && errno == EINTR);
        if (n < 0) {
            int e = errno;
            w32_set_last_error(e == ENOSPC ? W32_ERROR_DISK_FULL :
                               W32_ERROR_READ_FAULT);
            return 0;
        }
        if (n == 0)
            break;
        while (off < (size_t)n) {
            ssize_t w;
            do {
                w = write(dfd, buf + off, (size_t)n - off);
            } while (w < 0 && errno == EINTR);
            if (w <= 0) {
                int e = (w == 0) ? EIO : errno;
                w32_set_last_error(e == ENOSPC ? W32_ERROR_DISK_FULL :
                                   W32_ERROR_WRITE_FAULT);
                return 0;
            }
            off += (size_t)w;
        }
        done += (uint64_t)n;
        /* The callback borrows real handles; the copy's offsets are saved
         * and restored around it so a curious callback cannot desync us. */
        if (cb) {
            int64_t spos = lseek(sfd, 0, SEEK_CUR);
            int64_t dpos = lseek(dfd, 0, SEEK_CUR);
            int stop = fs_progress(&cb, total, done, srcH, dstH,
                                   o ? o->data : NULL, 0,
                                   o ? o->cancel : NULL);
            if (spos >= 0)
                lseek(sfd, spos, SEEK_SET);
            if (dpos >= 0)
                lseek(dfd, dpos, SEEK_SET);
            if (stop)
                return 0;
        }
    }
    return 1;
}

static W32_BOOL fs_copy_file(const char *src, const char *dst, int failExists,
                             struct fs_copy_opts *o) {
    struct stat sst, dstt;
    int have_dst = 0;
    int sfd = -1;
    int dfd = -1;
    W32_HANDLE srcH = NULL;
    W32_HANDLE dstH = NULL;
    W32_BOOL ok = 0;

    if (lstat(src, &sst) != 0) {
        w32_set_last_error(w32_error_from_c(-1));
        return 0;
    }
    if (o && o->copy_symlink && S_ISLNK(sst.st_mode)) {
        /* COPY_SYMLINK: replicate the link instead of the target. */
        char target[4096];
        long n = (long)readlink(src, target, sizeof(target) - 1);
        if (n < 0) {
            w32_set_last_error(w32_error_from_c(-1));
            return 0;
        }
        target[n] = '\0';
        if (symlink(target, dst) != 0) {
            int e = errno;
            if (e == EEXIST && !failExists && unlink(dst) == 0 &&
                symlink(target, dst) == 0)
                return 1;
            w32_set_last_error((e == EEXIST && failExists) ?
                               W32_ERROR_FILE_EXISTS : w32_error_from_c(-1));
            return 0;
        }
        return 1;
    }
    if (stat(src, &sst) != 0) {
        w32_set_last_error(w32_error_from_c(-1));
        return 0;
    }
    if (!S_ISREG(sst.st_mode))
        return fs_fail(W32_ERROR_ACCESS_DENIED);
    if (fs_share_conflict(FS_ST_INO(sst), W32_GENERIC_READ,
                          W32_FILE_SHARE_READ | W32_FILE_SHARE_WRITE |
                          W32_FILE_SHARE_DELETE))
        return fs_fail(W32_ERROR_SHARING_VIOLATION);
    if (lstat(dst, &dstt) == 0) {
        have_dst = 1;
        if (FS_ST_INO(dstt) != 0 && FS_ST_INO(dstt) == FS_ST_INO(sst))
            return fs_fail(W32_ERROR_ACCESS_DENIED);    /* src == dst */
        if (failExists)
            return fs_fail(W32_ERROR_FILE_EXISTS);
    }
    sfd = open(src, O_RDONLY);
    if (sfd < 0) {
        w32_set_last_error(w32_error_from_c(-1));
        return 0;
    }
    dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (dfd < 0) {
        int e = errno;
        close(sfd);
        w32_set_last_error(w32_error_from_c(-1));
        (void)e;
        return 0;
    }
    /* Real handles for the progress callback, owning dup'd fds. */
    if (o && o->progress) {
        int sd = dup(sfd);
        int dd = dup(dfd);
        if (sd >= 0)
            srcH = w32_handle_alloc(sd, 1);
        if (srcH == NULL && sd >= 0)
            close(sd);
        if (dd >= 0)
            dstH = w32_handle_alloc(dd, 1);
        if (dstH == NULL && dd >= 0)
            close(dd);
    }
    ok = fs_copy_bytes(sfd, dfd, (uint64_t)sst.st_size, srcH, dstH, o);
    if (ok) {
        /* Metadata follows best-effort: the mode bits and stamps are copied
         * when the fs allows; a refusal does not fail the copy. */
        fchmod(dfd, sst.st_mode & 0777);
        {
            struct timespec ts[2];
            ts[0].tv_sec = (int64_t)sst.st_atime;
            ts[0].tv_nsec = 0;
            ts[1].tv_sec = (int64_t)sst.st_mtime;
            ts[1].tv_nsec = 0;
            futimens(dfd, ts);
        }
        if (close(dfd) != 0) {
            /* Write-back errors surface here, not at write(). */
            int e = errno;
            dfd = -1;
            ok = 0;
            w32_set_last_error(e == ENOSPC ? W32_ERROR_DISK_FULL :
                               W32_ERROR_WRITE_FAULT);
        } else {
            dfd = -1;
        }
    }
    if (!ok && w32_get_last_error_raw() == W32_ERROR_OPERATION_ABORTED) {
        /* A cancelled copy removes its partial target, as on Windows. */
        unlink(dst);
    }
    if (srcH) {
        int fd = w32_handle_release(srcH);
        if (fd >= 0)
            close(fd);
    }
    if (dstH) {
        int fd = w32_handle_release(dstH);
        if (fd >= 0)
            close(fd);
    }
    if (sfd >= 0)
        close(sfd);
    if (dfd >= 0)
        close(dfd);
    (void)have_dst;
    return ok;
}

W32ABI W32_BOOL CopyFileW(W32_LPCWSTR src, W32_LPCWSTR dst,
                          W32_BOOL failIfExists) {
    char *s8 = NULL;
    char *d8 = NULL;
    char *sh = NULL;
    char *dh = NULL;
    W32_BOOL ok;

    s8 = fs_w16_dup(src);
    if (!s8)
        return 0;
    d8 = fs_w16_dup(dst);
    if (!d8) {
        free(s8);
        return 0;
    }
    sh = fs_xlate(s8);
    free(s8);
    if (!sh) {
        free(d8);
        return 0;
    }
    dh = fs_xlate(d8);
    free(d8);
    if (!dh) {
        free(sh);
        return 0;
    }
    ok = fs_copy_file(sh, dh, failIfExists != 0, NULL);
    free(sh);
    free(dh);
    return ok;
}

W32ABI W32_BOOL CopyFileExW(W32_LPCWSTR src, W32_LPCWSTR dst,
                            W32_PROGRESS_CB progress, void *data,
                            W32_BOOL *cancel, W32_DWORD flags) {
    char *s8 = NULL;
    char *d8 = NULL;
    char *sh = NULL;
    char *dh = NULL;
    struct fs_copy_opts o;
    W32_BOOL ok;

    /* FAIL_IF_EXISTS, OPEN_SOURCE_FOR_WRITE (no-op: we always open the
     * source read-only... actually honoured: without it the source opens
     * O_RDONLY, with it O_RDWR — no. OPEN_SOURCE_FOR_WRITE exists for
     * files that deny read sharing; our share table is advisory between w32
     * handles, and O_RDONLY either way. Accepted and ignored.), */
    if (flags & ~(0x1u | 0x4u | 0x8u | 0x20u))
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    s8 = fs_w16_dup(src);
    if (!s8)
        return 0;
    d8 = fs_w16_dup(dst);
    if (!d8) {
        free(s8);
        return 0;
    }
    sh = fs_xlate(s8);
    free(s8);
    if (!sh) {
        free(d8);
        return 0;
    }
    dh = fs_xlate(d8);
    free(d8);
    if (!dh) {
        free(sh);
        return 0;
    }
    o.progress = progress;
    o.data = data;
    o.cancel = cancel;
    o.copy_symlink = (flags & 0x20u) != 0;
    ok = fs_copy_file(sh, dh, (flags & 0x1u) != 0, &o);
    free(sh);
    free(dh);
    return ok;
}

/* ---- move ------------------------------------------------------------------- */

static W32_BOOL fs_move_copy_fallback(const char *sh, const char *dh,
                                      W32_PROGRESS_CB progress, void *data,
                                      int writeThrough) {
    struct fs_copy_opts o;
    struct stat st;
    W32_BOOL ok;
    int dfd;

    o.progress = progress;
    o.data = data;
    o.cancel = NULL;
    o.copy_symlink = 0;
    /* The fallback copies with REPLACE semantics: the pre-checks already
     * ran, so the copy must not fail the file into place. */
    if (stat(sh, &st) != 0) {
        w32_set_last_error(w32_error_from_c(-1));
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        /* Cross-volume directory moves need a recursive copy the VFS walk
         * cannot promise; refused instead of half-moved. */
        return fs_fail(W32_ERROR_NOT_SAME_DEVICE);
    }
    ok = fs_copy_file(sh, dh, 0, progress ? &o : NULL);
    if (!ok)
        return 0;
    if (writeThrough) {
        dfd = open(dh, O_RDONLY);
        if (dfd >= 0) {
            fsync(dfd);
            close(dfd);
        }
    }
    if (unlink(sh) != 0) {
        /* Copied but the source survives: report it, keep the copy. */
        w32_set_last_error(w32_error_from_c(-1));
        return 0;
    }
    return 1;
}

static W32_BOOL fs_move(const char *sh, const char *dh, W32_DWORD flags,
                        W32_PROGRESS_CB progress, void *data) {
    struct stat sst;
    struct stat dstt;
    int dstExists;
    int replace = (flags & W32_MOVEFILE_REPLACE_EXISTING) != 0;
    int copyAllowed = (flags & W32_MOVEFILE_COPY_ALLOWED) != 0;
    int writeThrough = (flags & W32_MOVEFILE_WRITE_THROUGH) != 0;
    int hardlink = (flags & W32_MOVEFILE_CREATE_HARDLINK) != 0;

    if (flags & ~(W32_MOVEFILE_REPLACE_EXISTING | W32_MOVEFILE_COPY_ALLOWED |
                  W32_MOVEFILE_DELAY_UNTIL_REBOOT | W32_MOVEFILE_WRITE_THROUGH |
                  W32_MOVEFILE_CREATE_HARDLINK |
                  W32_MOVEFILE_FAIL_IF_NOT_TRACKABLE))
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    if (flags & W32_MOVEFILE_DELAY_UNTIL_REBOOT) {
        /* No boot-time mover exists; a deferred move would be a dropped
         * move, so the flag is refused rather than swallowed. */
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    }
    if (flags & W32_MOVEFILE_FAIL_IF_NOT_TRACKABLE) {
        /* Trackability is object IDs, which this volume does not keep. */
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    }
    if (strcmp(sh, dh) == 0)
        return 1;               /* moving a file onto itself: done */
    if (stat(sh, &sst) != 0) {
        w32_set_last_error(w32_error_from_c(-1));
        return 0;
    }
    dstExists = (lstat(dh, &dstt) == 0);
    if (dstExists && !replace)
        return fs_fail(W32_ERROR_ALREADY_EXISTS);
    if (hardlink) {
        if (S_ISDIR(sst.st_mode))
            return fs_fail(W32_ERROR_ACCESS_DENIED);
        if (dstExists && replace)
            unlink(dh);
        if (link(sh, dh) != 0) {
            int e = errno;
            if (e == EXDEV)
                return fs_fail(W32_ERROR_NOT_SAME_DEVICE);
            if (e == EEXIST)
                return fs_fail(W32_ERROR_ALREADY_EXISTS);
            w32_set_last_error(w32_error_from_c(-1));
            return 0;
        }
        /* The source stays in place: CREATE_HARDLINK links the
         * destination alongside it rather than moving. */
        return 1;
    }
    if (rename(sh, dh) == 0) {
        if (writeThrough) {
            int dfd = open(dh, O_RDONLY);
            if (dfd >= 0) {
                fsync(dfd);
                close(dfd);
            }
        }
        if (progress) {
            /* The rename was atomic; the callback still gets its
             * switch + finished pair so progress callers see one step. */
            W32_PROGRESS_CB cb = progress;
            uint64_t total = S_ISREG(sst.st_mode) ? (uint64_t)sst.st_size : 0;
            fs_progress(&cb, total, 0, NULL, NULL, data, 1, NULL);
            fs_progress(&cb, total, total, NULL, NULL, data, 0, NULL);
        }
        return 1;
    } else {
        int e = errno;
        if (e == EXDEV && copyAllowed)
            return fs_move_copy_fallback(sh, dh, progress, data, writeThrough);
        if (e == EXDEV)
            return fs_fail(W32_ERROR_NOT_SAME_DEVICE);
        if (e == EISDIR || e == ENOTDIR)
            return fs_fail(W32_ERROR_ACCESS_DENIED);
        w32_set_last_error(w32_error_from_c(-1));
        return 0;
    }
}

W32ABI W32_BOOL MoveFileW(W32_LPCWSTR src, W32_LPCWSTR dst) {
    char *s8 = NULL;
    char *d8 = NULL;
    char *sh = NULL;
    char *dh = NULL;
    W32_BOOL ok;

    s8 = fs_w16_dup(src);
    if (!s8)
        return 0;
    d8 = fs_w16_dup(dst);
    if (!d8) {
        free(s8);
        return 0;
    }
    sh = fs_xlate(s8);
    free(s8);
    if (!sh) {
        free(d8);
        return 0;
    }
    dh = fs_xlate(d8);
    free(d8);
    if (!dh) {
        free(sh);
        return 0;
    }
    /* Plain MoveFile allows the cross-volume copy, like on Windows. */
    ok = fs_move(sh, dh, W32_MOVEFILE_COPY_ALLOWED, NULL, NULL);
    free(sh);
    free(dh);
    return ok;
}

W32ABI W32_BOOL MoveFileExW(W32_LPCWSTR src, W32_LPCWSTR dst, W32_DWORD flags) {
    char *s8 = NULL;
    char *d8 = NULL;
    char *sh = NULL;
    char *dh = NULL;
    W32_BOOL ok;

    s8 = fs_w16_dup(src);
    if (!s8)
        return 0;
    d8 = fs_w16_dup(dst);
    if (!d8) {
        free(s8);
        return 0;
    }
    sh = fs_xlate(s8);
    free(s8);
    if (!sh) {
        free(d8);
        return 0;
    }
    dh = fs_xlate(d8);
    free(d8);
    if (!dh) {
        free(sh);
        return 0;
    }
    ok = fs_move(sh, dh, flags, NULL, NULL);
    free(sh);
    free(dh);
    return ok;
}

W32ABI W32_BOOL MoveFileWithProgressW(W32_LPCWSTR src, W32_LPCWSTR dst,
                                      W32_PROGRESS_CB progress, void *data,
                                      W32_DWORD flags) {
    char *s8 = NULL;
    char *d8 = NULL;
    char *sh = NULL;
    char *dh = NULL;
    W32_BOOL ok;

    s8 = fs_w16_dup(src);
    if (!s8)
        return 0;
    d8 = fs_w16_dup(dst);
    if (!d8) {
        free(s8);
        return 0;
    }
    sh = fs_xlate(s8);
    free(s8);
    if (!sh) {
        free(d8);
        return 0;
    }
    dh = fs_xlate(d8);
    free(d8);
    if (!dh) {
        free(sh);
        return 0;
    }
    ok = fs_move(sh, dh, flags, progress, data);
    free(sh);
    free(dh);
    return ok;
}

W32ABI W32_BOOL ReplaceFileW(W32_LPCWSTR replaced, W32_LPCWSTR replacement,
                              W32_LPCWSTR backup, W32_DWORD flags,
                              void *reserved1, void *reserved2) {
    char *r8 = NULL;
    char *p8 = NULL;
    char *b8 = NULL;
    char *rh = NULL;
    char *ph = NULL;
    char *bh = NULL;
    W32_BOOL ok = 0;

    /* WRITE_THROUGH is honoured (fsync); the two IGNORE flags are accepted
     * because there are no merge points or ACLs here to error on. */
    if (flags & ~(W32_REPLACEFILE_WRITE_THROUGH |
                  W32_REPLACEFILE_IGNORE_MERGE_ERRORS |
                  W32_REPLACEFILE_IGNORE_ACL_ERRORS))
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    if (reserved1 != NULL || reserved2 != NULL)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    r8 = fs_w16_dup(replaced);
    if (!r8)
        return 0;
    p8 = fs_w16_dup(replacement);
    if (!p8) {
        free(r8);
        return 0;
    }
    rh = fs_xlate(r8);
    free(r8);
    if (!rh) {
        free(p8);
        return 0;
    }
    ph = fs_xlate(p8);
    free(p8);
    if (!ph) {
        free(rh);
        return 0;
    }
    if (backup) {
        b8 = fs_w16_dup(backup);
        if (!b8) {
            free(rh);
            free(ph);
            return 0;
        }
        bh = fs_xlate(b8);
        free(b8);
        if (!bh) {
            free(rh);
            free(ph);
            return 0;
        }
        if (strcmp(bh, rh) == 0 || strcmp(bh, ph) == 0) {
            free(rh);
            free(ph);
            free(bh);
            return fs_fail(W32_ERROR_INVALID_PARAMETER);
        }
    }
    if (access(rh, F_OK) != 0 || access(ph, F_OK) != 0) {
        free(rh);
        free(ph);
        free(bh);
        return fs_fail(W32_ERROR_FILE_NOT_FOUND);
    }
    if (bh) {
        struct stat bst;
        if (lstat(bh, &bst) == 0) {
            /* An existing backup is not overwritten; the caller names a
             * fresh one.  Documented choice. */
            free(rh);
            free(ph);
            free(bh);
            return fs_fail(W32_ERROR_ALREADY_EXISTS);
        }
        if (rename(rh, bh) != 0) {
            W32_DWORD code = w32_error_from_c(-1);
            free(rh);
            free(ph);
            free(bh);
            w32_set_last_error(code);
            return 0;
        }
    }
    if (rename(ph, rh) != 0) {
        W32_DWORD code = w32_error_from_c(-1);
        /* The backup already holds the original; report and stop. */
        free(rh);
        free(ph);
        free(bh);
        w32_set_last_error(code);
        return 0;
    }
    if (flags & W32_REPLACEFILE_WRITE_THROUGH) {
        int dfd = open(rh, O_RDONLY);
        if (dfd >= 0) {
            fsync(dfd);
            close(dfd);
        }
    }
    ok = 1;
    free(rh);
    free(ph);
    free(bh);
    return ok;
}

/* ---- delete, directories, links --------------------------------------------- */

static W32_BOOL fs_delete_file(const char *host) {
    struct stat st;

    if (unlink(host) == 0)
        return 1;
    {
        int e = errno;
        if ((e == EISDIR || e == EPERM || e == EACCES) &&
            lstat(host, &st) == 0 && S_ISDIR(st.st_mode))
            return fs_fail(W32_ERROR_ACCESS_DENIED);
        w32_set_last_error(w32_error_from_c(-1));
        return 0;
    }
}

W32ABI W32_BOOL DeleteFileA(W32_LPCSTR name) {
    char *host = NULL;
    W32_BOOL ok;

    if (!name)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    host = fs_xlate(name);      /* ACP is UTF-8: direct */
    if (!host)
        return 0;
    ok = fs_delete_file(host);
    free(host);
    return ok;
}

W32ABI W32_BOOL DeleteFileW(W32_LPCWSTR name) {
    char *utf8 = NULL;
    char *host = NULL;
    W32_BOOL ok;

    utf8 = fs_w16_dup(name);
    if (!utf8)
        return 0;
    host = fs_xlate(utf8);
    free(utf8);
    if (!host)
        return 0;
    ok = fs_delete_file(host);
    free(host);
    return ok;
}

W32ABI W32_BOOL RemoveDirectoryW(W32_LPCWSTR name) {
    char *utf8 = NULL;
    char *host = NULL;

    utf8 = fs_w16_dup(name);
    if (!utf8)
        return 0;
    host = fs_xlate(utf8);
    free(utf8);
    if (!host)
        return 0;
    if (rmdir(host) != 0) {
        int e = errno;
        free(host);
        if (e == ENOTEMPTY)
            return fs_fail(W32_ERROR_DIR_NOT_EMPTY);
        w32_set_last_error(w32_error_from_c(-1));
        return 0;
    }
    free(host);
    return 1;
}

W32ABI W32_BOOL CreateDirectoryW(W32_LPCWSTR name, W32_SECURITY_ATTRIBUTES *sa) {
    char *utf8 = NULL;
    char *host = NULL;

    (void)sa;                   /* descriptors are not kept */
    utf8 = fs_w16_dup(name);
    if (!utf8)
        return 0;
    host = fs_xlate(utf8);
    free(utf8);
    if (!host)
        return 0;
    if (mkdir(host, 0777) != 0) {
        int e = errno;
        free(host);
        if (e == EEXIST)
            return fs_fail(W32_ERROR_ALREADY_EXISTS);
        w32_set_last_error(w32_error_from_c(-1));
        return 0;
    }
    free(host);
    return 1;
}

W32ABI W32_BOOL CreateHardLinkW(W32_LPCWSTR linkName, W32_LPCWSTR target,
                                 void *reserved) {
    char *l8 = NULL;
    char *t8 = NULL;
    char *lh = NULL;
    char *th = NULL;

    if (reserved != NULL)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    l8 = fs_w16_dup(linkName);
    if (!l8)
        return 0;
    t8 = fs_w16_dup(target);
    if (!t8) {
        free(l8);
        return 0;
    }
    lh = fs_xlate(l8);
    free(l8);
    if (!lh) {
        free(t8);
        return 0;
    }
    th = fs_xlate(t8);
    free(t8);
    if (!th) {
        free(lh);
        return 0;
    }
    if (link(th, lh) != 0) {
        int e = errno;
        free(lh);
        free(th);
        if (e == EEXIST)
            return fs_fail(W32_ERROR_ALREADY_EXISTS);
        if (e == EXDEV)
            return fs_fail(W32_ERROR_NOT_SAME_DEVICE);
        w32_set_last_error(w32_error_from_c(-1));
        return 0;
    }
    free(lh);
    free(th);
    return 1;
}

/* ---- sizes and free space -----------------------------------------------------
 *
 * GetCompressedFileSize reads the block count, so sparse files report their
 * real footprint.  GetDiskFreeSpace passes the VFS numbers through — and the
 * VFS answers are fixed constants (see q10_stubs.c), so the "free space" is
 * the VFS's answer, honestly relayed and documented here, not measured.
 */

W32ABI W32_DWORD GetCompressedFileSizeW(W32_LPCWSTR name, W32_DWORD *high) {
    char *utf8 = NULL;
    char *host = NULL;
    struct stat st;
    uint64_t bytes;

    utf8 = fs_w16_dup(name);
    if (!utf8)
        return W32_INVALID_FILE_SIZE;
    host = fs_xlate(utf8);
    free(utf8);
    if (!host)
        return W32_INVALID_FILE_SIZE;
    if (stat(host, &st) != 0) {
        W32_DWORD code = w32_error_from_c(-1);
        free(host);
        w32_set_last_error(code);
        return W32_INVALID_FILE_SIZE;
    }
    free(host);
    bytes = (uint64_t)st.st_blocks * 512u;
    if (high)
        *high = (W32_DWORD)(bytes >> 32);
    return (W32_DWORD)(bytes & 0xFFFFFFFFu);
}

static uint64_t fs_sat_mul(uint64_t a, uint64_t b) {
    if (a != 0 && b > 0xFFFFFFFFFFFFFFFFULL / a)
        return 0xFFFFFFFFFFFFFFFFULL;
    return a * b;
}

W32ABI W32_BOOL GetDiskFreeSpaceW(W32_LPCWSTR root, W32_DWORD *secPerClus,
                                  W32_DWORD *bytesPerSec, W32_DWORD *freeClus,
                                  W32_DWORD *totalClus) {
    char *utf8 = NULL;
    char *host = NULL;
    struct statvfs v;
    uint64_t bpc;
    uint64_t spc;
    uint64_t unitsz;
    W32_BOOL ok = 0;

    if (!secPerClus || !bytesPerSec || !freeClus || !totalClus)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    if (!root) {
        host = (char *)malloc(2);
        if (!host)
            return fs_fail(W32_ERROR_NOT_ENOUGH_MEMORY);
        host[0] = '/';
        host[1] = '\0';
    } else {
        utf8 = fs_w16_dup(root);
        if (!utf8)
            return 0;
        host = fs_xlate(utf8);
        free(utf8);
        if (!host)
            return 0;
    }
    if (statvfs(host, &v) != 0) {
        W32_DWORD code = w32_error_from_c(-1);
        free(host);
        w32_set_last_error(code);
        return 0;
    }
    free(host);
    /* One VFS block is one cluster; 512-byte sectors.  Zero fragment size
     * (a degenerate VFS answer) falls back to 4 KiB rather than dividing
     * by zero. */
    bpc = v.f_frsize ? (uint64_t)v.f_frsize : 4096u;
    spc = bpc / 512u;
    if (spc == 0)
        spc = 1;
    unitsz = spc * 512u;
    *secPerClus = (W32_DWORD)spc;
    *bytesPerSec = 512u;
    *freeClus = (W32_DWORD)(fs_sat_mul((uint64_t)v.f_bavail, bpc) / unitsz);
    *totalClus = (W32_DWORD)(fs_sat_mul((uint64_t)v.f_blocks, bpc) / unitsz);
    ok = 1;
    return ok;
}

W32ABI W32_BOOL GetDiskFreeSpaceExW(W32_LPCWSTR root,
                                    W32_ULARGE_INTEGER *freeAvail,
                                    W32_ULARGE_INTEGER *total,
                                    W32_ULARGE_INTEGER *totalFree) {
    char *utf8 = NULL;
    char *host = NULL;
    struct statvfs v;
    uint64_t bpc;

    if (!root) {
        host = (char *)malloc(2);
        if (!host)
            return fs_fail(W32_ERROR_NOT_ENOUGH_MEMORY);
        host[0] = '/';
        host[1] = '\0';
    } else {
        utf8 = fs_w16_dup(root);
        if (!utf8)
            return 0;
        host = fs_xlate(utf8);
        free(utf8);
        if (!host)
            return 0;
    }
    if (statvfs(host, &v) != 0) {
        W32_DWORD code = w32_error_from_c(-1);
        free(host);
        w32_set_last_error(code);
        return 0;
    }
    free(host);
    bpc = v.f_frsize ? (uint64_t)v.f_frsize : 4096u;
    if (freeAvail)
        freeAvail->QuadPart = fs_sat_mul((uint64_t)v.f_bavail, bpc);
    if (total)
        total->QuadPart = fs_sat_mul((uint64_t)v.f_blocks, bpc);
    if (totalFree)
        totalFree->QuadPart = fs_sat_mul((uint64_t)v.f_bfree, bpc);
    return 1;
}

/* ---- drives and volumes ------------------------------------------------------- */

W32ABI W32_UINT GetDriveTypeW(W32_LPCWSTR root) {
    char *utf8 = NULL;
    int drv;
    W32_UINT type;

    if (!root)
        return W32_DRIVE_FIXED;         /* the current drive is C: */
    utf8 = fs_w16_dup(root);
    if (!utf8)
        return W32_DRIVE_UNKNOWN;
    if (utf8[0] == '\0') {
        free(utf8);
        return W32_DRIVE_UNKNOWN;
    }
    drv = fs_drive_of(utf8);
    free(utf8);
    if (drv == -2)
        return W32_DRIVE_NO_ROOT_DIR;   /* UNC: no provider, no root */
    if (drv == -1)
        return W32_DRIVE_FIXED;         /* relative: the current drive */
    type = (drv == 'C') ? W32_DRIVE_FIXED : W32_DRIVE_NO_ROOT_DIR;
    return type;
}

W32ABI W32_DWORD GetLogicalDriveStringsW(W32_DWORD cch, W32_LPWSTR buf) {
    /* One drive: "C:\" + NUL + final NUL is 4 units; the return counts the
     * per-string NUL but not the final one... which for a single drive is
     * still 4 units written and 4 returned: "C:\0" is the string (4 with
     * its NUL) and the extra NUL terminates the list. */
    static const W32_WCHAR drives[4] = { 'C', ':', '\\', 0 };

    if (cch == 0 || !buf)
        return 4;
    if (cch < 4)
        return 4;
    buf[0] = drives[0];
    buf[1] = drives[1];
    buf[2] = drives[2];
    buf[3] = 0;
    return 4;
}

W32ABI W32_BOOL GetVolumeInformationW(W32_LPCWSTR root, W32_LPWSTR volName,
                                      W32_DWORD volNameSize, W32_DWORD *serial,
                                      W32_DWORD *maxCompLen, W32_DWORD *fsFlags,
                                      W32_LPWSTR fsName, W32_DWORD fsNameSize) {
    char *utf8 = NULL;
    int drv;
    static const char label[] = "AURALITE";
    static const char fstype[] = "AURALFS";
    size_t i;

    if (root) {
        utf8 = fs_w16_dup(root);
        if (!utf8)
            return 0;
        drv = fs_drive_of(utf8);
        free(utf8);
        if (drv == -2)
            return fs_fail(W32_ERROR_BAD_NETPATH);
        if (drv != -1 && drv != 'C')
            return fs_fail(W32_ERROR_PATH_NOT_FOUND);
    }
    if (volName && volNameSize > 0) {
        size_t n = sizeof(label) - 1;
        if (n >= volNameSize)
            return fs_fail(W32_ERROR_INSUFFICIENT_BUFFER);
        for (i =  0; i <= n; i++)
            volName[i] = (W32_WCHAR)(unsigned char)label[i];
    }
    if (fsName && fsNameSize > 0) {
        size_t n = sizeof(fstype) - 1;
        if (n >= fsNameSize)
            return fs_fail(W32_ERROR_INSUFFICIENT_BUFFER);
        for (i = 0; i <= n; i++)
            fsName[i] = (W32_WCHAR)(unsigned char)fstype[i];
    }
    if (serial)
        *serial = FS_VOL_SERIAL;
    if (maxCompLen)
        *maxCompLen = 255u;
    if (fsFlags)
        *fsFlags = 0x00000002u;         /* FILE_CASE_PRESERVED_NAMES */
    return 1;
}

/* ---- names of open files and full paths -----------------------------------------
 *
 * GetFinalPathNameByHandle reads /proc/self/fd, which resolves the fd to the
 * real path — including through renames, which is exactly the documented
 * semantic.  Anything readlink returns that is not a path (pipes, sockets)
 * is refused: inventing a DOS path for a pipe would be fiction.
 */

W32ABI W32_DWORD GetFinalPathNameByHandleW(W32_HANDLE h, W32_LPWSTR buf,
                                           W32_DWORD cch, W32_DWORD flags) {
    int fd;
    char proc[64];
    char target[4096];
    long n;
    char *dos = NULL;
    size_t i, len;
    W32_DWORD need;

    if (flags & 0x3u)   /* VOLUME_NAME_GUID/NT: no GUIDs, no NT namespace */
        return 0;
    if ((flags & ~0xFu) != 0)
        return 0;
    fd = w32_handle_to_fd(h);
    if (fd < 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return 0;
    }
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    n = (long)readlink(proc, target, sizeof(target) - 1);
    if (n < 0) {
        w32_set_last_error(w32_error_from_c(-1));
        return 0;
    }
    target[n] = '\0';
    if (target[0] != '/') {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    /* "/x/y" -> "\\?\C:\x\y". */
    len = 7 + (size_t)n;        /* "\\?\" + "C:" + rest, NUL counted later */
    dos = (char *)malloc(len + 1);
    if (!dos) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    dos[0] = '\\';
    dos[1] = '\\';
    dos[2] = '?';
    dos[3] = '\\';
    dos[4] = 'C';
    dos[5] = ':';
    for (i = 0; i < (size_t)n; i++)
        dos[6 + i] = (target[i] == '/') ? '\\' : target[i];
    dos[6 + (size_t)n] = '\0';
    need = fs_copy_out_w16(dos, buf, cch);
    free(dos);
    if (need == 0)
        return 0;
    if (cch < need)             /* short (or measuring): needed, excl. NUL */
        return need - 1;
    return need - 1;
}

W32ABI W32_DWORD GetFullPathNameW(W32_LPCWSTR name, W32_DWORD cch,
                                 W32_LPWSTR buf, W32_LPWSTR *filePart) {
    char *utf8 = NULL;
    char *host = NULL;
    char *abs = NULL;
    char *out8 = NULL;
    size_t cap;
    size_t i, w;
    W32_DWORD need;
    W32_DWORD ret = 0;

    /* Component stack for dot-normalisation. */
    size_t *starts = NULL;
    size_t ncomp = 0;
    size_t compcap = 0;

    if (filePart)
        *filePart = NULL;
    utf8 = fs_w16_dup(name);
    if (!utf8)
        return 0;
    if (utf8[0] == '\0') {
        free(utf8);
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    {
        int drv = fs_drive_of(utf8);
        if (drv == -2) {
            free(utf8);
            w32_set_last_error(W32_ERROR_BAD_NETPATH);
            return 0;
        }
        if (drv != -1 && drv != 'C') {
            free(utf8);
            w32_set_last_error(W32_ERROR_INVALID_NAME);
            return 0;
        }
    }
    host = fs_xlate(utf8);
    free(utf8);
    if (!host)
        return 0;
    if (host[0] == '/') {
        abs = host;
        host = NULL;
    } else {
        char cwd[4096];
        size_t cl, hl;
        if (!getcwd(cwd, sizeof(cwd))) {
            W32_DWORD code = w32_error_from_c(-1);
            free(host);
            w32_set_last_error(code);
            return 0;
        }
        cl = strlen(cwd);
        hl = strlen(host);
        abs = (char *)malloc(cl + 1 + hl + 1);
        if (!abs) {
            free(host);
            w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        memcpy(abs, cwd, cl);
        abs[cl] = '/';
        memcpy(abs + cl + 1, host, hl + 1);
        free(host);
    }
    /* Normalise: split into components, drop ".", pop on "..". */
    cap = strlen(abs) + 1;
    out8 = (char *)malloc(cap + 3);     /* room for "C:" */
    starts = (size_t *)malloc((cap + 1) * sizeof(size_t));
    if (!out8 || !starts) {
        free(abs);
        free(out8);
        free(starts);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    out8[0] = 'C';
    out8[1] = ':';
    w = 2;
    compcap = cap + 1;
    i = 0;
    while (abs[i] != '\0') {
        size_t s, e;
        while (abs[i] == '/')
            i++;
        if (abs[i] == '\0')
            break;
        s = i;
        while (abs[i] != '\0' && abs[i] != '/')
            i++;
        e = i;
        if (e - s == 1 && abs[s] == '.') {
            continue;
        } else if (e - s == 2 && abs[s] == '.' && abs[s + 1] == '.') {
            if (ncomp > 0) {
                ncomp--;
                w = starts[ncomp];
            }
            continue;
        } else {
            if (ncomp < compcap) {
                starts[ncomp++] = w;
                out8[w++] = '\\';
                memcpy(out8 + w, abs + s, e - s);
                w += e - s;
            }
        }
    }
    free(abs);
    free(starts);
    if (w == 2) {
        /* Root: "C:\". */
        out8[w++] = '\\';
    }
    out8[w] = '\0';
    need = fs_copy_out_w16(out8, buf, cch);
    free(out8);
    if (need == 0)
        return 0;
    if (cch < need)
        return need - 1;
    ret = need - 1;
    if (filePart) {
        /* After the last backslash, unless the path is root/ends in one. */
        W32_DWORD k = ret;
        *filePart = NULL;
        while (k > 0) {
            k--;
            if (buf[k] == (W32_WCHAR)'\\') {
                if (k + 1 < ret)
                    *filePart = buf + k + 1;
                break;
            }
        }
    }
    return ret;
}

W32ABI W32_DWORD GetLongPathNameW(W32_LPCWSTR shortPath, W32_LPWSTR out,
                                 W32_DWORD cch) {
    char *utf8 = NULL;
    char *host = NULL;
    struct stat st;
    W32_DWORD need;

    /* This volume mints no short names, so every existing path is already
     * its own long form; the call still validates existence. */
    utf8 = fs_w16_dup(shortPath);
    if (!utf8)
        return 0;
    host = fs_xlate(utf8);
    if (!host) {
        free(utf8);
        return 0;
    }
    if (stat(host, &st) != 0) {
        W32_DWORD code = w32_error_from_c(-1);
        free(utf8);
        free(host);
        w32_set_last_error(code);
        return 0;
    }
    free(host);
    /* Echo the input spelling (slashes as given): it is already long. */
    need = fs_copy_out_w16(utf8, out, cch);
    free(utf8);
    if (need == 0)
        return 0;
    return need - 1;
}

/* ---- file times ------------------------------------------------------------------- */

W32ABI W32_BOOL SetFileTime(W32_HANDLE h, const W32_FILETIME *creation,
                             const W32_FILETIME *access,
                             const W32_FILETIME *write) {
    int fd;
    struct timespec ts[2];
    uint64_t u;

    (void)creation;             /* birth time is not kept; accepted */
    fd = w32_handle_to_fd(h);
    if (fd < 0)
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    if (!access && !write)
        return 1;
    ts[0].tv_nsec = UTIME_OMIT;
    ts[0].tv_sec = 0;
    ts[1].tv_nsec = UTIME_OMIT;
    ts[1].tv_sec = 0;
    if (access) {
        u = fs_ft_join(access);
        ts[0].tv_sec = (int64_t)(u / FS_TICKS_PER_SEC) - FS_EPOCH_DIFF;
        ts[0].tv_nsec = (long)((u % FS_TICKS_PER_SEC) * 100u);
    }
    if (write) {
        u = fs_ft_join(write);
        ts[1].tv_sec = (int64_t)(u / FS_TICKS_PER_SEC) - FS_EPOCH_DIFF;
        ts[1].tv_nsec = (long)((u % FS_TICKS_PER_SEC) * 100u);
    }
    if (futimens(fd, ts) != 0)
        return fs_fail_c(-1);
    return 1;
}

W32ABI W32_BOOL FileTimeToLocalFileTime(const W32_FILETIME *in,
                                       W32_FILETIME *out) {
    if (!in || !out)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    /* UTC throughout: local time IS file time.  GetTimeZoneInformation is
     * the witness a caller can check. */
    *out = *in;
    return 1;
}

W32ABI W32_BOOL FileTimeToSystemTime(const W32_FILETIME *in,
                                    W32_SYSTEMTIME *out) {
    W32_SYSTEMTIME st;
    if (!in || !out)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    fs_ft_to_systemtime(fs_ft_join(in), &st);
    if (st.wYear < 1601u || st.wYear > 30827u)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    *out = st;
    return 1;
}

W32ABI W32_BOOL FileTimeToDosDateTime(const W32_FILETIME *in, W32_WORD *date,
                                      W32_WORD *time) {
    W32_SYSTEMTIME st;
    if (!in || !date || !time)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    fs_ft_to_systemtime(fs_ft_join(in), &st);
    if (st.wYear < 1980u || st.wYear > 2107u) {
        /* Outside the DOS range the fields go to zero — this is what the
         * API does rather than fail. */
        *date = 0;
        *time = 0;
        return 1;
    }
    *date = (W32_WORD)(((st.wYear - 1980u) << 9) | (st.wMonth << 5) | st.wDay);
    *time = (W32_WORD)((st.wHour << 11) | (st.wMinute << 5) |
                       (st.wSecond / 2u));
    return 1;
}

W32ABI W32_BOOL DosDateTimeToFileTime(W32_WORD date, W32_WORD time,
                                      W32_FILETIME *out) {
    W32_SYSTEMTIME st;
    if (!out)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    st.wYear = (W32_WORD)(((date >> 9) & 0x7Fu) + 1980u);
    st.wMonth = (W32_WORD)((date >> 5) & 0xFu);
    st.wDay = (W32_WORD)(date & 0x1Fu);
    st.wDayOfWeek = 0;
    st.wHour = (W32_WORD)((time >> 11) & 0x1Fu);
    st.wMinute = (W32_WORD)((time >> 5) & 0x3Fu);
    st.wSecond = (W32_WORD)(((time & 0x1Fu) * 2u));
    st.wMilliseconds = 0;
    if (!fs_valid_systemtime(&st))
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    fs_ft_split(fs_systemtime_to_ft(&st), out);
    return 1;
}

W32ABI W32_BOOL LocalFileTimeToFileTime(const W32_FILETIME *in,
                                       W32_FILETIME *out) {
    if (!in || !out)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    *out = *in;                 /* UTC: the inverse identity */
    return 1;
}

W32ABI W32_LONG CompareFileTime(const W32_FILETIME *a, const W32_FILETIME *b) {
    uint64_t ua;
    uint64_t ub;
    if (!a || !b) {
        /* No failure channel exists (the return is a comparison); a NULL
         * sorts below everything, and the error slot still says why. */
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        if (!a && !b)
            return 0;
        return (!a) ? -1 : 1;
    }
    ua = fs_ft_join(a);
    ub = fs_ft_join(b);
    if (ua < ub)
        return -1;
    if (ua > ub)
        return 1;
    return 0;
}

W32ABI W32_BOOL SystemTimeToTzSpecificLocalTime(
    const W32_TIME_ZONE_INFORMATION *tz, const W32_SYSTEMTIME *in,
    W32_SYSTEMTIME *out) {
    if (!in || !out)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    if (!fs_valid_systemtime(in))
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    if (tz) {
        /* Only a UTC-equivalent zone converts here; a real zone with bias
         * or DST rules has no converter to call, so it is refused. */
        if (tz->Bias != 0 || tz->StandardBias != 0 || tz->DaylightBias != 0)
            return fs_fail(W32_ERROR_INVALID_PARAMETER);
    }
    *out = *in;
    out->wDayOfWeek = 0;        /* recomputed below */
    {
        uint64_t ft = fs_systemtime_to_ft(in);
        fs_ft_to_systemtime(ft, out);
    }
    return 1;
}

W32ABI void GetSystemTimeAsFileTime(W32_FILETIME *out) {
    struct timespec ts;
    if (!out)
        return;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        /* The clock cannot fail here; a zero time still beats garbage. */
        out->dwLowDateTime = 0;
        out->dwHighDateTime = 0;
        return;
    }
    fs_ft_split(fs_unix_to_ft((int64_t)ts.tv_sec, ts.tv_nsec), out);
}

W32ABI void GetLocalTime(W32_SYSTEMTIME *out) {
    W32_FILETIME ft;
    if (!out)
        return;
    GetSystemTimeAsFileTime(&ft);       /* UTC: local is system */
    fs_ft_to_systemtime(fs_ft_join(&ft), out);
}

W32ABI W32_DWORD GetTimeZoneInformation(W32_TIME_ZONE_INFORMATION *out) {
    if (!out) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0xFFFFFFFFu;
    }
    memset(out, 0, sizeof(*out));
    out->StandardName[0] = (W32_WCHAR)'U';
    out->StandardName[1] = (W32_WCHAR)'T';
    out->StandardName[2] = (W32_WCHAR)'C';
    return 0;                   /* TIME_ZONE_ID_UNKNOWN: no DST here */
}

W32ABI W32_BOOL QueryPerformanceCounter(W32_LARGE_INTEGER *out) {
    struct timespec ts;
    if (!out)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return fs_fail_c(-1);
    /* 10 MHz ticks.  The ns/100 truncation can disagree with a second call
     * by one tick; that jitter is documented, not hidden. */
    out->QuadPart = (int64_t)ts.tv_sec * 10000000LL +
        (int64_t)(ts.tv_nsec / 100);
    return 1;
}

W32ABI W32_BOOL QueryPerformanceFrequency(W32_LARGE_INTEGER *out) {
    if (!out)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    out->QuadPart = 10000000LL;
    return 1;
}

/* ---- CreateFile -------------------------------------------------------------------
 *
 * The core takes a UTF-8 path (the A form passes its bytes through, the W
 * form converts first) and honours: all five dispositions (with the
 * ALREADY_EXISTS success code where Windows sets it), BACKUP_SEMANTICS for
 * directory opens, WRITE_THROUGH via O_SYNC, DELETE_ON_CLOSE via
 * unlink-at-open (the file leaves the namespace at once and its storage
 * follows the last handle — exactly the documented semantic), the share
 * check against live rows, and the FILE-vs-PATH distinction for missing
 * names.  OPEN_REPARSE_POINT is refused (there are no reparse points to
 * open); unknown access and flag bits are ignored the way Windows ignores
 * future bits.  Template handles are refused: copying attributes off one
 * would need attribute storage that does not exist.
 */

static int fs_is_pipe_prefix(const char *p) {
    static const char *pfx1 = "\\\\.\\pipe\\";
    static const char *pfx2 = "//./pipe/";
    size_t i;
    for (i = 0; i < 9; i++) {
        char a = p[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != pfx1[i] && a != pfx2[i])
            return 0;
        if (a == '\0')
            return 0;
    }
    return 1;
}

static W32_HANDLE fs_pipe_client_open(const char *path, W32_DWORD access);

W32_HANDLE w32_fs_create(const char *path, W32_DWORD access, W32_DWORD share,
                         W32_DWORD disposition, W32_DWORD flags,
                         W32_HANDLE tmpl) {
    char *host = NULL;
    int oflags;
    int backup;
    int existed = 0;
    int fd = -1;
    struct stat st;
    W32_HANDLE h;

    if (!path)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    if (share & ~(W32_FILE_SHARE_READ | W32_FILE_SHARE_WRITE |
                  W32_FILE_SHARE_DELETE))
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    if (disposition < 1u || disposition > 5u)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    if (tmpl != NULL)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    if (flags & W32_FILE_FLAG_OPEN_REPARSE_POINT)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    if (fs_is_pipe_prefix(path)) {
        if (disposition != 3u)   /* pipes open existing, never create */
            return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
        return fs_pipe_client_open(path, access);
    }

    host = fs_xlate(path);
    if (!host)
        return W32_INVALID_HANDLE_VALUE;

    oflags = 0;
#ifdef O_CLOEXEC
    oflags |= O_CLOEXEC;
#endif
    if ((access & (W32_GENERIC_READ | W32_GENERIC_ALL)) != 0 &&
        (access & (W32_GENERIC_WRITE | W32_GENERIC_ALL)) != 0)
        oflags |= O_RDWR;
    else if ((access & (W32_GENERIC_WRITE | W32_GENERIC_ALL)) != 0)
        oflags |= O_WRONLY;
    else
        oflags |= O_RDONLY;     /* READ, EXECUTE-only, or metadata (0) */
    if (flags & W32_FILE_FLAG_WRITE_THROUGH) {
#ifdef O_SYNC
        oflags |= O_SYNC;
#else
        /* No O_SYNC on this libc: the flag degrades to a best-effort fsync
         * at close... which CloseHandle does not do.  Refusing would break
         * callers that pass it routinely, ignoring it risks durability the
         * caller asked for.  The honest middle: accept, and document that
         * write-through is NOT honoured on this build. */
#endif
    }
    backup = (flags & W32_FILE_FLAG_BACKUP_SEMANTICS) != 0;
    switch (disposition) {
    case 1:                     /* CREATE_NEW */
        oflags |= O_CREAT | O_EXCL;
        break;
    case 2:                     /* CREATE_ALWAYS */
        oflags |= O_CREAT | O_TRUNC;
        break;
    case 4:                     /* OPEN_ALWAYS */
        oflags |= O_CREAT;
        break;
    case 5:                     /* TRUNCATE_EXISTING */
        oflags |= O_TRUNC;
        break;
    default:                    /* OPEN_EXISTING */
        break;
    }
    if (disposition == 2u || disposition == 4u) {
        struct stat probe;
        existed = (lstat(host, &probe) == 0);
    }

    fd = open(host, oflags, 0666);
    if (fd < 0) {
        int e = errno;
        W32_DWORD code;
        if (e == ENOENT) {
            /* Which half is missing?  The parent decides. */
            char *slash = strrchr(host, '/');
            if (slash && slash != host) {
                struct stat parent;
                *slash = '\0';
                code = (lstat(host, &parent) == 0) ?
                    W32_ERROR_FILE_NOT_FOUND : W32_ERROR_PATH_NOT_FOUND;
            } else {
                code = W32_ERROR_FILE_NOT_FOUND;
            }
        } else if (e == EEXIST) {
            code = W32_ERROR_FILE_EXISTS;
        } else if (e == EISDIR || e == ENOTDIR) {
            code = (e == ENOTDIR) ? W32_ERROR_PATH_NOT_FOUND :
                W32_ERROR_ACCESS_DENIED;
        } else {
            code = w32_error_from_c(-1);
        }
        free(host);
        return fs_fail_h(code);
    }
    if (fstat(fd, &st) != 0) {
        W32_DWORD code = w32_error_from_c(-1);
        close(fd);
        free(host);
        return fs_fail_h(code);
    }
    if (S_ISDIR(st.st_mode)) {
        if (!backup || (disposition != 3u && disposition != 4u)) {
            close(fd);
            free(host);
            return fs_fail_h(W32_ERROR_ACCESS_DENIED);
        }
    }
    if (flags & W32_FILE_FLAG_DELETE_ON_CLOSE) {
        if (unlink(host) != 0) {
            W32_DWORD code = w32_error_from_c(-1);
            close(fd);
            free(host);
            return fs_fail_h(code);
        }
    }
    free(host);
    if (fs_share_conflict(FS_ST_INO(st), access, share)) {
        close(fd);
        return fs_fail_h(W32_ERROR_SHARING_VIOLATION);
    }
    h = w32_handle_alloc(fd, 1);
    if (!h) {
        close(fd);
        return fs_fail_h(W32_ERROR_TOO_MANY_OPEN_FILES);
    }
    fs_share_add(FS_ST_INO(st), access, share, fd);
    if ((disposition == 2u || disposition == 4u) && existed)
        w32_set_last_error(W32_ERROR_ALREADY_EXISTS);
    else
        w32_set_last_error(W32_ERROR_SUCCESS);
    return h;
}

W32ABI W32_HANDLE CreateFileW(W32_LPCWSTR path, W32_DWORD access,
                              W32_DWORD share, W32_SECURITY_ATTRIBUTES *sa,
                              W32_DWORD disposition, W32_DWORD flags,
                              W32_HANDLE tmpl) {
    char *utf8 = NULL;
    W32_HANDLE h;

    (void)sa;                   /* descriptors are not kept */
    utf8 = fs_w16_dup(path);
    if (!utf8)
        return W32_INVALID_HANDLE_VALUE;
    h = w32_fs_create(utf8, access, share, disposition, flags, tmpl);
    free(utf8);
    return h;
}

/* ---- file pointers, ends, sizes, types ------------------------------------------ */

W32ABI W32_DWORD SetFilePointer(W32_HANDLE h, W32_LONG distLow,
                                W32_LONG *distHigh, W32_DWORD method) {
    int fd;
    int64_t off;
    int64_t pos;

    if (method > 2u)
        return W32_INVALID_SET_FILE_POINTER;
    fd = w32_handle_to_fd(h);
    if (fd < 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_INVALID_SET_FILE_POINTER;
    }
    off = (int64_t)distLow;
    if (distHigh)
        off |= (int64_t)*distHigh << 32;
    pos = lseek(fd, (off_t)off,
                method == 0u ? SEEK_SET : (method == 1u ? SEEK_CUR : SEEK_END));
    if (pos < 0) {
        int e = errno;
        w32_set_last_error(e == ESPIPE ? W32_ERROR_INVALID_PARAMETER :
                           w32_error_from_c(-1));
        return W32_INVALID_SET_FILE_POINTER;
    }
    w32_set_last_error(W32_ERROR_SUCCESS);
    if (distHigh)
        *distHigh = (W32_LONG)((uint64_t)pos >> 32);
    return (W32_DWORD)((uint64_t)pos & 0xFFFFFFFFu);
}

W32ABI W32_BOOL SetFilePointerEx(W32_HANDLE h, W32_LARGE_INTEGER dist,
                                  W32_LARGE_INTEGER *pos, W32_DWORD method) {
    int fd;
    int64_t at;

    if (method > 2u)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    fd = w32_handle_to_fd(h);
    if (fd < 0)
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    at = (int64_t)lseek(fd, (off_t)dist.QuadPart,
                        method == 0u ? SEEK_SET :
                        (method == 1u ? SEEK_CUR : SEEK_END));
    if (at < 0) {
        int e = errno;
        if (e == ESPIPE)
            return fs_fail(W32_ERROR_INVALID_PARAMETER);
        return fs_fail_c(-1);
    }
    if (pos)
        pos->QuadPart = at;
    return 1;
}

W32ABI W32_BOOL SetEndOfFile(W32_HANDLE h) {
    int fd;
    int64_t at;

    fd = w32_handle_to_fd(h);
    if (fd < 0)
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    at = (int64_t)lseek(fd, 0, SEEK_CUR);
    if (at < 0)
        return fs_fail_c(-1);
    if (ftruncate(fd, (off_t)at) != 0)
        return fs_fail_c(-1);
    return 1;
}

W32ABI W32_BOOL FlushFileBuffers(W32_HANDLE h) {
    int fd = w32_handle_to_fd(h);
    if (fd < 0)
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    if (fsync(fd) != 0)
        return fs_fail_c(-1);
    return 1;
}

W32ABI W32_DWORD GetFileSize(W32_HANDLE h, W32_DWORD *high) {
    int fd;
    struct stat st;

    fd = w32_handle_to_fd(h);
    if (fd < 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_INVALID_FILE_SIZE;
    }
    if (fstat(fd, &st) != 0) {
        W32_DWORD code = w32_error_from_c(-1);
        w32_set_last_error(code);
        return W32_INVALID_FILE_SIZE;
    }
    if (high)
        *high = (W32_DWORD)((uint64_t)st.st_size >> 32);
    return (W32_DWORD)((uint64_t)st.st_size & 0xFFFFFFFFu);
}

W32ABI W32_BOOL GetFileSizeEx(W32_HANDLE h, W32_LARGE_INTEGER *out) {
    int fd;
    struct stat st;

    if (!out)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    fd = w32_handle_to_fd(h);
    if (fd < 0)
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    if (fstat(fd, &st) != 0)
        return fs_fail_c(-1);
    out->QuadPart = (int64_t)st.st_size;
    return 1;
}

W32ABI W32_DWORD GetFileType(W32_HANDLE h) {
    int fd;
    struct stat st;

    fd = w32_handle_to_fd(h);
    if (fd < 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FILE_TYPE_UNKNOWN;
    }
    if (fstat(fd, &st) != 0) {
        W32_DWORD code = w32_error_from_c(-1);
        w32_set_last_error(code);
        return W32_FILE_TYPE_UNKNOWN;
    }
    if (S_ISCHR(st.st_mode))
        return W32_FILE_TYPE_CHAR;
    if (S_ISFIFO(st.st_mode))
        return W32_FILE_TYPE_PIPE;
    if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))
        return W32_FILE_TYPE_DISK;
    return W32_FILE_TYPE_UNKNOWN;
}

W32ABI W32_BOOL GetFileInformationByHandle(W32_HANDLE h,
                                            W32_BY_HANDLE_FILE_INFORMATION *out) {
    int fd;
    struct stat st;
    char proc[64];
    char target[4096];
    long n;
    const char *base = "";

    if (!out)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    fd = w32_handle_to_fd(h);
    if (fd < 0)
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    if (fstat(fd, &st) != 0)
        return fs_fail_c(-1);
    /* The basename feeds the HIDDEN bit; best-effort (pipes have none). */
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    n = (long)readlink(proc, target, sizeof(target) - 1);
    if (n > 0) {
        char *slash;
        target[n] = '\0';
        slash = strrchr(target, '/');
        base = slash ? slash + 1 : target;
    }
    out->dwFileAttributes = fs_attrs_from_stat(&st, base);
    fs_times_from_stat(&st, &out->ftCreationTime, &out->ftLastAccessTime,
                       &out->ftLastWriteTime);
    out->dwVolumeSerialNumber = FS_VOL_SERIAL;
    out->nFileSizeHigh = (W32_DWORD)((uint64_t)st.st_size >> 32);
    out->nFileSizeLow = (W32_DWORD)((uint64_t)st.st_size & 0xFFFFFFFFu);
    out->nNumberOfLinks = (W32_DWORD)st.st_nlink;
    out->nFileIndexHigh = (W32_DWORD)(FS_ST_INO(st) >> 32);
    out->nFileIndexLow = (W32_DWORD)(FS_ST_INO(st) & 0xFFFFFFFFu);
    return 1;
}

/* All I/O in this personality is synchronous, so an overlapped result is
 * always complete: ReadFile/WriteFile retire the OVERLAPPED they are given
 * (Internal 0, InternalHigh the count), and this call reports it.  A
 * STATUS_PENDING Internal (0x103) never comes out of our I/O, so one means
 * a foreign or un-driven overlapped and is refused. */
W32ABI W32_BOOL GetOverlappedResult(W32_HANDLE h, W32_OVERLAPPED *ov,
                                     W32_DWORD *nbytes, W32_BOOL wait) {
    int fd;

    (void)wait;                 /* synchronous: nothing to wait for */
    if (!ov)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    fd = w32_handle_to_fd(h);
    if (fd < 0)
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    if (ov->Internal == 0x103u)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    if (nbytes)
        *nbytes = (W32_DWORD)(ov->InternalHigh & 0xFFFFFFFFu);
    return 1;
}

/* SetHandleInformation tracks the inherit bit in a side table (fd-keyed,
 * dropped at close) that CreateProcess consults when it scrubs fds for the
 * child.  Only INHERIT exists here; there are no other handle flags. */
W32ABI W32_BOOL SetHandleInformation(W32_HANDLE h, W32_DWORD mask,
                                      W32_DWORD flags) {
    int fd;
    size_t i;

    if (mask & ~W32_HANDLE_FLAG_INHERIT)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    fd = w32_handle_to_fd(h);
    if (fd < 0)
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    if ((mask & W32_HANDLE_FLAG_INHERIT) == 0)
        return 1;
    for (i = 0; i < FS_INH_MAX; i++) {
        if (fs_inh[i].in_use && fs_inh[i].fd == fd) {
            fs_inh[i].inherit = (flags & W32_HANDLE_FLAG_INHERIT) != 0;
            return 1;
        }
    }
    for (i = 0; i < FS_INH_MAX; i++) {
        if (!fs_inh[i].in_use) {
            fs_inh[i].in_use = 1;
            fs_inh[i].fd = fd;
            fs_inh[i].inherit = (flags & W32_HANDLE_FLAG_INHERIT) != 0;
            return 1;
        }
    }
    return fs_fail(W32_ERROR_NOT_ENOUGH_MEMORY);
}

int w32_fs_is_inheritable(int fd) {
    size_t i;
    for (i = 0; i < FS_INH_MAX; i++) {
        if (fs_inh[i].in_use && fs_inh[i].fd == fd)
            return fs_inh[i].inherit;
    }
    return 0;
}

/* Duplex pipe ends: one handle, two fds.  ReadFile takes the read end,
 * WriteFile the write end, CloseHandle closes the peer of whichever the
 * table hands it. */
int w32_fs_pipe_fds(W32_HANDLE h, int *rfd, int *wfd) {
    size_t i;
    for (i = 0; i < FS_AUX_MAX; i++) {
        if (fs_aux[i].in_use && fs_aux[i].h == h) {
            if (rfd)
                *rfd = fs_aux[i].rfd;
            if (wfd)
                *wfd = fs_aux[i].wfd;
            return 1;
        }
    }
    return 0;
}

static int fs_aux_add(W32_HANDLE h, int rfd, int wfd) {
    size_t i;
    for (i = 0; i < FS_AUX_MAX; i++) {
        if (!fs_aux[i].in_use) {
            fs_aux[i].in_use = 1;
            fs_aux[i].h = h;
            fs_aux[i].rfd = rfd;
            fs_aux[i].wfd = wfd;
            return 0;
        }
    }
    return -1;
}

int w32_fs_pipe_drop(W32_HANDLE h, int *peer_out) {
    size_t i;
    for (i = 0; i < FS_AUX_MAX; i++) {
        if (fs_aux[i].in_use && fs_aux[i].h == h) {
            int wrapped = w32_handle_to_fd(h);
            fs_aux[i].in_use = 0;
            if (peer_out) {
                if (wrapped == fs_aux[i].rfd)
                    *peer_out = fs_aux[i].wfd;
                else if (wrapped == fs_aux[i].wfd)
                    *peer_out = fs_aux[i].rfd;
                else
                    *peer_out = -1;
            }
            return 0;
        }
    }
    return -1;
}

/* ---- file mappings --------------------------------------------------------------
 *
 * A mapping object holds a dup'd fd (so closing the file handle does not
 * kill the mapping — Windows semantics), a size, and a protection.  Views
 * are real mmap regions tracked in a table so UnmapViewOfFile can refuse a
 * foreign pointer instead of unmapping blindly.  Named mappings are
 * refused: without a cross-process namespace a name would promise sharing
 * that does not happen.
 */

struct fs_map {
    int fd;                     /* dup'd file fd, or -1 for pagefile */
    uint64_t size;
    W32_DWORD protect;
};

static void fs_map_free(void *p) {
    struct fs_map *m = (struct fs_map *)p;
    if (!m)
        return;
    if (m->fd >= 0)
        close(m->fd);
    free(m);
}

static int fs_prot_ok(W32_DWORD p) {
    switch (p & 0xFFu) {
    case W32_PAGE_NOACCESS:
    case W32_PAGE_READONLY:
    case W32_PAGE_READWRITE:
    case W32_PAGE_WRITECOPY:
    case W32_PAGE_EXECUTE_READ:
    case W32_PAGE_EXECUTE_READWRITE:
        break;
    default:
        return 0;
    }
    /* SEC_COMMIT/RESERVE are accepted and ignored (commit is implicit). */
    if (p & ~(0xFFu | W32_SEC_COMMIT | W32_SEC_RESERVE))
        return 0;
    return 1;
}

static W32_HANDLE fs_create_mapping(int fd, W32_DWORD protect, uint64_t size,
                                    int named) {
    struct fs_map *m = NULL;
    struct stat st;
    int dfd = -1;
    W32_HANDLE h;

    if (!fs_prot_ok(protect))
        return fs_fail_null(W32_ERROR_INVALID_PARAMETER);
    if (named)
        return fs_fail_null(W32_ERROR_INVALID_PARAMETER);
    if (fd >= 0) {
        if (fstat(fd, &st) != 0)
            return fs_fail_null(w32_error_from_c(-1));
        if (!S_ISREG(st.st_mode))
            return fs_fail_null(W32_ERROR_INVALID_PARAMETER);
        if (size == 0) {
            if (st.st_size <= 0)
                return fs_fail_null(W32_ERROR_INVALID_PARAMETER);
            size = (uint64_t)st.st_size;
        }
        dfd = dup(fd);
        if (dfd < 0)
            return fs_fail_null(w32_error_from_c(-1));
    } else {
        if (size == 0)
            return fs_fail_null(W32_ERROR_INVALID_PARAMETER);
    }
    m = (struct fs_map *)malloc(sizeof(*m));
    if (!m) {
        if (dfd >= 0)
            close(dfd);
        return fs_fail_null(W32_ERROR_NOT_ENOUGH_MEMORY);
    }
    m->fd = dfd;
    m->size = size;
    m->protect = protect;
    h = w32_handle_alloc_obj(W32_HANDLE_KIND_MAP, m, fs_map_free);
    if (!h) {
        fs_map_free(m);
        return fs_fail_null(W32_ERROR_TOO_MANY_OPEN_FILES);
    }
    return h;
}

W32ABI W32_HANDLE CreateFileMappingW(W32_HANDLE file,
                                    W32_SECURITY_ATTRIBUTES *sa,
                                    W32_DWORD protect, W32_DWORD sizeHigh,
                                    W32_DWORD sizeLow, W32_LPCWSTR name) {
    int fd = -1;
    (void)sa;
    if (file != W32_INVALID_HANDLE_VALUE) {
        fd = w32_handle_to_fd(file);
        if (fd < 0)
            return fs_fail_h(W32_ERROR_INVALID_HANDLE);
    }
    return fs_create_mapping(fd, protect,
                             ((uint64_t)sizeHigh << 32) | sizeLow,
                             name != NULL);
}

W32ABI W32_HANDLE CreateFileMappingA(W32_HANDLE file,
                                    W32_SECURITY_ATTRIBUTES *sa,
                                    W32_DWORD protect, W32_DWORD sizeHigh,
                                    W32_DWORD sizeLow, W32_LPCSTR name) {
    int fd = -1;
    (void)sa;
    if (file != W32_INVALID_HANDLE_VALUE) {
        fd = w32_handle_to_fd(file);
        if (fd < 0)
            return fs_fail_h(W32_ERROR_INVALID_HANDLE);
    }
    return fs_create_mapping(fd, protect,
                             ((uint64_t)sizeHigh << 32) | sizeLow,
                             name != NULL);
}

W32ABI void *MapViewOfFile(W32_HANDLE map, W32_DWORD access, W32_DWORD offHigh,
                           W32_DWORD offLow, W32_SIZE_T bytes) {
    struct fs_map *m;
    uint64_t off;
    uint64_t len;
    int prot = PROT_NONE;
    int flags = 0;
    int wantWrite = 0;
    int wantCopy = 0;
    int wantExec = 0;
    void *at = NULL;
    size_t i;

    m = (struct fs_map *)w32_handle_get_obj(map, W32_HANDLE_KIND_MAP);
    if (!m) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return NULL;
    }
    if (access & ~(W32_FILE_MAP_READ | W32_FILE_MAP_WRITE | W32_FILE_MAP_COPY |
                   W32_FILE_MAP_EXECUTE | W32_FILE_MAP_ALL_ACCESS)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    wantWrite = (access & (W32_FILE_MAP_WRITE | 0x8u | 0x10u)) != 0;
    wantCopy = (access & W32_FILE_MAP_COPY) != 0;
    wantExec = (access & W32_FILE_MAP_EXECUTE) != 0;
    /* View-vs-mapping compatibility, enforced rather than assumed. */
    switch (m->protect & 0xFFu) {
    case W32_PAGE_NOACCESS:
        w32_set_last_error(W32_ERROR_ACCESS_DENIED);
        return NULL;
    case W32_PAGE_READONLY:
        if (wantWrite || wantCopy) {
            w32_set_last_error(W32_ERROR_ACCESS_DENIED);
            return NULL;
        }
        break;
    case W32_PAGE_READWRITE:
    case W32_PAGE_EXECUTE_READWRITE:
        break;
    case W32_PAGE_WRITECOPY:
    case W32_PAGE_EXECUTE_READ:
        if (wantWrite && !wantCopy) {
            w32_set_last_error(W32_ERROR_ACCESS_DENIED);
            return NULL;
        }
        break;
    default:
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (wantExec && (m->protect & 0xFFu) != W32_PAGE_EXECUTE_READ &&
        (m->protect & 0xFFu) != W32_PAGE_EXECUTE_READWRITE) {
        w32_set_last_error(W32_ERROR_ACCESS_DENIED);
        return NULL;
    }
    off = ((uint64_t)offHigh << 32) | offLow;
    if (off >= m->size) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    len = (bytes == 0) ? (m->size - off) : bytes;
    if (len > m->size - off) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    prot |= PROT_READ;
    if (wantWrite || wantCopy)
        prot |= PROT_WRITE;
    if (wantExec)
        prot |= PROT_EXEC;
    if (wantCopy || (m->protect & 0xFFu) == W32_PAGE_WRITECOPY)
        flags |= MAP_PRIVATE;
    else
        flags |= MAP_SHARED;
    if (m->fd < 0)
        flags |= MAP_ANON;
    at = mmap(NULL, (size_t)len, prot, flags, m->fd < 0 ? -1 : m->fd,
              (off_t)off);
    if (at == MAP_FAILED) {
        w32_set_last_error(w32_error_from_c(-1));
        return NULL;
    }
    for (i = 0; i < FS_VIEW_MAX; i++) {
        if (!fs_views[i].in_use) {
            fs_views[i].in_use = 1;
            fs_views[i].addr = (uintptr_t)at;
            fs_views[i].len = (size_t)len;
            return at;
        }
    }
    munmap(at, (size_t)len);
    w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
}

W32ABI W32_BOOL UnmapViewOfFile(const void *addr) {
    size_t i;
    if (!addr)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    for (i = 0; i < FS_VIEW_MAX; i++) {
        if (fs_views[i].in_use && fs_views[i].addr == (uintptr_t)addr) {
            fs_views[i].in_use = 0;
            if (munmap((void *)addr, fs_views[i].len) != 0)
                return fs_fail_c(-1);
            return 1;
        }
    }
    return fs_fail(W32_ERROR_INVALID_PARAMETER);
}

/* ---- pipes ------------------------------------------------------------------------
 *
 * Anonymous pipes are pipe() with two handles.  Named pipes are per-process
 * rendezvous: CreateNamedPipe parks a listening instance, the client's
 * CreateFile on \\.\pipe\name binds it, and ConnectNamedPipe reports the
 * handshake.  No filesystem namespace backs the names, so instances die
 * with the process (fork children inherit the fds and keep talking).
 * Byte mode only: message framing is refused, not faked.
 */

W32ABI W32_BOOL CreatePipe(W32_HANDLE *readOut, W32_HANDLE *writeOut,
                             W32_SECURITY_ATTRIBUTES *sa, W32_DWORD size) {
    int fds[2];
    W32_HANDLE rh = NULL;
    W32_HANDLE wh = NULL;

    (void)sa;
    (void)size;                 /* advisory */
    if (!readOut || !writeOut)
        return fs_fail(W32_ERROR_INVALID_PARAMETER);
    *readOut = NULL;
    *writeOut = NULL;
    if (pipe(fds) != 0)
        return fs_fail_c(-1);
    rh = w32_handle_alloc(fds[0], 1);
    if (!rh) {
        close(fds[0]);
        close(fds[1]);
        return fs_fail(W32_ERROR_TOO_MANY_OPEN_FILES);
    }
    wh = w32_handle_alloc(fds[1], 1);
    if (!wh) {
        int fd = w32_handle_release(rh);
        if (fd >= 0)
            close(fd);
        close(fds[1]);
        return fs_fail(W32_ERROR_TOO_MANY_OPEN_FILES);
    }
    *readOut = rh;
    *writeOut = wh;
    return 1;
}

static const char *fs_pipe_name(const char *path) {
    /* path already passed fs_is_pipe_prefix: the name is the rest. */
    return path + 9;
}

static int fs_pipe_instances(const char *name) {
    int n = 0;
    size_t i;
    for (i = 0; i < FS_NP_MAX; i++) {
        if (fs_nps[i].in_use && strcmp(fs_nps[i].name, name) == 0)
            n++;
    }
    return n;
}

W32ABI W32_HANDLE CreateNamedPipeA(W32_LPCSTR name, W32_DWORD openMode,
                                  W32_DWORD pipeMode, W32_DWORD maxInst,
                                  W32_DWORD outBuf, W32_DWORD inBuf,
                                  W32_DWORD timeout,
                                  W32_SECURITY_ATTRIBUTES *sa) {
    const char *nm;
    W32_DWORD dir;
    int p1[2] = { -1, -1 };
    int p2[2] = { -1, -1 };
    int want = 0;
    size_t i;
    struct fs_np *np = NULL;
    int wrap = -1;
    W32_HANDLE h;

    (void)sa;
    (void)outBuf;
    (void)inBuf;                /* advisory */
    if (!name || !fs_is_pipe_prefix(name))
        return fs_fail_h(W32_ERROR_FILE_NOT_FOUND);
    nm = fs_pipe_name(name);
    if (nm[0] == '\0' || strchr(nm, '\\') || strchr(nm, '/') ||
        strlen(nm) >= sizeof(np->name))
        return fs_fail_h(W32_ERROR_FILE_NOT_FOUND);
    dir = openMode & 0x3u;
    if (dir != W32_PIPE_ACCESS_INBOUND && dir != W32_PIPE_ACCESS_OUTBOUND &&
        dir != W32_PIPE_ACCESS_DUPLEX)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    if (openMode & ~(0x3u | W32_FILE_FLAG_FIRST_PIPE_INSTANCE |
                     W32_FILE_FLAG_OVERLAPPED | W32_FILE_FLAG_WRITE_THROUGH)) {
        if (openMode & W32_FILE_FLAG_FIRST_PIPE_INSTANCE) {
            /* FIRST_PIPE_INSTANCE is honoured below; anything else unknown
             * in the mode word is refused. */
        } else {
            return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
        }
        if (openMode & ~(0x3u | W32_FILE_FLAG_FIRST_PIPE_INSTANCE |
                         W32_FILE_FLAG_OVERLAPPED |
                         W32_FILE_FLAG_WRITE_THROUGH))
            return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    }
    if ((openMode & W32_FILE_FLAG_FIRST_PIPE_INSTANCE) &&
        fs_pipe_instances(nm) > 0)
        return fs_fail_h(W32_ERROR_ACCESS_DENIED);
    if ((pipeMode & 0x4u) != W32_PIPE_TYPE_BYTE)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);  /* no message mode */
    if ((pipeMode & 0x2u) != W32_PIPE_READMODE_BYTE)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    if ((pipeMode & 0x1u) != W32_PIPE_WAIT)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);  /* no NOWAIT reads */
    if (pipeMode & ~0x7u)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    if (maxInst == 0)
        maxInst = W32_PIPE_UNLIMITED_INSTANCES;
    if (maxInst > W32_PIPE_UNLIMITED_INSTANCES)
        return fs_fail_h(W32_ERROR_INVALID_PARAMETER);
    if (fs_pipe_instances(nm) >= (int)maxInst)
        return fs_fail_h(W32_ERROR_PIPE_BUSY);

    for (i = 0; i < FS_NP_MAX; i++) {
        if (!fs_nps[i].in_use) {
            np = &fs_nps[i];
            break;
        }
    }
    if (!np)
        return fs_fail_h(W32_ERROR_TOO_MANY_OPEN_FILES);

    want = (dir == W32_PIPE_ACCESS_DUPLEX) ? 2 : 1;
    if (pipe(p1) != 0)
        return fs_fail_h(w32_error_from_c(-1));
    if (want == 2 && pipe(p2) != 0) {
        W32_DWORD code = w32_error_from_c(-1);
        close(p1[0]);
        close(p1[1]);
        return fs_fail_h(code);
    }
    memset(np, 0, sizeof(*np));
    np->in_use = 1;
    np->server_r = -1;
    np->server_w = -1;
    np->client_r = -1;
    np->client_w = -1;
    for (i = 0; nm[i] != '\0'; i++)
        np->name[i] = nm[i];
    np->name[i] = '\0';
    if (dir == W32_PIPE_ACCESS_INBOUND) {
        np->server_r = p1[0];   /* server reads */
        np->client_w = p1[1];   /* client writes */
        wrap = p1[0];
    } else if (dir == W32_PIPE_ACCESS_OUTBOUND) {
        np->server_w = p1[1];   /* server writes */
        np->client_r = p1[0];   /* client reads */
        wrap = p1[1];
    } else {
        np->server_r = p1[0];   /* client -> server */
        np->client_w = p1[1];
        np->server_w = p2[1];   /* server -> client */
        np->client_r = p2[0];
        wrap = p1[0];
    }
    np->connected = 0;
    np->server_open = 1;
    np->client_open = 0;
    np->openMode = openMode;
    np->pipeMode = pipeMode;
    np->maxInst = maxInst;
    np->timeout = timeout;
    h = w32_handle_alloc(wrap, 1);
    if (!h) {
        close(p1[0]);
        close(p1[1]);
        if (want == 2) {
            close(p2[0]);
            close(p2[1]);
        }
        np->in_use = 0;
        return fs_fail_h(W32_ERROR_TOO_MANY_OPEN_FILES);
    }
    if (dir == W32_PIPE_ACCESS_DUPLEX) {
        if (fs_aux_add(h, np->server_r, np->server_w) != 0) {
            int fd = w32_handle_release(h);
            if (fd >= 0)
                close(fd);
            close(p1[1]);
            close(p2[0]);
            close(p2[1]);
            np->in_use = 0;
            return fs_fail_h(W32_ERROR_NOT_ENOUGH_MEMORY);
        }
    }
    return h;
}

static W32_HANDLE fs_pipe_client_open(const char *path, W32_DWORD access) {
    const char *nm;
    size_t i;
    struct fs_np *np = NULL;
    int any = 0;
    W32_DWORD dir;
    int wrap = -1;
    W32_HANDLE h;

    nm = fs_pipe_name(path);
    for (i = 0; i < FS_NP_MAX; i++) {
        if (fs_nps[i].in_use && strcmp(fs_nps[i].name, nm) == 0) {
            any = 1;
            if (!fs_nps[i].connected && fs_nps[i].server_open) {
                np = &fs_nps[i];
                break;
            }
        }
    }
    if (!np)
        return fs_fail_h(any ? W32_ERROR_PIPE_BUSY : W32_ERROR_FILE_NOT_FOUND);
    dir = np->openMode & 0x3u;
    if (dir == W32_PIPE_ACCESS_INBOUND &&
        (access & (W32_GENERIC_WRITE | W32_GENERIC_ALL)) == 0)
        return fs_fail_h(W32_ERROR_ACCESS_DENIED);
    if (dir == W32_PIPE_ACCESS_OUTBOUND &&
        (access & (W32_GENERIC_READ | W32_GENERIC_ALL)) == 0)
        return fs_fail_h(W32_ERROR_ACCESS_DENIED);
    if (dir == W32_PIPE_ACCESS_INBOUND) {
        wrap = np->client_w;
    } else if (dir == W32_PIPE_ACCESS_OUTBOUND) {
        wrap = np->client_r;
    } else {
        wrap = np->client_r;
    }
    h = w32_handle_alloc(wrap, 1);
    if (!h)
        return fs_fail_h(W32_ERROR_TOO_MANY_OPEN_FILES);
    if (dir == W32_PIPE_ACCESS_DUPLEX) {
        if (fs_aux_add(h, np->client_r, np->client_w) != 0) {
            int fd = w32_handle_release(h);
            if (fd >= 0)
                close(fd);
            return fs_fail_h(W32_ERROR_NOT_ENOUGH_MEMORY);
        }
    }
    np->connected = 1;
    np->client_open = 1;
    w32_set_last_error(W32_ERROR_SUCCESS);
    return h;
}

W32ABI W32_BOOL ConnectNamedPipe(W32_HANDLE h, W32_OVERLAPPED *ov) {
    int fd;
    size_t i;

    (void)ov;                   /* the rendezvous never pends */
    fd = w32_handle_to_fd(h);
    if (fd < 0)
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    for (i = 0; i < FS_NP_MAX; i++) {
        struct fs_np *np = &fs_nps[i];
        if (!np->in_use)
            continue;
        if (fd != np->server_r && fd != np->server_w)
            continue;
        if (np->connected)
            return fs_fail(W32_ERROR_PIPE_CONNECTED);
        /* Single-process rendezvous: the client either arrived already
         * (connected) or it has not, and nothing blocks here.  A listening
         * server whose client has not called CreateFile yet reports
         * NOT_CONNECTED — the poll-and-retry shape, documented. */
        return fs_fail(W32_ERROR_PIPE_NOT_CONNECTED);
    }
    return fs_fail(W32_ERROR_INVALID_HANDLE);
}

W32ABI W32_BOOL WaitNamedPipeA(W32_LPCSTR name, W32_DWORD timeout) {
    const char *nm;
    W32_DWORD start;
    W32_DWORD now;

    if (!name || !fs_is_pipe_prefix(name))
        return fs_fail(W32_ERROR_FILE_NOT_FOUND);
    nm = fs_pipe_name(name);
    if (nm[0] == '\0')
        return fs_fail(W32_ERROR_FILE_NOT_FOUND);
    /* Instances are per-process, so the wait only ever observes this
     * process's own table; the poll loop keeps the Windows shape (and stays
     * correct if threads ever create instances concurrently). */
    if (timeout == W32_NMPWAIT_NOWAIT || timeout == W32_NMPWAIT_USE_DEFAULT_WAIT) {
        size_t i;
        for (i = 0; i < FS_NP_MAX; i++) {
            if (fs_nps[i].in_use && !fs_nps[i].connected &&
                strcmp(fs_nps[i].name, nm) == 0)
                return 1;
        }
        return fs_fail(W32_ERROR_PIPE_BUSY);
    }
    start = GetTickCount();
    for (;;) {
        size_t i;
        for (i = 0; i < FS_NP_MAX; i++) {
            if (fs_nps[i].in_use && !fs_nps[i].connected &&
                strcmp(fs_nps[i].name, nm) == 0)
                return 1;
        }
        if (timeout != W32_NMPWAIT_WAIT_FOREVER) {
            now = GetTickCount();
            if (now - start >= timeout)
                return fs_fail(W32_ERROR_SEM_TIMEOUT);
        }
        Sleep(10);
    }
}

/* ---- io control: the documented refusal -------------------------------------------
 *
 * No device namespace exists behind these handles, so every control code is
 * refused the way Windows refuses a code the device does not know.  The log
 * line names the code so a caller can see its ioctl die loudly.
 */

W32ABI W32_BOOL CancelIo(W32_HANDLE h) {
    if (w32_handle_kind(h) < 0 && w32_handle_to_fd(h) < 0)
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    return 1;                   /* synchronous I/O: nothing pending */
}

W32ABI W32_BOOL DeviceIoControl(W32_HANDLE h, W32_DWORD code,
                                 const void *in, W32_DWORD inLen,
                                 void *out, W32_DWORD outLen,
                                 W32_DWORD *ret, W32_OVERLAPPED *ov) {
    (void)in;
    (void)inLen;
    (void)out;
    (void)outLen;
    (void)ov;
    if (w32_handle_kind(h) < 0 && w32_handle_to_fd(h) < 0)
        return fs_fail(W32_ERROR_INVALID_HANDLE);
    if (ret)
        *ret = 0;
    fprintf(stderr, "w32: DeviceIoControl(code 0x%lx) refused: no devices\n",
            (unsigned long)code);
    return fs_fail(W32_ERROR_INVALID_FUNCTION);
}

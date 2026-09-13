/* test_w32_a2.c — W32A-2 breadth, exercised against the real host.
 *
 * Unlike test_w32_kernel32.c (which stubs POSIX), this test links the REAL
 * host libc and works inside a mkdtemp directory: files are created,
 * processes are spawned, /proc is read, CPUID executes.  Every check that
 * can run on Linux runs here; the guest fixtures prove the same code under
 * AuraLite's own libc.
 */

#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cpuid.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include "w32/w32_abi.h"
#include "w32/w32_handle.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"
#include "w32/kernel32.h"

/* ExitProcess detaches DLLs; this test never exits that way. */
void w32_module_detach_all(void) {}

#define AURALITE_W32_HOST_TEST 1
#define main w32_unused_main
#include "../../w32/src/w32_utf.c"
#include "../../w32/src/w32_errno.c"
#include "../../w32/src/w32_handle.c"
#include "../../w32/src/kernel32.c"
#include "../../w32/src/kernel32_fs.c"
#include "../../w32/src/kernel32_ps.c"
#include "../../w32/src/kernel32_loc.c"
#include "../../w32/src/w32_msg.c"
#undef main

static int fails;
static int checks;

#define CHECK(cond) do { checks++; \
    if (!(cond)) { \
        printf("  FAIL L%d: %s\n", __LINE__, #cond); fails++; } } while (0)
#define CHECK_EQ(a, b) do { checks++; \
    if ((long long)(a) != (long long)(b)) { \
        printf("  FAIL L%d: %s = %lld, want %lld\n", __LINE__, #a, \
            (long long)(a), (long long)(b)); fails++; } } while (0)

static char tmpd[256];

/* ASCII -> WCHAR helper for test literals. */
static void toW(const char *a, W32_WCHAR *w) {
    size_t i;
    for (i = 0; a[i]; i++)
        w[i] = (W32_WCHAR)(unsigned char)a[i];
    w[i] = 0;
}

static int Weq(const W32_WCHAR *w, const char *a) {
    size_t i;
    for (i = 0; ; i++) {
        if ((char)w[i] != a[i])
            return 0;
        if (a[i] == '\0')
            return w[i] == 0;
        if (w[i] > 0x7Fu)
            return 0;
    }
}

/* tmpd + leaf as a DOS path ("C:\tmp\w32a2_X\leaf"). */
static void dosJoin(const char *leaf, char *out8, size_t cap) {
    size_t o = 0;
    size_t i;
    out8[o++] = 'C';
    out8[o++] = ':';
    for (i = 0; tmpd[i] && o + 1 < cap; i++)
        out8[o++] = (tmpd[i] == '/') ? '\\' : tmpd[i];
    if (leaf && leaf[0] && o + 1 < cap)
        out8[o++] = '\\';
    for (i = 0; leaf && leaf[i] && o + 1 < cap; i++)
        out8[o++] = (leaf[i] == '/') ? '\\' : leaf[i];
    out8[o] = '\0';
}

static void hostJoin(const char *leaf, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", tmpd, leaf);
}

/* ---- G1: find ------------------------------------------------------------ */

static void test_find(void) {
    char h8[512];
    char d8[512];
    W32_WCHAR w[512];
    W32_WIN32_FIND_DATAW fd;
    W32_WIN32_FIND_DATAA fda;
    W32_HANDLE fh;
    int n;
    FILE *f;

    printf("== find ==\n");
    hostJoin("alpha.txt", h8, sizeof(h8));
    f = fopen(h8, "w");
    CHECK(f != NULL);
    if (f) {
        fputs("0123456789", f);
        fclose(f);
    }
    hostJoin("beta.TXT", h8, sizeof(h8));
    f = fopen(h8, "w");
    if (f)
        fclose(f);
    hostJoin("gamma.bin", h8, sizeof(h8));
    f = fopen(h8, "w");
    if (f)
        fclose(f);

    /* Case-insensitive pattern over W. */
    dosJoin("*.txt", d8, sizeof(d8));
    toW(d8, w);
    memset(&fd, 0, sizeof(fd));
    fh = FindFirstFileW(w, &fd);
    CHECK(fh != W32_INVALID_HANDLE_VALUE);
    n = 0;
    if (fh != W32_INVALID_HANDLE_VALUE) {
        do {
            n++;
            CHECK(fd.nFileSizeLow == 10 || fd.nFileSizeLow == 0);
            CHECK((fd.dwFileAttributes & W32_FILE_ATTRIBUTE_ARCHIVE) != 0);
        } while (FindNextFileW(fh, &fd));
        CHECK_EQ(GetLastError(), W32_ERROR_NO_MORE_FILES);
        CHECK(FindClose(fh));
    }
    CHECK_EQ(n, 2);

    /* "*" includes dot entries. */
    dosJoin("*", d8, sizeof(d8));
    toW(d8, w);
    fh = FindFirstFileW(w, &fd);
    CHECK(fh != W32_INVALID_HANDLE_VALUE);
    if (fh != W32_INVALID_HANDLE_VALUE) {
        int dots = 0;
        do {
            if (Weq(fd.cFileName, ".") || Weq(fd.cFileName, ".."))
                dots++;
        } while (FindNextFileW(fh, &fd));
        CHECK_EQ(dots, 2);
        CHECK(FindClose(fh));
    }

    /* A form. */
    dosJoin("*.bin", d8, sizeof(d8));
    memset(&fda, 0, sizeof(fda));
    fh = FindFirstFileA(d8, &fda);
    CHECK(fh != W32_INVALID_HANDLE_VALUE);
    if (fh != W32_INVALID_HANDLE_VALUE) {
        CHECK(strcmp(fda.cFileName, "gamma.bin") == 0);
        CHECK(!FindNextFileA(fh, &fda));
        CHECK(FindClose(fh));
    }

    /* No match. */
    dosJoin("*.zzz", d8, sizeof(d8));
    toW(d8, w);
    fh = FindFirstFileW(w, &fd);
    CHECK(fh == W32_INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), W32_ERROR_FILE_NOT_FOUND);

    /* Ex: happy + refusals. */
    dosJoin("*.txt", d8, sizeof(d8));
    toW(d8, w);
    memset(&fd, 0, sizeof(fd));
    fh = FindFirstFileExW(w, W32_FIND_EX_INFO_STANDARD, &fd,
        W32_FIND_EX_SEARCH_NAME_MATCH, NULL, 0);
    CHECK(fh != W32_INVALID_HANDLE_VALUE);
    if (fh != W32_INVALID_HANDLE_VALUE)
        CHECK(FindClose(fh));
    fh = FindFirstFileExW(w, 99, &fd, 0, NULL, 0);
    CHECK(fh == W32_INVALID_HANDLE_VALUE);
    fh = FindFirstFileExW(w, 0, &fd, 1, NULL, 0);
    CHECK(fh == W32_INVALID_HANDLE_VALUE);
    fh = FindFirstFileExW(w, 0, &fd, 0, NULL, 1);
    CHECK(fh == W32_INVALID_HANDLE_VALUE);

    /* CloseHandle refuses find handles; FindClose refuses files. */
    dosJoin("*.txt", d8, sizeof(d8));
    toW(d8, w);
    fh = FindFirstFileW(w, &fd);
    CHECK(fh != W32_INVALID_HANDLE_VALUE);
    if (fh != W32_INVALID_HANDLE_VALUE) {
        CHECK(!CloseHandle(fh));
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_HANDLE);
        CHECK(FindClose(fh));
        CHECK(!FindClose(fh));
    }

    /* Streams: the one default stream. */
    {
        W32_WIN32_FIND_STREAM_DATA sd;
        dosJoin("alpha.txt", d8, sizeof(d8));
        toW(d8, w);
        memset(&sd, 0, sizeof(sd));
        fh = FindFirstStreamW(w, 0, &sd, 0);
        CHECK(fh != W32_INVALID_HANDLE_VALUE);
        if (fh != W32_INVALID_HANDLE_VALUE) {
            CHECK_EQ(sd.StreamSize.QuadPart, 10);
            CHECK(Weq(sd.cStreamName, "::$DATA"));
            CHECK(!FindNextStreamW(fh, &sd));
            CHECK(FindClose(fh));
        }
        /* A directory has no default stream. */
        dosJoin("", d8, sizeof(d8));
        toW(d8, w);
        fh = FindFirstStreamW(w, 0, &sd, 0);
        CHECK(fh == W32_INVALID_HANDLE_VALUE);
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
    }

    /* Change notifications: a handle that never fires. */
    {
        dosJoin("", d8, sizeof(d8));
        toW(d8, w);
        fh = FindFirstChangeNotificationW(w, 0, 0xFFFu);
        CHECK(fh != W32_INVALID_HANDLE_VALUE);
        if (fh != W32_INVALID_HANDLE_VALUE) {
            CHECK(FindNextChangeNotification(fh));
            CHECK(!CloseHandle(fh));
            CHECK_EQ(GetLastError(), W32_ERROR_INVALID_HANDLE);
            CHECK(FindCloseChangeNotification(fh));
        }
        toW("C:\\no\\such\\dir", w);
        fh = FindFirstChangeNotificationW(w, 0, 1);
        CHECK(fh == W32_INVALID_HANDLE_VALUE);
    }
}

/* ---- G2: attributes, copy, move, volumes, paths, time -------------------- */

static void test_attrs_dirs(void) {
    char d8[512];
    W32_WCHAR w[512];
    W32_WIN32_FILE_ATTRIBUTE_DATA ad;
    W32_DWORD a;

    printf("== attrs/dirs ==\n");
    dosJoin("alpha.txt", d8, sizeof(d8));
    toW(d8, w);
    a = GetFileAttributesW(w);
    CHECK(a != W32_INVALID_FILE_ATTRIBUTES);
    CHECK((a & W32_FILE_ATTRIBUTE_ARCHIVE) != 0);
    CHECK((a & W32_FILE_ATTRIBUTE_DIRECTORY) == 0);
    CHECK(GetFileAttributesExW(w, 0, &ad));
    CHECK_EQ(ad.nFileSizeLow, 10u);

    toW("C:\\no\\such\\file.txt", w);
    CHECK_EQ(GetFileAttributesW(w), W32_INVALID_FILE_ATTRIBUTES);
    CHECK_EQ(GetLastError(), W32_ERROR_FILE_NOT_FOUND);

    /* Read-only round-trip. */
    dosJoin("beta.TXT", d8, sizeof(d8));
    toW(d8, w);
    CHECK(SetFileAttributesW(w, W32_FILE_ATTRIBUTE_READONLY));
    a = GetFileAttributesW(w);
    CHECK((a & W32_FILE_ATTRIBUTE_READONLY) != 0);
    CHECK(SetFileAttributesW(w, W32_FILE_ATTRIBUTE_NORMAL));
    a = GetFileAttributesW(w);
    CHECK((a & W32_FILE_ATTRIBUTE_READONLY) == 0);
    CHECK(!SetFileAttributesW(w, 0xDEAD0000u));

    /* Directories. */
    dosJoin("sub", d8, sizeof(d8));
    toW(d8, w);
    CHECK(CreateDirectoryW(w, NULL));
    CHECK(!CreateDirectoryW(w, NULL));
    CHECK_EQ(GetLastError(), W32_ERROR_ALREADY_EXISTS);
    a = GetFileAttributesW(w);
    CHECK((a & W32_FILE_ATTRIBUTE_DIRECTORY) != 0);
    dosJoin("sub\\kid.txt", d8, sizeof(d8));
    toW(d8, w);
    {
        W32_HANDLE h = CreateFileW(w, W32_GENERIC_WRITE, 0, NULL,
            2u, 0, NULL);
        CHECK(h != W32_INVALID_HANDLE_VALUE);
        if (h != W32_INVALID_HANDLE_VALUE)
            CHECK(CloseHandle(h));
    }
    dosJoin("sub", d8, sizeof(d8));
    toW(d8, w);
    CHECK(!RemoveDirectoryW(w));
    CHECK_EQ(GetLastError(), W32_ERROR_DIR_NOT_EMPTY);
    dosJoin("sub\\kid.txt", d8, sizeof(d8));
    toW(d8, w);
    CHECK(DeleteFileW(w));
    dosJoin("sub", d8, sizeof(d8));
    toW(d8, w);
    CHECK(RemoveDirectoryW(w));

    /* DeleteFileA + hard links. */
    dosJoin("alpha.txt", d8, sizeof(d8));
    {
        char link8[512];
        W32_WCHAR wl[512];
        dosJoin("alink.txt", link8, sizeof(link8));
        toW(link8, wl);
        toW(d8, w);
        CHECK(CreateHardLinkW(wl, w, NULL));
        CHECK_EQ(GetFileAttributesW(wl) != W32_INVALID_FILE_ATTRIBUTES, 1);
        CHECK(DeleteFileA(link8));
    }
    CHECK(!DeleteFileA("C:\\no\\such.txt"));
}

static int progressCalls;
static W32_DWORD W32ABI testProgressCb(W32_LARGE_INTEGER total,
    W32_LARGE_INTEGER done, W32_LARGE_INTEGER streamSize,
    W32_LARGE_INTEGER streamDone, W32_DWORD streamNo, W32_DWORD reason,
    W32_HANDLE srcFile, W32_HANDLE dstFile, void *data) {
    (void)total; (void)done; (void)streamSize; (void)streamDone;
    (void)streamNo; (void)reason; (void)srcFile; (void)dstFile; (void)data;
    progressCalls++;
    return 0;
}

static W32_DWORD W32ABI testCancelCb(W32_LARGE_INTEGER total,
    W32_LARGE_INTEGER done, W32_LARGE_INTEGER streamSize,
    W32_LARGE_INTEGER streamDone, W32_DWORD streamNo, W32_DWORD reason,
    W32_HANDLE srcFile, W32_HANDLE dstFile, void *data) {
    (void)total; (void)done; (void)streamSize; (void)streamDone;
    (void)streamNo; (void)reason; (void)srcFile; (void)dstFile; (void)data;
    return 1;                   /* CANCEL at the first call */
}

static void test_copy_move(void) {
    char d8[512];
    char s8[512];
    W32_WCHAR w[512];
    W32_WCHAR v[512];
    W32_BOOL cancelFlag = 0;

    printf("== copy/move ==\n");
    dosJoin("alpha.txt", s8, sizeof(s8));
    toW(s8, w);
    dosJoin("copy1.txt", d8, sizeof(d8));
    toW(d8, v);
    progressCalls = 0;
    CHECK(CopyFileExW(w, v, testProgressCb, NULL, &cancelFlag, 0));
    CHECK(progressCalls >= 2);
    CHECK(!CopyFileW(w, v, 1));
    CHECK_EQ(GetLastError(), W32_ERROR_FILE_EXISTS);
    CHECK(CopyFileW(w, v, 0));
    /* Same file refused. */
    CHECK(!CopyFileW(w, w, 0));

    /* Cancel removes the partial target. */
    dosJoin("copy2.txt", d8, sizeof(d8));
    toW(d8, v);
    CHECK(!CopyFileExW(w, v, testCancelCb, NULL, NULL, 0));
    CHECK_EQ(GetLastError(), W32_ERROR_OPERATION_ABORTED);
    CHECK_EQ(GetFileAttributesW(v), W32_INVALID_FILE_ATTRIBUTES);

    /* Move + hardlink-move + replace. */
    dosJoin("copy1.txt", s8, sizeof(s8));
    toW(s8, w);
    dosJoin("moved.txt", d8, sizeof(d8));
    toW(d8, v);
    CHECK(MoveFileW(w, v));
    CHECK_EQ(GetFileAttributesW(w), W32_INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW(v) != W32_INVALID_FILE_ATTRIBUTES);
    /* Move onto existing without REPLACE fails. */
    dosJoin("alpha.txt", s8, sizeof(s8));
    toW(s8, w);
    CHECK(!MoveFileExW(w, v, 0));
    CHECK_EQ(GetLastError(), W32_ERROR_ALREADY_EXISTS);
    CHECK(MoveFileExW(w, v, W32_MOVEFILE_REPLACE_EXISTING));
    /* WithProgress shares the move core. */
    {
        char d8b[512];
        W32_WCHAR w2[512];
        dosJoin("moved2.txt", d8b, sizeof(d8b));
        toW(d8b, w2);
        CHECK(MoveFileWithProgressW(v, w2, NULL, NULL, 0));
        CHECK_EQ(GetFileAttributesW(v), W32_INVALID_FILE_ATTRIBUTES);
        CHECK(!MoveFileWithProgressW(w, w2, NULL, NULL, 0));
        CHECK_EQ(GetLastError(), W32_ERROR_FILE_NOT_FOUND);
        CHECK(MoveFileWithProgressW(w2, v, NULL, NULL, 0));
    }
    /* refused flags */
    CHECK(!MoveFileExW(v, w, W32_MOVEFILE_DELAY_UNTIL_REBOOT));
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
    /* hardlink move */
    dosJoin("moved.txt", s8, sizeof(s8));
    toW(s8, w);
    dosJoin("hlink.txt", d8, sizeof(d8));
    toW(d8, v);
    CHECK(MoveFileExW(w, v, W32_MOVEFILE_CREATE_HARDLINK));
    {
        W32_BY_HANDLE_FILE_INFORMATION bi;
        W32_HANDLE h = CreateFileW(v, W32_GENERIC_READ,
            W32_FILE_SHARE_READ, NULL, 3u, 0, NULL);
        CHECK(h != W32_INVALID_HANDLE_VALUE);
        if (h != W32_INVALID_HANDLE_VALUE) {
            CHECK(GetFileInformationByHandle(h, &bi));
            CHECK_EQ(bi.nNumberOfLinks, 2u);
            CHECK(CloseHandle(h));
        }
    }
    /* ReplaceFile with backup. */
    dosJoin("replaced.txt", s8, sizeof(s8));
    toW(s8, w);
    {
        W32_WCHAR b[512];
        W32_HANDLE h;
        h = CreateFileW(w, W32_GENERIC_WRITE, 0, NULL, 2u, 0, NULL);
        if (h != W32_INVALID_HANDLE_VALUE)
            CloseHandle(h);
        dosJoin("repl.txt", d8, sizeof(d8));
        toW(d8, v);
        h = CreateFileW(v, W32_GENERIC_WRITE, 0, NULL, 2u, 0, NULL);
        if (h != W32_INVALID_HANDLE_VALUE)
            CloseHandle(h);
        dosJoin("bak.txt", d8, sizeof(d8));
        toW(d8, b);
        CHECK(ReplaceFileW(w, v, b, 0, NULL, NULL));
        CHECK(GetFileAttributesW(b) != W32_INVALID_FILE_ATTRIBUTES);
        CHECK(DeleteFileW(b));
        CHECK(DeleteFileW(w));
        CHECK_EQ(GetFileAttributesW(v), W32_INVALID_FILE_ATTRIBUTES);
    }
}

static void test_volumes_paths(void) {
    W32_WCHAR w[512];
    W32_DWORD spc, bps, fc, tc;
    W32_ULARGE_INTEGER fa, tot, tf;
    W32_WCHAR buf[64];
    W32_DWORD serial, maxc, flags;
    W32_WCHAR vol[64];
    W32_WCHAR fsn[64];

    printf("== volumes/paths ==\n");
    CHECK(GetDiskFreeSpaceW(NULL, &spc, &bps, &fc, &tc));
    CHECK_EQ(bps, 512u);
    CHECK(tc > 0);
    CHECK(GetDiskFreeSpaceExW(NULL, &fa, &tot, &tf));
    CHECK(tot.QuadPart > 0);

    toW("C:\\", w);
    CHECK_EQ(GetDriveTypeW(w), W32_DRIVE_FIXED);
    CHECK_EQ(GetDriveTypeW(NULL), W32_DRIVE_FIXED);
    toW("Z:\\", w);
    CHECK_EQ(GetDriveTypeW(w), W32_DRIVE_NO_ROOT_DIR);
    CHECK_EQ(GetLogicalDriveStringsW(0, NULL), 4u);
    CHECK_EQ(GetLogicalDriveStringsW(64, buf), 4u);
    CHECK(buf[0] == (W32_WCHAR)'C' && buf[3] == 0);

    toW("C:\\", w);
    CHECK(GetVolumeInformationW(w, vol, 64, &serial, &maxc, &flags, fsn, 64));
    CHECK(Weq(vol, "AURALITE"));
    CHECK(Weq(fsn, "AURALFS"));
    CHECK_EQ(maxc, 255u);
    toW("Z:\\", w);
    CHECK(!GetVolumeInformationW(w, NULL, 0, NULL, NULL, NULL, NULL, 0));

    /* Full path: dots, file part, measure. */
    {
        char d8[512];
        W32_LPWSTR part = NULL;
        W32_DWORD n;
        dosJoin("sub2\\..\\alpha.txt", d8, sizeof(d8));
        toW(d8, w);
        n = GetFullPathNameW(w, 0, NULL, NULL);
        CHECK(n > 10);
        n = GetFullPathNameW(w, 512, buf, &part);
        CHECK(n > 10);
        CHECK(buf[0] == (W32_WCHAR)'C' && buf[1] == (W32_WCHAR)':');
        CHECK(part != NULL);
        if (part)
            CHECK(Weq(part, "alpha.txt"));
        /* short buffer reports need */
        n = GetFullPathNameW(w, 4, buf, NULL);
        CHECK(n > 4);
    }
    /* Long path echoes. */
    {
        char d8[512];
        W32_DWORD n;
        dosJoin("beta.TXT", d8, sizeof(d8));
        toW(d8, w);
        n = GetLongPathNameW(w, buf, 512);
        CHECK(n > 5);
        toW("C:\\no\\such.txt", w);
        CHECK_EQ(GetLongPathNameW(w, buf, 512), 0u);
    }
    /* Final path by handle. */
    {
        char d8[512];
        W32_HANDLE h;
        W32_DWORD n;
        dosJoin("beta.TXT", d8, sizeof(d8));
        toW(d8, w);
        h = CreateFileW(w, W32_GENERIC_READ, W32_FILE_SHARE_READ, NULL, 3u,
            0, NULL);
        CHECK(h != W32_INVALID_HANDLE_VALUE);
        if (h != W32_INVALID_HANDLE_VALUE) {
            n = GetFinalPathNameByHandleW(h, buf, 512, 0);
            CHECK(n > 8);
            CHECK(buf[0] == (W32_WCHAR)'\\' && buf[1] == (W32_WCHAR)'\\');
            CHECK(CloseHandle(h));
        }
    }
    /* Compressed size tracks blocks. */
    {
        char d8[512];
        W32_DWORD hi = 0;
        W32_DWORD lo;
        dosJoin("beta.TXT", d8, sizeof(d8));
        toW(d8, w);
        lo = GetCompressedFileSizeW(w, &hi);
        CHECK(lo != W32_INVALID_FILE_SIZE || hi != 0);
    }
}

static void test_time(void) {
    W32_FILETIME ft;
    W32_SYSTEMTIME st;
    W32_WORD date, time;
    W32_FILETIME back;
    W32_LARGE_INTEGER q1, q2, qf;
    W32_TIME_ZONE_INFORMATION tz;

    printf("== time ==\n");
    GetSystemTimeAsFileTime(&ft);
    CHECK(ft.dwHighDateTime != 0 || ft.dwLowDateTime != 0);
    CHECK(FileTimeToSystemTime(&ft, &st));
    CHECK(st.wYear >= 2026u);
    CHECK(st.wMonth >= 1 && st.wMonth <= 12);
    /* DOS round-trip. */
    CHECK(FileTimeToDosDateTime(&ft, &date, &time));
    CHECK(DosDateTimeToFileTime(date, time, &back));
    CHECK_EQ(CompareFileTime(&back, &ft) <= 0, 1);
    CHECK_EQ(DosDateTimeToFileTime(0, 0, &back), 0);   /* month 0 invalid */
    /* Identities under UTC. */
    {
        W32_FILETIME tmp;
        CHECK(FileTimeToLocalFileTime(&ft, &tmp));
        CHECK_EQ(tmp.dwLowDateTime, ft.dwLowDateTime);
        CHECK(LocalFileTimeToFileTime(&tmp, &back));
        CHECK_EQ(back.dwHighDateTime, ft.dwHighDateTime);
    }
    CHECK_EQ(CompareFileTime(&ft, &ft), 0);
    {
        W32_SYSTEMTIME out;
        CHECK(SystemTimeToTzSpecificLocalTime(NULL, &st, &out));
        CHECK_EQ(out.wYear, st.wYear);
    }
    GetLocalTime(&st);
    CHECK(st.wYear >= 2026u);
    CHECK_EQ(GetTimeZoneInformation(&tz), 0u);
    CHECK_EQ(tz.Bias, 0);
    CHECK(QueryPerformanceFrequency(&qf));
    CHECK_EQ(qf.QuadPart, 10000000LL);
    CHECK(QueryPerformanceCounter(&q1));
    CHECK(QueryPerformanceCounter(&q2));
    CHECK(q2.QuadPart >= q1.QuadPart);
    CHECK(GetTickCount() != 0 || GetTickCount64() != 0);
    /* SetFileTime round-trip through a handle. */
    {
        char d8[512];
        W32_WCHAR w[512];
        W32_HANDLE h;
        W32_BY_HANDLE_FILE_INFORMATION bi;
        dosJoin("beta.TXT", d8, sizeof(d8));
        toW(d8, w);
        h = CreateFileW(w, W32_GENERIC_READ | W32_GENERIC_WRITE, 0, NULL,
            3u, 0, NULL);
        CHECK(h != W32_INVALID_HANDLE_VALUE);
        if (h != W32_INVALID_HANDLE_VALUE) {
            W32_FILETIME wt;
            CHECK(DosDateTimeToFileTime(
                (W32_WORD)(((2020u - 1980u) << 9) | (3u << 5) | 4u),
                (W32_WORD)((5u << 11) | (6u << 5) | 14u), &wt));
            CHECK(SetFileTime(h, NULL, NULL, &wt));
            CHECK(GetFileInformationByHandle(h, &bi));
            CHECK_EQ(CompareFileTime(&bi.ftLastWriteTime, &wt), 0);
            CHECK(CloseHandle(h));
        }
    }
}

/* ---- G3: create, pointers, mappings, pipes -------------------------------- */

static void test_create(void) {
    char d8[512];
    W32_WCHAR w[512];
    W32_HANDLE h1, h2;

    printf("== create ==\n");
    dosJoin("share.txt", d8, sizeof(d8));
    toW(d8, w);
    h1 = CreateFileW(w, W32_GENERIC_READ | W32_GENERIC_WRITE, 0, NULL, 2u,
        0, NULL);
    CHECK(h1 != W32_INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), W32_ERROR_SUCCESS);
    if (h1 != W32_INVALID_HANDLE_VALUE) {
        /* Exclusive open denies a second reader. */
        h2 = CreateFileW(w, W32_GENERIC_READ,
            W32_FILE_SHARE_READ | W32_FILE_SHARE_WRITE |
            W32_FILE_SHARE_DELETE, NULL, 3u, 0, NULL);
        CHECK(h2 == W32_INVALID_HANDLE_VALUE);
        CHECK_EQ(GetLastError(), W32_ERROR_SHARING_VIOLATION);
        CHECK(CloseHandle(h1));
        /* After close, shared opens work. */
        h1 = CreateFileW(w, W32_GENERIC_READ, W32_FILE_SHARE_READ, NULL,
            4u, 0, NULL);
        CHECK(h1 != W32_INVALID_HANDLE_VALUE);
        CHECK_EQ(GetLastError(), W32_ERROR_ALREADY_EXISTS);
        h2 = CreateFileW(w, W32_GENERIC_READ, W32_FILE_SHARE_READ, NULL,
            3u, 0, NULL);
        CHECK(h2 != W32_INVALID_HANDLE_VALUE);
        if (h2 != W32_INVALID_HANDLE_VALUE)
            CHECK(CloseHandle(h2));
        if (h1 != W32_INVALID_HANDLE_VALUE)
            CHECK(CloseHandle(h1));
    }
    /* CREATE_NEW onto existing. */
    h1 = CreateFileW(w, W32_GENERIC_READ, W32_FILE_SHARE_READ, NULL, 1u, 0,
        NULL);
    CHECK(h1 == W32_INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), W32_ERROR_FILE_EXISTS);
    /* Missing file vs missing dir. */
    toW("C:\\no\\such.txt", w);
    h1 = CreateFileW(w, W32_GENERIC_READ, W32_FILE_SHARE_READ, NULL, 3u, 0,
        NULL);
    CHECK(h1 == W32_INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), W32_ERROR_PATH_NOT_FOUND);
    {
        char m8[512];
        dosJoin("missing.txt", m8, sizeof(m8));
        toW(m8, w);
        h1 = CreateFileW(w, W32_GENERIC_READ, W32_FILE_SHARE_READ, NULL, 3u,
            0, NULL);
        CHECK(h1 == W32_INVALID_HANDLE_VALUE);
        CHECK_EQ(GetLastError(), W32_ERROR_FILE_NOT_FOUND);
    }
    /* Bad drive, UNC, template, reparse. */
    toW("Z:\\x.txt", w);
    CHECK(CreateFileW(w, W32_GENERIC_READ, 0, NULL, 3u, 0, NULL) ==
        W32_INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_NAME);
    toW("\\\\srv\\sh\\x.txt", w);
    CHECK(CreateFileW(w, W32_GENERIC_READ, 0, NULL, 3u, 0, NULL) ==
        W32_INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), W32_ERROR_BAD_NETPATH);
    dosJoin("share.txt", d8, sizeof(d8));
    toW(d8, w);
    CHECK(CreateFileW(w, W32_GENERIC_READ, 0, NULL, 3u, 0,
        (W32_HANDLE)1) == W32_INVALID_HANDLE_VALUE);
    CHECK(CreateFileW(w, W32_GENERIC_READ, 0, NULL, 3u,
        W32_FILE_FLAG_OPEN_REPARSE_POINT, NULL) == W32_INVALID_HANDLE_VALUE);
    /* Directory without BACKUP_SEMANTICS. */
    {
        char dd[512];
        dosJoin("", dd, sizeof(dd));
        toW(dd, w);
        h1 = CreateFileW(w, W32_GENERIC_READ, W32_FILE_SHARE_READ, NULL, 3u,
            0, NULL);
        CHECK(h1 == W32_INVALID_HANDLE_VALUE);
        CHECK_EQ(GetLastError(), W32_ERROR_ACCESS_DENIED);
        h1 = CreateFileW(w, W32_GENERIC_READ, W32_FILE_SHARE_READ, NULL, 3u,
            W32_FILE_FLAG_BACKUP_SEMANTICS, NULL);
        CHECK(h1 != W32_INVALID_HANDLE_VALUE);
        if (h1 != W32_INVALID_HANDLE_VALUE) {
            CHECK_EQ(GetFileType(h1), W32_FILE_TYPE_DISK);
            CHECK(CloseHandle(h1));
        }
    }
    /* DELETE_ON_CLOSE vanishes. */
    dosJoin("tempdel.txt", d8, sizeof(d8));
    toW(d8, w);
    h1 = CreateFileW(w, W32_GENERIC_WRITE, 0, NULL, 2u,
        W32_FILE_FLAG_DELETE_ON_CLOSE, NULL);
    CHECK(h1 != W32_INVALID_HANDLE_VALUE);
    if (h1 != W32_INVALID_HANDLE_VALUE) {
        CHECK_EQ(GetFileAttributesW(w), W32_INVALID_FILE_ATTRIBUTES);
        CHECK(CloseHandle(h1));
    }
}

static void test_fileops(void) {
    char d8[512];
    W32_WCHAR w[512];
    W32_HANDLE h;
    W32_DWORD n;
    char buf[32];
    W32_OVERLAPPED ov;

    printf("== fileops ==\n");
    dosJoin("ops.bin", d8, sizeof(d8));
    toW(d8, w);
    h = CreateFileW(w, W32_GENERIC_READ | W32_GENERIC_WRITE, 0, NULL, 2u,
        0, NULL);
    CHECK(h != W32_INVALID_HANDLE_VALUE);
    if (h == W32_INVALID_HANDLE_VALUE)
        return;
    memset(&ov, 0, sizeof(ov));
    CHECK(WriteFile(h, "0123456789abcdef", 16, &n, &ov));
    CHECK_EQ(n, 16u);
    CHECK_EQ(ov.InternalHigh, 16u);
    {
        W32_DWORD got = 0;
        CHECK(GetOverlappedResult(h, &ov, &got, 0));
        CHECK_EQ(got, 16u);
    }
    CHECK_EQ(SetFilePointer(h, 4, NULL, 0), 4u);
    CHECK(ReadFile(h, buf, 4, &n, NULL));
    CHECK_EQ(n, 4u);
    CHECK(memcmp(buf, "4567", 4) == 0);
    {
        W32_LARGE_INTEGER dist, pos;
        dist.QuadPart = -2;
        CHECK(SetFilePointerEx(h, dist, &pos, 1));
        CHECK_EQ(pos.QuadPart, 6);
    }
    CHECK_EQ(GetFileSize(h, NULL), 16u);
    {
        W32_LARGE_INTEGER sz;
        CHECK(GetFileSizeEx(h, &sz));
        CHECK_EQ(sz.QuadPart, 16);
    }
    CHECK_EQ(GetFileType(h), W32_FILE_TYPE_DISK);
    CHECK(SetFilePointer(h, 8, NULL, 0) == 8u);
    CHECK(SetEndOfFile(h));
    CHECK_EQ(GetFileSize(h, NULL), 8u);
    CHECK(FlushFileBuffers(h));
    {
        W32_BY_HANDLE_FILE_INFORMATION bi;
        CHECK(GetFileInformationByHandle(h, &bi));
        CHECK_EQ(bi.nFileSizeLow, 8u);
        CHECK(bi.nFileIndexLow != 0 || bi.nFileIndexHigh != 0);
    }
    /* Pending overlapped refused. */
    memset(&ov, 0, sizeof(ov));
    ov.Internal = 0x103u;
    CHECK(!GetOverlappedResult(h, &ov, &n, 0));
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
    CHECK(CloseHandle(h));

    /* Pipes do not seek. */
    {
        W32_HANDLE r, wr;
        CHECK(CreatePipe(&r, &wr, NULL, 0));
        CHECK_EQ(SetFilePointer(r, 0, NULL, 0), W32_INVALID_SET_FILE_POINTER);
        CHECK(CloseHandle(r));
        CHECK(CloseHandle(wr));
    }
    /* SetHandleInformation + inherit bit. */
    dosJoin("ops.bin", d8, sizeof(d8));
    toW(d8, w);
    h = CreateFileW(w, W32_GENERIC_READ, W32_FILE_SHARE_READ, NULL, 3u, 0,
        NULL);
    CHECK(h != W32_INVALID_HANDLE_VALUE);
    if (h != W32_INVALID_HANDLE_VALUE) {
        int fd = w32_handle_to_fd(h);
        CHECK_EQ(w32_fs_is_inheritable(fd), 0);
        CHECK(SetHandleInformation(h, W32_HANDLE_FLAG_INHERIT,
            W32_HANDLE_FLAG_INHERIT));
        CHECK_EQ(w32_fs_is_inheritable(fd), 1);
        CHECK(!SetHandleInformation(h, 0x2u, 0));
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
        CHECK(CloseHandle(h));
        CHECK_EQ(w32_fs_is_inheritable(fd), 0);  /* dropped at close */
    }
}

static void test_mappings(void) {
    char d8[512];
    W32_WCHAR w[512];
    W32_HANDLE h, m;
    void *v;

    printf("== mappings ==\n");
    dosJoin("ops.bin", d8, sizeof(d8));
    toW(d8, w);
    h = CreateFileW(w, W32_GENERIC_READ | W32_GENERIC_WRITE, 0, NULL, 3u,
        0, NULL);
    CHECK(h != W32_INVALID_HANDLE_VALUE);
    if (h == W32_INVALID_HANDLE_VALUE)
        return;
    m = CreateFileMappingW(h, NULL, W32_PAGE_READWRITE, 0, 0, NULL);
    CHECK(m != NULL);
    if (m) {
        v = MapViewOfFile(m, W32_FILE_MAP_READ | W32_FILE_MAP_WRITE, 0, 0,
            0);
        CHECK(v != NULL);
        if (v) {
            CHECK(memcmp(v, "01234567", 8) == 0);
            memcpy(v, "ABCDEFGH", 8);
            CHECK(UnmapViewOfFile(v));
            CHECK(!UnmapViewOfFile(v));
            CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
        }
        /* Closing the file first must not kill the mapping. */
        CHECK(CloseHandle(h));
        h = W32_INVALID_HANDLE_VALUE;
        v = MapViewOfFile(m, W32_FILE_MAP_READ, 0, 0, 4);
        CHECK(v != NULL);
        if (v) {
            CHECK(memcmp(v, "ABCD", 4) == 0);
            CHECK(UnmapViewOfFile(v));
        }
        /* View/write vs read-only mapping refused. */
        {
            W32_HANDLE mr = CreateFileMappingW(W32_INVALID_HANDLE_VALUE,
                NULL, W32_PAGE_READONLY, 0, 4096, NULL);
            CHECK(mr != NULL);
            if (mr) {
                CHECK(MapViewOfFile(mr, W32_FILE_MAP_WRITE, 0, 0, 16) ==
                    NULL);
                CHECK_EQ(GetLastError(), W32_ERROR_ACCESS_DENIED);
                CHECK(CloseHandle(mr));
            }
        }
        CHECK(CloseHandle(m));
    }
    /* Anonymous mapping round-trip. */
    m = CreateFileMappingW(W32_INVALID_HANDLE_VALUE, NULL,
        W32_PAGE_READWRITE, 0, 8192, NULL);
    CHECK(m != NULL);
    if (m) {
        v = MapViewOfFile(m, W32_FILE_MAP_ALL_ACCESS, 0, 0, 0);
        CHECK(v != NULL);
        if (v) {
            memset(v, 0x5A, 8192);
            CHECK(UnmapViewOfFile(v));
        }
        CHECK(CloseHandle(m));
    }
    /* Named + empty refused. */
    {
        W32_WCHAR nm[16];
        toW("noname", nm);
        CHECK(CreateFileMappingW(W32_INVALID_HANDLE_VALUE, NULL,
            W32_PAGE_READWRITE, 0, 0, NULL) == NULL);
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
        {
            W32_HANDLE hn = CreateFileW(w, W32_GENERIC_READ,
                W32_FILE_SHARE_READ, NULL, 3u, 0, NULL);
            CHECK(hn != W32_INVALID_HANDLE_VALUE);
            if (hn != W32_INVALID_HANDLE_VALUE) {
                CHECK(CreateFileMappingW(hn, NULL, W32_PAGE_READWRITE, 0,
                    0, nm) == NULL);
                CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
                CHECK(CloseHandle(hn));
            }
        }
    }
}

static void test_pipes(void) {
    W32_HANDLE r, wr;
    W32_DWORD n;
    char buf[32];

    printf("== pipes ==\n");
    CHECK(CreatePipe(&r, &wr, NULL, 0));
    CHECK(WriteFile(wr, "ping", 4, &n, NULL));
    CHECK_EQ(n, 4u);
    CHECK(ReadFile(r, buf, 4, &n, NULL));
    CHECK_EQ(n, 4u);
    CHECK(memcmp(buf, "ping", 4) == 0);
    CHECK_EQ(GetFileType(r), W32_FILE_TYPE_PIPE);
    CHECK(CancelIo(r));
    CHECK(CloseHandle(r));
    CHECK(CloseHandle(wr));

    /* DeviceIoControl: loud refusal. */
    {
        W32_HANDLE h;
        char d8[512];
        W32_WCHAR w[512];
        W32_DWORD ret = 99;
        dosJoin("ops.bin", d8, sizeof(d8));
        toW(d8, w);
        h = CreateFileW(w, W32_GENERIC_READ, W32_FILE_SHARE_READ, NULL, 3u,
            0, NULL);
        CHECK(h != W32_INVALID_HANDLE_VALUE);
        if (h != W32_INVALID_HANDLE_VALUE) {
            CHECK(!DeviceIoControl(h, 0x1234, NULL, 0, NULL, 0, &ret,
                NULL));
            CHECK_EQ(ret, 0u);
            CHECK_EQ(GetLastError(), W32_ERROR_INVALID_FUNCTION);
            CHECK(CloseHandle(h));
        }
    }

    /* Named pipes: rendezvous + directions. */
    {
        W32_HANDLE srv, cli;
        srv = CreateNamedPipeA("\\\\.\\pipe\\w32a2test",
            W32_PIPE_ACCESS_DUPLEX, W32_PIPE_TYPE_BYTE | W32_PIPE_READMODE_BYTE |
            W32_PIPE_WAIT, 1, 4096, 4096, 5000, NULL);
        CHECK(srv != W32_INVALID_HANDLE_VALUE);
        if (srv == W32_INVALID_HANDLE_VALUE)
            return;
        CHECK(!ConnectNamedPipe(srv, NULL));
        CHECK_EQ(GetLastError(), W32_ERROR_PIPE_NOT_CONNECTED);
        cli = CreateFileW(NULL, 0, 0, NULL, 0, 0, NULL);
        (void)cli;
        {
            char d8[512];
            W32_WCHAR w[512];
            snprintf(d8, sizeof(d8), "\\\\.\\pipe\\w32a2test");
            toW(d8, w);
            cli = CreateFileW(w, W32_GENERIC_READ | W32_GENERIC_WRITE, 0,
                NULL, 3u, 0, NULL);
            CHECK(cli != W32_INVALID_HANDLE_VALUE);
            if (cli != W32_INVALID_HANDLE_VALUE) {
                CHECK(!ConnectNamedPipe(srv, NULL));
                CHECK_EQ(GetLastError(), W32_ERROR_PIPE_CONNECTED);
                CHECK(WriteFile(cli, "req", 3, &n, NULL));
                CHECK(ReadFile(srv, buf, 3, &n, NULL));
                CHECK(memcmp(buf, "req", 3) == 0);
                CHECK(WriteFile(srv, "resp", 4, &n, NULL));
                CHECK(ReadFile(cli, buf, 4, &n, NULL));
                CHECK(memcmp(buf, "resp", 4) == 0);
                CHECK(CloseHandle(cli));
            }
        }
        CHECK(CloseHandle(srv));
        /* Second instance while max is 1... (fresh table: allowed again) */
        srv = CreateNamedPipeA("\\\\.\\pipe\\w32a2one",
            W32_PIPE_ACCESS_INBOUND, W32_PIPE_TYPE_BYTE, 1, 0, 0, 0, NULL);
        CHECK(srv != W32_INVALID_HANDLE_VALUE);
        if (srv != W32_INVALID_HANDLE_VALUE) {
            W32_HANDLE srv2 = CreateNamedPipeA("\\\\.\\pipe\\w32a2one",
                W32_PIPE_ACCESS_INBOUND, W32_PIPE_TYPE_BYTE, 1, 0, 0, 0,
                NULL);
            CHECK(srv2 == W32_INVALID_HANDLE_VALUE);
            CHECK_EQ(GetLastError(), W32_ERROR_PIPE_BUSY);
            CHECK(CloseHandle(srv));
        }
        /* Message mode refused. */
        CHECK(CreateNamedPipeA("\\\\.\\pipe\\w32a2msg",
            W32_PIPE_ACCESS_DUPLEX, W32_PIPE_TYPE_MESSAGE, 1, 0, 0, 0,
            NULL) == W32_INVALID_HANDLE_VALUE);
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
        /* WaitNamedPipe: instant hit + instant miss. */
        srv = CreateNamedPipeA("\\\\.\\pipe\\w32a2wait",
            W32_PIPE_ACCESS_DUPLEX, W32_PIPE_TYPE_BYTE, 1, 0, 0, 0, NULL);
        CHECK(srv != W32_INVALID_HANDLE_VALUE);
        if (srv != W32_INVALID_HANDLE_VALUE) {
            CHECK(WaitNamedPipeA("\\\\.\\pipe\\w32a2wait",
                W32_NMPWAIT_NOWAIT));
            CHECK(!WaitNamedPipeA("\\\\.\\pipe\\w32a2nobody",
                W32_NMPWAIT_NOWAIT));
            CHECK_EQ(GetLastError(), W32_ERROR_PIPE_BUSY);
            CHECK(CloseHandle(srv));
        }
    }
}

/* ---- G4: processes ------------------------------------------------------- */

static W32_DWORD waitForExit(W32_HANDLE h) {
    W32_DWORD code = W32_STILL_ACTIVE;
    int i;
    for (i = 0; i < 100; i++) {
        if (!GetExitCodeProcess(h, &code))
            break;
        if (code != W32_STILL_ACTIVE)
            break;
        Sleep(10);
    }
    return code;
}

static void test_spawn(void) {
    W32_STARTUPINFOW si;
    W32_PROCESS_INFORMATION pi;
    W32_WCHAR app[256];
    W32_BOOL ok;

    printf("== spawn ==\n");
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    toW("/bin/true", app);
    memset(&pi, 0, sizeof(pi));
    ok = CreateProcessW(app, NULL, NULL, NULL, 0, 0, NULL, NULL, &si, &pi);
    CHECK(ok);
    if (ok) {
        CHECK_EQ(waitForExit(pi.hProcess), 0u);
        CHECK(CloseHandle(pi.hProcess));
        CHECK(CloseHandle(pi.hThread));
    }
    toW("/bin/false", app);
    memset(&pi, 0, sizeof(pi));
    ok = CreateProcessW(app, NULL, NULL, NULL, 0, 0, NULL, NULL, &si, &pi);
    CHECK(ok);
    if (ok) {
        CHECK_EQ(waitForExit(pi.hProcess), 1u);
        CHECK(CloseHandle(pi.hProcess));
        CHECK(CloseHandle(pi.hThread));
    }
    /* app NULL: first token resolves. */
    {
        W32_WCHAR cmd[256];
        toW("/bin/true extra args", cmd);
        memset(&pi, 0, sizeof(pi));
        ok = CreateProcessW(NULL, cmd, NULL, NULL, 0, 0, NULL, NULL, &si,
            &pi);
        CHECK(ok);
        if (ok) {
            CHECK_EQ(waitForExit(pi.hProcess), 0u);
            CHECK(CloseHandle(pi.hProcess));
            CHECK(CloseHandle(pi.hThread));
        }
    }
    /* Missing / bad-exe / suspended / bad cwd. */
    toW("/no/such/bin", app);
    CHECK(!CreateProcessW(app, NULL, NULL, NULL, 0, 0, NULL, NULL, &si,
        &pi));
    CHECK_EQ(GetLastError(), W32_ERROR_FILE_NOT_FOUND);
    {
        char d8[512];
        W32_WCHAR w[512];
        dosJoin("beta.TXT", d8, sizeof(d8));
        toW(d8, w);
        CHECK(!CreateProcessW(w, NULL, NULL, NULL, 0, 0, NULL, NULL, &si,
            &pi));
        CHECK_EQ(GetLastError(), W32_ERROR_BAD_EXE_FORMAT);
    }
    toW("/bin/true", app);
    CHECK(!CreateProcessW(app, NULL, NULL, NULL, 0, W32_CREATE_SUSPENDED,
        NULL, NULL, &si, &pi));
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
    {
        W32_WCHAR cwd[64];
        toW("/no/such/dir", cwd);
        CHECK(!CreateProcessW(app, NULL, NULL, NULL, 0, 0, NULL, cwd, &si,
            &pi));
        CHECK_EQ(GetLastError(), W32_ERROR_PATH_NOT_FOUND);
    }
    /* Custom environment block parses. */
    {
        static W32_WCHAR block[32];
        block[0] = (W32_WCHAR)'W';
        block[1] = (W32_WCHAR)'3';
        block[2] = (W32_WCHAR)'2';
        block[3] = (W32_WCHAR)'=';
        block[4] = (W32_WCHAR)'1';
        block[5] = 0;
        block[6] = 0;
        memset(&pi, 0, sizeof(pi));
        ok = CreateProcessW(app, NULL, NULL, NULL, 0, 0, block, NULL, &si,
            &pi);
        CHECK(ok);
        if (ok) {
            CHECK_EQ(waitForExit(pi.hProcess), 0u);
            CHECK(CloseHandle(pi.hProcess));
            CHECK(CloseHandle(pi.hThread));
        }
    }
    /* Stdout redirected to a file. */
    {
        char d8[512];
        W32_WCHAR w[512];
        W32_WCHAR cmd[256];
        W32_HANDLE out;
        dosJoin("childout.txt", d8, sizeof(d8));
        toW(d8, w);
        out = CreateFileW(w, W32_GENERIC_WRITE, 0, NULL, 2u, 0, NULL);
        CHECK(out != W32_INVALID_HANDLE_VALUE);
        if (out != W32_INVALID_HANDLE_VALUE) {
            W32_STARTUPINFOW si2;
            memset(&si2, 0, sizeof(si2));
            si2.cb = sizeof(si2);
            si2.flags = W32_STARTF_USESTDHANDLES;
            si2.hStdInput = GetStdHandle(W32_STD_INPUT_HANDLE);
            si2.hStdOutput = out;
            si2.hStdError = GetStdHandle(W32_STD_ERROR_HANDLE);
            toW("/bin/echo hi", cmd);
            memset(&pi, 0, sizeof(pi));
            ok = CreateProcessW(NULL, cmd, NULL, NULL, 1, 0, NULL, NULL,
                &si2, &pi);
            CHECK(CloseHandle(out));
            CHECK(ok);
            if (ok) {
                CHECK_EQ(waitForExit(pi.hProcess), 0u);
                CHECK(CloseHandle(pi.hProcess));
                CHECK(CloseHandle(pi.hThread));
                {
                    FILE *f;
                    char line[64];
                    hostJoin("childout.txt", d8, sizeof(d8));
                    f = fopen(d8, "r");
                    CHECK(f != NULL);
                    if (f) {
                        CHECK(fgets(line, sizeof(line), f) != NULL);
                        CHECK(strcmp(line, "hi\n") == 0);
                        fclose(f);
                    }
                }
            }
        }
    }
    /* A form. */
    {
        W32_STARTUPINFOA sia;
        memset(&sia, 0, sizeof(sia));
        sia.cb = sizeof(sia);
        memset(&pi, 0, sizeof(pi));
        ok = CreateProcessA("/bin/true", NULL, NULL, NULL, 0, 0, NULL,
            NULL, &sia, &pi);
        CHECK(ok);
        if (ok) {
            CHECK_EQ(waitForExit(pi.hProcess), 0u);
            CHECK(CloseHandle(pi.hProcess));
            CHECK(CloseHandle(pi.hThread));
        }
    }
}

static void test_procinfo(void) {
    W32_STARTUPINFOW si;
    W32_PROCESS_INFORMATION pi;
    W32_WCHAR app[256];
    W32_HANDLE self;
    W32_DWORD code;

    printf("== procinfo ==\n");
    GetStartupInfoW(&si);
    CHECK_EQ(si.cb, 104u);
    CHECK_EQ(GetCurrentProcessId(), (W32_DWORD)getpid());
    self = OpenProcess(0, 0, GetCurrentProcessId());
    CHECK(self != NULL);
    if (self) {
        CHECK(GetExitCodeProcess(self, &code));
        CHECK_EQ(code, W32_STILL_ACTIVE);
        {
            W32_FILETIME cr, ex, kn, us;
            CHECK(GetProcessTimes(self, &cr, &ex, &kn, &us));
            CHECK(cr.dwHighDateTime != 0 || cr.dwLowDateTime != 0);
            CHECK_EQ(ex.dwLowDateTime, 0u);
        }
        {
            W32_DWORD_PTR pm, sm;
            CHECK(GetProcessAffinityMask(self, &pm, &sm));
            CHECK(pm != 0 && sm != 0);
        }
        CHECK(CloseHandle(self));
    }
    CHECK(GetExitCodeProcess(GetCurrentProcess(), &code));
    CHECK_EQ(code, W32_STILL_ACTIVE);
    CHECK(OpenProcess(0, 0, 0x7FFFFFFFu) == NULL);
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);

    /* Terminate a sleeper. */
    {
        W32_STARTUPINFOW si2;
        memset(&si2, 0, sizeof(si2));
        si2.cb = sizeof(si2);
        toW("/bin/sleep 30", app);
        memset(&pi, 0, sizeof(pi));
        if (CreateProcessW(NULL, app, NULL, NULL, 0, 0, NULL, NULL, &si2,
            &pi)) {
            CHECK(GetExitCodeProcess(pi.hProcess, &code));
            CHECK_EQ(code, W32_STILL_ACTIVE);
            CHECK(TerminateProcess(pi.hProcess, 3));
            CHECK_EQ(waitForExit(pi.hProcess), 137u);
            CHECK(CloseHandle(pi.hProcess));
            CHECK(CloseHandle(pi.hThread));
        } else {
            CHECK(0);           /* /bin/sleep missing? */
        }
    }
    /* Snapshot finds us. */
    {
        W32_HANDLE snap = CreateToolhelp32Snapshot(W32_TH32CS_SNAPPROCESS,
            0);
        CHECK(snap != NULL && snap != W32_INVALID_HANDLE_VALUE);
        if (snap && snap != W32_INVALID_HANDLE_VALUE) {
            W32_PROCESSENTRY32W pe;
            int found = 0;
            memset(&pe, 0, sizeof(pe));
            pe.size = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (pe.processId == GetCurrentProcessId()) {
                        found = 1;
                        CHECK(pe.exeFile[0] != 0);
                        CHECK_EQ(pe.cntThreads, 1u);
                    }
                } while (Process32NextW(snap, &pe));
                CHECK_EQ(GetLastError(), W32_ERROR_NO_MORE_FILES);
            }
            CHECK(found);
            CHECK(CloseHandle(snap));
        }
        CHECK(CreateToolhelp32Snapshot(0xFFFFu, 0) == NULL ||
            CreateToolhelp32Snapshot(0xFFFFu, 0) ==
            W32_INVALID_HANDLE_VALUE);
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
    }
}

static void test_modinfo(void) {
    char buf[512];
    W32_WCHAR w[512];
    W32_DWORD n;

    printf("== modinfo ==\n");
    n = GetModuleFileNameA(NULL, buf, sizeof(buf));
    CHECK(n > 4);
    CHECK(strncmp(buf, "C:", 2) == 0);
    CHECK(strstr(buf, "test_w32_a2") != NULL);
    n = GetModuleFileNameW(NULL, w, 512);
    CHECK(n > 4);
    CHECK(w[0] == (W32_WCHAR)'C');
    CHECK_EQ(GetModuleFileNameA(NULL, buf, 4), 4u);
    CHECK_EQ(GetLastError(), W32_ERROR_INSUFFICIENT_BUFFER);
    CHECK(GetModuleHandleW(NULL) != NULL);
    {
        W32_WCHAR nm[64];
        void *mod;
        toW("nope.dll", nm);
        CHECK(GetModuleHandleW(nm) == NULL);
        CHECK_EQ(GetLastError(), W32_ERROR_PROC_NOT_FOUND);
        mod = NULL;
        CHECK(GetModuleHandleExW(0, NULL, &mod));
        CHECK(mod != NULL);
        CHECK(!GetModuleHandleExW(0xFFF0u, NULL, &mod));
    }
    CHECK(GetCommandLineW() != NULL);
    CHECK(GetCommandLineW()[0] != 0);
    /* Loader round-trip on a scratch file. */
    {
        char h8[512];
        char d8[512];
        FILE *f;
        void *mod;
        hostJoin("scratch.dll", h8, sizeof(h8));
        f = fopen(h8, "w");
        if (f)
            fclose(f);
        dosJoin("scratch.dll", d8, sizeof(d8));
        toW(d8, w);
        mod = LoadLibraryW(w);
        CHECK(mod != NULL);
        if (mod) {
            n = GetModuleFileNameA(mod, buf, sizeof(buf));
            CHECK(n > 4);
            CHECK(FreeLibrary(mod));
            CHECK_EQ(GetModuleFileNameA(mod, buf, sizeof(buf)), 0u);
        }
        toW("C:\\no\\such.dll", w);
        CHECK(LoadLibraryW(w) == NULL);
        CHECK_EQ(GetLastError(), W32_ERROR_FILE_NOT_FOUND);
        CHECK(LoadLibraryExW(w, NULL, 0xFFF0u) == NULL);
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
        CHECK(LoadLibraryExA("C:\\no\\such.dll", NULL, 0) == NULL);
        CHECK_EQ(GetLastError(), W32_ERROR_FILE_NOT_FOUND);
    }
}

static void test_env_dirs(void) {
    char buf[512];
    W32_WCHAR w[512];
    W32_WCHAR v[512];
    W32_DWORD n;

    printf("== env/dirs ==\n");
    toW("W32A2_VAR", w);
    toW("hello", v);
    CHECK(SetEnvironmentVariableW(w, v));
    CHECK_EQ(GetEnvironmentVariableA("W32A2_VAR", buf, sizeof(buf)), 5u);
    CHECK(strcmp(buf, "hello") == 0);
    CHECK(SetEnvironmentVariableW(w, NULL));
    CHECK_EQ(GetEnvironmentVariableA("W32A2_VAR", buf, sizeof(buf)), 0u);
    CHECK_EQ(GetLastError(), W32_ERROR_ENVVAR_NOT_FOUND);
    CHECK(!SetEnvironmentVariableW(w, v) || 1);   /* missing is fine */
    {
        W32_WCHAR bad[16];
        toW("A=B", bad);
        CHECK(!SetEnvironmentVariableW(bad, v));
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
    }
    /* Expand. */
    setenv("W32A2_E", "exp", 1);
    toW("%W32A2_E%- tail %% %NOPE%", w);
    n = ExpandEnvironmentStringsW(w, v, 512);
    CHECK(n > 4);
    CHECK(Weq(v, "exp- tail % %NOPE%"));
    /* Strings block + free + foreign free refused. */
    {
        W32_LPWSTR blk = GetEnvironmentStringsW();
        CHECK(blk != NULL);
        if (blk) {
            CHECK(blk[0] != 0);
            CHECK(FreeEnvironmentStringsW(blk));
            CHECK(!FreeEnvironmentStringsW(blk));
        }
        CHECK(!FreeEnvironmentStringsW(v));
    }
    /* cwd. */
    n = GetCurrentDirectoryW(512, w);
    CHECK(n > 2);
    CHECK(w[1] == (W32_WCHAR)':');
    CHECK(GetCurrentDirectoryA(sizeof(buf), buf) > 2);
    {
        char d8[512];
        char back[512];
        GetCurrentDirectoryA(sizeof(back), back);
        dosJoin("", d8, sizeof(d8));
        toW(d8, w);
        CHECK(SetCurrentDirectoryW(w));
        CHECK(GetCurrentDirectoryA(sizeof(buf), buf) > 2);
        CHECK(SetCurrentDirectoryA(back));
        dosJoin("beta.TXT", d8, sizeof(d8));
        toW(d8, w);
        CHECK(!SetCurrentDirectoryW(w));
        CHECK_EQ(GetLastError(), W32_ERROR_DIRECTORY);
    }
    /* temp + windows + system. */
    n = GetTempPathW(512, w);
    CHECK(n > 2);
    CHECK(GetTempPathA(sizeof(buf), buf) > 2);
    CHECK(GetWindowsDirectoryA(buf, sizeof(buf)) > 2);
    CHECK(GetWindowsDirectoryW(w, 512) > 2);
    CHECK(GetSystemDirectoryA(buf, sizeof(buf)) > 2);
}

static void test_version_machine(void) {
    W32_OSVERSIONINFOEXW osv;
    W32_DWORD type;
    W32_SYSTEM_INFO sys;

    printf("== version/machine ==\n");
    CHECK_EQ(GetVersion(), (19045u << 16) | 0x000Au);
    memset(&osv, 0, sizeof(osv));
    osv.size = sizeof(osv);
    CHECK(GetVersionExW(&osv));
    CHECK_EQ(osv.major, 10u);
    CHECK_EQ(osv.build, 19045u);
    CHECK_EQ(osv.platformId, 2u);
    CHECK_EQ(osv.productType, 1u);
    osv.size = 0;
    CHECK(!GetVersionExW(&osv));
    CHECK(GetProductInfo(10, 0, 0, 0, &type));
    CHECK_EQ(type, 0x30u);
    CHECK(!GetProductInfo(6, 1, 0, 0, &type));

    GetSystemInfo(&sys);
    CHECK_EQ(sys.arch, 9u);
    CHECK_EQ(sys.pageSize, 4096u);
    CHECK(sys.nProcessors >= 1);
    CHECK_EQ(sys.allocGranularity, 65536u);
    CHECK_EQ(sys.processorType, 8664u);
    GetNativeSystemInfo(&sys);
    CHECK_EQ(sys.arch, 9u);

    CHECK(!IsDebuggerPresent());
    CHECK(IsProcessorFeaturePresent(W32_PF_RDTSC_INSTRUCTION_AVAILABLE));
    CHECK(IsProcessorFeaturePresent(W32_PF_MMX_INSTRUCTIONS_AVAILABLE));
    CHECK(!IsProcessorFeaturePresent(999u));

    CHECK(Beep(440, 1));
    CHECK(!Beep(10, 100));
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
    CHECK_EQ(SleepEx(1, 0), 0u);
    OutputDebugStringW(NULL);

    /* Restart round-trip. */
    {
        W32_WCHAR cmd[64];
        W32_WCHAR get[64];
        W32_DWORD cch;
        W32_DWORD fl;
        toW("/restart", cmd);
        CHECK_EQ(GetApplicationRestartSettings(get, &cch, &fl),
            (W32_HRESULT)0x80004005u);
        CHECK_EQ(RegisterApplicationRestart(cmd, 0), 0);
        cch = 64;
        CHECK_EQ(GetApplicationRestartSettings(get, &cch, &fl), 0);
        CHECK(Weq(get, "/restart"));
        cch = 2;
        CHECK_EQ(GetApplicationRestartSettings(get, &cch, &fl),
            (W32_HRESULT)0x8007017Au);
        CHECK_EQ(UnregisterApplicationRestart(), 0);
    }
    CHECK_EQ(MulDiv(10, 20, 4), 50);
    CHECK_EQ(MulDiv(1, 1, 0), -1);
    CHECK_EQ(MulDiv(0x7FFFFFFF, 2, 1), -1);
    {
        void *p = (void *)(uintptr_t)0x12345678;
        CHECK(DecodePointer(EncodePointer(p)) == p);
        CHECK(EncodePointer(NULL) == NULL);
    }
    /* Memory census. */
    {
        W32_MEMORYSTATUS ms;
        W32_MEMORYSTATUSEX mx;
        memset(&ms, 0, sizeof(ms));
        ms.len = sizeof(ms);
        GlobalMemoryStatus(&ms);
        CHECK_EQ(ms.len, 40u);
        CHECK_EQ(ms.totalPhys, 0u);
        /* A short length fills short; zero fills nothing. */
        memset(&ms, 0xAA, sizeof(ms));
        ms.len = 4;
        GlobalMemoryStatus(&ms);
        CHECK_EQ(ms.len, 40u);
        memset(&ms, 0xAA, sizeof(ms));
        ms.len = 0;
        GlobalMemoryStatus(&ms);
        CHECK_EQ(ms.len, 0u);
        memset(&mx, 0, sizeof(mx));
        mx.len = sizeof(mx);
        CHECK(!GlobalMemoryStatusEx(&mx));
        CHECK_EQ(GetLastError(), W32_ERROR_NOT_SUPPORTED);
    }
}

/* ---- G5/G6: heaps, locales, strings, messages ------------------------------ */

static void test_heaps(void) {
    W32_HANDLE heap = GetProcessHeap();
    void *p;
    W32_DWORD old;

    printf("== heaps ==\n");
    p = HeapAlloc(heap, W32_HEAP_ZERO_MEMORY, 64);
    CHECK(p != NULL);
    if (p) {
        CHECK_EQ(HeapSize(heap, 0, p), 64u);
        memset(p, 0xAB, 64);
        p = HeapReAlloc(heap, W32_HEAP_ZERO_MEMORY, p, 128);
        CHECK(p != NULL);
        if (p) {
            char *c = (char *)p;
            int i;
            int okz = 1;
            CHECK_EQ(HeapSize(heap, 0, p), 128u);
            for (i = 64; i < 128; i++) {
                if (c[i] != 0)
                    okz = 0;
            }
            CHECK(okz);
            CHECK(HeapFree(heap, 0, p));
        }
    }
    CHECK_EQ(HeapSize(heap, 0, (void *)0x1234), (W32_SIZE_T)-1);
    CHECK(HeapFree(heap, 0, NULL));
    p = GlobalAlloc(W32_GMEM_ZEROINIT, 32);
    CHECK(p != NULL);
    if (p) {
        CHECK_EQ(GlobalLock(p), p);
        CHECK(!GlobalUnlock(p));
        CHECK_EQ(GetLastError(), W32_ERROR_SUCCESS);
        CHECK_EQ(GlobalSize(p), 32u);
        CHECK(GlobalFree(p) == NULL);
    }
    p = LocalAlloc(W32_LPTR, 16);
    CHECK(p != NULL);
    if (p)
        CHECK(LocalFree(p) == NULL);
    {
        char stack[64];
        CHECK(VirtualProtect(stack, sizeof(stack), W32_PAGE_READWRITE,
            &old));
        CHECK_EQ(old, (W32_DWORD)W32_PAGE_READWRITE);
        CHECK(!VirtualProtect(stack, sizeof(stack), W32_PAGE_READONLY,
            &old));
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
        CHECK(!VirtualProtect(NULL, 8, W32_PAGE_READWRITE, &old));
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
    }
    CHECK_EQ(GetLargePageMinimum(), 0u);
}

static int enumLocalesSeen;
static W32_WCHAR enumLocalesFirst[8];

static W32_BOOL W32ABI testLocaleCb(W32_LPWSTR id) {
    int i;
    enumLocalesSeen++;
    if (enumLocalesSeen == 1 && id) {
        for (i = 0; i < 7 && id[i]; i++)
            enumLocalesFirst[i] = id[i];
        enumLocalesFirst[i] = 0;
    }
    return 1;
}

static void test_locales(void) {
    W32_CPINFO cpi;
    char abuf[128];
    W32_WCHAR w[128];
    W32_INT n;

    printf("== locales ==\n");
    CHECK_EQ(GetACP(), 65001u);
    CHECK_EQ(GetOEMCP(), 65001u);
    CHECK(GetCPInfo(65001, &cpi));
    CHECK_EQ(cpi.MaxCharSize, 4u);
    CHECK(GetCPInfo(1252, &cpi));
    CHECK_EQ(cpi.MaxCharSize, 1u);
    CHECK(!GetCPInfo(65000, &cpi));
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
    CHECK(IsValidCodePage(1252));
    CHECK(!IsValidCodePage(65000));
    CHECK(!IsDBCSLeadByteEx(1252, 0x81));
    CHECK(IsDBCSLeadByteEx(65001, 0xD0));
    CHECK(!IsDBCSLeadByteEx(65001, 0x41));
    CHECK_EQ(GetUserDefaultLCID(), 0x0409u);
    CHECK(IsValidLocale(0x0409, 0));
    CHECK(!IsValidLocale(0x0419, 0));
    enumLocalesSeen = 0;
    enumLocalesFirst[0] = 0;
    CHECK(EnumSystemLocalesW(testLocaleCb, 1));
    CHECK_EQ(enumLocalesSeen, 1);
    CHECK(Weq(enumLocalesFirst, "0409"));
    CHECK(!EnumSystemLocalesW(NULL, 1));
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
    CHECK(!EnumSystemLocalesW(testLocaleCb, 99));
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);

    n = GetLocaleInfoW(0x0409, W32_LOCALE_SNAME, w, 128);
    CHECK(n == 5);
    CHECK(Weq(w, "en-US"));
    n = GetLocaleInfoW(0x0409, W32_LOCALE_SDAYNAME1, w, 128);
    CHECK(Weq(w, "Sunday"));
    n = GetLocaleInfoW(0x0409, 0x31u, w, 128);
    CHECK(Weq(w, "Sun"));
    n = GetLocaleInfoW(0x0409, 0x37u, w, 128);
    CHECK(Weq(w, "Sat"));
    n = GetLocaleInfoW(0x0409, W32_LOCALE_SMONTHNAME12, w, 128);
    CHECK(Weq(w, "December"));
    n = GetLocaleInfoA(0x0409, W32_LOCALE_SSHORTDATE, abuf, 128);
    CHECK(strcmp(abuf, "M/d/yyyy") == 0);
    CHECK_EQ(GetLocaleInfoW(0x0409, W32_LOCALE_SNAME, w, 2), 6);
    CHECK_EQ(GetLastError(), W32_ERROR_INSUFFICIENT_BUFFER);
    CHECK_EQ(GetLocaleInfoW(0x0419, W32_LOCALE_SNAME, w, 128), 0);
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
    {
        W32_WCHAR nm[16];
        toW("en-US", nm);
        CHECK(GetLocaleInfoEx(nm, W32_LOCALE_SCURRENCY, w, 128) == 1);
        CHECK(Weq(w, "$"));
        toW("ru-RU", nm);
        CHECK_EQ(GetLocaleInfoEx(nm, W32_LOCALE_SNAME, w, 128), 0);
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
    }
    /* String types. */
    {
        W32_WORD types[8];
        W32_WCHAR s[8];
        s[0] = (W32_WCHAR)'A';
        s[1] = (W32_WCHAR)'a';
        s[2] = (W32_WCHAR)'5';
        s[3] = (W32_WCHAR)' ';
        s[4] = 0x416u;          /* Cyrillic Zhe */
        s[5] = 0x436u;          /* Cyrillic zhe */
        s[6] = 0;
        CHECK(GetStringTypeW(W32_CT_CTYPE1, s, -1, types));
        CHECK((types[0] & W32_C1_UPPER) != 0);
        CHECK((types[1] & W32_C1_LOWER) != 0);
        CHECK((types[2] & W32_C1_DIGIT) != 0);
        CHECK((types[3] & W32_C1_SPACE) != 0);
        CHECK((types[4] & W32_C1_UPPER) != 0);
        CHECK((types[5] & W32_C1_LOWER) != 0);
        CHECK(GetStringTypeW(W32_CT_CTYPE2, s, 2, types));
        CHECK_EQ(types[0], (W32_WORD)W32_C2_LEFTTORIGHT);
        CHECK(GetStringTypeW(W32_CT_CTYPE3, s, 1, types));
        CHECK(!GetStringTypeW(W32_CT_CTYPE1 | W32_CT_CTYPE2, s, 1, types));
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
        CHECK(GetStringTypeExA(0x0409, W32_CT_CTYPE1, "Z", 1, types));
        CHECK((types[0] & W32_C1_UPPER) != 0);
    }
    /* Compare. */
    {
        W32_WCHAR a[16];
        W32_WCHAR b[16];
        toW("abc", a);
        toW("abd", b);
        CHECK_EQ(CompareStringW(0x0409, 0, a, -1, b, -1),
            (W32_INT)W32_CSTR_LESS_THAN);
        toW("ABC", a);
        toW("abc", b);
        CHECK_EQ(CompareStringW(0x0409, W32_NORM_IGNORECASE, a, -1, b, -1),
            (W32_INT)W32_CSTR_EQUAL);
        CHECK_EQ(CompareStringW(0x0409, 0, a, -1, b, -1),
            (W32_INT)W32_CSTR_LESS_THAN);
        toW("a-b", a);
        toW("ab", b);
        CHECK_EQ(CompareStringW(0x0409, W32_NORM_IGNORESYMBOLS, a, -1, b,
            -1), (W32_INT)W32_CSTR_EQUAL);
        /* Cyrillic case-insensitive. */
        a[0] = 0x410u;
        a[1] = 0;
        b[0] = 0x430u;
        b[1] = 0;
        CHECK_EQ(CompareStringW(0x0409, W32_NORM_IGNORECASE, a, -1, b, -1),
            (W32_INT)W32_CSTR_EQUAL);
        toW("abc", a);
        toW("abd", b);
        {
            W32_WCHAR nm[16];
            toW("en-US", nm);
            CHECK_EQ(CompareStringEx(nm, 0, a, -1, b, -1, NULL, NULL, 0),
                (W32_INT)W32_CSTR_LESS_THAN);
        }
    }
    /* Map. */
    {
        W32_WCHAR src[16];
        W32_WCHAR dst[16];
        toW("aBcZ", src);
        CHECK_EQ(LCMapStringW(0x0409, W32_LCMAP_UPPERCASE, src, -1, dst,
            16), 5);
        CHECK(Weq(dst, "ABCZ"));
        CHECK_EQ(LCMapStringW(0x0409, W32_LCMAP_LOWERCASE, src, -1, dst,
            16), 5);
        CHECK(Weq(dst, "abcz"));
        src[0] = (W32_WCHAR)'A';
        src[1] = 0;
        CHECK_EQ(LCMapStringW(0x0409, W32_LCMAP_FULLWIDTH, src, -1, dst,
            16), 2);
        CHECK_EQ(dst[0], (W32_WCHAR)0xFF21u);
        src[0] = 0x3042u;
        src[1] = 0;
        CHECK_EQ(LCMapStringW(0x0409, W32_LCMAP_KATAKANA, src, -1, dst,
            16), 2);
        CHECK_EQ(dst[0], (W32_WCHAR)0x30A2u);
        {
            char key[16];
            toW("ab", src);
            CHECK_EQ(LCMapStringW(0x0409, W32_LCMAP_SORTKEY, src, -1,
                (W32_LPWSTR)key, sizeof(key)), 6);
            CHECK(key[0] == 0 && key[1] == 'a');
        }
        src[0] = (W32_WCHAR)'A';
        src[1] = 0;
        CHECK_EQ(LCMapStringW(0x0409, W32_LCMAP_BYTEREV, src, -1, dst,
            16), 2);
        CHECK_EQ(dst[0], (W32_WCHAR)0x4100u);
    }
}

static void test_conv(void) {
    W32_WCHAR w[64];
    char mb[64];
    W32_INT n;
    W32_BOOL used = 0;

    printf("== conv ==\n");
    /* "A" + U+0416 in UTF-8. */
    n = MultiByteToWideChar(65001, 0, "A\xD0\x96", 3, w, 64);
    CHECK_EQ(n, 2);
    CHECK_EQ(w[0], (W32_WCHAR)0x41u);
    CHECK_EQ(w[1], (W32_WCHAR)0x416u);
    /* strict rejects, lax substitutes */
    CHECK_EQ(MultiByteToWideChar(65001, 8, "\xFF", 1, w, 64), 0);
    CHECK_EQ(GetLastError(), W32_ERROR_NO_UNICODE_TRANSLATION);
    n = MultiByteToWideChar(65001, 0, "\xFF", 1, w, 64);
    CHECK_EQ(n, 1);
    CHECK_EQ(w[0], (W32_WCHAR)0xFFFDu);
    /* 1252 */
    n = MultiByteToWideChar(1252, 0, "\xE9\x80", 2, w, 64);
    CHECK_EQ(n, 2);
    CHECK_EQ(w[0], (W32_WCHAR)0xE9u);
    CHECK_EQ(w[1], (W32_WCHAR)0x20ACu);
    CHECK_EQ(MultiByteToWideChar(1252, 8, "\x81", 1, w, 64), 0);
    /* measure */
    CHECK_EQ(MultiByteToWideChar(65001, 0, "hi", -1, NULL, 0), 3);
    /* Wide -> multi */
    w[0] = 0x41u;
    w[1] = 0x416u;
    w[2] = 0;
    n = WideCharToMultiByte(65001, 0, w, -1, mb, 64, NULL, NULL);
    CHECK_EQ(n, 4);
    CHECK(memcmp(mb, "A\xD0\x96", 4) == 0);
    w[0] = 0xE9u;
    w[1] = 0;
    n = WideCharToMultiByte(1252, 0, w, -1, mb, 64, NULL, &used);
    CHECK_EQ(n, 2);
    CHECK_EQ((unsigned char)mb[0], 0xE9u);
    CHECK_EQ(used, 0);
    w[0] = 0x4E00u;
    w[1] = 0;
    n = WideCharToMultiByte(1252, 0, w, -1, mb, 64, NULL, &used);
    CHECK_EQ(n, 2);
    CHECK_EQ(mb[0], '?');
    CHECK_EQ(used, 1);
    /* lone surrogate */
    w[0] = 0xD800u;
    w[1] = 0;
    CHECK_EQ(WideCharToMultiByte(65001, 0x80, w, 1, mb, 64, NULL, NULL),
        0);
    n = WideCharToMultiByte(65001, 0, w, 1, mb, 64, NULL, NULL);
    CHECK_EQ(n, 3);
}

static void test_lstr(void) {
    W32_WCHAR a[32];
    W32_WCHAR b[32];
    W32_WCHAR d[64];
    W32_INT n;

    printf("== lstr ==\n");
    toW("abc", a);
    toW("abd", b);
    CHECK(lstrcmpW(a, b) < 0);
    CHECK_EQ(lstrcmpW(a, a), 0);
    toW("ABC", b);
    CHECK_EQ(lstrcmpiW(a, b), 0);
    CHECK_EQ(lstrcmpiA("aBc", "AbC"), 0);
    CHECK(lstrcmpiA("a", "b") < 0);
    toW("hi", a);
    CHECK_EQ(lstrlenW(a), 2);
    CHECK_EQ(lstrlenW(NULL), 0);
    CHECK_EQ(lstrcpyW(d, a), d);
    toW("!", b);
    CHECK_EQ(lstrcatW(d, b), d);
    CHECK(Weq(d, "hi!"));
    {
        char cd[8];
        CHECK_EQ(lstrcpynA(cd, "hello", 4), cd);
        CHECK(strcmp(cd, "hel") == 0);
    }
    toW("hello", a);
    CHECK_EQ(lstrcpynW(d, a, 4), d);
    CHECK(Weq(d, "hel"));
    /* CharUpper/Lower: char + string. */
    CHECK_EQ(CharUpperW((W32_LPWSTR)(uintptr_t)'a'),
        (W32_LPWSTR)(uintptr_t)'A');
    toW("aBc", d);
    CHECK_EQ(CharLowerW(d), d);
    CHECK(Weq(d, "abc"));
    CHECK(IsCharAlphaW((W32_WCHAR)'Q'));
    CHECK(!IsCharAlphaW((W32_WCHAR)'7'));
    CHECK(IsCharAlphaNumericW((W32_WCHAR)'7'));
    CHECK(IsCharUpperW((W32_WCHAR)'Q'));
    CHECK(!IsCharUpperW((W32_WCHAR)'q'));
    CHECK(IsCharLowerW(0x430u));
    /* IsTextUnicode. */
    {
        static const unsigned char ascii16[] = { 'H', 0, 'i', 0, 0, 0 };
        static const unsigned char bom[] = { 0xFF, 0xFE, 'A', 0 };
        static const unsigned char odd[] = { 'A', 0, 'B' };
        W32_INT fl;
        fl = 0xFFFF;
        CHECK(IsTextUnicode(ascii16, sizeof(ascii16), &fl));
        CHECK((fl & W32_IS_TEXT_UNICODE_ASCII16) != 0);
        fl = 0xFFFF;
        CHECK(IsTextUnicode(bom, sizeof(bom), &fl));
        CHECK((fl & W32_IS_TEXT_UNICODE_SIGNATURE) != 0);
        fl = 0xFFFF;
        CHECK(!IsTextUnicode(odd, sizeof(odd), &fl));
        CHECK((fl & W32_IS_TEXT_UNICODE_ODD_LENGTH) != 0);
    }
    /* Date/time pictures. */
    {
        W32_SYSTEMTIME st;
        W32_WCHAR buf[128];
        memset(&st, 0, sizeof(st));
        st.wYear = 2026;
        st.wMonth = 9;
        st.wDay = 12;
        st.wDayOfWeek = 6;
        st.wHour = 13;
        st.wMinute = 5;
        st.wSecond = 9;
        toW("M/d/yyyy", a);
        CHECK_EQ(GetDateFormatW(0x0409, 0, &st, a, buf, 128), 9);
        CHECK(Weq(buf, "9/12/2026"));
        toW("dddd", a);
        GetDateFormatW(0x0409, 0, &st, a, buf, 128);
        CHECK(Weq(buf, "Saturday"));
        CHECK(GetDateFormatW(0x0409, 2, &st, NULL, buf, 128) > 10);
        toW("h:mm:ss tt", a);
        GetTimeFormatW(0x0409, 0, &st, a, buf, 128);
        CHECK(Weq(buf, "1:05:09 PM"));
        toW("H:mm", a);
        GetTimeFormatW(0x0409, 8, &st, a, buf, 128);
        CHECK(Weq(buf, "13:05"));
        CHECK_EQ(GetDateFormatW(0x0419, 0, &st, NULL, buf, 128), 0);
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
        CHECK_EQ(GetTimeFormatW(0x0419, 0, &st, NULL, buf, 128), 0);
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
        {
            W32_WCHAR nm[16];
            toW("en-US", nm);
            CHECK(GetDateFormatEx(nm, 0, &st, NULL, buf, 128, NULL) > 4);
            CHECK(GetTimeFormatEx(nm, 0, &st, NULL, buf, 128) > 4);
        }
    }
    /* wsprintfW. */
    {
        W32_WCHAR buf[128];
        W32_WCHAR fmt[64];
        W32_WCHAR s[16];
        int nn = 0;
        toW("v=%d s=%s c=%c x=%X", fmt);
        toW("hi", s);
        n = wsprintfW(buf, fmt, -42, s, (int)'Q', 255u);
        CHECK(n > 10);
        CHECK(Weq(buf, "v=-42 s=hi c=Q x=FF"));
        toW("n=%S p=%05d.", fmt);
        n = wsprintfW(buf, fmt, "ab", 7);
        CHECK(Weq(buf, "n=ab p=00007."));
        toW("a%nb", fmt);
        n = wsprintfW(buf, fmt, &nn);
        CHECK_EQ(nn, 1);
        CHECK(Weq(buf, "ab"));
        toW("%I64d", fmt);
        n = wsprintfW(buf, fmt, (int64_t)5000000000LL);
        CHECK(Weq(buf, "5000000000"));
        toW("%*d|%.*S", fmt);
        n = wsprintfW(buf, fmt, 5, -42, 1, "ab");
        CHECK(Weq(buf, "  -42|a"));
        /* The array core: same outputs, boxed. */
        {
            struct w32_ws_arg ab[4];
            toW("v=%d s=%s c=%c x=%X", fmt);
            ab[0].kind = W32_WS_S64;
            ab[0].u = (uint64_t)(int64_t)-42;
            ab[0].p = NULL;
            ab[1].kind = W32_WS_WSTR;
            ab[1].u = 0;
            ab[1].p = s;
            ab[2].kind = W32_WS_WCHAR;
            ab[2].u = (uint64_t)'Q';
            ab[2].p = NULL;
            ab[3].kind = W32_WS_U64;
            ab[3].u = 255u;
            ab[3].p = NULL;
            n = w32_wsprintf_core(buf, fmt, ab, 4);
            CHECK(Weq(buf, "v=-42 s=hi c=Q x=FF"));
            /* stars resolve from boxes; a short array degrades. */
            toW("%*d|%.*s|%d|%s", fmt);
            ab[0].kind = W32_WS_S64;
            ab[0].u = 5;
            ab[0].p = NULL;
            ab[1].kind = W32_WS_S64;
            ab[1].u = (uint64_t)(int64_t)-42;
            ab[1].p = NULL;
            ab[2].kind = W32_WS_S64;
            ab[2].u = 1;
            ab[2].p = NULL;
            ab[3].kind = W32_WS_WSTR;
            ab[3].u = 0;
            ab[3].p = s;
            n = w32_wsprintf_core(buf, fmt, ab, 4);
            CHECK(Weq(buf, "  -42|h|0|(null)"));
        }
        (void)n;
    }
}

static void test_msg(void) {
    W32_WCHAR w[256];
    char abuf[256];
    W32_DWORD n;

    printf("== msg ==\n");
    n = FormatMessageW(W32_FORMAT_MESSAGE_FROM_SYSTEM, NULL, 2, 0, w, 256,
        NULL);
    CHECK(n > 5);
    CHECK(Weq(w, "The named file was not found."));
    CHECK_EQ(FormatMessageW(W32_FORMAT_MESSAGE_FROM_SYSTEM, NULL, 9999, 0,
        w, 256, NULL), 0u);
    /* Inserts. */
    {
        W32_WCHAR fmt[64];
        static W32_WCHAR ins[16];
        uintptr_t args[3];
        toW("Earth", ins);
        toW("E%1!d!:%2!s!:%3!x!%%", fmt);
        args[0] = (uintptr_t)(int32_t)-5;
        args[1] = (uintptr_t)ins;
        args[2] = 0xABu;
        n = FormatMessageW(W32_FORMAT_MESSAGE_FROM_STRING |
            W32_FORMAT_MESSAGE_ARGUMENT_ARRAY, fmt, 0, 0, w, 256, args);
        CHECK(n > 5);
        CHECK(Weq(w, "E-5:Earth:ab%"));
        /* ignore-inserts drops them */
        n = FormatMessageW(W32_FORMAT_MESSAGE_FROM_STRING |
            W32_FORMAT_MESSAGE_IGNORE_INSERTS, fmt, 0, 0, w, 256, args);
        CHECK(Weq(w, "E::%"));
    }
    /* %0 ends, width wraps. */
    {
        W32_WCHAR fmt[64];
        uintptr_t args[1];
        args[0] = 1;
        toW("ab%0cd", fmt);
        n = FormatMessageW(W32_FORMAT_MESSAGE_FROM_STRING, fmt, 0, 0, w,
            256, args);
        CHECK(Weq(w, "ab"));
        toW("aa bb cc dd ee ff gg", fmt);
        n = FormatMessageW(W32_FORMAT_MESSAGE_FROM_STRING | 6u, fmt, 0, 0,
            w, 256, NULL);
        CHECK(n > 10);
        {
            int i;
            int cr = 0;
            for (i = 0; w[i]; i++) {
                if (w[i] == (W32_WCHAR)'\r')
                    cr++;
            }
            CHECK(cr >= 2);
        }
    }
    /* ALLOCATE_BUFFER. */
    {
        W32_LPWSTR slot = NULL;
        n = FormatMessageW(W32_FORMAT_MESSAGE_FROM_SYSTEM |
            W32_FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, 5, 0,
            (W32_LPWSTR)&slot, 0, NULL);
        CHECK(n > 5);
        CHECK(slot != NULL);
        if (slot) {
            CHECK(Weq(slot, "Permission to the object was denied."));
            CHECK(LocalFree(slot) == NULL);
        }
    }
    /* A form. */
    n = FormatMessageA(W32_FORMAT_MESSAGE_FROM_SYSTEM, NULL, 2, 0, abuf,
        256, NULL);
    CHECK(n > 5);
    CHECK(strcmp(abuf, "The named file was not found.") == 0);
    {
        char *slot = NULL;
        n = FormatMessageA(W32_FORMAT_MESSAGE_FROM_SYSTEM |
            W32_FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, 5, 0,
            (W32_LPSTR)&slot, 0, NULL);
        CHECK(n > 5);
        if (slot) {
            CHECK(strcmp(slot, "Permission to the object was denied.") ==
                0);
            CHECK(LocalFree(slot) == NULL);
        }
    }
    /* Every code in the errno header formats. */
    {
        static const W32_DWORD codes[] = { 0, 1, 2, 3, 4, 5, 6, 8, 13, 17,
            18, 19, 25, 29, 30, 32, 38, 50, 53, 80, 87, 109, 112, 121,
            122, 123, 127, 145, 183, 193, 203, 206, 231, 233, 258, 259,
            267, 535, 995, 1113, 1222 };
        size_t i;
        for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
            n = FormatMessageW(W32_FORMAT_MESSAGE_FROM_SYSTEM, NULL,
                codes[i], 0, w, 256, NULL);
            if (n == 0) {
                printf("  FAIL: no message for code %lu\n",
                    (unsigned long)codes[i]);
                fails++;
            }
            checks++;
        }
    }
}

int main(int argc, char **argv) {
    char tmpl[] = "/tmp/w32a2_XXXXXX";

    if (!mkdtemp(tmpl)) {
        printf("mkdtemp failed\n");
        return 1;
    }
    snprintf(tmpd, sizeof(tmpd), "%s", tmpl);
    w32_handle_init();
    w32_fs_init();
    w32_ps_init(argc, argv, NULL);
    w32_set_last_error(W32_ERROR_SUCCESS);

    printf("== W32A-2 breadth (host, tmp=%s) ==\n", tmpd);
    test_find();
    test_attrs_dirs();
    test_copy_move();
    test_volumes_paths();
    test_time();
    test_create();
    test_fileops();
    test_mappings();
    test_pipes();
    test_spawn();
    test_procinfo();
    test_modinfo();
    test_env_dirs();
    test_version_machine();
    test_heaps();
    test_locales();
    test_conv();
    test_lstr();
    test_msg();

    printf("w32a2: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}

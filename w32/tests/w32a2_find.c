/* w32/tests/w32a2_find.c — W32A-2 guest fixture: files and volumes.
 *
 * Groups find/enumerate + attributes/time + the volume/path half of
 * handles/files.  Asserts REAL behaviour and every refusal by name, exits
 * 55/1.  Idempotent: every scratch file is CREATE_ALWAYS and every
 * directory tolerates pre-existence, so a re-run passes too.
 *
 * It also plants the round-trip file the gate reads back through the
 * native shell (C:\tmp\w32a2_rt.txt), and reads the shipping /etc/motd
 * to prove the other direction.
 */

#include "w32a2_common.h"

static int progressCalls;

static DWORD CALLBACK progressCb(LARGE_INTEGER total, LARGE_INTEGER done,
        LARGE_INTEGER streamSize, LARGE_INTEGER streamDone, DWORD streamNo,
        DWORD reason, HANDLE srcFile, HANDLE dstFile, LPVOID data) {
    (void)streamSize; (void)streamDone; (void)streamNo; (void)reason;
    (void)srcFile; (void)dstFile; (void)data;
    if (done.QuadPart <= total.QuadPart)
        progressCalls++;
    return PROGRESS_CONTINUE;
}

static DWORD CALLBACK cancelCb(LARGE_INTEGER total, LARGE_INTEGER done,
        LARGE_INTEGER streamSize, LARGE_INTEGER streamDone, DWORD streamNo,
        DWORD reason, HANDLE srcFile, HANDLE dstFile, LPVOID data) {
    (void)total; (void)done; (void)streamSize; (void)streamDone;
    (void)streamNo; (void)reason; (void)srcFile; (void)dstFile; (void)data;
    return PROGRESS_CANCEL;
}

static void writeWhole(const WCHAR *path, const char *data, DWORD len) {
    HANDLE h;
    DWORD n = 0;
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    CHECKX(h != INVALID_HANDLE_VALUE, "create-scratch");
    if (h == INVALID_HANDLE_VALUE)
        return;
    CHECKX(WriteFile(h, data, len, &n, NULL) && n == len, "write-scratch");
    CloseHandle(h);
}

void __stdcall winstart(void) {
    WCHAR tmp[512], dir[512], sub[512], star[512];
    WCHAR alpha[512], beta[512], sub2[512], kid[512];
    WCHAR copy1[512], copy2[512], moved[512], moved2[512];
    WCHAR replaced[512], repl[512], bak[512], rt[512], motd[512];
    WCHAR buf[512];
    HANDLE fh;
    WIN32_FIND_DATAW fd;
    WIN32_FIND_STREAM_DATA sd;
    WIN32_FILE_ATTRIBUTE_DATA ad;
    DWORD n, a, i;

    w32a2_out = GetStdHandle(STD_OUTPUT_HANDLE);

    /* Scratch: C:\tmp must exist, then a per-fixture directory. */
    n = GetTempPathW(512, tmp);
    CHECKX(n > 2 && n < 500 && tmp[n - 1] == L'\\', "tmp-shape");
    if (n < 3 || n >= 500) {
        w32a2_done("FIND");
        return;
    }
    for (i = 0; i + 1 < n; i++)
        dir[i] = tmp[i];
    dir[n - 1] = 0;
    if (!CreateDirectoryW(dir, NULL))
        CHECKX(GetLastError() == ERROR_ALREADY_EXISTS, "tmp-exists");
    wjoin(tmp, L"w32a2find", sub);
    if (!CreateDirectoryW(sub, NULL))
        CHECKX(GetLastError() == ERROR_ALREADY_EXISTS, "sub-exists");

    wjoin(sub, L"alpha.txt", alpha);
    wjoin(sub, L"beta.TXT", beta);
    writeWhole(alpha, "0123456789", 10);
    writeWhole(beta, "beta-content", 12);

    /* Enumerate. */
    wjoin(sub, L"*", star);
    fh = FindFirstFileW(star, &fd);
    CHECKX(fh != INVALID_HANDLE_VALUE, "find-first");
    if (fh != INVALID_HANDLE_VALUE) {
        int count = 0, seenAlpha = 0, seenBeta = 0;
        do {
            count++;
            if (weq(fd.cFileName, "alpha.txt")) {
                seenAlpha = 1;
                CHECKX(fd.nFileSizeLow == 10 && fd.nFileSizeHigh == 0,
                    "find-size");
                CHECKX((fd.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE) != 0,
                    "find-arch");
            }
            if (weq(fd.cFileName, "beta.TXT"))
                seenBeta = 1;
        } while (FindNextFileW(fh, &fd));
        CHECKX(GetLastError() == ERROR_NO_MORE_FILES, "find-exhausted");
        CHECKX(count >= 2 && seenAlpha && seenBeta, "find-members");
        CHECKX(FindClose(fh), "find-close");
    }

    /* No match, by name. */
    wjoin(sub, L"*.zzz", star);
    fh = FindFirstFileW(star, &fd);
    CHECKX(fh == INVALID_HANDLE_VALUE, "find-nomatch-handle");
    CHECKX(GetLastError() == ERROR_FILE_NOT_FOUND, "find-nomatch-code");

    /* A find handle is not a CloseHandle handle. */
    wjoin(sub, L"*", star);
    fh = FindFirstFileW(star, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        CHECKX(!CloseHandle(fh), "find-closehandle-fails");
        CHECKX(GetLastError() == ERROR_INVALID_HANDLE,
            "find-closehandle-code");
        FindClose(fh);
    } else {
        CHECKX(0, "find-reopen");
    }

    /* Streams: the default stream, then nothing. */
    fh = FindFirstStreamW(alpha, 0, &sd, 0);
    CHECKX(fh != INVALID_HANDLE_VALUE, "stream-first");
    if (fh != INVALID_HANDLE_VALUE) {
        CHECKX(sd.StreamSize.QuadPart == 10, "stream-size");
        CHECKX(weq(sd.cStreamName, "::$DATA"), "stream-name");
        CHECKX(!FindNextStreamW(fh, &sd), "stream-single");
        CHECKX(FindClose(fh), "stream-close");
    }
    fh = FindFirstStreamW(sub, 0, &sd, 0);
    CHECKX(fh == INVALID_HANDLE_VALUE, "stream-dir-handle");
    CHECKX(GetLastError() == ERROR_INVALID_PARAMETER, "stream-dir-code");

    /* Change notifications: a handle that never fires. */
    fh = FindFirstChangeNotificationW(sub, FALSE, 0xFFFu);
    CHECKX(fh != INVALID_HANDLE_VALUE, "change-first");
    if (fh != INVALID_HANDLE_VALUE) {
        CHECKX(FindNextChangeNotification(fh), "change-next");
        CHECKX(!CloseHandle(fh), "change-closehandle-fails");
        CHECKX(GetLastError() == ERROR_INVALID_HANDLE,
            "change-closehandle-code");
        CHECKX(FindCloseChangeNotification(fh), "change-close");
    }

    /* Attributes. */
    a = GetFileAttributesW(alpha);
    CHECKX(a != INVALID_FILE_ATTRIBUTES, "attr-ok");
    CHECKX((a & FILE_ATTRIBUTE_ARCHIVE) != 0, "attr-arch");
    CHECKX((a & FILE_ATTRIBUTE_DIRECTORY) == 0, "attr-nodir");
    CHECKX(GetFileAttributesExW(alpha, GetFileExInfoStandard, &ad),
        "attr-ex");
    CHECKX(ad.nFileSizeLow == 10, "attr-ex-size");
    CHECKX(GetFileAttributesW(L"C:\\no\\such\\file.txt") ==
        INVALID_FILE_ATTRIBUTES, "attr-missing-handle");
    CHECKX(GetLastError() == ERROR_FILE_NOT_FOUND, "attr-missing-code");

    /* Read-only round-trip. */
    CHECKX(SetFileAttributesW(beta, FILE_ATTRIBUTE_READONLY),
        "attr-ro-set");
    a = GetFileAttributesW(beta);
    CHECKX((a & FILE_ATTRIBUTE_READONLY) != 0, "attr-ro-on");
    CHECKX(SetFileAttributesW(beta, FILE_ATTRIBUTE_NORMAL), "attr-ro-clear");
    a = GetFileAttributesW(beta);
    CHECKX((a & FILE_ATTRIBUTE_READONLY) == 0, "attr-ro-off");
    CHECKX(!SetFileAttributesW(beta, 0xDEAD0000u), "attr-badflags");

    /* Directories. */
    wjoin(sub, L"sub2", sub2);
    if (!CreateDirectoryW(sub2, NULL))
        CHECKX(GetLastError() == ERROR_ALREADY_EXISTS, "mkdir-tolerate");
    CHECKX(!CreateDirectoryW(sub2, NULL), "mkdir-existing-fails");
    CHECKX(GetLastError() == ERROR_ALREADY_EXISTS, "mkdir-existing-code");
    a = GetFileAttributesW(sub2);
    CHECKX((a & FILE_ATTRIBUTE_DIRECTORY) != 0, "mkdir-isdir");
    wjoin(sub2, L"kid.txt", kid);
    writeWhole(kid, "k", 1);
    CHECKX(!RemoveDirectoryW(sub2), "rmdir-nonempty-fails");
    CHECKX(GetLastError() == ERROR_DIR_NOT_EMPTY, "rmdir-nonempty-code");
    CHECKX(DeleteFileW(kid), "rmdir-kid-gone");
    CHECKX(RemoveDirectoryW(sub2), "rmdir-ok");

    /* Copy, with progress and cancel. */
    wjoin(sub, L"copy1.txt", copy1);
    wjoin(sub, L"copy2.txt", copy2);
    {
        BOOL cancelFlag = FALSE;
        progressCalls = 0;
        CHECKX(CopyFileExW(alpha, copy1, progressCb, NULL, &cancelFlag, 0),
            "copy-ex");
        CHECKX(progressCalls >= 1, "copy-progress");
    }
    CHECKX(!CopyFileW(alpha, copy1, TRUE), "copy-exists-fails");
    CHECKX(GetLastError() == ERROR_FILE_EXISTS, "copy-exists-code");
    CHECKX(CopyFileW(alpha, copy1, FALSE), "copy-overwrite");
    CHECKX(!CopyFileW(alpha, alpha, FALSE), "copy-self-fails");
    CHECKX(!CopyFileExW(alpha, copy2, cancelCb, NULL, NULL, 0),
        "copy-cancel-fails");
    CHECKX(GetLastError() == ERROR_OPERATION_ABORTED, "copy-cancel-code");
    CHECKX(GetFileAttributesW(copy2) == INVALID_FILE_ATTRIBUTES,
        "copy-cancel-removed");

    /* Move. */
    wjoin(sub, L"moved.txt", moved);
    DeleteFileW(moved);
    CHECKX(MoveFileW(copy1, moved), "move-ok");
    CHECKX(GetFileAttributesW(copy1) == INVALID_FILE_ATTRIBUTES,
        "move-src-gone");
    CHECKX(GetFileAttributesW(moved) != INVALID_FILE_ATTRIBUTES,
        "move-dst-here");
    CHECKX(!MoveFileExW(alpha, moved, 0), "move-noreplace-fails");
    CHECKX(GetLastError() == ERROR_ALREADY_EXISTS, "move-noreplace-code");
    CHECKX(MoveFileExW(alpha, moved, MOVEFILE_REPLACE_EXISTING),
        "move-replace");
    wjoin(sub, L"moved2.txt", moved2);
    DeleteFileW(moved2);
    CHECKX(MoveFileWithProgressW(moved, moved2, NULL, NULL, 0),
        "move-progress");
    CHECKX(GetFileAttributesW(moved) == INVALID_FILE_ATTRIBUTES,
        "move-progress-src-gone");
    CHECKX(!MoveFileWithProgressW(alpha, moved2, NULL, NULL, 0),
        "move-progress-missing-fails");
    CHECKX(GetLastError() == ERROR_FILE_NOT_FOUND,
        "move-progress-missing-code");
    CHECKX(MoveFileWithProgressW(moved2, moved, NULL, NULL, 0),
        "move-progress-back");
    CHECKX(!MoveFileExW(moved, alpha, MOVEFILE_DELAY_UNTIL_REBOOT),
        "move-delay-fails");
    CHECKX(GetLastError() == ERROR_INVALID_PARAMETER, "move-delay-code");
    writeWhole(alpha, "0123456789", 10);

    /* ReplaceFile with backup. */
    wjoin(sub, L"replaced.txt", replaced);
    wjoin(sub, L"repl.txt", repl);
    wjoin(sub, L"bak.txt", bak);
    writeWhole(replaced, "old", 3);
    writeWhole(repl, "new", 3);
    DeleteFileW(bak);
    CHECKX(ReplaceFileW(replaced, repl, bak, 0, NULL, NULL), "replace-ok");
    CHECKX(GetFileAttributesW(bak) != INVALID_FILE_ATTRIBUTES,
        "replace-backup");
    CHECKX(DeleteFileW(bak), "replace-bak-gone");
    CHECKX(DeleteFileW(replaced), "replace-dst-gone");
    CHECKX(GetFileAttributesW(repl) == INVALID_FILE_ATTRIBUTES,
        "replace-src-gone");

    /* Volumes. */
    {
        DWORD spc, bps, fc, tc;
        ULARGE_INTEGER fa, tot, tf;
        WCHAR vol[64], fsn[64];
        DWORD serial, maxc, flags;
        CHECKX(GetDiskFreeSpaceW(NULL, &spc, &bps, &fc, &tc), "vol-free");
        CHECKX(bps == 512, "vol-bps");
        CHECKX(tc > 0, "vol-total");
        CHECKX(GetDiskFreeSpaceExW(NULL, &fa, &tot, &tf), "vol-ex");
        CHECKX(tot.QuadPart > 0, "vol-ex-total");
        CHECKX(GetDriveTypeW(L"C:\\") == DRIVE_FIXED, "vol-drivetype");
        CHECKX(GetDriveTypeW(NULL) == DRIVE_FIXED, "vol-drivetype-null");
        CHECKX(GetDriveTypeW(L"Z:\\") == DRIVE_NO_ROOT_DIR,
            "vol-drivetype-bad");
        CHECKX(GetLogicalDriveStringsW(0, NULL) == 4, "vol-drives-measure");
        CHECKX(GetLogicalDriveStringsW(64, buf) == 4, "vol-drives");
        CHECKX(buf[0] == L'C' && buf[3] == 0, "vol-drives-shape");
        CHECKX(GetVolumeInformationW(L"C:\\", vol, 64, &serial, &maxc,
            &flags, fsn, 64), "vol-info");
        CHECKX(weq(vol, "AURALITE"), "vol-name");
        CHECKX(weq(fsn, "AURALFS"), "vol-fs");
        CHECKX(maxc == 255, "vol-maxc");
        CHECKX(!GetVolumeInformationW(L"Z:\\", NULL, 0, NULL, NULL, NULL,
            NULL, 0), "vol-info-bad");
    }

    /* Paths. */
    {
        LPWSTR part = NULL;
        wjoin(sub, L"sub2\\..\\alpha.txt", star);
        n = GetFullPathNameW(star, 0, NULL, NULL);
        CHECKX(n > 10, "path-measure");
        n = GetFullPathNameW(star, 512, buf, &part);
        CHECKX(n > 10 && buf[0] == L'C' && buf[1] == L':', "path-full");
        CHECKX(part != NULL && weq(part, "alpha.txt"), "path-part");
        n = GetFullPathNameW(star, 4, buf, NULL);
        CHECKX(n > 4, "path-short");
        n = GetLongPathNameW(beta, buf, 512);
        CHECKX(n > 5, "path-long");
        CHECKX(GetLongPathNameW(L"C:\\no\\such.txt", buf, 512) == 0,
            "path-long-missing");
    }
    {
        HANDLE h;
        DWORD hi = 0;
        h = CreateFileW(beta, GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, 0, NULL);
        CHECKX(h != INVALID_HANDLE_VALUE, "path-final-open");
        if (h != INVALID_HANDLE_VALUE) {
            n = GetFinalPathNameByHandleW(h, buf, 512, 0);
            CHECKX(n > 8 && buf[0] == L'\\' && buf[1] == L'\\',
                "path-final");
            CHECKX(CloseHandle(h), "path-final-close");
        }
        n = GetCompressedFileSizeW(beta, &hi);
        CHECKX(n != INVALID_FILE_SIZE || hi != 0, "path-compressed");
    }

    /* The shipping motd proves shell -> fixture. */
    {
        HANDLE h;
        char mbuf[64];
        DWORD got = 0;
        wjoin(L"C:\\etc", L"motd", motd);
        h = CreateFileW(motd, GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, 0, NULL);
        CHECKX(h != INVALID_HANDLE_VALUE, "motd-open");
        if (h != INVALID_HANDLE_VALUE) {
            int ok;
            ReadFile(h, mbuf, sizeof(mbuf) - 1, &got, NULL);
            mbuf[got < sizeof(mbuf) - 1 ? got : sizeof(mbuf) - 1] = 0;
            ok = got >= 10 && memcmp(mbuf, "AuraLite OS", 10) == 0;
            CHECKX(ok, "motd-content");
            if (ok)
                say("MOTD-OK\r\n");
            CloseHandle(h);
        }
    }

    /* The round-trip file proves fixture -> shell; the gate cats it. */
    wjoin(tmp, L"w32a2_rt.txt", rt);
    writeWhole(rt, "W32A2-ROUNDTRIP-OK\n", 19);
    say("ROUNDTRIP-WROTE\r\n");

    /* Best-effort cleanup (the round-trip file stays). */
    DeleteFileW(alpha);
    DeleteFileW(beta);
    DeleteFileW(copy1);
    DeleteFileW(copy2);
    DeleteFileW(moved);
    DeleteFileW(moved2);
    DeleteFileW(replaced);
    DeleteFileW(repl);
    DeleteFileW(bak);
    RemoveDirectoryW(sub);

    w32a2_done("FIND");
}

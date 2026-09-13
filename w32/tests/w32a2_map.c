/* w32/tests/w32a2_map.c — W32A-2 guest fixture: file ops and mappings.
 *
 * The create/seek/mapping half of handles/files: CreateFile breadth,
 * pointers, truncation, completed overlapped I/O (true async is
 * FAIL-CLEAN), handle inheritance, file mappings (shared, anonymous,
 * refused shapes), and the DeviceIoControl named refusal.  Exits 55/1.
 */

#include "w32a2_common.h"

void __stdcall winstart(void) {
    WCHAR tmp[512], file[512];
    HANDLE h, h2, m, mr;
    void *v;
    DWORD n;
    OVERLAPPED ov;
    char buf[32];
    LARGE_INTEGER dist, pos, sz;

    w32a2_out = GetStdHandle(STD_OUTPUT_HANDLE);

    n = GetTempPathW(512, tmp);
    CHECKX(n > 2 && n < 500, "map-tmp");
    if (n < 3 || n >= 500) {
        w32a2_done("MAP");
        return;
    }
    wjoin(tmp, L"w32a2_ops.bin", file);
    DeleteFileW(file);

    /* Create breadth: sharing, dispositions, bad names. */
    h = CreateFileW(file, GENERIC_READ | GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, 0, NULL);
    CHECKX(h != INVALID_HANDLE_VALUE, "map-create");
    if (h == INVALID_HANDLE_VALUE) {
        w32a2_done("MAP");
        return;
    }
    h2 = CreateFileW(file, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, 0, NULL);
    CHECKX(h2 == INVALID_HANDLE_VALUE, "map-share-handle");
    CHECKX(GetLastError() == ERROR_SHARING_VIOLATION, "map-share-code");
    CHECKX(CloseHandle(h), "map-close1");
    h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL,
        TRUNCATE_EXISTING, 0, NULL);
    CHECKX(h != INVALID_HANDLE_VALUE, "map-truncate");
    CHECKX(GetLastError() == ERROR_ALREADY_EXISTS, "map-truncate-code");
    CHECKX(CloseHandle(h), "map-close2");
    h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, CREATE_NEW,
        0, NULL);
    CHECKX(h == INVALID_HANDLE_VALUE, "map-createnew-handle");
    CHECKX(GetLastError() == ERROR_FILE_EXISTS, "map-createnew-code");
    h = CreateFileW(L"C:\\no\\such.txt", GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, 0, NULL);
    CHECKX(h == INVALID_HANDLE_VALUE, "map-nodir-handle");
    CHECKX(GetLastError() == ERROR_PATH_NOT_FOUND, "map-nodir-code");
    wjoin(tmp, L"w32a2_noleaf.txt", file);
    DeleteFileW(file);
    h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, 0, NULL);
    CHECKX(h == INVALID_HANDLE_VALUE, "map-noleaf-handle");
    CHECKX(GetLastError() == ERROR_FILE_NOT_FOUND, "map-noleaf-code");
    h = CreateFileW(L"Z:\\x.txt", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0,
        NULL);
    CHECKX(h == INVALID_HANDLE_VALUE, "map-baddrive-handle");
    CHECKX(GetLastError() == ERROR_INVALID_NAME, "map-baddrive-code");
    h = CreateFileW(L"\\\\srv\\sh\\x.txt", GENERIC_READ, 0, NULL,
        OPEN_EXISTING, 0, NULL);
    CHECKX(h == INVALID_HANDLE_VALUE, "map-unc-handle");
    CHECKX(GetLastError() == ERROR_BAD_NETPATH, "map-unc-code");
    h = CreateFileW(tmp, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, 0, NULL);
    CHECKX(h == INVALID_HANDLE_VALUE, "map-diropen-handle");
    CHECKX(GetLastError() == ERROR_ACCESS_DENIED, "map-diropen-code");

    /* Pointers, sizes, truncation. */
    wjoin(tmp, L"w32a2_ops.bin", file);
    h = CreateFileW(file, GENERIC_READ | GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, 0, NULL);
    CHECKX(h != INVALID_HANDLE_VALUE, "map-reopen");
    if (h == INVALID_HANDLE_VALUE) {
        w32a2_done("MAP");
        return;
    }
    memset(&ov, 0, sizeof(ov));
    CHECKX(WriteFile(h, "0123456789abcdef", 16, &n, &ov), "map-write");
    CHECKX(n == 16, "map-writelen");
    CHECKX(ov.InternalHigh == 16, "map-ov-high");
    {
        DWORD got = 0;
        CHECKX(GetOverlappedResult(h, &ov, &got, FALSE), "map-ov-result");
        CHECKX(got == 16, "map-ov-got");
    }
    CHECKX(SetFilePointer(h, 4, NULL, FILE_BEGIN) == 4, "map-seek");
    CHECKX(ReadFile(h, buf, 4, &n, NULL) && n == 4, "map-read");
    CHECKX(memcmp(buf, "4567", 4) == 0, "map-read-data");
    dist.QuadPart = -2;
    CHECKX(SetFilePointerEx(h, dist, &pos, FILE_CURRENT), "map-seekex");
    CHECKX(pos.QuadPart == 6, "map-seekex-pos");
    CHECKX(GetFileSize(h, NULL) == 16, "map-size");
    CHECKX(GetFileSizeEx(h, &sz) && sz.QuadPart == 16, "map-sizeex");
    CHECKX(GetFileType(h) == FILE_TYPE_DISK, "map-type");
    CHECKX(SetFilePointer(h, 8, NULL, FILE_BEGIN) == 8, "map-seek8");
    CHECKX(SetEndOfFile(h), "map-trunc");
    CHECKX(GetFileSize(h, NULL) == 8, "map-size8");
    CHECKX(FlushFileBuffers(h), "map-flush");
    {
        BY_HANDLE_FILE_INFORMATION bi;
        CHECKX(GetFileInformationByHandle(h, &bi), "map-byhandle");
        CHECKX(bi.nFileSizeLow == 8, "map-byhandle-size");
        CHECKX(bi.nFileIndexLow != 0 || bi.nFileIndexHigh != 0,
            "map-byhandle-index");
    }
    /* Pending overlapped is refused, never waited on. */
    memset(&ov, 0, sizeof(ov));
    ov.Internal = 0x103u;
    CHECKX(!GetOverlappedResult(h, &ov, &n, FALSE), "map-ov-pending-fails");
    CHECKX(GetLastError() == ERROR_INVALID_PARAMETER,
        "map-ov-pending-code");
    CHECKX(CloseHandle(h), "map-close3");

    /* Pipes do not seek. */
    {
        HANDLE r, wr;
        CHECKX(CreatePipe(&r, &wr, NULL, 0), "map-pipe");
        CHECKX(SetFilePointer(r, 0, NULL, FILE_BEGIN) ==
            INVALID_SET_FILE_POINTER, "map-pipeseek");
        CloseHandle(r);
        CloseHandle(wr);
    }

    /* Inheritance bit. */
    h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, 0, NULL);
    CHECKX(h != INVALID_HANDLE_VALUE, "map-inherit-open");
    if (h != INVALID_HANDLE_VALUE) {
        CHECKX(SetHandleInformation(h, HANDLE_FLAG_INHERIT,
            HANDLE_FLAG_INHERIT), "map-inherit-set");
        CHECKX(!SetHandleInformation(h, 0x2u, 0), "map-inherit-badmask");
        CHECKX(GetLastError() == ERROR_INVALID_PARAMETER,
            "map-inherit-code");
        CHECKX(CloseHandle(h), "map-inherit-close");
    }

    /* Mappings: shared file views persist. */
    h = CreateFileW(file, GENERIC_READ | GENERIC_WRITE, 0, NULL,
        OPEN_EXISTING, 0, NULL);
    CHECKX(h != INVALID_HANDLE_VALUE, "map-mapopen");
    if (h == INVALID_HANDLE_VALUE) {
        w32a2_done("MAP");
        return;
    }
    m = CreateFileMappingW(h, NULL, PAGE_READWRITE, 0, 0, NULL);
    CHECKX(m != NULL, "map-create");
    if (m != NULL) {
        v = MapViewOfFile(m, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
        CHECKX(v != NULL, "map-view");
        if (v != NULL) {
            CHECKX(memcmp(v, "01234567", 8) == 0, "map-content");
            memcpy(v, "ABCDEFGH", 8);
            CHECKX(UnmapViewOfFile(v), "map-unmap");
            CHECKX(!UnmapViewOfFile(v), "map-doubleunmap-fails");
            CHECKX(GetLastError() == ERROR_INVALID_PARAMETER,
                "map-doubleunmap-code");
        }
        /* Closing the file first must not kill the mapping. */
        CHECKX(CloseHandle(h), "map-fileclose-first");
        h = INVALID_HANDLE_VALUE;
        v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 4);
        CHECKX(v != NULL, "map-view2");
        if (v != NULL) {
            CHECKX(memcmp(v, "ABCD", 4) == 0, "map-persist");
            CHECKX(UnmapViewOfFile(v), "map-unmap2");
        }
        CHECKX(CloseHandle(m), "map-close");
    }

    /* A WRITE view of a READONLY mapping is refused by name. */
    mr = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READONLY, 0,
        4096, NULL);
    CHECKX(mr != NULL, "map-pagefile-ro");
    if (mr != NULL) {
        CHECKX(MapViewOfFile(mr, FILE_MAP_WRITE, 0, 0, 16) == NULL,
            "map-ro-write-fails");
        CHECKX(GetLastError() == ERROR_ACCESS_DENIED, "map-ro-write-code");
        CHECKX(CloseHandle(mr), "map-ro-close");
    }

    /* Anonymous mapping round-trip. */
    m = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
        8192, NULL);
    CHECKX(m != NULL, "map-anon");
    if (m != NULL) {
        DWORD i, okz = 1;
        v = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        CHECKX(v != NULL, "map-anon-view");
        if (v != NULL) {
            volatile char *c = (volatile char *)v;
            for (i = 0; i < 8192; i++)
                c[i] = (char)0x5A;
            for (i = 0; i < 8192; i++) {
                if (c[i] != (char)0x5A)
                    okz = 0;
            }
            CHECKX(okz, "map-anon-data");
            CHECKX(UnmapViewOfFile(v), "map-anon-unmap");
        }
        CHECKX(CloseHandle(m), "map-anon-close");
    }

    /* Empty and named mappings are refused. */
    CHECKX(CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, 0, NULL) == NULL, "map-zero-fails");
    CHECKX(GetLastError() == ERROR_INVALID_PARAMETER, "map-zero-code");
    h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        CHECKX(CreateFileMappingW(h, NULL, PAGE_READWRITE, 0, 0,
            L"noname") == NULL, "map-named-fails");
        CHECKX(GetLastError() == ERROR_INVALID_PARAMETER,
            "map-named-code");
        CloseHandle(h);
    } else {
        CHECKX(0, "map-named-open");
    }

    /* DeviceIoControl: loud refusal, nothing half-done. */
    h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD ret = 99;
        CHECKX(!DeviceIoControl(h, 0x1234, NULL, 0, NULL, 0, &ret, NULL),
            "map-ioctl-fails");
        CHECKX(ret == 0, "map-ioctl-ret");
        CHECKX(GetLastError() == ERROR_INVALID_FUNCTION, "map-ioctl-code");
        CloseHandle(h);
    } else {
        CHECKX(0, "map-ioctl-open");
    }
    DeleteFileW(file);

    w32a2_done("MAP");
}

/* kernel32.h — the bounded first import set.  WIN32_PLAN.md phase W32-4.
 *
 * Per decision D7, this is not "implement KERNEL32".  It is exactly the
 * functions the phase's own gate binaries import, discovered by dumping their
 * import tables, and it grows only when a new gate binary needs something.
 * That keeps the surface auditable and stops the phase becoming endless.
 *
 * Every export is W32ABI.  A missing annotation is silent -- see
 * tests/unit/test_w32_abi.c and its negative control.
 */

#ifndef AURALITE_W32_KERNEL32_H
#define AURALITE_W32_KERNEL32_H

#include "w32/w32_abi.h"
#include "w32/w32_handle.h"
#include "w32/w32_errno.h"

/* --- process ------------------------------------------------------------- */
W32ABI void      ExitProcess(unsigned int exit_code) __attribute__((noreturn));
W32ABI W32_DWORD GetLastError(void);
W32ABI void      SetLastError(W32_DWORD code);

/* --- handles and I/O ----------------------------------------------------- */
W32ABI W32_HANDLE GetStdHandle(W32_DWORD which);
W32ABI W32_BOOL   CloseHandle(W32_HANDLE h);
W32ABI W32_BOOL   WriteFile(W32_HANDLE h, const void *buf, W32_DWORD len,
                            W32_DWORD *written, void *overlapped);
W32ABI W32_BOOL   ReadFile(W32_HANDLE h, void *buf, W32_DWORD len,
                           W32_DWORD *got, void *overlapped);

/* Desired-access and creation-disposition values used by CreateFileA. */
#define W32_GENERIC_READ   0x80000000u
#define W32_GENERIC_WRITE  0x40000000u
#define W32_CREATE_NEW        1u
#define W32_CREATE_ALWAYS     2u
#define W32_OPEN_EXISTING     3u
#define W32_OPEN_ALWAYS       4u
#define W32_TRUNCATE_EXISTING 5u

W32ABI W32_HANDLE CreateFileA(const char *path, W32_DWORD access,
                              W32_DWORD share, void *sa,
                              W32_DWORD disposition, W32_DWORD flags,
                              W32_HANDLE tmpl);

/* --- memory --------------------------------------------------------------- */
#define W32_MEM_COMMIT   0x1000u
#define W32_MEM_RESERVE  0x2000u
#define W32_MEM_RELEASE  0x8000u
#define W32_PAGE_READWRITE 0x04u

W32ABI void    *VirtualAlloc(void *addr, unsigned long long size,
                             W32_DWORD type, W32_DWORD protect);
W32ABI W32_BOOL VirtualFree(void *addr, unsigned long long size, W32_DWORD type);

/* A minimal process heap.  HeapAlloc/HeapFree are what a CRT actually calls;
 * GetProcessHeap returns an opaque token this implementation does not need to
 * distinguish, because there is only one heap. */
W32ABI W32_HANDLE GetProcessHeap(void);
W32ABI void      *HeapAlloc(W32_HANDLE heap, W32_DWORD flags,
                            unsigned long long size);
W32ABI W32_BOOL   HeapFree(W32_HANDLE heap, W32_DWORD flags, void *mem);

/* --- time ----------------------------------------------------------------- */
W32ABI void          Sleep(W32_DWORD ms);
W32ABI W32_ULONGLONG GetTickCount64(void);

/* --- command line --------------------------------------------------------- */
W32ABI const char *GetCommandLineA(void);


/* --- W32A-2: files -----------------------------------------------------------
 *
 * Breadth phase: every declaration below is implemented (no stubs in this
 * file's companions).  Layouts are the documented Win32 ones; the static
 * asserts pin the sizes so a padding slip fails the build instead of
 * corrupting a caller's stack frame.  mingw's windows.h agrees with these
 * sizes -- the guest fixtures prove it by running.
 */

typedef char           W32_CHAR;
typedef uint16_t       W32_WCHAR;
typedef W32_CHAR      *W32_LPSTR;
typedef const W32_CHAR *W32_LPCSTR;
typedef W32_WCHAR     *W32_LPWSTR;
typedef const W32_WCHAR *W32_LPCWSTR;
typedef uint32_t W32_LCID;
typedef uint16_t W32_LANGID;
typedef int32_t  W32_LONG;
typedef uint32_t W32_UINT;
typedef int      W32_INT;
typedef uint64_t W32_SIZE_T;
typedef uint64_t W32_DWORD_PTR;
typedef int64_t  W32_LONG_PTR;
typedef int32_t  W32_HRESULT;

#define W32_MAX_PATH 260u

/* File attributes. */
#define W32_FILE_ATTRIBUTE_READONLY   0x00000001u
#define W32_FILE_ATTRIBUTE_HIDDEN     0x00000002u
#define W32_FILE_ATTRIBUTE_SYSTEM     0x00000004u
#define W32_FILE_ATTRIBUTE_DIRECTORY  0x00000010u
#define W32_FILE_ATTRIBUTE_ARCHIVE    0x00000020u
#define W32_FILE_ATTRIBUTE_NORMAL     0x00000080u
#define W32_FILE_ATTRIBUTE_TEMPORARY  0x00000100u

/* Desired access beyond W32_GENERIC_READ/WRITE. */
#define W32_GENERIC_EXECUTE 0x20000000u
#define W32_GENERIC_ALL     0x10000000u

/* Share modes, honoured between w32 handles. */
#define W32_FILE_SHARE_READ   0x00000001u
#define W32_FILE_SHARE_WRITE  0x00000002u
#define W32_FILE_SHARE_DELETE 0x00000004u

/* CreateFile flags. */
#define W32_FILE_FLAG_WRITE_THROUGH    0x80000000u
#define W32_FILE_FLAG_OVERLAPPED       0x40000000u
#define W32_FILE_FLAG_NO_BUFFERING     0x20000000u
#define W32_FILE_FLAG_RANDOM_ACCESS    0x10000000u
#define W32_FILE_FLAG_SEQUENTIAL_SCAN  0x08000000u
#define W32_FILE_FLAG_DELETE_ON_CLOSE  0x04000000u
#define W32_FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define W32_FILE_FLAG_POSIX_SEMANTICS  0x01000000u
#define W32_FILE_FLAG_OPEN_REPARSE_POINT 0x00200000u
#define W32_FILE_FLAG_OPEN_NO_RECALL   0x00100000u
#define W32_FILE_FLAG_FIRST_PIPE_INSTANCE 0x00080000u

#define W32_INVALID_FILE_ATTRIBUTES  ((W32_DWORD)-1)
#define W32_INVALID_SET_FILE_POINTER ((W32_DWORD)-1)
#define W32_INVALID_FILE_SIZE        ((W32_DWORD)-1)

/* GetFileType. */
#define W32_FILE_TYPE_UNKNOWN 0u
#define W32_FILE_TYPE_DISK    1u
#define W32_FILE_TYPE_CHAR    2u
#define W32_FILE_TYPE_PIPE    3u

/* MoveFile flags.  DELAY_UNTIL_REBOOT has no meaning without a boot-time
 * mover and is refused; the rest are honoured. */
#define W32_MOVEFILE_REPLACE_EXISTING  0x00000001u
#define W32_MOVEFILE_COPY_ALLOWED      0x00000002u
#define W32_MOVEFILE_DELAY_UNTIL_REBOOT 0x00000004u
#define W32_MOVEFILE_WRITE_THROUGH     0x00000008u
#define W32_MOVEFILE_CREATE_HARDLINK   0x00000010u
#define W32_MOVEFILE_FAIL_IF_NOT_TRACKABLE 0x00000020u

/* ReplaceFile flags. */
#define W32_REPLACEFILE_WRITE_THROUGH       0x00000001u
#define W32_REPLACEFILE_IGNORE_MERGE_ERRORS 0x00000002u
#define W32_REPLACEFILE_IGNORE_ACL_ERRORS   0x00000004u

/* FindFirstFileEx levels, ops and flags. */
#define W32_FIND_EX_INFO_STANDARD 0u
#define W32_FIND_EX_INFO_BASIC    1u
#define W32_FIND_EX_SEARCH_NAME_MATCH 0u
#define W32_FIND_EX_SEARCH_LIMIT_TO_DIRECTORIES 1u
#define W32_FIND_FIRST_EX_CASE_SENSITIVE 0x00000001u
#define W32_FIND_FIRST_EX_LARGE_FETCH    0x00000002u

/* Stream info level. */
#define W32_FIND_STREAM_INFO_STANDARD 0u

/* Change-notification filters. */
#define W32_FILE_NOTIFY_CHANGE_FILE_NAME    0x00000001u
#define W32_FILE_NOTIFY_CHANGE_DIR_NAME     0x00000002u
#define W32_FILE_NOTIFY_CHANGE_ATTRIBUTES   0x00000004u
#define W32_FILE_NOTIFY_CHANGE_SIZE         0x00000008u
#define W32_FILE_NOTIFY_CHANGE_LAST_WRITE   0x00000010u
#define W32_FILE_NOTIFY_CHANGE_LAST_ACCESS  0x00000020u
#define W32_FILE_NOTIFY_CHANGE_CREATION     0x00000040u
#define W32_FILE_NOTIFY_CHANGE_SECURITY     0x00000100u

/* Drive types. */
#define W32_DRIVE_UNKNOWN     0u
#define W32_DRIVE_NO_ROOT_DIR 1u
#define W32_DRIVE_REMOVABLE   2u
#define W32_DRIVE_FIXED       3u
#define W32_DRIVE_REMOTE      4u
#define W32_DRIVE_CDROM       5u
#define W32_DRIVE_RAMDISK     6u

/* SetHandleInformation. */
#define W32_HANDLE_FLAG_INHERIT 0x00000001u

/* File mapping page protection and access. */
#define W32_PAGE_NOACCESS          0x01u
#define W32_PAGE_READONLY          0x02u
#define W32_PAGE_READWRITE         0x04u
#define W32_PAGE_WRITECOPY         0x08u
#define W32_PAGE_EXECUTE_READ      0x20u
#define W32_PAGE_EXECUTE_READWRITE 0x40u
#define W32_SEC_COMMIT             0x08000000u
#define W32_SEC_RESERVE            0x04000000u
#define W32_FILE_MAP_READ          0x0004u
#define W32_FILE_MAP_WRITE         0x0002u
#define W32_FILE_MAP_ALL_ACCESS    0x001fu
#define W32_FILE_MAP_COPY          0x0001u
#define W32_FILE_MAP_EXECUTE       0x0020u

/* Named-pipe modes. */
#define W32_PIPE_ACCESS_INBOUND  0x00000001u
#define W32_PIPE_ACCESS_OUTBOUND 0x00000002u
#define W32_PIPE_ACCESS_DUPLEX   0x00000003u
#define W32_PIPE_TYPE_BYTE       0x00000000u
#define W32_PIPE_TYPE_MESSAGE    0x00000004u
#define W32_PIPE_READMODE_BYTE   0x00000000u
#define W32_PIPE_READMODE_MESSAGE 0x00000002u
#define W32_PIPE_WAIT            0x00000000u
#define W32_PIPE_NOWAIT          0x00000001u
#define W32_PIPE_UNLIMITED_INSTANCES 255u
#define W32_NMPWAIT_USE_DEFAULT_WAIT 0x00000000u
#define W32_NMPWAIT_NOWAIT           0x00000001u
#define W32_NMPWAIT_WAIT_FOREVER     0xFFFFFFFFu

typedef struct {
    W32_DWORD dwLowDateTime;
    W32_DWORD dwHighDateTime;
} W32_FILETIME;

typedef struct {
    W32_DWORD LowPart;
    W32_LONG  HighPart;
} W32_LARGE_INTEGER_DW;
typedef union {
    W32_LARGE_INTEGER_DW u;
    int64_t  QuadPart;
} W32_LARGE_INTEGER;
typedef union {
    struct { W32_DWORD LowPart; W32_DWORD HighPart; } u;
    uint64_t QuadPart;
} W32_ULARGE_INTEGER;

typedef struct {
    W32_WORD wYear;
    W32_WORD wMonth;
    W32_WORD wDayOfWeek;
    W32_WORD wDay;
    W32_WORD wHour;
    W32_WORD wMinute;
    W32_WORD wSecond;
    W32_WORD wMilliseconds;
} W32_SYSTEMTIME;

typedef struct {
    W32_LONG  Bias;
    W32_WCHAR StandardName[32];
    W32_SYSTEMTIME StandardDate;
    W32_LONG  StandardBias;
    W32_WCHAR DaylightName[32];
    W32_SYSTEMTIME DaylightDate;
    W32_LONG  DaylightBias;
} W32_TIME_ZONE_INFORMATION;

typedef struct {
    W32_DWORD dwFileAttributes;
    W32_FILETIME ftCreationTime;
    W32_FILETIME ftLastAccessTime;
    W32_FILETIME ftLastWriteTime;
    W32_DWORD nFileSizeHigh;
    W32_DWORD nFileSizeLow;
    W32_DWORD dwReserved0;
    W32_DWORD dwReserved1;
    W32_CHAR  cFileName[260];
    W32_CHAR  cAlternateFileName[14];
} W32_WIN32_FIND_DATAA;

typedef struct {
    W32_DWORD dwFileAttributes;
    W32_FILETIME ftCreationTime;
    W32_FILETIME ftLastAccessTime;
    W32_FILETIME ftLastWriteTime;
    W32_DWORD nFileSizeHigh;
    W32_DWORD nFileSizeLow;
    W32_DWORD dwReserved0;
    W32_DWORD dwReserved1;
    W32_WCHAR cFileName[260];
    W32_WCHAR cAlternateFileName[14];
} W32_WIN32_FIND_DATAW;

typedef struct {
    W32_LARGE_INTEGER StreamSize;
    W32_WCHAR cStreamName[296];   /* MAX_PATH + 36, per the docs */
} W32_WIN32_FIND_STREAM_DATA;

typedef struct {
    W32_DWORD dwFileAttributes;
    W32_FILETIME ftCreationTime;
    W32_FILETIME ftLastAccessTime;
    W32_FILETIME ftLastWriteTime;
    W32_DWORD nFileSizeHigh;
    W32_DWORD nFileSizeLow;
} W32_WIN32_FILE_ATTRIBUTE_DATA;

typedef struct {
    W32_DWORD dwFileAttributes;
    W32_FILETIME ftCreationTime;
    W32_FILETIME ftLastAccessTime;
    W32_FILETIME ftLastWriteTime;
    W32_DWORD dwVolumeSerialNumber;
    W32_DWORD nFileSizeHigh;
    W32_DWORD nFileSizeLow;
    W32_DWORD nNumberOfLinks;
    W32_DWORD nFileIndexHigh;
    W32_DWORD nFileIndexLow;
} W32_BY_HANDLE_FILE_INFORMATION;

typedef struct {
    uint64_t  Internal;
    uint64_t  InternalHigh;
    W32_DWORD Offset;
    W32_DWORD OffsetHigh;
    W32_HANDLE hEvent;
} W32_OVERLAPPED;

typedef struct {
    W32_DWORD nLength;
    void     *lpSecurityDescriptor;
    W32_BOOL  bInheritHandle;
} W32_SECURITY_ATTRIBUTES;

_Static_assert(sizeof(W32_WIN32_FIND_DATAA) == 320, "find_data_a");
_Static_assert(sizeof(W32_WIN32_FIND_DATAW) == 592, "find_data_w");
_Static_assert(sizeof(W32_WIN32_FIND_STREAM_DATA) == 600, "stream_data");
_Static_assert(sizeof(W32_WIN32_FILE_ATTRIBUTE_DATA) == 36, "attr_data");
_Static_assert(sizeof(W32_BY_HANDLE_FILE_INFORMATION) == 52, "by_handle");
_Static_assert(sizeof(W32_OVERLAPPED) == 32, "overlapped");
_Static_assert(sizeof(W32_SECURITY_ATTRIBUTES) == 24, "sec_attr");
_Static_assert(sizeof(W32_TIME_ZONE_INFORMATION) == 172, "tzinfo");

/* --- find --------------------------------------------------------------- */
W32ABI W32_HANDLE FindFirstFileA(W32_LPCSTR name, W32_WIN32_FIND_DATAA *out);
W32ABI W32_HANDLE FindFirstFileW(W32_LPCWSTR name, W32_WIN32_FIND_DATAW *out);
W32ABI W32_BOOL   FindNextFileA(W32_HANDLE h, W32_WIN32_FIND_DATAA *out);
W32ABI W32_BOOL   FindNextFileW(W32_HANDLE h, W32_WIN32_FIND_DATAW *out);
W32ABI W32_BOOL   FindClose(W32_HANDLE h);
W32ABI W32_HANDLE FindFirstFileExW(W32_LPCWSTR name, W32_DWORD level,
                                  void *out, W32_DWORD searchOp,
                                  void *reserved, W32_DWORD flags);
W32ABI W32_HANDLE FindFirstStreamW(W32_LPCWSTR name, W32_DWORD infoLevel,
                                  W32_WIN32_FIND_STREAM_DATA *out,
                                  W32_DWORD flags);
W32ABI W32_BOOL   FindNextStreamW(W32_HANDLE h, W32_WIN32_FIND_STREAM_DATA *out);
W32ABI W32_HANDLE FindFirstChangeNotificationW(W32_LPCWSTR path,
                                              W32_BOOL subtree,
                                              W32_DWORD filter);
W32ABI W32_BOOL   FindNextChangeNotification(W32_HANDLE h);
W32ABI W32_BOOL   FindCloseChangeNotification(W32_HANDLE h);

/* --- attributes, directories, volumes ------------------------------------ */
W32ABI W32_DWORD GetFileAttributesW(W32_LPCWSTR name);
W32ABI W32_BOOL  GetFileAttributesExW(W32_LPCWSTR name, W32_DWORD level,
                                     W32_WIN32_FILE_ATTRIBUTE_DATA *out);
W32ABI W32_BOOL  SetFileAttributesW(W32_LPCWSTR name, W32_DWORD attrs);
W32ABI W32_BOOL  CopyFileW(W32_LPCWSTR src, W32_LPCWSTR dst, W32_BOOL failIfExists);
typedef W32_DWORD (W32ABI *W32_PROGRESS_CB)(W32_LARGE_INTEGER total,
    W32_LARGE_INTEGER done, W32_LARGE_INTEGER streamSize,
    W32_LARGE_INTEGER streamDone, W32_DWORD streamNo, W32_DWORD reason,
    W32_HANDLE srcFile, W32_HANDLE dstFile, void *data);
W32ABI W32_BOOL  CopyFileExW(W32_LPCWSTR src, W32_LPCWSTR dst,
                             W32_PROGRESS_CB progress, void *data,
                             W32_BOOL *cancel, W32_DWORD flags);
W32ABI W32_BOOL  MoveFileW(W32_LPCWSTR src, W32_LPCWSTR dst);
W32ABI W32_BOOL  MoveFileExW(W32_LPCWSTR src, W32_LPCWSTR dst, W32_DWORD flags);
W32ABI W32_BOOL  MoveFileWithProgressW(W32_LPCWSTR src, W32_LPCWSTR dst,
                                      W32_PROGRESS_CB progress, void *data,
                                      W32_DWORD flags);
W32ABI W32_BOOL  ReplaceFileW(W32_LPCWSTR replaced, W32_LPCWSTR replacement,
                              W32_LPCWSTR backup, W32_DWORD flags,
                              void *reserved1, void *reserved2);
W32ABI W32_BOOL  DeleteFileA(W32_LPCSTR name);
W32ABI W32_BOOL  DeleteFileW(W32_LPCWSTR name);
W32ABI W32_BOOL  RemoveDirectoryW(W32_LPCWSTR name);
W32ABI W32_BOOL  CreateDirectoryW(W32_LPCWSTR name, W32_SECURITY_ATTRIBUTES *sa);
W32ABI W32_BOOL  CreateHardLinkW(W32_LPCWSTR linkName, W32_LPCWSTR target,
                                 void *reserved);
W32ABI W32_DWORD GetCompressedFileSizeW(W32_LPCWSTR name, W32_DWORD *high);
W32ABI W32_BOOL  GetDiskFreeSpaceW(W32_LPCWSTR root, W32_DWORD *secPerClus,
                                  W32_DWORD *bytesPerSec, W32_DWORD *freeClus,
                                  W32_DWORD *totalClus);
W32ABI W32_BOOL  GetDiskFreeSpaceExW(W32_LPCWSTR root,
                                    W32_ULARGE_INTEGER *freeAvail,
                                    W32_ULARGE_INTEGER *total,
                                    W32_ULARGE_INTEGER *totalFree);
W32ABI W32_UINT  GetDriveTypeW(W32_LPCWSTR root);
W32ABI W32_DWORD GetLogicalDriveStringsW(W32_DWORD cch, W32_LPWSTR buf);
W32ABI W32_BOOL  GetVolumeInformationW(W32_LPCWSTR root, W32_LPWSTR volName,
                                      W32_DWORD volNameSize, W32_DWORD *serial,
                                      W32_DWORD *maxCompLen, W32_DWORD *fsFlags,
                                      W32_LPWSTR fsName, W32_DWORD fsNameSize);
W32ABI W32_DWORD GetFinalPathNameByHandleW(W32_HANDLE h, W32_LPWSTR buf,
                                           W32_DWORD cch, W32_DWORD flags);
W32ABI W32_DWORD GetFullPathNameW(W32_LPCWSTR name, W32_DWORD cch,
                                 W32_LPWSTR buf, W32_LPWSTR *filePart);
W32ABI W32_DWORD GetLongPathNameW(W32_LPCWSTR shortPath, W32_LPWSTR out,
                                 W32_DWORD cch);
W32ABI W32_BOOL  SetFileTime(W32_HANDLE h, const W32_FILETIME *creation,
                             const W32_FILETIME *access,
                             const W32_FILETIME *write);
W32ABI W32_BOOL  FileTimeToLocalFileTime(const W32_FILETIME *in, W32_FILETIME *out);
W32ABI W32_BOOL  FileTimeToSystemTime(const W32_FILETIME *in, W32_SYSTEMTIME *out);
W32ABI W32_BOOL  FileTimeToDosDateTime(const W32_FILETIME *in, W32_WORD *date,
                                      W32_WORD *time);
W32ABI W32_BOOL  DosDateTimeToFileTime(W32_WORD date, W32_WORD time,
                                      W32_FILETIME *out);
W32ABI W32_BOOL  LocalFileTimeToFileTime(const W32_FILETIME *in, W32_FILETIME *out);
W32ABI W32_LONG  CompareFileTime(const W32_FILETIME *a, const W32_FILETIME *b);
W32ABI W32_BOOL  SystemTimeToTzSpecificLocalTime(const W32_TIME_ZONE_INFORMATION *tz,
                                                const W32_SYSTEMTIME *in,
                                                W32_SYSTEMTIME *out);
W32ABI void      GetSystemTimeAsFileTime(W32_FILETIME *out);
W32ABI void      GetLocalTime(W32_SYSTEMTIME *out);
W32ABI W32_DWORD GetTimeZoneInformation(W32_TIME_ZONE_INFORMATION *out);
W32ABI W32_BOOL  QueryPerformanceCounter(W32_LARGE_INTEGER *out);
W32ABI W32_BOOL  QueryPerformanceFrequency(W32_LARGE_INTEGER *out);

/* --- files, mappings, pipes ---------------------------------------------- */
W32ABI W32_HANDLE CreateFileW(W32_LPCWSTR path, W32_DWORD access,
                              W32_DWORD share, W32_SECURITY_ATTRIBUTES *sa,
                              W32_DWORD disposition, W32_DWORD flags,
                              W32_HANDLE tmpl);
W32ABI W32_BOOL   GetOverlappedResult(W32_HANDLE h, W32_OVERLAPPED *ov,
                                     W32_DWORD *nbytes, W32_BOOL wait);
W32ABI W32_DWORD  SetFilePointer(W32_HANDLE h, W32_LONG distLow,
                                W32_LONG *distHigh, W32_DWORD method);
W32ABI W32_BOOL   SetFilePointerEx(W32_HANDLE h, W32_LARGE_INTEGER dist,
                                  W32_LARGE_INTEGER *pos, W32_DWORD method);
W32ABI W32_BOOL   SetEndOfFile(W32_HANDLE h);
W32ABI W32_BOOL   FlushFileBuffers(W32_HANDLE h);
W32ABI W32_DWORD  GetFileSize(W32_HANDLE h, W32_DWORD *high);
W32ABI W32_BOOL   GetFileSizeEx(W32_HANDLE h, W32_LARGE_INTEGER *out);
W32ABI W32_DWORD  GetFileType(W32_HANDLE h);
W32ABI W32_BOOL   GetFileInformationByHandle(W32_HANDLE h,
                                            W32_BY_HANDLE_FILE_INFORMATION *out);
W32ABI W32_BOOL   SetHandleInformation(W32_HANDLE h, W32_DWORD mask,
                                      W32_DWORD flags);
W32ABI W32_HANDLE CreateFileMappingA(W32_HANDLE file,
                                    W32_SECURITY_ATTRIBUTES *sa,
                                    W32_DWORD protect, W32_DWORD sizeHigh,
                                    W32_DWORD sizeLow, W32_LPCSTR name);
W32ABI W32_HANDLE CreateFileMappingW(W32_HANDLE file,
                                    W32_SECURITY_ATTRIBUTES *sa,
                                    W32_DWORD protect, W32_DWORD sizeHigh,
                                    W32_DWORD sizeLow, W32_LPCWSTR name);
W32ABI void      *MapViewOfFile(W32_HANDLE map, W32_DWORD access,
                               W32_DWORD offHigh, W32_DWORD offLow,
                               W32_SIZE_T bytes);
W32ABI W32_BOOL   UnmapViewOfFile(const void *addr);
W32ABI W32_BOOL   CancelIo(W32_HANDLE h);
W32ABI W32_BOOL   DeviceIoControl(W32_HANDLE h, W32_DWORD code,
                                 const void *in, W32_DWORD inLen,
                                 void *out, W32_DWORD outLen,
                                 W32_DWORD *ret, W32_OVERLAPPED *ov);
W32ABI W32_BOOL   CreatePipe(W32_HANDLE *readOut, W32_HANDLE *writeOut,
                             W32_SECURITY_ATTRIBUTES *sa, W32_DWORD size);
W32ABI W32_HANDLE CreateNamedPipeA(W32_LPCSTR name, W32_DWORD openMode,
                                  W32_DWORD pipeMode, W32_DWORD maxInst,
                                  W32_DWORD outBuf, W32_DWORD inBuf,
                                  W32_DWORD timeout, W32_SECURITY_ATTRIBUTES *sa);
W32ABI W32_BOOL   ConnectNamedPipe(W32_HANDLE h, W32_OVERLAPPED *ov);
W32ABI W32_BOOL   WaitNamedPipeA(W32_LPCSTR name, W32_DWORD timeout);

/* Internal to the personality, not Win32 exports. */
void       w32_fs_init(void);
void       w32_fs_note_close(int fd);
W32_HANDLE w32_fs_create(const char *path, W32_DWORD access, W32_DWORD share,
                         W32_DWORD disposition, W32_DWORD flags,
                         W32_HANDLE tmpl);
/* Full-Unicode case fold of one code unit for enumeration matching; defined
 * in kernel32_loc.c next to the casing tables it shares with CompareString. */
W32_WCHAR w32_fold_char(W32_WCHAR c);
/* Duplex pipe ends: one handle, two fds.  w32_fs_pipe_fds reports them (1)
 * or says the handle is plain (0); w32_fs_pipe_drop forgets the row at
 * close and hands back the peer fd.  kernel32.c's Read/Write/Close use
 * both; the rows live in kernel32_fs.c. */
int w32_fs_pipe_fds(W32_HANDLE h, int *rfd, int *wfd);
int w32_fs_pipe_drop(W32_HANDLE h, int *peer_out);
/* SetHandleInformation's inherit bit, consulted by CreateProcess when it
 * scrubs fds.  Default 0 (no inherit). */
int w32_fs_is_inheritable(int fd);


/* --- W32A-2: processes ----------------------------------------------------- */

typedef struct {
    W32_DWORD cb;
    W32_LPSTR reserved;
    W32_LPSTR desktop;
    W32_LPSTR title;
    W32_DWORD x;
    W32_DWORD y;
    W32_DWORD xSize;
    W32_DWORD ySize;
    W32_DWORD xCountChars;
    W32_DWORD yCountChars;
    W32_DWORD fillAttribute;
    W32_DWORD flags;
    W32_WORD  showWindow;
    W32_WORD  cbReserved2;
    uint8_t  *reserved2;
    W32_HANDLE hStdInput;
    W32_HANDLE hStdOutput;
    W32_HANDLE hStdError;
} W32_STARTUPINFOA;

typedef struct {
    W32_DWORD cb;
    W32_LPWSTR reserved;
    W32_LPWSTR desktop;
    W32_LPWSTR title;
    W32_DWORD x;
    W32_DWORD y;
    W32_DWORD xSize;
    W32_DWORD ySize;
    W32_DWORD xCountChars;
    W32_DWORD yCountChars;
    W32_DWORD fillAttribute;
    W32_DWORD flags;
    W32_WORD  showWindow;
    W32_WORD  cbReserved2;
    uint8_t  *reserved2;
    W32_HANDLE hStdInput;
    W32_HANDLE hStdOutput;
    W32_HANDLE hStdError;
} W32_STARTUPINFOW;

typedef struct {
    W32_HANDLE hProcess;
    W32_HANDLE hThread;
    W32_DWORD  processId;
    W32_DWORD  threadId;
} W32_PROCESS_INFORMATION;

/* STARTUPINFO flags. */
#define W32_STARTF_USESHOWWINDOW    0x00000001u
#define W32_STARTF_USESIZE          0x00000002u
#define W32_STARTF_USEPOSITION      0x00000004u
#define W32_STARTF_USECOUNTCHARS    0x00000008u
#define W32_STARTF_USEFILLATTRIBUTE 0x00000010u
#define W32_STARTF_RUNFULLSCREEN    0x00000020u
#define W32_STARTF_FORCEONFEEDBACK  0x00000040u
#define W32_STARTF_FORCEOFFFEEDBACK 0x00000080u
#define W32_STARTF_USESTDHANDLES    0x00000100u
#define W32_STARTF_USEHOTKEY        0x00000200u

/* Creation flags.  CREATE_SUSPENDED needs threads (W32A-3) and is refused. */
#define W32_CREATE_SUSPENDED 0x00000004u
#define W32_STILL_ACTIVE 259u
#define W32_CREATE_NO_WINDOW 0x08000000u
#define W32_DETACHED_PROCESS 0x00000008u
#define W32_NORMAL_PRIORITY_CLASS 0x00000020u

/* GetModuleHandleEx flags. */
#define W32_GET_MODULE_HANDLE_EX_FLAG_PIN                0x00000001u
#define W32_GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002u
#define W32_GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS       0x00000004u

/* LoadLibraryEx flags. */
#define W32_DONT_RESOLVE_DLL_REFERENCES   0x00000001u
#define W32_LOAD_IGNORE_CODE_AUTHZ_LEVEL  0x00000010u
#define W32_LOAD_WITH_ALTERED_SEARCH_PATH 0x00000008u
#define W32_LOAD_LIBRARY_AS_DATAFILE      0x00000002u

/* Version identity: the one sanctioned impersonation.  Windows 10 build
 * 19045, one constant, greppable, in one file (w32/src/kernel32_ps.c). */
#define W32_VER_PLATFORM_WIN32_NT 2u
#define W32_PRODUCT_PROFESSIONAL  0x00000030u

typedef struct {
    W32_DWORD size;
    W32_DWORD major;
    W32_DWORD minor;
    W32_DWORD build;
    W32_DWORD platformId;
    W32_WCHAR csdVersion[128];
    W32_WORD  servicePackMajor;
    W32_WORD  servicePackMinor;
    W32_WORD  suiteMask;
    uint8_t   productType;
    uint8_t   reserved;
} W32_OSVERSIONINFOEXW;

/* SYSTEM_INFO. */
#define W32_PROCESSOR_ARCHITECTURE_AMD64 9u
typedef struct {
    W32_WORD  arch;
    W32_WORD  reserved0;
    W32_DWORD pageSize;
    void     *minAppAddr;
    void     *maxAppAddr;
    W32_DWORD_PTR activeMask;
    W32_DWORD nProcessors;
    W32_DWORD processorType;
    W32_DWORD allocGranularity;
    W32_WORD  processorLevel;
    W32_WORD  processorRevision;
} W32_SYSTEM_INFO;

/* Processor features for IsProcessorFeaturePresent. */
#define W32_PF_FLOATING_POINT_PRECISION_ERRATA  0u
#define W32_PF_FLOATING_POINT_EMULATED          1u
#define W32_PF_COMPARE_EXCHANGE_DOUBLE          2u
#define W32_PF_MMX_INSTRUCTIONS_AVAILABLE       3u
#define W32_PF_XMMI_INSTRUCTIONS_AVAILABLE      6u
#define W32_PF_XMMI64_INSTRUCTIONS_AVAILABLE   10u
#define W32_PF_SSE3_INSTRUCTIONS_AVAILABLE     13u
#define W32_PF_SSSE3_INSTRUCTIONS_AVAILABLE    36u
#define W32_PF_SSE4_1_INSTRUCTIONS_AVAILABLE   37u
#define W32_PF_SSE4_2_INSTRUCTIONS_AVAILABLE   38u
#define W32_PF_AVX_INSTRUCTIONS_AVAILABLE      39u
#define W32_PF_RDTSC_INSTRUCTION_AVAILABLE      8u
#define W32_PF_RDTSCP_INSTRUCTION_AVAILABLE    32u
#define W32_PF_NX_ENABLED                      12u
#define W32_PF_3DNOW_INSTRUCTIONS_AVAILABLE     7u

/* Toolhelp snapshot flags.  Only TH32CS_SNAPPROCESS is implemented. */
#define W32_TH32CS_SNAPPROCESS  0x00000002u

typedef struct {
    W32_DWORD size;
    W32_DWORD cntUsage;
    W32_DWORD processId;
    uint64_t  defaultHeapId;
    W32_DWORD moduleId;
    W32_DWORD cntThreads;
    W32_DWORD parentProcessId;
    W32_LONG  basePriority;
    W32_DWORD flags;
    W32_WCHAR exeFile[260];
} W32_PROCESSENTRY32W;

/* Memory census.  Total-phys comes from the OS page count; avail-phys has
 * no userspace interface, so the Ex form refuses and the void form reports
 * zero -- documented at the implementation, not smuggled. */
/* The void census: six DWORDs then two SIZE_T, the Windows shape.  An
 * all-SIZE_T spelling would be 56 bytes against the caller's 40 and smash
 * a real binary's stack. */
typedef struct {
    W32_DWORD len;
    W32_DWORD memLoad;
    W32_DWORD totalPhys;
    W32_DWORD availPhys;
    W32_DWORD totalPageFile;
    W32_DWORD availPageFile;
    W32_SIZE_T totalVirtual;
    W32_SIZE_T availVirtual;
} W32_MEMORYSTATUS;

typedef struct {
    W32_DWORD len;
    W32_DWORD memLoad;
    uint64_t  totalPhys;
    uint64_t  availPhys;
    uint64_t  totalPageFile;
    uint64_t  availPageFile;
    uint64_t  totalVirtual;
    uint64_t  availVirtual;
    uint64_t  availExtendedVirtual;   /* reserved, always zero */
} W32_MEMORYSTATUSEX;

_Static_assert(sizeof(W32_STARTUPINFOA) == 104, "startupinfo_a");
_Static_assert(sizeof(W32_STARTUPINFOW) == 104, "startupinfo_w");
_Static_assert(sizeof(W32_PROCESS_INFORMATION) == 24, "procinfo");
_Static_assert(sizeof(W32_OSVERSIONINFOEXW) == 284, "osverex");
_Static_assert(sizeof(W32_SYSTEM_INFO) == 48, "sysinfo");
_Static_assert(sizeof(W32_PROCESSENTRY32W) == 568, "processentry");
_Static_assert(sizeof(W32_MEMORYSTATUS) == 40, "memstatus");
_Static_assert(sizeof(W32_MEMORYSTATUSEX) == 64, "memstatusex");

W32ABI W32_BOOL CreateProcessA(W32_LPCSTR app, W32_LPSTR cmdline,
                               W32_SECURITY_ATTRIBUTES *procSa,
                               W32_SECURITY_ATTRIBUTES *threadSa,
                               W32_BOOL inherit, W32_DWORD flags,
                               void *env, W32_LPCSTR cwd,
                               const W32_STARTUPINFOA *si,
                               W32_PROCESS_INFORMATION *pi);
W32ABI W32_BOOL CreateProcessW(W32_LPCWSTR app, W32_LPWSTR cmdline,
                               W32_SECURITY_ATTRIBUTES *procSa,
                               W32_SECURITY_ATTRIBUTES *threadSa,
                               W32_BOOL inherit, W32_DWORD flags,
                               void *env, W32_LPCWSTR cwd,
                               const W32_STARTUPINFOW *si,
                               W32_PROCESS_INFORMATION *pi);
W32ABI void    GetStartupInfoA(W32_STARTUPINFOA *out);
W32ABI void    GetStartupInfoW(W32_STARTUPINFOW *out);
W32ABI W32_BOOL GetExitCodeProcess(W32_HANDLE h, W32_DWORD *code);
W32ABI W32_BOOL TerminateProcess(W32_HANDLE h, W32_DWORD code);
W32ABI W32_HANDLE OpenProcess(W32_DWORD access, W32_BOOL inherit, W32_DWORD pid);
W32ABI W32_BOOL GetProcessTimes(W32_HANDLE h, W32_FILETIME *creation,
                               W32_FILETIME *exit, W32_FILETIME *kernel,
                               W32_FILETIME *user);
W32ABI W32_HANDLE GetCurrentProcess(void);
W32ABI W32_DWORD  GetCurrentProcessId(void);
W32ABI W32_DWORD  GetModuleFileNameA(W32_HANDLE mod, W32_LPSTR buf, W32_DWORD cch);
W32ABI W32_DWORD  GetModuleFileNameW(W32_HANDLE mod, W32_LPWSTR buf,
                                    W32_DWORD cch);
W32ABI void      *GetModuleHandleW(W32_LPCWSTR name);
W32ABI W32_BOOL   GetModuleHandleExW(W32_DWORD flags, const void *nameOrAddr,
                                    void **modOut);
W32ABI W32_LPCWSTR GetCommandLineW(void);
W32ABI W32_LPWSTR GetEnvironmentStringsW(void);
W32ABI W32_BOOL  FreeEnvironmentStringsW(W32_LPWSTR block);
W32ABI W32_DWORD GetEnvironmentVariableA(W32_LPCSTR name, W32_LPSTR buf,
                                         W32_DWORD cch);
W32ABI W32_BOOL  SetEnvironmentVariableW(W32_LPCWSTR name, W32_LPCWSTR value);
W32ABI W32_DWORD ExpandEnvironmentStringsW(W32_LPCWSTR src, W32_LPWSTR dst,
                                           W32_DWORD cch);
W32ABI W32_DWORD GetCurrentDirectoryA(W32_DWORD cch, W32_LPSTR buf);
W32ABI W32_DWORD GetCurrentDirectoryW(W32_DWORD cch, W32_LPWSTR buf);
W32ABI W32_BOOL  SetCurrentDirectoryA(W32_LPCSTR path);
W32ABI W32_BOOL  SetCurrentDirectoryW(W32_LPCWSTR path);
W32ABI W32_DWORD GetTempPathA(W32_DWORD cch, W32_LPSTR buf);
W32ABI W32_DWORD GetTempPathW(W32_DWORD cch, W32_LPWSTR buf);
W32ABI W32_UINT  GetWindowsDirectoryA(W32_LPSTR buf, W32_UINT cch);
W32ABI W32_UINT  GetWindowsDirectoryW(W32_LPWSTR buf, W32_UINT cch);
W32ABI W32_UINT  GetSystemDirectoryA(W32_LPSTR buf, W32_UINT cch);
W32ABI W32_DWORD GetVersion(void);
W32ABI W32_BOOL  GetVersionExW(W32_OSVERSIONINFOEXW *out);
W32ABI W32_BOOL  GetProductInfo(W32_DWORD major, W32_DWORD minor,
                               W32_DWORD spMajor, W32_DWORD spMinor,
                               W32_DWORD *type);
W32ABI W32_BOOL  GetProcessAffinityMask(W32_HANDLE h, W32_DWORD_PTR *procMask,
                                       W32_DWORD_PTR *sysMask);
W32ABI W32_HANDLE CreateToolhelp32Snapshot(W32_DWORD flags, W32_DWORD pid);
W32ABI W32_BOOL  Process32FirstW(W32_HANDLE snap, W32_PROCESSENTRY32W *out);
W32ABI W32_BOOL  Process32NextW(W32_HANDLE snap, W32_PROCESSENTRY32W *out);
W32ABI W32_BOOL  IsDebuggerPresent(void);
W32ABI W32_BOOL  IsProcessorFeaturePresent(W32_DWORD feature);
W32ABI void      GetNativeSystemInfo(W32_SYSTEM_INFO *out);
W32ABI void      GetSystemInfo(W32_SYSTEM_INFO *out);
W32ABI W32_DWORD GetTickCount(void);
W32ABI W32_BOOL  Beep(W32_DWORD freq, W32_DWORD dur);
W32ABI W32_DWORD SleepEx(W32_DWORD ms, W32_BOOL alertable);
W32ABI void      OutputDebugStringW(W32_LPCWSTR str);
W32ABI W32_HRESULT GetApplicationRestartSettings(W32_LPWSTR cmdline,
                                                W32_DWORD *cch, W32_DWORD *flags);
W32ABI W32_HRESULT RegisterApplicationRestart(W32_LPCWSTR cmdline, W32_DWORD flags);
W32ABI W32_HRESULT UnregisterApplicationRestart(void);
W32ABI W32_INT   MulDiv(W32_INT a, W32_INT b, W32_INT c);
W32ABI void     *EncodePointer(void *p);
W32ABI void     *DecodePointer(void *p);
W32ABI void      GlobalMemoryStatus(W32_MEMORYSTATUS *out);
W32ABI W32_BOOL  GlobalMemoryStatusEx(W32_MEMORYSTATUSEX *out);
W32ABI void     *LoadLibraryW(W32_LPCWSTR name);
W32ABI void     *LoadLibraryExA(W32_LPCSTR name, W32_HANDLE reserved,
                               W32_DWORD flags);
W32ABI void     *LoadLibraryExW(W32_LPCWSTR name, W32_HANDLE reserved,
                               W32_DWORD flags);
W32ABI W32_BOOL   FreeLibrary(void *mod);

/* Internal to the personality, not Win32 exports. */
void w32_ps_init(int argc, char **argv, char **envp);

/* --- W32A-2: heaps --------------------------------------------------------- */
#define W32_HEAP_ZERO_MEMORY 0x00000008u
#define W32_GMEM_FIXED    0x0000u
#define W32_GMEM_MOVEABLE 0x0002u
#define W32_GMEM_ZEROINIT 0x0040u
#define W32_GPTR          0x0040u
#define W32_GHND          0x0042u
#define W32_LMEM_FIXED    0x0000u
#define W32_LMEM_MOVEABLE 0x0002u
#define W32_LMEM_ZEROINIT 0x0040u
#define W32_LPTR          0x0040u
#define W32_LHND          0x0042u

W32ABI void     *HeapReAlloc(W32_HANDLE heap, W32_DWORD flags, void *mem,
                            unsigned long long size);
W32ABI W32_SIZE_T HeapSize(W32_HANDLE heap, W32_DWORD flags, const void *mem);
W32ABI void     *GlobalAlloc(W32_UINT flags, W32_SIZE_T size);
W32ABI void     *GlobalLock(void *h);
W32ABI W32_BOOL  GlobalUnlock(void *h);
W32ABI void     *GlobalFree(void *h);
W32ABI W32_SIZE_T GlobalSize(void *h);
W32ABI void     *LocalAlloc(W32_UINT flags, W32_SIZE_T size);
W32ABI void     *LocalFree(void *h);
W32ABI W32_BOOL  VirtualProtect(void *addr, W32_SIZE_T size,
                               W32_DWORD newProt, W32_DWORD *oldProt);
W32ABI W32_SIZE_T GetLargePageMinimum(void);

/* --- W32A-2: locales and strings -------------------------------------------- */
#define W32_CP_ACP        0u
#define W32_CP_OEMCP      1u
#define W32_CP_MACCP      2u
#define W32_CP_THREAD_ACP 3u
#define W32_CP_SYMBOL     42u
#define W32_CP_UTF7       65000u
#define W32_CP_UTF8       65001u

#define W32_LOCALE_USER_DEFAULT    0x0400u
#define W32_LOCALE_SYSTEM_DEFAULT  0x0800u
#define W32_LOCALE_INVARIANT       0x007Fu
#define W32_LOCALE_EN_US           0x0409u
#define W32_LANG_EN_US             0x0409u

/* LCTYPEs.  String-valued unless noted (N). */
#define W32_LOCALE_SNAME              0x0000005Cu
#define W32_LOCALE_SENGLANGUAGE        0x00001001u
#define W32_LOCALE_SENGCOUNTRY         0x00001002u
#define W32_LOCALE_SABBREVCTRYNAME     0x00000007u
#define W32_LOCALE_SNATIVECTRYNAME     0x00000008u
#define W32_LOCALE_SISO3166CTRYNAME    0x0000005Au
#define W32_LOCALE_SISO639LANGNAME     0x00000059u
#define W32_LOCALE_SABBREVLANGNAME     0x00000003u
#define W32_LOCALE_SNATIVELANGNAME     0x00000004u
#define W32_LOCALE_IDATE               0x00000021u  /* N */
#define W32_LOCALE_ILDATE              0x00000022u  /* N */
#define W32_LOCALE_ITIME               0x00000023u  /* N */
#define W32_LOCALE_ICURRDIGITS         0x00000019u  /* N */
#define W32_LOCALE_IINTLCURRDIGITS     0x0000001Au  /* N */
#define W32_LOCALE_INEGNUMBER          0x00001010u  /* N */
#define W32_LOCALE_STHOUSAND           0x0000000Fu
#define W32_LOCALE_SDECIMAL            0x0000000Eu
#define W32_LOCALE_SCURRENCY           0x00000014u
#define W32_LOCALE_SINTLSYMBOL         0x00000015u
#define W32_LOCALE_SMONDECIMALSEP      0x00000016u
#define W32_LOCALE_SMONTHOUSANDSEP     0x00000017u
#define W32_LOCALE_SSHORTDATE          0x0000001Fu
#define W32_LOCALE_SLONGDATE           0x00000020u
#define W32_LOCALE_STIMEFORMAT         0x00001003u
#define W32_LOCALE_S1159               0x00000028u
#define W32_LOCALE_S2359               0x00000029u
#define W32_LOCALE_SDAYNAME1           0x0000002Au
#define W32_LOCALE_SDAYNAME7           0x00000030u
#define W32_LOCALE_SABBREVDAYNAME7     0x00000037u
#define W32_LOCALE_SMONTHNAME1         0x00000038u
#define W32_LOCALE_SMONTHNAME12        0x00000043u
#define W32_LOCALE_SABBREVMONTHNAME12  0x0000004Fu
#define W32_LOCALE_SPOSITIVESIGN       0x00000050u
#define W32_LOCALE_SNEGATIVESIGN       0x00000051u

/* String flags. */
#define W32_NORM_IGNORECASE     0x00000001u
#define W32_NORM_IGNORENONSPACE 0x00000002u
#define W32_NORM_IGNORESYMBOLS  0x00000004u
#define W32_NORM_IGNOREKANATYPE 0x00010000u
#define W32_NORM_IGNOREWIDTH    0x00020000u
#define W32_LINGUISTIC_IGNORECASE 0x00000010u
#define W32_SORT_STRINGSORT     0x00001000u
#define W32_CSTR_LESS_THAN    1u
#define W32_CSTR_EQUAL        2u
#define W32_CSTR_GREATER_THAN 3u
#define W32_LCMAP_LOWERCASE 0x00000100u
#define W32_LCMAP_UPPERCASE 0x00000200u
#define W32_LCMAP_SORTKEY   0x00000400u
#define W32_LCMAP_BYTEREV   0x00000800u
#define W32_LCMAP_HIRAGANA  0x00100000u
#define W32_LCMAP_KATAKANA  0x00200000u
#define W32_LCMAP_HALFWIDTH 0x00400000u
#define W32_LCMAP_FULLWIDTH 0x00800000u

/* GetStringType classes. */
#define W32_CT_CTYPE1 0x00000001u
#define W32_CT_CTYPE2 0x00000002u
#define W32_CT_CTYPE3 0x00000004u
#define W32_C1_UPPER  0x0001u
#define W32_C1_LOWER  0x0002u
#define W32_C1_DIGIT  0x0004u
#define W32_C1_SPACE  0x0008u
#define W32_C1_PUNCT  0x0010u
#define W32_C1_CNTRL  0x0020u
#define W32_C1_BLANK  0x0040u
#define W32_C1_XDIGIT 0x0080u
#define W32_C1_ALPHA  0x0100u
#define W32_C2_LEFTTORIGHT 0x0001u
#define W32_C2_RIGHTTOLEFT 0x0002u
#define W32_C2_EUROPENUMBER 0x0003u
#define W32_C2_EUROPESEPARATOR 0x0004u
#define W32_C2_EUROPETERMINATOR 0x0005u
#define W32_C2_ARABICNUMBER 0x0006u
#define W32_C2_COMMONSEPARATOR 0x0007u
#define W32_C2_BLOCKSEPARATOR 0x0008u
#define W32_C2_SEGMENTSEPARATOR 0x0009u
#define W32_C2_WHITESPACE 0x000Au
#define W32_C2_OTHERNEUTRAL 0x000Bu
#define W32_C2_NOTAPPLICABLE 0x0000u
#define W32_C3_NONSPACING 0x0001u
#define W32_C3_DIACRITIC  0x0002u
#define W32_C3_VOWELMARK  0x0004u
#define W32_C3_SYMBOL     0x0008u
#define W32_C3_KATAKANA   0x0010u
#define W32_C3_HIRAGANA   0x0020u
#define W32_C3_HALFWIDTH  0x0040u
#define W32_C3_FULLWIDTH  0x0080u
#define W32_C3_IDEOGRAPH  0x0100u
#define W32_C3_KASHIDA    0x0200u
#define W32_C3_LEXICAL    0x0400u
#define W32_C3_ALPHA      0x8000u
#define W32_C3_NOTAPPLICABLE 0x0000u

/* IsTextUnicode flags. */
#define W32_IS_TEXT_UNICODE_ASCII16            0x0001u
#define W32_IS_TEXT_UNICODE_REVERSE_ASCII16    0x0010u
#define W32_IS_TEXT_UNICODE_STATISTICS         0x0002u
#define W32_IS_TEXT_UNICODE_REVERSE_STATISTICS 0x0020u
#define W32_IS_TEXT_UNICODE_CONTROLS           0x0004u
#define W32_IS_TEXT_UNICODE_REVERSE_CONTROLS   0x0040u
#define W32_IS_TEXT_UNICODE_SIGNATURE          0x0008u
#define W32_IS_TEXT_UNICODE_REVERSE_SIGNATURE  0x0080u
#define W32_IS_TEXT_UNICODE_ILLEGAL_CHARS      0x0100u
#define W32_IS_TEXT_UNICODE_ODD_LENGTH         0x0200u
#define W32_IS_TEXT_UNICODE_DBCS_LEADBYTE      0x0400u
#define W32_IS_TEXT_UNICODE_NULL_BYTES         0x1000u
#define W32_IS_TEXT_UNICODE_NOT_UNICODE_MASK   0x0F00u
#define W32_IS_TEXT_UNICODE_NOT_ASCII_MASK     0xF000u

/* FormatMessage flags. */
#define W32_FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100u
#define W32_FORMAT_MESSAGE_IGNORE_INSERTS  0x00000200u
#define W32_FORMAT_MESSAGE_FROM_STRING     0x00000400u
#define W32_FORMAT_MESSAGE_FROM_SYSTEM     0x00001000u
#define W32_FORMAT_MESSAGE_ARGUMENT_ARRAY  0x00002000u
#define W32_FORMAT_MESSAGE_MAX_WIDTH_MASK  0x000000FFu

typedef struct {
    W32_UINT MaxCharSize;
    uint8_t  DefaultChar[2];
    uint8_t  LeadByte[12];
} W32_CPINFO;

typedef W32_BOOL (W32ABI *W32_LOCALE_ENUMPROC)(W32_LPWSTR locale);

W32ABI W32_UINT GetACP(void);
W32ABI W32_UINT GetOEMCP(void);
W32ABI W32_BOOL GetCPInfo(W32_UINT cp, W32_CPINFO *out);
W32ABI W32_BOOL IsValidCodePage(W32_UINT cp);
W32ABI W32_BOOL IsDBCSLeadByteEx(W32_UINT cp, uint8_t byte);
W32ABI W32_LCID GetUserDefaultLCID(void);
W32ABI W32_LANGID GetUserDefaultLangID(void);
W32ABI W32_LANGID GetSystemDefaultLangID(void);
W32ABI W32_INT  GetLocaleInfoA(W32_LCID locale, W32_DWORD lctype,
                              W32_LPSTR buf, W32_INT cch);
W32ABI W32_INT  GetLocaleInfoW(W32_LCID locale, W32_DWORD lctype,
                              W32_LPWSTR buf, W32_INT cch);
W32ABI W32_INT  GetLocaleInfoEx(W32_LPCWSTR name, W32_DWORD lctype,
                               W32_LPWSTR buf, W32_INT cch);
W32ABI W32_BOOL GetStringTypeExA(W32_LCID locale, W32_DWORD infoType,
                                W32_LPCSTR src, W32_INT count, W32_WORD *types);
W32ABI W32_BOOL GetStringTypeExW(W32_LCID locale, W32_DWORD infoType,
                                W32_LPCWSTR src, W32_INT count, W32_WORD *types);
W32ABI W32_BOOL GetStringTypeW(W32_DWORD infoType, W32_LPCWSTR src,
                              W32_INT count, W32_WORD *types);
W32ABI W32_BOOL IsValidLocale(W32_LCID locale, W32_DWORD flags);
W32ABI W32_BOOL EnumSystemLocalesW(W32_LOCALE_ENUMPROC cb, W32_DWORD flags);
W32ABI W32_INT  CompareStringW(W32_LCID locale, W32_DWORD flags,
                              W32_LPCWSTR s1, W32_INT n1,
                              W32_LPCWSTR s2, W32_INT n2);
W32ABI W32_INT  CompareStringEx(W32_LPCWSTR name, W32_DWORD flags,
                               W32_LPCWSTR s1, W32_INT n1,
                               W32_LPCWSTR s2, W32_INT n2,
                               void *version, void *reserved, W32_LONG_PTR param);
W32ABI W32_INT  LCMapStringA(W32_LCID locale, W32_DWORD flags,
                            W32_LPCSTR src, W32_INT srclen,
                            W32_LPSTR dst, W32_INT dstlen);
W32ABI W32_INT  LCMapStringW(W32_LCID locale, W32_DWORD flags,
                            W32_LPCWSTR src, W32_INT srclen,
                            W32_LPWSTR dst, W32_INT dstlen);
W32ABI W32_INT  LCMapStringEx(W32_LPCWSTR name, W32_DWORD flags,
                             W32_LPCWSTR src, W32_INT srclen,
                             W32_LPWSTR dst, W32_INT dstlen,
                             void *version, void *reserved, W32_LONG_PTR param);
W32ABI W32_BOOL IsTextUnicode(const void *buf, W32_INT len, W32_INT *flags);
W32ABI W32_INT  GetDateFormatW(W32_LCID locale, W32_DWORD flags,
                              const W32_SYSTEMTIME *time, W32_LPCWSTR fmt,
                              W32_LPWSTR buf, W32_INT cch);
W32ABI W32_INT  GetDateFormatEx(W32_LPCWSTR name, W32_DWORD flags,
                               const W32_SYSTEMTIME *time, W32_LPCWSTR fmt,
                               W32_LPWSTR buf, W32_INT cch, W32_LPCWSTR cal);
W32ABI W32_INT  GetTimeFormatW(W32_LCID locale, W32_DWORD flags,
                              const W32_SYSTEMTIME *time, W32_LPCWSTR fmt,
                              W32_LPWSTR buf, W32_INT cch);
W32ABI W32_INT  GetTimeFormatEx(W32_LPCWSTR name, W32_DWORD flags,
                               const W32_SYSTEMTIME *time, W32_LPCWSTR fmt,
                               W32_LPWSTR buf, W32_INT cch);
W32ABI W32_INT  MultiByteToWideChar(W32_UINT cp, W32_DWORD flags,
                                   W32_LPCSTR mb, W32_INT cbMulti,
                                   W32_LPWSTR wc, W32_INT cchWide);
W32ABI W32_INT  WideCharToMultiByte(W32_UINT cp, W32_DWORD flags,
                                   W32_LPCWSTR wc, W32_INT cchWide,
                                   W32_LPSTR mb, W32_INT cbMulti,
                                   W32_LPCSTR defaultChar, W32_BOOL *usedDefault);
W32ABI W32_INT  lstrcmpW(W32_LPCWSTR a, W32_LPCWSTR b);
W32ABI W32_INT  lstrcmpiA(W32_LPCSTR a, W32_LPCSTR b);
W32ABI W32_INT  lstrcmpiW(W32_LPCWSTR a, W32_LPCWSTR b);
W32ABI W32_LPWSTR lstrcpyW(W32_LPWSTR dst, W32_LPCWSTR src);
W32ABI W32_LPSTR  lstrcpynA(W32_LPSTR dst, W32_LPCSTR src, W32_INT n);
W32ABI W32_LPWSTR lstrcpynW(W32_LPWSTR dst, W32_LPCWSTR src, W32_INT n);
W32ABI W32_LPWSTR lstrcatW(W32_LPWSTR dst, W32_LPCWSTR src);
W32ABI W32_INT  lstrlenW(W32_LPCWSTR s);
W32ABI W32_LPWSTR CharUpperW(W32_LPWSTR s);
W32ABI W32_LPWSTR CharLowerW(W32_LPWSTR s);
W32ABI W32_BOOL  IsCharAlphaW(W32_WCHAR c);
W32ABI W32_BOOL  IsCharAlphaNumericW(W32_WCHAR c);
W32ABI W32_BOOL  IsCharUpperW(W32_WCHAR c);
W32ABI W32_BOOL  IsCharLowerW(W32_WCHAR c);
/* wsprintfW argument boxes.  The C shell (below) boxes varargs; the guest
 * bind layer boxes trapped slots; w32_wsprintf_core formats from boxes.
 * u carries integers/chars/pointers, p carries strings and %n targets. */
struct w32_ws_arg { int kind; uint64_t u; const void *p; };
#define W32_WS_S64    0
#define W32_WS_U64    1
#define W32_WS_WSTR   2
#define W32_WS_ASTR   3
#define W32_WS_WCHAR  4
#define W32_WS_ACHAR  5
#define W32_WS_PTR    6
#define W32_WS_INTPTR 7
W32ABI W32_INT w32_wsprintf_core(W32_LPWSTR buf, W32_LPCWSTR fmt,
                                const struct w32_ws_arg *args, int nargs);
/* NOTE: no W32ABI — the shell is sysv varargs (a sysv caller cannot feed
 * an ms_abi varargs callee; see kernel32_loc.c).  The guest path feeds
 * the core, never this shell. */
W32_INT wsprintfW(W32_LPWSTR buf, W32_LPCWSTR fmt, ...);
W32ABI W32_DWORD FormatMessageA(W32_DWORD flags, const void *src,
                               W32_DWORD msgId, W32_DWORD langId,
                               W32_LPSTR buf, W32_DWORD cch, void *args);
W32ABI W32_DWORD FormatMessageW(W32_DWORD flags, const void *src,
                               W32_DWORD msgId, W32_DWORD langId,
                               W32_LPWSTR buf, W32_DWORD cch, void *args);

/* Called by the CRT stub before anything else; not a Win32 export. */
void w32_kernel32_init(int argc, char **argv);

#endif /* AURALITE_W32_KERNEL32_H */

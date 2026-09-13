/* w32/tests/w32a2_time.c — W32A-2 guest fixture: time and formats.
 *
 * The time half of attributes/time: file/system time conversion, DOS
 * dates, time zones, the performance counter, ticks, SetFileTime, and
 * the date/time picture formats.  Exits 55/1.
 */

#include "w32a2_common.h"

void __stdcall winstart(void) {
    FILETIME ft, back, tmpft;
    SYSTEMTIME st, out;
    WORD date, time;
    LARGE_INTEGER q1, q2, qf;
    TIME_ZONE_INFORMATION tz;
    WCHAR tmp[512], file[512], buf[128], pic[64];
    DWORD n;

    w32a2_out = GetStdHandle(STD_OUTPUT_HANDLE);

    GetSystemTimeAsFileTime(&ft);
    CHECKX(ft.dwLowDateTime != 0 || ft.dwHighDateTime != 0, "time-now");
    CHECKX(FileTimeToSystemTime(&ft, &st), "time-ft2st");
    CHECKX(st.wYear >= 2020 && st.wYear < 2100, "time-year");
    CHECKX(st.wMonth >= 1 && st.wMonth <= 12, "time-month");

    /* DOS round-trip (2-second granularity: back <= ft). */
    CHECKX(FileTimeToDosDateTime(&ft, &date, &time), "time-dos");
    CHECKX(DosDateTimeToFileTime(date, time, &back), "time-dos-back");
    CHECKX(CompareFileTime(&back, &ft) <= 0, "time-dos-order");
    CHECKX(!DosDateTimeToFileTime(0, 0, &back), "time-dos-zero");

    /* Identities under UTC. */
    CHECKX(FileTimeToLocalFileTime(&ft, &tmpft), "time-local");
    CHECKX(tmpft.dwLowDateTime == ft.dwLowDateTime, "time-local-id");
    CHECKX(LocalFileTimeToFileTime(&tmpft, &back), "time-unlocal");
    CHECKX(back.dwHighDateTime == ft.dwHighDateTime, "time-unlocal-id");
    CHECKX(CompareFileTime(&ft, &ft) == 0, "time-compare-eq");
    CHECKX(SystemTimeToTzSpecificLocalTime(NULL, &st, &out),
        "time-tzlocal");
    CHECKX(out.wYear == st.wYear, "time-tzlocal-year");

    GetLocalTime(&st);
    CHECKX(st.wYear >= 2020 && st.wYear < 2100, "time-local-year");
    CHECKX(GetTimeZoneInformation(&tz) == TIME_ZONE_ID_UNKNOWN,
        "time-tzid");
    CHECKX(tz.Bias == 0, "time-tz-bias");

    CHECKX(QueryPerformanceFrequency(&qf), "time-qpf");
    CHECKX(qf.QuadPart == 10000000LL, "time-qpf-value");
    CHECKX(QueryPerformanceCounter(&q1), "time-qpc1");
    CHECKX(QueryPerformanceCounter(&q2), "time-qpc2");
    CHECKX(q2.QuadPart >= q1.QuadPart, "time-qpc-mono");

    CHECKX(GetTickCount() != 0 || GetTickCount64() != 0, "time-tick");

    /* SetFileTime round-trip through a handle. */
    n = GetTempPathW(512, tmp);
    if (n > 2 && n < 500) {
        HANDLE h;
        BY_HANDLE_FILE_INFORMATION bi;
        FILETIME wt;
        wjoin(tmp, L"w32a2_time.bin", file);
        DeleteFileW(file);
        h = CreateFileW(file, GENERIC_READ | GENERIC_WRITE, 0, NULL,
            CREATE_ALWAYS, 0, NULL);
        CHECKX(h != INVALID_HANDLE_VALUE, "time-file");
        if (h != INVALID_HANDLE_VALUE) {
            CHECKX(DosDateTimeToFileTime(
                (WORD)(((2020u - 1980u) << 9) | (3u << 5) | 4u),
                (WORD)((5u << 11) | (6u << 5) | 14u), &wt), "time-dos-fixed");
            CHECKX(SetFileTime(h, NULL, NULL, &wt), "time-set");
            CHECKX(GetFileInformationByHandle(h, &bi), "time-byhandle");
            CHECKX(CompareFileTime(&bi.ftLastWriteTime, &wt) == 0,
                "time-roundtrip");
            CHECKX(CloseHandle(h), "time-close");
        }
        DeleteFileW(file);
    } else {
        CHECKX(0, "time-tmp");
    }

    /* Date/time pictures on a fixed Saturday. */
    memset(&st, 0, sizeof(st));
    st.wYear = 2026;
    st.wMonth = 9;
    st.wDay = 12;
    st.wDayOfWeek = 6;
    st.wHour = 13;
    st.wMinute = 5;
    st.wSecond = 9;
    /* M/d/yyyy */
    pic[0] = L'M'; pic[1] = L'/'; pic[2] = L'd'; pic[3] = L'/';
    pic[4] = L'y'; pic[5] = L'y'; pic[6] = L'y'; pic[7] = L'y';
    pic[8] = 0;
    CHECKX(GetDateFormatW(0x0409, 0, &st, pic, buf, 128) == 9,
        "time-pic-count");
    CHECKX(weq(buf, "9/12/2026"), "time-pic-mdy");
    /* dddd */
    pic[0] = L'd'; pic[1] = L'd'; pic[2] = L'd'; pic[3] = L'd';
    pic[4] = 0;
    GetDateFormatW(0x0409, 0, &st, pic, buf, 128);
    CHECKX(weq(buf, "Saturday"), "time-pic-day");
    CHECKX(GetDateFormatW(0x0409, 2, &st, NULL, buf, 128) > 10,
        "time-pic-default");
    /* h:mm:ss tt */
    pic[0] = L'h'; pic[1] = L':'; pic[2] = L'm'; pic[3] = L'm';
    pic[4] = L':'; pic[5] = L's'; pic[6] = L's'; pic[7] = L' ';
    pic[8] = L't'; pic[9] = L't'; pic[10] = 0;
    GetTimeFormatW(0x0409, 0, &st, pic, buf, 128);
    CHECKX(weq(buf, "1:05:09 PM"), "time-pic-hms");
    /* H:mm, no seconds */
    pic[0] = L'H'; pic[1] = L':'; pic[2] = L'm'; pic[3] = L'm';
    pic[4] = 0;
    GetTimeFormatW(0x0409, 8, &st, pic, buf, 128);
    CHECKX(weq(buf, "13:05"), "time-pic-hm");

    /* Locales beyond en-US refuse by name. */
    CHECKX(GetDateFormatW(0x0419, 0, &st, NULL, buf, 128) == 0,
        "time-date-ru-fails");
    CHECKX(GetLastError() == ERROR_INVALID_PARAMETER, "time-date-ru-code");
    CHECKX(GetTimeFormatW(0x0419, 0, &st, NULL, buf, 128) == 0,
        "time-time-ru-fails");
    CHECKX(GetLastError() == ERROR_INVALID_PARAMETER, "time-time-ru-code");

    w32a2_done("TIME");
}

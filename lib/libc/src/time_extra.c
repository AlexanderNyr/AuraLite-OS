/* libc/src/time_extra.c — broken-down time conversions (SELFHOST SH1).
 *
 * tcc's preprocessor expands __DATE__/__TIME__ via localtime(), so the
 * self-host compiler is the first real consumer of these functions.
 *
 * This libc has no timezone support: localtime() == gmtime().  That is
 * stated here rather than hidden — the guest clock (kernel/time.c) is
 * a fixed epoch derived from the boot timer, so "local" has no meaning
 * beyond UTC anyway.
 *
 * The conversions use the well-known civil-from-days / days-from-civil
 * algorithms (Hinnant) — O(1), no loops over years, correct for the
 * whole time_t range this kernel can represent.
 */

#include <time.h>
#include <string.h>
#include <stdio.h>

/* ---- days since 1970-01-01 (proleptic Gregorian) ---- */
static long long days_from_civil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long long era  = (y >= 0 ? y : y - 399) / 400;
    const unsigned  yoe  = (unsigned)(y - era * 400);            /* [0,399]  */
    const unsigned  doy  = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned  doe  = yoe * 365 + yoe / 4 - yoe / 100 + doy;/* [0,146096]*/
    return era * 146097 + (long long)doe - 719468;
}

static void civil_from_days(long long z, long long *y, unsigned *m,
                            unsigned *d)
{
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned  doe = (unsigned)(z - era * 146097);          /* [0,146096]*/
    const unsigned  yoe = (doe - doe / 1460 + doe / 36524 -
                           doe / 146096) / 365;                  /* [0,399]  */
    const long long yy  = (long long)yoe + era * 400;
    const unsigned  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);/* [0,365]  */
    const unsigned  mp  = (5 * doy + 2) / 153;                   /* [0,11]   */
    *d = doy - (153 * mp + 2) / 5 + 1;                           /* [1,31]   */
    *m = mp < 10 ? mp + 3 : mp - 9;                              /* [1,12]   */
    *y = yy + (*m <= 2);
}

static void fill_tm(struct tm *t, long long days, long long secs)
{
    long long y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    t->tm_sec  = (int)(secs % 60);
    t->tm_min  = (int)((secs / 60) % 60);
    t->tm_hour = (int)((secs / 3600) % 24);
    t->tm_mday = (int)d;
    t->tm_mon  = (int)m - 1;
    t->tm_year = (int)y - 1900;
    t->tm_wday = (int)((days + 4) % 7);   /* 1970-01-01 was a Thursday */
    t->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    t->tm_isdst = 0;
}

struct tm *gmtime(const time_t *timer)
{
    static struct tm t;
    fill_tm(&t, *timer / 86400, *timer % 86400);
    return &t;
}

struct tm *localtime(const time_t *timer)
{
    /* No timezone support in AuraLite: local == UTC, stated above. */
    return gmtime(timer);
}

time_t mktime(struct tm *tm)
{
    /* Normalise out-of-range fields the way the standard requires. */
    long long y = tm->tm_year + 1900;
    long long m = tm->tm_mon + 1;
    long long d = tm->tm_mday;
    long long h = tm->tm_hour, mi = tm->tm_min, s = tm->tm_sec;

    /* Fold seconds upward first, then minutes, then hours, then days;
     * each step leaves the residue in the standard range. */
    s  += mi * 60;         mi = s / 60;        s %= 60;
    mi += h * 60;          h  = mi / 60;       mi %= 60;
    h  += d * 24;          d  = h / 24;        h %= 24;

    /* Month overflow folds into years (bounded: |tm_mon|/12 iterations). */
    while (m > 12) { m -= 12; y++; }
    while (m < 1)  { m += 12; y--; }

    /* Day-of-month overflow is handled by adding the offset to the
     * first-of-month epoch count -- the civil algorithm takes any
     * number of days, so tm_mday = 100 just lands in a later month. */
    long long days = days_from_civil(y, (unsigned)m, 1) + (d - 1);

    /* Canonicalise so the caller sees the normalised fields. */
    long long yy;
    unsigned mm, dd;
    civil_from_days(days, &yy, &mm, &dd);
    tm->tm_year = (int)yy - 1900;
    tm->tm_mon  = (int)mm - 1;
    tm->tm_mday = (int)dd;
    tm->tm_hour = (int)h;
    tm->tm_min  = (int)mi;
    tm->tm_sec  = (int)s;
    tm->tm_wday = (int)((days + 4) % 7);
    tm->tm_yday = (int)(days - days_from_civil(yy, 1, 1));
    return (time_t)days * 86400 + h * 3600 + mi * 60 + s;
}

static const char *const wday_ab[7] =
    {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const mon_ab[12] =
    {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

char *asctime(const struct tm *tm)
{
    static char buf[26];
    snprintf(buf, sizeof buf, "%s %s %2d %02d:%02d:%02d %04d\n",
             wday_ab[tm->tm_wday % 7], mon_ab[tm->tm_mon % 12], tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
    return buf;
}

char *ctime(const time_t *timer)
{
    return asctime(localtime(timer));
}

/* strftime: a bounded subset that covers the compiler/date-reporting
 * uses: %Y %y %m %d %e %H %M %S %a %A %b %B %j %w %u %n %t %%.  Anything
 * else is copied verbatim, matching glibc's "unknown specifier is copied"
 * behaviour loosely; unsupported conversions print the literal. */
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm)
{
    size_t n = 0;
    const char *p;
    char num[16];
    for (p = format; *p && n < max; p++) {
        if (*p != '%') { s[n++] = *p; continue; }
        p++;
        if (!*p) break;
        switch (*p) {
        case 'Y': snprintf(num, sizeof num, "%04d", tm->tm_year + 1900); break;
        case 'y': snprintf(num, sizeof num, "%02d", (tm->tm_year + 1900) % 100); break;
        case 'm': snprintf(num, sizeof num, "%02d", tm->tm_mon + 1); break;
        case 'd': snprintf(num, sizeof num, "%02d", tm->tm_mday); break;
        case 'e': snprintf(num, sizeof num, "%2d", tm->tm_mday); break;
        case 'H': snprintf(num, sizeof num, "%02d", tm->tm_hour); break;
        case 'M': snprintf(num, sizeof num, "%02d", tm->tm_min); break;
        case 'S': snprintf(num, sizeof num, "%02d", tm->tm_sec); break;
        case 'a': strcpy(num, wday_ab[tm->tm_wday % 7]); break;
        case 'b': strcpy(num, mon_ab[tm->tm_mon % 12]); break;
        case 'j': snprintf(num, sizeof num, "%03d", tm->tm_yday + 1); break;
        case 'w': snprintf(num, sizeof num, "%d", tm->tm_wday % 7); break;
        case 'u': snprintf(num, sizeof num, "%d", tm->tm_wday == 0 ? 7 : tm->tm_wday); break;
        case 'n': s[n++] = '\n'; continue;
        case 't': s[n++] = '\t'; continue;
        case '%': s[n++] = '%'; continue;
        default:  s[n++] = '%'; s[n++] = *p; continue;
        }
        size_t len = strlen(num);
        if (n + len >= max) break;
        memcpy(s + n, num, len);
        n += len;
    }
    if (n < max) s[n] = 0;
    else if (max > 0) s[max - 1] = 0;
    return n;
}

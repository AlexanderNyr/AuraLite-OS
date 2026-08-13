#ifndef AURALITE_LIBC_STDIO_H
#define AURALITE_LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

/* User-space stdio for AuraLite OS: FILE* streams over the fd syscalls. */

#define EOF (-1)
#define BUFSIZ 1024

/* Buffering modes for setvbuf(). */
#define _IOFBF 0   /* fully buffered */
#define _IOLBF 1   /* line buffered  */
#define _IONBF 2   /* unbuffered     */

typedef long fpos_t;

typedef struct _FILE {
    int   fd;
    int   flags;         /* see FILE_* below */
    int   bufmode;       /* _IOFBF / _IOLBF / _IONBF */
    char *buf;
    int   bufsz;
    int   bufpos;        /* bytes currently staged in buf */
    int   bufcap;        /* valid bytes in buf for reading */
    int   readpos;       /* read cursor within buf */
    int   dir;           /* 0 = none, 1 = reading, 2 = writing */
    int   ungot;         /* pushed-back char, or -1 */
    char  ibuf[BUFSIZ];  /* default internal buffer */
} FILE;

/* FILE flags. */
#define FILE_EOF    0x01
#define FILE_ERR    0x02
#define FILE_ALLOC  0x04   /* fd was opened by fopen() and must be closed */

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* Character / string output (legacy, go to stdout). */
int putchar(int c);
int puts(const char *s);
int printf(const char *fmt, ...);
void perror(const char *s);

/* Streams. */
FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
int   fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int   fgetc(FILE *f);
int   getc(FILE *f);
int   getchar(void);
int   ungetc(int c, FILE *f);
char *fgets(char *s, int size, FILE *f);
int   fputc(int c, FILE *f);
int   putc(int c, FILE *f);
int   fputs(const char *s, FILE *f);
int   fprintf(FILE *f, const char *fmt, ...);
int   vfprintf(FILE *f, const char *fmt, va_list ap);
int   snprintf(char *str, size_t size, const char *fmt, ...);
int   vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
int   sprintf(char *str, const char *fmt, ...);
int   vsprintf(char *str, const char *fmt, va_list ap);

/* Seek origins.  C requires these in <stdio.h>; AuraLite previously defined
 * them only in <unistd.h>, so portable code calling fseek() with SEEK_SET
 * failed to compile even though everything it needed existed.  Guarded so
 * including both headers in either order is fine. */
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* rename(): C puts it in <stdio.h>, but AuraLite declared it only in
 * <unistd.h>.  remove() is already declared further down with the other
 * file operations. */
int   rename(const char *from, const char *to);

/* File positioning (libc/src/stdio_extra.c).  fseek() discards the read
 * buffer and ftell() reports the LOGICAL position, not the fd's read-ahead
 * one -- see the comment there. */
int   fseek(FILE *f, long offset, int whence);
long  ftell(FILE *f);
void  rewind(FILE *f);
int   fgetpos(FILE *f, fpos_t *pos);
int   fsetpos(FILE *f, const fpos_t *pos);

/* Formatted input (libc/src/stdio_extra.c). */
int   sscanf(const char *str, const char *fmt, ...);
int   vsscanf(const char *str, const char *fmt, va_list ap);
int   scanf(const char *fmt, ...);
int   fscanf(FILE *f, const char *fmt, ...);

/* Temporary files (libc/src/stdio_extra.c). */
FILE *tmpfile(void);
char *tmpnam(char *s);
int   mkstemp(char *tmpl);
int   remove(const char *path);
int   fflush(FILE *f);
int   feof(FILE *f);
int   ferror(FILE *f);
void  clearerr(FILE *f);
int   fileno(FILE *f);
int   setvbuf(FILE *f, char *buf, int mode, size_t size);

/* ---- POSIX.1-2024 stdio extensions (Phase Q2) ---- */
ssize_t  getdelim(char **lineptr, size_t *n, int delim, FILE *stream);
ssize_t  getline(char **lineptr, size_t *n, FILE *stream);
int      dprintf(int fd, const char *fmt, ...);
int      vdprintf(int fd, const char *fmt, va_list ap);
int      asprintf(char **strp, const char *fmt, ...);
int      vasprintf(char **strp, const char *fmt, va_list ap);
FILE    *fmemopen(void *buf, size_t size, const char *mode);
FILE    *open_memstream(char **ptr, size_t *sizeloc);
FILE    *popen(const char *command, const char *type);
int      pclose(FILE *stream);
void     flockfile(FILE *f);
void     funlockfile(FILE *f);
int      ftrylockfile(FILE *f);
int      getc_unlocked(FILE *f);
int      putc_unlocked(int c, FILE *f);
int      fgetc_unlocked(FILE *f);

#endif /* AURALITE_LIBC_STDIO_H */

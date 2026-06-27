#ifndef AURALITE_LIBC_STDIO_H
#define AURALITE_LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

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
int   fflush(FILE *f);
int   feof(FILE *f);
int   ferror(FILE *f);
void  clearerr(FILE *f);
int   fileno(FILE *f);
int   setvbuf(FILE *f, char *buf, int mode, size_t size);

#endif /* AURALITE_LIBC_STDIO_H */

#ifndef AURALITE_LIBC_CTYPE_H
#define AURALITE_LIBC_CTYPE_H

/*
 * ctype.h — character classification and case mapping (C locale only).
 *
 * All functions take an int that must be representable as unsigned char or
 * equal EOF (-1); any other value is undefined behaviour per the C standard.
 * Implementations live in libc/src/libc.c and use a 257-entry classification
 * table indexed by (c + 1) so that EOF maps to index 0.
 */

int isalnum(int c);
int isalpha(int c);
int isblank(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);

int tolower(int c);
int toupper(int c);

#endif /* AURALITE_LIBC_CTYPE_H */

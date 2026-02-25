#ifndef _USER_STRING_H
#define _USER_STRING_H

typedef unsigned int size_t;

size_t strlen(const char *s);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    strcmp(const char *a, const char *b);
char  *strcpy(char *dst, const char *src);

char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strncpy(char *dst, const char *src, size_t n);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
char  *strstr(const char *haystack, const char *needle);
int    memcmp(const void *a, const void *b, size_t n);
void  *memmove(void *dst, const void *src, size_t n);

#ifndef NULL
#define NULL ((void *)0)
#endif

#endif

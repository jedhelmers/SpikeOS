#ifndef _USER_STDLIB_H
#define _USER_STDLIB_H

#include "string.h"  /* for size_t */

/* Memory allocation */
void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t new_size);

/* Conversions */
int   atoi(const char *s);
long  strtol(const char *s, char **endptr, int base);

/* Math */
int   abs(int n);

/* Random numbers (LCG) */
int   rand(void);
void  srand(unsigned int seed);

#define RAND_MAX 32767

/* Process */
void  exit(int status);

#ifndef NULL
#define NULL ((void *)0)
#endif

#endif

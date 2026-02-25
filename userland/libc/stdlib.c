#include "stdlib.h"
#include "unistd.h"

/* ------------------------------------------------------------------ */
/*  Random number generator (Linear Congruential Generator)           */
/* ------------------------------------------------------------------ */

static unsigned int _rand_seed = 1;

void srand(unsigned int seed) {
    _rand_seed = seed;
}

int rand(void) {
    _rand_seed = _rand_seed * 1103515245 + 12345;
    return (int)((_rand_seed >> 16) & RAND_MAX);
}

/* ------------------------------------------------------------------ */
/*  Conversions                                                        */
/* ------------------------------------------------------------------ */

int atoi(const char *s) {
    int result = 0;
    int sign = 1;

    while (*s == ' ' || *s == '\t' || *s == '\n') s++;

    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }

    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }

    return sign * result;
}

long strtol(const char *s, char **endptr, int base) {
    long result = 0;
    int sign = 1;

    while (*s == ' ' || *s == '\t' || *s == '\n') s++;

    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }

    /* Auto-detect base */
    if (base == 0) {
        if (*s == '0') {
            s++;
            if (*s == 'x' || *s == 'X') { base = 16; s++; }
            else { base = 8; }
        } else {
            base = 10;
        }
    } else if (base == 16) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    }

    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;

        if (digit >= base) break;
        result = result * base + digit;
        s++;
    }

    if (endptr) *endptr = (char *)s;
    return sign * result;
}

/* ------------------------------------------------------------------ */
/*  Math                                                               */
/* ------------------------------------------------------------------ */

int abs(int n) {
    return n < 0 ? -n : n;
}

/* ------------------------------------------------------------------ */
/*  Process                                                            */
/* ------------------------------------------------------------------ */

void exit(int status) {
    _exit(status);
}

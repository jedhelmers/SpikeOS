#include "stdio.h"
#include "unistd.h"
#include "string.h"
#include <stdarg.h>

int putchar(int c) {
    char ch = (char)c;
    write(1, &ch, 1);
    return c;
}

int puts(const char *s) {
    int len = (int)strlen(s);
    write(1, s, len);
    write(1, "\n", 1);
    return len + 1;
}

static void print_uint(unsigned int v, int base) {
    char buf[12];
    int i = 0;
    if (v == 0) { putchar('0'); return; }
    while (v > 0) {
        int d = v % base;
        buf[i++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
        v /= base;
    }
    while (i > 0) putchar(buf[--i]);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int count = 0;

    for (; *fmt; fmt++) {
        if (*fmt != '%') { putchar(*fmt); count++; continue; }
        fmt++;
        switch (*fmt) {
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) { putchar('-'); v = -v; count++; }
            print_uint((unsigned int)v, 10);
            count++;
            break;
        }
        case 'u':
            print_uint(va_arg(ap, unsigned int), 10);
            count++;
            break;
        case 'x':
            print_uint(va_arg(ap, unsigned int), 16);
            count++;
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) { putchar(*s++); count++; }
            break;
        }
        case 'c':
            putchar(va_arg(ap, int));
            count++;
            break;
        case '%':
            putchar('%');
            count++;
            break;
        default:
            putchar('%');
            putchar(*fmt);
            count += 2;
            break;
        }
    }

    va_end(ap);
    return count;
}

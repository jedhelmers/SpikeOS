#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include "stdio.h"
#include "string.h"

static bool print(const char* data, size_t length) {
    const unsigned char* bytes = (const unsigned char*) data;
    for (size_t i = 0; i < length; i++) {
        if (putchar(bytes[i]) == EOF) {
            return false;
        }
    }

    return true;
}

static void print_hex_uint(unsigned int value) {
    char buffer[8];
    const char* hex = "0123456789ABCDEF";

    for (int i = 7; i >= 0; i--) {
        buffer[i] = hex[value & 0xF];
        value >>= 4;
    }

    print(buffer, 8);
}

static void print_uint(unsigned int value) {
    char buffer[10]; // max 32-bit unsigned = 4294967295
    int i = 0;

    if (value == 0) {
        putchar('0');
        return;
    }

    while (value > 0) {
        buffer[i++] = '0' + (value % 10);
        value /= 10;
    }

    // digits are reversed
    while (i--) {
        putchar(buffer[i]);
    }
}

int printf(const char* restrict format, ...) {
    va_list params;
    va_start(params, format);

    int written = 0;

    while (*format != '\0') {
            size_t maxrem = INT_MAX - written;

            if (format[0] != '%' || format[1] == '%') {
                if (format[0] == '%') {
                    format++;
                }

                size_t amount = 1;

                while (format[amount] && format[amount] != '%') {
                    amount++;
                }

                if (maxrem < amount) {
                    // TODO: Set errno to EOVERFLOW.
                    return -1;
                }

                if (!print(format, amount)) {
                    return -1;
                }

                format += amount;
                written += amount;
                continue;
            }

            const char* format_begun_at = format++;

            if (*format == 'c') {
                format++;

                /* char promotes to int */
                char c = (char) va_arg(params, int);

                if (!maxrem) {
                    // TODO: Set errno to EOVERFLOW.
                    return -1;
                }

                if (!print(&c, sizeof(c))) {
                    return -1;
                }

                written++;
            } else if (*format == 's') {
                format++;
                const char* str = va_arg(params, const char*);

                if (!str) {
                    str = "(null)";
                }

                size_t len = strlen(str);

                if (maxrem < len) {
                    return -1;
                }

                if (!print(str, len)) {
                    return -1;
                }

                written += len;
            } else if (*format == 'x') {
                format++;

                unsigned int value = va_arg(params, unsigned int);

                if (!maxrem) {
                    return -1;
                }

                print_hex_uint(value);
                written += 8;
            } else if (*format == 'u') {
                format++;

                unsigned int value = va_arg(params, unsigned int);

                if (!maxrem) {
                    return -1;
                }

                print_uint(value);
                // approximate written count
                if (value == 0) {
                    written += 1;
                } else {
                    unsigned int tmp = value;
                    while (tmp) {
                        written++;
                        tmp /= 10;
                    }
                }
            } else {
                format = format_begun_at;
                size_t len = strlen(format);

                if (maxrem < len) {
                    // TODO: Set errno to EOVERFLOW.
                    return -1;
                }

                if (!print(format, len)) {
                    return -1;
                }

                written += len;
                format += len;
            }
        }


    va_end(params);
    return written;
}

#ifndef _IO_H
#define _IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    // Used to read a single byte (8 bits) from a specified I/O port
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void insw(uint16_t port, void *addr, uint32_t count) {
    __asm__ volatile ("rep insw"
        : "+D"(addr), "+c"(count)
        : "d"(port)
        : "memory");
}

static inline void outsw(uint16_t port, const void *addr, uint32_t count) {
    __asm__ volatile ("rep outsw"
        : "+S"(addr), "+c"(count)
        : "d"(port)
        : "memory");
}

static inline void io_wait(void) {
    __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}

#endif

/*
 * hello.c — user program for SpikeOS using the userland libc.
 *
 * Build with: make -C userland
 */
#include "libc/stdio.h"
#include "libc/unistd.h"

int main(void) {
    printf("Hello from userland!\n");
    printf("My PID is %d\n", getpid());
    return 0;
}

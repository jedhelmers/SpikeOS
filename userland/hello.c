/*
 * hello.c — minimal freestanding user program for SpikeOS.
 *
 * Compiled with:
 *   i386-elf-gcc -nostdlib -ffreestanding -static -Wl,-Ttext=0x08048000 \
 *                -o hello.elf hello.c
 *
 * Uses sys_write and sys_exit via int $0x80.
 */

static const char msg[] = "\nHello from userland ELF!\n\n";

void _start(void) {
    /* sys_write(fd=1, buf=msg, len=25) */
    asm volatile(
        "mov $1, %%eax\n"       /* SYS_WRITE */
        "mov $1, %%ebx\n"       /* fd = stdout */
        "mov %0, %%ecx\n"       /* buf */
        "mov %1, %%edx\n"       /* len */
        "int $0x80\n"
        :
        : "r"(msg), "r"((unsigned int)(sizeof(msg) - 1))
        : "eax", "ebx", "ecx", "edx"
    );

    /* sys_exit(0) */
    asm volatile(
        "mov $0, %%eax\n"       /* SYS_EXIT */
        "mov $0, %%ebx\n"       /* status = 0 */
        "int $0x80\n"
        :
        :
        : "eax", "ebx"
    );

    for (;;) asm volatile("hlt");
}

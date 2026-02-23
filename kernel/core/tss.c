#include <kernel/tss.h>
#include <kernel/gdt.h>
#include <string.h>
#include <stdio.h>

struct tss_entry kernel_tss;

void tss_init(void) {
    memset(&kernel_tss, 0, sizeof(kernel_tss));

    /* Kernel data segment — CPU loads this into SS on ring-3 -> ring-0. */
    kernel_tss.ss0 = 0x10;

    /* Set esp0 to current kernel stack for the boot-time case. */
    uint32_t current_esp;
    __asm__ volatile("mov %%esp, %0" : "=r"(current_esp));
    kernel_tss.esp0 = current_esp;

    /* No I/O permission bitmap — offset points past the TSS. */
    kernel_tss.iomap_base = sizeof(struct tss_entry);

    /*
     * Install TSS descriptor into GDT slot 5 (selector 0x28).
     * access=0x89: P=1, DPL=0, type=1001 (32-bit TSS, available)
     * gran=0x00: byte granularity, no flags
     */
    gdt_install_tss((uint32_t)&kernel_tss,
                    sizeof(struct tss_entry) - 1,
                    0x89, 0x00);

    /* Load the Task Register. */
    tss_flush(0x28);

    printf("[tss] initialized, esp0=0x%x ss0=0x%x\n",
           kernel_tss.esp0, kernel_tss.ss0);
}

void tss_set_kernel_stack(uint32_t esp0) {
    kernel_tss.esp0 = esp0;
}

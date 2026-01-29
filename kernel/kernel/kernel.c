#include <stdio.h>

#include <kernel/tty.h>
#include <kernel/gdt.h>
#include <kernel/idt.h>
#include <kernel/pic.h>
#include <kernel/timer.h>
#include <kernel/keyboard.h>
#include <kernel/shell.h>

void kernel_main(void) {
    terminal_initialize();
    printf("\nHello,\n\tkernels!\n");

    /*
        GDT setup.
        We're using a flat-model GDT so we can later
        implement paging. Securoty will largely be handled
        by the pages.
    */
    gdt_init();
    printf("INIT GDT\n");

    /*
        TESTING GDT
    */
    printf("Testing GDT\n");
    uint16_t cs, ds, ss;
    asm volatile ("mov %%cs, %0" : "=r"(cs));
    asm volatile ("mov %%ds, %0" : "=r"(ds));
    asm volatile ("mov %%ss, %0" : "=r"(ss));
    printf("CS=%x DS=%x SS=%x\n", cs, ds, ss);

    idt_init();
    printf("INIT IDT\n");


    // Remap PICs
    printf("REMAP PIC\n");
    pic_remap(0x20, 0x28);

    printf("INIT TIMER\n");
    timer_init(100); // 100Hz
    pic_clear_mask(0); // timer IRQ0
    printf("PIC: UNMASK 0\n");

    printf("INIT KEYBOARD\n");
    keyboard_init();
    pic_clear_mask(1); // keybaord IRQ1
    printf("PIC: UNMASK 1\n");


    __asm__ volatile ("sti");

    /*
        TESTING INTERRUPTS
    */
    // volatile int x = 1;
    // volatile int y = 0;
    // volatile int z = x / y; // EIP=002008F5 CS=00000008 EFLAGS=00010006
    // (void)z;

    // asm volatile ("ud2"); // EIP=002008DD CS=00000008 EFLAGS=00010046

    // asm volatile("int $3"); // EIP=002008DE CS=00000008 EFLAGS=00000046

    /*
        Start Kernel Shell
    */

   shell_run();
   printf("INIT K-SHELL\n");
}
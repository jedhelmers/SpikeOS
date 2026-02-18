#include <stdio.h>

#include <kernel/tty.h>
#include <kernel/gdt.h>
#include <kernel/paging.h>
#include <kernel/idt.h>
#include <kernel/uart.h>
#include <kernel/isr.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/pic.h>
#include <kernel/timer.h>
#include <kernel/keyboard.h>
#include <kernel/shell.h>

extern void kprint_howdy(void);
extern void paging_enable(uint32_t);
extern uint32_t endkernel;

void thread_inc(void) {
    // int idx = 42;

    // uint32_t virt = (uint32_t)&idx;
    // uint32_t phys = virt_to_phys(virt);

    // printf("\nThread_inc:\n");
    // printf("Virtual:  %x\n", virt);
    // printf("Physical: %x\n", phys);

    for (;;) {
        terminal_putchar('+');
        for (volatile int i = 0; i < 1000000; i++);
    }
}

void thread_mid(void) {
    int idx = 1;
    
    for (;;) {
        idx++;
        // terminal_setbackground((idx % 15) + 1);
        terminal_putchar('=');

        for (volatile int i = 0; i < 1000000; i++);
    }
}

void thread_dec(void) {
    for (;;) {
        terminal_putchar('-');

        for (volatile int i = 0; i < 1000000; i++);
    }
}

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
    printf("INIT Global Descriptor Table (GDT)\n");

    /*
        TESTING GDT
    */
    printf("Testing  GDT\n");
    uint16_t cs, ds, ss;
    asm volatile ("mov %%cs, %0" : "=r"(cs));
    asm volatile ("mov %%ds, %0" : "=r"(ds));
    asm volatile ("mov %%ss, %0" : "=r"(ss));
    printf("CS=%x DS=%x SS=%x\n", cs, ds, ss);

    idt_init();
    printf("INIT Interrupt Descriptor Table (IDT)\n");

    printf("INIT Paging\n");
    paging_init();
    printf("ENABLE Paging\n");
    // CR3 requires a physical address; page_directory lives in the higher half,
    // so subtract KERNEL_VMA_OFFSET to convert VMA → physical.
    paging_enable((uint32_t)page_directory - KERNEL_VMA_OFFSET);

    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    printf("CR0 = %x\n", cr0);


    // Remap PICs
    printf("REMAP Programmable Interrupt Controller (PIC)\n");
    printf("\tTo avoid catastrophic conflics with the CPU,\n");
    printf("\tthe IRQs (Interrupt Requests) from the PIC must be remapped.\n");
    pic_remap(0x20, 0x28);

    printf("INIT IRQ0 (Timer)\n");
    timer_init(100); // 100Hz
    pic_clear_mask(0); // timer IRQ0
    printf("PIC: UNMASK Timer (enable hardware interrupt)\n");


    printf("INIT Process\n");
    process_init();

    printf("INIT Scheduler\n");
    scheduler_init();

    printf("INIT IRQ1 (Keyboard)\n");
    keyboard_init();
    pic_clear_mask(1); // keybaord IRQ1
    printf("PIC: UNMASK Keyboard (enable hardware interrupt)\n");

    printf("INIT UART (COM1)\n");
    uart_init();
    irq_install_handler(4, uart_irq_handler);
    pic_clear_mask(4);   // UNMASK IRQ4 (COM1)
    printf("PIC: UNMASK UART (IRQ4)\n");

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

    // uint32_t virt = 0x0060cc24;
    // uint32_t phys = virt_to_phys(virt);

    // printf("Alias Virtual:  %x\n", virt);
    // printf("Alias Physical: %x\n", phys);

    printf("Kernel end: %x\n", (uint32_t)&endkernel); // THIS PRINTS: 0022F800

    proc_create_kernel_thread(thread_inc); // THIS CAUSES A PAGE FAULT
    // proc_create_kernel_thread(thread_mid);
    // proc_create_kernel_thread(thread_dec);
    proc_create_kernel_thread(shell_run);
    

    kprint_howdy();

    // shell_run();
    // printf("INIT K-SHELL\n");

    asm volatile ("sti");
}
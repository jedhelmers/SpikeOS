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
#include <kernel/heap.h>
#include <kernel/tss.h>
#include <kernel/syscall.h>

extern void kprint_howdy(void);
extern void paging_enable(uint32_t);
extern uint32_t endkernel;

void thread_inc(void) {
    int idx = 42;

    uint32_t virt = (uint32_t)&idx;
    uint32_t phys = virt_to_phys(virt);

    printf("\nThread_inc:\n");
    printf("Virtual:  %x\n", virt);
    printf("Physical: %x\n", phys);

    for (;;) {
        terminal_putchar('+');
        for (volatile int i = 0; i < 10000000; i++);
    }
}

void thread_mid(void) {
    int idx = 1;
    
    for (;;) {
        idx++;
        // terminal_setbackground((idx % 15) + 1);
        terminal_putchar('=');

        for (volatile int i = 0; i < 10000000; i++);
    }
}

void thread_dec(void) {
    for (;;) {
        terminal_putchar('-');

        for (volatile int i = 0; i < 10000000; i++);
    }
}

/*
 * Ring-3 test: proves user mode + syscalls work end-to-end.
 *
 * ring3_test_fn executes at CPL=3 and uses int $0x80 to invoke
 * real syscalls: sys_write to print a message, then sys_exit.
 *
 * We must mark the pages containing the test function, the
 * message string, and user stack with PAGE_USER, and also
 * the PDE covering them, so the CPU allows ring-3 access.
 */
static uint8_t ring3_user_stack[4096] __attribute__((aligned(4096)));

static const char ring3_msg[] = "[ring3] Hello from user mode!\n";

static void __attribute__((noinline)) ring3_test_fn(void) {
    /* sys_write(fd=1, buf=ring3_msg, len=30) */
    asm volatile(
        "mov $1, %%eax\n"       /* SYS_WRITE */
        "mov $1, %%ebx\n"       /* fd = stdout */
        "mov %0, %%ecx\n"       /* buf */
        "mov %1, %%edx\n"       /* len */
        "int $0x80\n"
        :
        : "r"(ring3_msg), "r"((uint32_t)sizeof(ring3_msg) - 1)
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

void ring3_test(void) {
    /*
     * Mark the PDE covering 0xC0000000-0xC03FFFFF as user-accessible.
     * Both PDE and PTE must have PAGE_USER for the CPU to allow ring-3 access.
     */
    page_directory[KERNEL_PDE_INDEX] |= PAGE_USER;

    /* Mark the page containing ring3_test_fn as user-accessible. */
    uint32_t fn_phys = (uint32_t)ring3_test_fn - KERNEL_VMA_OFFSET;
    uint32_t fn_pte = fn_phys >> 12;
    first_page_table[fn_pte] |= PAGE_USER;
    asm volatile("invlpg (%0)" :: "r"((uint32_t)ring3_test_fn) : "memory");

    /* Mark the user stack page as user-accessible. */
    uint32_t stk_phys = (uint32_t)ring3_user_stack - KERNEL_VMA_OFFSET;
    uint32_t stk_pte = stk_phys >> 12;
    first_page_table[stk_pte] |= PAGE_USER;
    asm volatile("invlpg (%0)" :: "r"((uint32_t)ring3_user_stack) : "memory");

    uint32_t user_esp = (uint32_t)&ring3_user_stack[4096];
    uint32_t user_eip = (uint32_t)ring3_test_fn;

    printf("[ring3_test] iret to ring 3: EIP=0x%x ESP=0x%x\n", user_eip, user_esp);

    /*
     * Build the 5-word iret frame for a privilege-level change:
     *   ss, useresp, eflags, cs, eip
     * Then iret transitions us from ring 0 to ring 3.
     */
    asm volatile(
        "mov $0x23, %%ax\n"     /* user data selector | RPL=3 */
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "pushl $0x23\n"          /* ss */
        "pushl %0\n"             /* useresp */
        "pushfl\n"               /* eflags */
        "orl $0x200, (%%esp)\n"  /* ensure IF=1 */
        "pushl $0x1B\n"          /* cs = user code | RPL=3 */
        "pushl %1\n"             /* eip */
        "iret\n"
        :
        : "r"(user_esp), "r"(user_eip)
        : "eax"
    );
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

    tss_init();
    printf("INIT Task State Segment (TSS)\n");

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

    heap_init();
    printf("INIT Kernel Heap\n");

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

    /* Ring-3 test: proves TSS + GDT + trapframe work.
       This will iret to ring 3, call int $0x80, and halt.
       Comment out to boot normally into the shell. */
    ring3_test();

    // proc_create_kernel_thread(thread_inc);
    // proc_create_kernel_thread(thread_mid);
    // proc_create_kernel_thread(thread_dec);
    proc_create_kernel_thread(shell_run);
    

    kprint_howdy();

    // shell_run();
    // printf("INIT K-SHELL\n");

    asm volatile ("sti");
}
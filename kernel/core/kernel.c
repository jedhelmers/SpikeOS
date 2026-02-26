#include <stdio.h>

#include <kernel/hal.h>
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
#include <kernel/multiboot.h>
#include <kernel/initrd.h>
#include <kernel/vfs.h>
#include <kernel/ata.h>
#include <kernel/spikefs.h>
#include <kernel/boot_splash.h>
#include <kernel/fd.h>
#include <kernel/pipe.h>
#include <kernel/window.h>
#include <kernel/framebuffer.h>
#include <kernel/fb_console.h>
#include <kernel/event.h>
#include <kernel/mouse.h>
#include <kernel/pci.h>
#include <kernel/e1000.h>
#include <kernel/net.h>
#include <kernel/dock.h>

extern void kprint_howdy(void);
extern void paging_enable(uint32_t);
extern uint32_t endkernel;
extern uint32_t multiboot_info_ptr;  /* saved EBX from boot.S (.boot.bss) */

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

/*
 * ring3_test_perprocess — per-process page directory test.
 *
 * Creates a new page directory (clone of kernel's), marks the test
 * function/message/stack pages as PAGE_USER in the new PD by cloning
 * the kernel's PDE[768] page table, then creates a scheduled user
 * process. The scheduler picks it up, loads its CR3, and irets to
 * ring 3. The test function calls sys_write + sys_exit.
 *
 * This proves:
 *   - pgdir_create() works
 *   - pgdir_map_user_page() clones kernel PTs correctly
 *   - CR3 switching in the scheduler works
 *   - Ring-3 code executes under a per-process PD
 *   - Syscalls work across the CR3 boundary
 *   - pgdir_destroy() cleans up on exit
 */
void ring3_test_perprocess(void) {
    uint32_t user_pd = pgdir_create();
    if (user_pd == 0) {
        printf("[ring3] ERROR: pgdir_create failed\n");
        return;
    }

    /* Mark the page containing ring3_test_fn as user-accessible.
       This will clone PDE[768]'s page table in the new PD. */
    uint32_t fn_virt = (uint32_t)ring3_test_fn;
    uint32_t fn_phys = fn_virt - KERNEL_VMA_OFFSET;
    pgdir_map_user_page(user_pd, fn_virt, fn_phys, PAGE_PRESENT | PAGE_USER);

    /* Mark the page containing ring3_msg as user-accessible */
    uint32_t msg_virt = (uint32_t)ring3_msg;
    uint32_t msg_phys = msg_virt - KERNEL_VMA_OFFSET;
    pgdir_map_user_page(user_pd, msg_virt, msg_phys, PAGE_PRESENT | PAGE_USER);

    /* Mark the user stack page as user-accessible + writable */
    uint32_t stk_virt = (uint32_t)ring3_user_stack;
    uint32_t stk_phys = stk_virt - KERNEL_VMA_OFFSET;
    pgdir_map_user_page(user_pd, stk_virt, stk_phys, PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);

    /* Create the user process — the scheduler will pick it up */
    uint32_t user_esp = (uint32_t)&ring3_user_stack[4096];
    uint32_t user_eip = (uint32_t)ring3_test_fn;

    struct process *p = proc_create_user_process(user_pd, user_eip, user_esp);
    if (p == NULL) {
        printf("[ring3] ERROR: proc_create_user_process failed\n");
        pgdir_destroy(user_pd);
        return;
    }

    printf("[ring3] user process PID %d, CR3=0x%x\n", p->pid, user_pd);
}

void kernel_main(void) {
    terminal_initialize();

#ifdef VERBOSE_BOOT
    printf("\nHello,\n\tkernels!\n");
#endif

    gdt_init();
#ifdef VERBOSE_BOOT
    printf("INIT Global Descriptor Table (GDT)\n");
#endif

    tss_init();
#ifdef VERBOSE_BOOT
    printf("INIT Task State Segment (TSS)\n");

    printf("Testing  GDT\n");
    uint16_t cs, ds, ss;
    asm volatile ("mov %%cs, %0" : "=r"(cs));
    asm volatile ("mov %%ds, %0" : "=r"(ds));
    asm volatile ("mov %%ss, %0" : "=r"(ss));
    printf("CS=%x DS=%x SS=%x\n", cs, ds, ss);
#endif

    idt_init();
#ifdef VERBOSE_BOOT
    printf("INIT Interrupt Descriptor Table (IDT)\n");
#endif

    /* Remap PIC immediately after IDT so that any accidental STI
       (e.g. from kmalloc) won't deliver IRQs on exception vectors.
       Default BIOS mapping: IRQ0→vec8, IRQ1→vec9, etc. which collide
       with CPU exceptions.  After remap: IRQ0→vec32, IRQ1→vec33, etc. */
    pic_remap(0x20, 0x28);
    for (int i = 0; i < 16; i++) pic_set_mask(i);
#ifdef VERBOSE_BOOT
    printf("REMAP PIC (IRQs -> vectors 32-47, all masked)\n");
#endif

#ifdef VERBOSE_BOOT
    printf("INIT Paging\n");
#endif
    paging_init();
    paging_enable((uint32_t)page_directory - KERNEL_VMA_OFFSET);
#ifdef VERBOSE_BOOT
    printf("ENABLE Paging\n");
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    printf("CR0 = %x\n", cr0);
#endif

    /* Save framebuffer info from multiboot before heap (just stores values) */
    {
        uint32_t mb_phys = multiboot_info_ptr;
        if (mb_phys != 0)
            fb_save_info((struct multiboot_info *)mb_phys);
    }

    heap_init();
#ifdef VERBOSE_BOOT
    printf("INIT Kernel Heap\n");
#endif

    /* Map framebuffer into kernel VA (needs paging + heap) */
    fb_init();
#ifdef VERBOSE_BOOT
    if (fb_info.available)
        printf("INIT Framebuffer (%dx%dx%d at 0x%x)\n",
               fb_info.width, fb_info.height, fb_info.bpp, fb_info.phys_addr);
#endif

    /* Parse Multiboot info to find initrd module */
    uint32_t mb_info_phys = multiboot_info_ptr;
    if (mb_info_phys != 0) {
        struct multiboot_info *mb = (struct multiboot_info *)mb_info_phys;

        if ((mb->flags & MB_FLAG_MODS) && mb->mods_count > 0) {
            struct multiboot_mod_entry *mods =
                (struct multiboot_mod_entry *)mb->mods_addr;
#ifdef VERBOSE_BOOT
            printf("INIT initrd (phys 0x%x-0x%x)\n",
                   mods[0].mod_start, mods[0].mod_end);
#endif
            initrd_init(mods[0].mod_start, mods[0].mod_end);
        }
#ifdef VERBOSE_BOOT
        else {
            printf("[initrd] no modules loaded\n");
        }
    } else {
        printf("[initrd] no multiboot info\n");
#endif
    }

#ifdef VERBOSE_BOOT
    printf("INIT ATA disk driver\n");
#endif
    ata_init();

    vfs_init(64);
#ifdef VERBOSE_BOOT
    printf("INIT Virtual File System (VFS)\n");
#endif
    vfs_import_initrd();

#ifdef VERBOSE_BOOT
    printf("INIT SpikeFS\n");
#endif
    spikefs_init();

    fd_init();
    pipe_init();
    event_init();
#ifdef VERBOSE_BOOT
    printf("INIT File Descriptors / Pipes / Events\n");
#endif

    process_init();
#ifdef VERBOSE_BOOT
    printf("INIT Process Table\n");
#endif

    scheduler_init();
#ifdef VERBOSE_BOOT
    printf("INIT Scheduler\n");
#endif

    /* Timer and IRQ unmask AFTER process/scheduler are ready,
       because IRQ0 triggers scheduler_tick() which needs
       current_process and kernel_cr3 to be initialized. */
    timer_init(100);
    pic_clear_mask(0);
#ifdef VERBOSE_BOOT
    printf("INIT Timer (100 Hz) + IRQ0 unmasked\n");
#endif

    keyboard_init();
    pic_clear_mask(1);
#ifdef VERBOSE_BOOT
    printf("INIT Keyboard + IRQ1 unmasked\n");
#endif

    mouse_init();
#ifdef VERBOSE_BOOT
    printf("INIT Mouse\n");
#endif

    uart_init();
    irq_install_handler(4, uart_irq_handler);
    pic_clear_mask(4);
#ifdef VERBOSE_BOOT
    printf("INIT UART + IRQ4 unmasked\n");
#endif

    pci_init();
#ifdef VERBOSE_BOOT
    {
        int pci_count = 0;
        pci_get_devices(&pci_count);
        printf("INIT PCI (%d devices found)\n", pci_count);
    }
#endif

    e1000_init();
#ifdef VERBOSE_BOOT
    if (nic)
        printf("INIT e1000 NIC (MAC=%02x:%02x:%02x:%02x:%02x:%02x, link=%s)\n",
               nic->mac[0], nic->mac[1], nic->mac[2],
               nic->mac[3], nic->mac[4], nic->mac[5],
               nic->link_up ? "UP" : "DOWN");
#endif

    net_init();
#ifdef VERBOSE_BOOT
    printf("INIT Network stack\n");
#endif

    /* DHCP: auto-configure IP (needs interrupts for e1000 RX) */
    if (nic) {
        dhcp_discover();
        uint32_t dhcp_deadline = timer_ticks() + 500; /* 5 second timeout */
        while (!net_cfg.configured && timer_ticks() < dhcp_deadline) {
            hal_irq_enable();
            hal_halt();
        }
        if (!net_cfg.configured) {
            printf("[net] DHCP timeout, no IP assigned\n");
        }
    }

    fb_enable();
#ifdef VERBOSE_BOOT
    printf("INIT Framebuffer enable\n");
#endif

    /* Show boot splash (only in non-verbose mode) */
#ifndef VERBOSE_BOOT
    boot_splash();
#endif

    /* Init window manager and framebuffer console */
    wm_init();
    fb_console_init();

    /* Init dock (app launcher at bottom of screen) */
    dock_init();

    terminal_switch_to_fb();

    /* Draw desktop (deskbar + icons + dock) */
    if (fb_info.available)
        wm_draw_desktop();

    mouse_show_cursor();

    /* Desktop event loop — replaces shell_run() as the main loop.
       Apps are launched from the dock. Never returns. */
    dock_desktop_loop();
}
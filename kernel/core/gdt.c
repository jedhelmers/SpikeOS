#include <kernel/gdt.h>

/**
 * GDT layout:
 *   [0] 0x00 - null descriptor         (required by x86)
 *   [1] 0x08 - kernel code  DPL=0      (CS for ring-0 kernel)
 *   [2] 0x10 - kernel data  DPL=0      (DS/SS for ring-0 kernel)
 *   [3] 0x18 - user code    DPL=3      (CS for ring-3, loaded as 0x1B)
 *   [4] 0x20 - user data    DPL=3      (DS/SS for ring-3, loaded as 0x23)
 *   [5] 0x28 - TSS                     (filled by tss_init() via gdt_install_tss)
 */
static struct gdt_entry gdt[6];
static struct gdt_ptr gp;

// Builds one entry
static void gdt_set_gate(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[i].base_low = base & 0xFFFF;
    gdt[i].base_mid = (base >> 16) & 0xFF;
    gdt[i].base_high = (base >> 24) & 0XFF;

    gdt[i].limit_low = limit & 0xFFFF;
    gdt[i].gran = (limit >> 16) & 0X0F;
    gdt[i].gran |= gran & 0XF0;

    gdt[i].access = access;
}

void gdt_init(void) {
    gp.limit = sizeof(gdt) - 1;
    gp.base = (uint32_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);                       // null
    gdt_set_gate(1, 0, 0XFFFFFFFF, 0X9A, 0XCF);        // kernel code (DPL=0)
    gdt_set_gate(2, 0, 0XFFFFFFFF, 0X92, 0XCF);        // kernel data (DPL=0)
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);        // user code   (DPL=3)
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);        // user data   (DPL=3)
    gdt_set_gate(5, 0, 0, 0, 0);                        // TSS placeholder

    gdt_flush((uint32_t)&gp);
}

void gdt_install_tss(uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_set_gate(5, base, limit, access, gran);
}

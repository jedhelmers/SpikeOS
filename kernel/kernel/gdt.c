#include <kernel/gdt.h>

/**
 * Create space in memory for 3 GDT entries
 * gdt[0] -> null descriptor
 * gdt[1] -> kernel code segment
 * gdt[2] -> kernel data segment
 */
static struct gdt_entry gdt[3];
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

    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0XFFFFFFFF, 0X9A, 0XCF); // 0X9A == 10011010 + 0XCF == 11001111
    gdt_set_gate(2, 0, 0XFFFFFFFF, 0X92, 0XCF); // 0X92 == 10010010 + 0XCF == 11001111

    gdt_flush((uint32_t)&gp);
}
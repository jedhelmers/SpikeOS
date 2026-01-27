#include <kernel/idt.h>
#include <kernel/isr.h>

static struct idt_entry idt[256];
static struct idt_ptr idtr;

extern void idt_load(struct idt_ptr* idtr);

extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

void idt_set_gate(uint8_t vec, uint32_t handler, uint16_t selector, uint8_t type_attr) {
    idt[vec].offset_low = (uint16_t)(handler & 0xFFFF);
    idt[vec].selector = selector;
    idt[vec].zero = 0;
    idt[vec].type_attr = type_attr;
    idt[vec].offset_high = (uint16_t)((handler >> 16) & 0xFFFF);
}

static void idt_clear(void) {
    for (int i = 0; i < 256; i++) {
        idt[i].offset_low = 0;
        idt[i].selector = 0;
        idt[i].zero = 0;
        idt[i].type_attr = 0;
        idt[i].offset_high = 0;
    }
}

void idt_init(void) {
    idt_clear();

    idt_set_gate(0, (uint32_t)isr0, 0x08, 0x8E);
    idt_set_gate(3, (uint32_t)isr3, 0x08, 0x8E);
    idt_set_gate(6, (uint32_t)isr6, 0x08, 0x8E);

    idtr.limit = (uint16_t)(sizeof(idt) - 1);
    idtr.base = (uint32_t)&idt[0];

    idt_load(&idtr);
}

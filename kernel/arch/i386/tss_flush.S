/*
 * tss_flush — load the Task Register with the TSS selector.
 * Called once during tss_init() after the TSS GDT entry is set up.
 */
.global tss_flush
tss_flush:
    mov 4(%esp), %eax
    ltr %ax
    ret

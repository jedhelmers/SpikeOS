/*
 * Debug logger: writes one NDJSON line to UART so that
 * -serial file:.cursor/debug.log captures logs for analysis.
 */
#include <kernel/uart.h>

static const char hex[] = "0123456789ABCDEF";

static unsigned int buf_hex(char *buf, unsigned int val) {
    for (int i = 7; i >= 0; i--)
        *buf++ = hex[(val >> (i * 4)) & 0xF];
    return 8;
}

static void uart_send_line(const char *s, unsigned int len) {
    for (unsigned int i = 0; i < len && s[i]; i++)
        uart_write((uint8_t)s[i]);
    uart_write('\n');
}

void debug_log_pgfault(uint32_t eip, uint32_t cr3) {
    char buf[128];
    unsigned int n = 0;
    const char *p = "{\"hypothesisId\":\"A\",\"message\":\"page_fault\",\"data\":{\"eip\":";
    while (*p) buf[n++] = *p++;
    n += buf_hex(buf + n, eip);
    p = ",\"cr3\":";
    while (*p) buf[n++] = *p++;
    n += buf_hex(buf + n, cr3);
    buf[n++] = '"'; buf[n++] = '}'; buf[n++] = '}'; buf[n] = '\0';
    uart_send_line(buf, n);
}

void debug_log_pgfault_live(uint32_t eip, uint32_t cr3, uint32_t pde, uint32_t pte) {
    char buf[192];
    unsigned int n = 0;
    const char *p = "{\"hypothesisId\":\"A\",\"message\":\"pgfault_live\",\"data\":{\"eip\":";
    while (*p) buf[n++] = *p++;
    n += buf_hex(buf + n, eip);
    p = ",\"cr3\":";
    while (*p) buf[n++] = *p++;
    n += buf_hex(buf + n, cr3);
    p = ",\"pde\":";
    while (*p) buf[n++] = *p++;
    n += buf_hex(buf + n, pde);
    p = ",\"pte\":";
    while (*p) buf[n++] = *p++;
    n += buf_hex(buf + n, pte);
    buf[n++] = '}'; buf[n++] = '}'; buf[n] = '\0';
    uart_send_line(buf, n);
}

void debug_log_sched_switch(uint32_t next_pid, uint32_t next_mm) {
    char buf[128];
    unsigned int n = 0;
    const char *p = "{\"hypothesisId\":\"A\",\"message\":\"sched_switch\",\"data\":{\"next_pid\":";
    while (*p) buf[n++] = *p++;
    n += buf_hex(buf + n, next_pid);
    p = ",\"next_mm\":";
    while (*p) buf[n++] = *p++;
    n += buf_hex(buf + n, next_mm);
    buf[n++] = '}'; buf[n++] = '}'; buf[n] = '\0';
    uart_send_line(buf, n);
}

void debug_log_user_create(uint32_t pd, uint32_t user_entry_phys, uint32_t pte_0x1000) {
    char buf[160];
    unsigned int n = 0;
    const char *p = "{\"hypothesisId\":\"B\",\"message\":\"user_create\",\"data\":{\"pd\":";
    while (*p) buf[n++] = *p++;
    n += buf_hex(buf + n, pd);
    p = ",\"user_entry_phys\":";
    while (*p) buf[n++] = *p++;
    n += buf_hex(buf + n, user_entry_phys);
    p = ",\"pte_0x1000\":";
    while (*p) buf[n++] = *p++;
    n += buf_hex(buf + n, pte_0x1000);
    buf[n++] = '}'; buf[n++] = '}'; buf[n] = '\0';
    uart_send_line(buf, n);
}

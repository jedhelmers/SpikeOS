#ifndef _KERNEL_SETTINGS_H
#define _KERNEL_SETTINGS_H

typedef struct {
    int natural_scroll;  /* 0 = standard (wheel up = content up), 1 = natural (wheel up = content down) */
} sys_settings_t;

extern sys_settings_t sys_settings;

void settings_init(void);

#endif

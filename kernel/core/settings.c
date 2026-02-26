#include <kernel/settings.h>

sys_settings_t sys_settings;

void settings_init(void) {
    sys_settings.natural_scroll = 0;  /* standard scroll by default */
}

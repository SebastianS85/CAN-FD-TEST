#include "app_mode_state.h"

static volatile app_display_mode_t s_mode = APP_DISPLAY_MODE_BRIDGE;
static volatile bool s_remote_enabled = false;

void app_mode_state_init(bool remote_enabled, app_display_mode_t initial_mode)
{
    s_remote_enabled = remote_enabled;
    s_mode = initial_mode;
}

bool app_mode_state_is_remote(void)
{
    return s_remote_enabled;
}

app_display_mode_t app_mode_state_get_mode(void)
{
    return s_mode;
}

bool app_mode_state_set_mode(app_display_mode_t mode)
{
    if (!s_remote_enabled) {
        return false;
    }
    if (mode != APP_DISPLAY_MODE_BRIDGE && mode != APP_DISPLAY_MODE_SD_LOGGER &&
        mode != APP_DISPLAY_MODE_TCP_SERVER) {
        return false;
    }
    s_mode = mode;
    return true;
}

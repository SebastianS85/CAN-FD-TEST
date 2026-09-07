#ifndef APP_MODE_STATE_H
#define APP_MODE_STATE_H

#include <stdbool.h>
#include "app_types.h"

/* Shared runtime mode state read by can_services, can_logger, tcp_service and
 * app_stats_ui so the active mode can be switched on the fly when the last
 * DIP switch enables remote (TCP-controlled) mode selection. */

void app_mode_state_init(bool remote_enabled, app_display_mode_t initial_mode);
bool app_mode_state_is_remote(void);
app_display_mode_t app_mode_state_get_mode(void);

/* Only succeeds (and updates the mode) when remote control is enabled and the
 * requested mode is one of BRIDGE/SD_LOGGER/TCP_SERVER. */
bool app_mode_state_set_mode(app_display_mode_t mode);

#endif

// SPDX-License-Identifier: MIT
// dw_portal — DragonWheeze web portal integration with dragon-core dc_portal
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the dragon-core dc_portal service with DragonWheeze product endpoints
esp_err_t dw_portal_start(void);

// Stops the dw_portal web service
esp_err_t dw_portal_stop(void);

#ifdef __cplusplus
}
#endif

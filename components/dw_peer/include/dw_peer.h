// SPDX-License-Identifier: MIT
#pragma once
//
// dw_peer — DragonWheeze's dc_peer provider. Starts the ESP-NOW transport and
// broadcasts the ANNOUNCE descriptor plus the DRYER status capability (mode, power,
// set temp/time, ambient temp/humidity, cycle time) so a console can discover and
// display the dryer. Status only: it publishes, it never accepts commands.

#include "esp_err.h"

// Start dc_peer (as "dragonwheeze-<hex>") and the publish task. Call after Wi-Fi.
esp_err_t dw_peer_start(void);

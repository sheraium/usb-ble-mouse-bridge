#pragma once

#include "esp_err.h"

/** Start the USB-to-BLE bridge and its decoupled input worker. */
esp_err_t mouse_bridge_start(void);

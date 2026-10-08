#pragma once

#include "esp_err.h"
#include "mouse_event.h"

/** Start a BLE HID mouse and advertise it for pairing. */
esp_err_t ble_hid_output_start(void);
void ble_hid_output_send(const MouseEvent *event);

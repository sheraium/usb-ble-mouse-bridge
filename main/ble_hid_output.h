#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mouse_event.h"

/** Start a BLE HID mouse and advertise it for pairing. */
esp_err_t ble_hid_output_start(void);
void ble_hid_output_send(const MouseEvent *event);
bool ble_hid_new_pairing_allowed(void);
bool ble_hid_connection_is_bonded(uint16_t conn_handle);
void ble_hid_new_pairing_completed(void);
void ble_hid_connection_opened(uint16_t conn_handle);
void ble_hid_connection_closed(void);

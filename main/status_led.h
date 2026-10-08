#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    STATUS_LED_FAULT_STARTUP = 1u << 0,
    STATUS_LED_FAULT_USB = 1u << 1,
    STATUS_LED_FAULT_BLE = 1u << 2,
} status_led_fault_t;

esp_err_t status_led_start(void);
void status_led_set_ble_connected(bool connected);
void status_led_set_receiver_connected(bool connected);
void status_led_set_fault(status_led_fault_t fault, bool active);
void status_led_mouse_activity(void);

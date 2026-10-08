#include "ble_hid_output.h"
#include "mouse_bridge.h"
#include "status_led.h"

#include "esp_err.h"
#include "esp_log.h"

void app_main(void)
{
    ESP_LOGI("mouse_bridge", "Starting BLE HID output and USB HID mouse host");
    esp_err_t err = status_led_start();
    if (err != ESP_OK) {
        ESP_LOGE("mouse_bridge", "Cannot start status LED: %s", esp_err_to_name(err));
        return;
    }

    err = ble_hid_output_start();
    if (err != ESP_OK) {
        status_led_set_fault(STATUS_LED_FAULT_STARTUP, true);
        ESP_LOGE("mouse_bridge", "Cannot start BLE HID: %s", esp_err_to_name(err));
        return;
    }

    err = mouse_bridge_start();
    if (err != ESP_OK) {
        status_led_set_fault(STATUS_LED_FAULT_STARTUP, true);
        ESP_LOGE("mouse_bridge", "Cannot start USB mouse bridge: %s", esp_err_to_name(err));
    }
}

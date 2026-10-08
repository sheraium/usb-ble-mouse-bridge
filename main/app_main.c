#include "ble_hid_output.h"
#include "mouse_bridge.h"

#include "esp_err.h"
#include "esp_log.h"

void app_main(void)
{
    ESP_LOGI("mouse_bridge", "Starting BLE HID output and USB HID mouse host");
    ESP_ERROR_CHECK(ble_hid_output_start());
    ESP_ERROR_CHECK(mouse_bridge_start());
}

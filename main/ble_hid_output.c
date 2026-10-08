#include "ble_hid_output.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_hidd.h"
#include "esp_hid_common.h"
#include "esp_hid_gap.h"
#include "esp_log.h"
#include "status_led.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "store/config/ble_store_config.h"

void ble_store_config_init(void);

static const char *TAG = "ble_hid_mouse";
static esp_hidd_dev_t *s_hid_device;
static unsigned s_consecutive_report_errors;

/* Standard relative mouse report: 8 buttons, X/Y, and both wheel axes. */
static const uint8_t s_mouse_report_map[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x02,       /* Usage (Mouse) */
    0xA1, 0x01,       /* Collection (Application) */
    0x09, 0x01,       /*   Usage (Pointer) */
    0xA1, 0x00,       /*   Collection (Physical) */
    0x05, 0x09,       /*     Usage Page (Button) */
    0x19, 0x01,       /*     Usage Minimum (Button 1) */
    0x29, 0x08,       /*     Usage Maximum (Button 8) */
    0x15, 0x00,       /*     Logical Minimum (0) */
    0x25, 0x01,       /*     Logical Maximum (1) */
    0x95, 0x08,       /*     Report Count (8) */
    0x75, 0x01,       /*     Report Size (1) */
    0x81, 0x02,       /*     Input (Data, Variable, Absolute) */
    0x05, 0x01,       /*     Usage Page (Generic Desktop) */
    0x09, 0x30,       /*     Usage (X) */
    0x09, 0x31,       /*     Usage (Y) */
    0x09, 0x38,       /*     Usage (Wheel) */
    0x15, 0x81,       /*     Logical Minimum (-127) */
    0x25, 0x7F,       /*     Logical Maximum (127) */
    0x75, 0x08,       /*     Report Size (8) */
    0x95, 0x03,       /*     Report Count (3) */
    0x81, 0x06,       /*     Input (Data, Variable, Relative) */
    0x05, 0x0C,       /*     Usage Page (Consumer) */
    0x0A, 0x38, 0x02, /*     Usage (AC Pan) */
    0x95, 0x01,       /*     Report Count (1) */
    0x81, 0x06,       /*     Input (Data, Variable, Relative) */
    0xC0,             /*   End Collection */
    0xC0,             /* End Collection */
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {.data = s_mouse_report_map, .len = sizeof(s_mouse_report_map)},
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id = 0x303A,
    .product_id = 0x4001,
    .version = 0x0100,
    .device_name = "ESP32-S3 Mouse",
    .manufacturer_name = "ESP32 Space",
    .serial_number = "ESP32S3-MOUSE-01",
    .report_maps = s_report_maps,
    .report_maps_len = 1,
};

static void nimble_host_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void hid_event_callback(void *handler_args, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;

    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT: {
        ESP_LOGI(TAG, "HID service ready; advertising as '%s'", s_hid_config.device_name);
        esp_err_t adv_err = esp_hid_ble_gap_adv_start();
        if (adv_err != ESP_OK) {
            status_led_set_fault(STATUS_LED_FAULT_STARTUP, true);
            ESP_LOGE(TAG, "BLE advertising failed: %s", esp_err_to_name(adv_err));
        }
        break;
    }
    case ESP_HIDD_CONNECT_EVENT:
        status_led_set_ble_connected(true);
        status_led_set_fault(STATUS_LED_FAULT_BLE, false);
        ESP_LOGI(TAG, "Laptop connected to BLE HID mouse");
        break;
    case ESP_HIDD_DISCONNECT_EVENT: {
        status_led_set_ble_connected(false);
        status_led_set_fault(STATUS_LED_FAULT_BLE, false);
        ESP_LOGI(TAG, "Laptop disconnected; restarting advertising");
        esp_err_t adv_err = esp_hid_ble_gap_adv_start();
        if (adv_err != ESP_OK) {
            status_led_set_fault(STATUS_LED_FAULT_STARTUP, true);
            ESP_LOGE(TAG, "Could not restart BLE advertising: %s", esp_err_to_name(adv_err));
        }
        break;
    }
    default:
        break;
    }
}

esp_err_t ble_hid_output_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Cannot erase NVS");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "Cannot initialize NVS");

    ESP_RETURN_ON_ERROR(esp_hid_gap_init(HIDD_BLE_MODE), TAG, "Cannot initialize BLE controller");
    ESP_RETURN_ON_ERROR(esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_MOUSE,
                                                  s_hid_config.device_name),
                        TAG, "Cannot initialize BLE advertising");
    ESP_RETURN_ON_ERROR(esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE,
                                           hid_event_callback, &s_hid_device),
                        TAG, "Cannot initialize BLE HID service");
    int gap_name_err = ble_svc_gap_device_name_set(s_hid_config.device_name);
    if (gap_name_err != 0) {
        ESP_LOGE(TAG, "Cannot set BLE GAP device name: %d", gap_name_err);
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(esp_hidd_dev_battery_set(s_hid_device, 100),
                        TAG, "Cannot set HID battery level");

    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "BLE HID mouse initialized; waiting for laptop pairing");
    return ESP_OK;
}

static int8_t clamp_axis(int16_t value)
{
    if (value > 127) return 127;
    if (value < -127) return -127;
    return (int8_t)value;
}

void ble_hid_output_send(const MouseEvent *event)
{
    if (!event || !s_hid_device || !esp_hidd_dev_connected(s_hid_device)) return;
    status_led_mouse_activity();

    /* BLE report has 8 buttons, then relative X/Y/vertical/horizontal wheel. */
    int32_t dx_left = event->dx;
    int32_t dy_left = event->dy;
    bool first = true;
    do {
        uint8_t report[5] = {
            event->buttons,
            (uint8_t)clamp_axis(dx_left),
            (uint8_t)clamp_axis(dy_left),
            first ? (uint8_t)event->wheel : 0,
            first ? (uint8_t)event->horizontal_wheel : 0,
        };
        esp_err_t err = esp_hidd_dev_input_set(s_hid_device, 0, 0, report, sizeof(report));
        if (err != ESP_OK) {
            if (++s_consecutive_report_errors >= 3) {
                status_led_set_fault(STATUS_LED_FAULT_BLE, true);
            }
            ESP_LOGW(TAG, "BLE mouse report failed: %s", esp_err_to_name(err));
        } else {
            s_consecutive_report_errors = 0;
            status_led_set_fault(STATUS_LED_FAULT_BLE, false);
        }
        dx_left -= (int8_t)report[1];
        dy_left -= (int8_t)report[2];
        first = false;
    } while (dx_left || dy_left);
}

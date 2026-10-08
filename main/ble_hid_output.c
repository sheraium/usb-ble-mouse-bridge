#include "ble_hid_output.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_hidd.h"
#include "esp_hid_common.h"
#include "esp_hid_gap.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
static volatile bool s_pairing_window_open;
static volatile TickType_t s_pairing_window_deadline;
static uint16_t s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_new_peer_in_progress;

#define PAIRING_BUTTON_GPIO GPIO_NUM_0
#define PAIRING_WINDOW_SECONDS 60
#define PAIRING_BUTTON_LONG_PRESS_MS 5000
#define PAIRING_BUTTON_DEBOUNCE_MS 40
#define MAX_BONDED_PEERS 3

static void set_pairing_window(bool open);

static int reject_bond_store_overflow(struct ble_store_status_event *event, void *arg)
{
    (void)arg;
    if (event->event_code == BLE_STORE_EVENT_OVERFLOW) {
        ESP_LOGW(TAG, "Bond store full; refusing to evict a saved device");
        return BLE_HS_ENOMEM;
    }
    return 0;
}

static int bonded_peer_count(void)
{
    ble_addr_t peers[MAX_BONDED_PEERS];
    int count = 0;
    int rc = ble_store_util_bonded_peers(peers, &count, MAX_BONDED_PEERS);
    if (rc != 0) {
        ESP_LOGW(TAG, "Could not count bonded devices: rc=%d", rc);
        return MAX_BONDED_PEERS;
    }
    return count;
}

bool ble_hid_connection_is_bonded(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    struct ble_store_key_sec key = {0};
    struct ble_store_value_sec value = {0};
    if (ble_gap_conn_find(conn_handle, &desc) != 0) return false;
    key.peer_addr = desc.peer_id_addr;
    return ble_store_read_peer_sec(&key, &value) == 0;
}

bool ble_hid_new_pairing_allowed(void)
{
    if (!s_pairing_window_open) return false;
    if (bonded_peer_count() >= MAX_BONDED_PEERS) {
        ESP_LOGW(TAG, "Pairing rejected: all %d bond slots are in use", MAX_BONDED_PEERS);
        return false;
    }
    return true;
}

void ble_hid_connection_opened(uint16_t conn_handle)
{
    s_connection_handle = conn_handle;
    s_new_peer_in_progress = !ble_hid_connection_is_bonded(conn_handle);
}

void ble_hid_connection_closed(void)
{
    s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
    s_new_peer_in_progress = false;
}

void ble_hid_new_pairing_completed(void)
{
    if (s_new_peer_in_progress) {
        s_new_peer_in_progress = false;
        set_pairing_window(false);
    }
}

static void set_pairing_window(bool open)
{
    s_pairing_window_open = open;
    status_led_set_pairing_window(open);
    if (open) {
        s_pairing_window_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PAIRING_WINDOW_SECONDS * 1000);
        ESP_LOGI(TAG, "New-device pairing open for %d seconds", PAIRING_WINDOW_SECONDS);
    } else {
        s_pairing_window_deadline = 0;
        ESP_LOGI(TAG, "New-device pairing closed");
    }
}

static void clear_all_bonds(void)
{
    ble_addr_t peers[MAX_BONDED_PEERS];
    int count = 0;
    int rc = ble_store_util_bonded_peers(peers, &count, MAX_BONDED_PEERS);
    if (rc != 0) {
        ESP_LOGE(TAG, "Cannot list bonds for clearing: rc=%d", rc);
        return;
    }
    for (int i = 0; i < count; ++i) {
        rc = ble_store_util_delete_peer(&peers[i]);
        if (rc != 0) {
            ESP_LOGE(TAG, "Could not delete bond %d: rc=%d", i + 1, rc);
            return;
        }
    }
    ESP_LOGW(TAG, "Cleared all %d bonded device(s)", count);
    status_led_show_bonds_cleared();
    if (s_connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(s_connection_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void pairing_button_task(void *arg)
{
    (void)arg;
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << PAIRING_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    ESP_LOGI(TAG, "BOOT button: short press opens pairing for 60s; hold 5s clears all bonds");

    bool pressed = false;
    bool long_press_handled = false;
    TickType_t pressed_at = 0;
    TickType_t debounce_at = 0;
    int stable_level = 1;
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        int level = gpio_get_level(PAIRING_BUTTON_GPIO);
        if (level != stable_level && now - debounce_at >= pdMS_TO_TICKS(PAIRING_BUTTON_DEBOUNCE_MS)) {
            stable_level = level;
            debounce_at = now;
            if (stable_level == 0) {
                pressed = true;
                long_press_handled = false;
                pressed_at = now;
            } else if (pressed) {
                pressed = false;
                if (!long_press_handled && now - pressed_at >= pdMS_TO_TICKS(PAIRING_BUTTON_DEBOUNCE_MS)) {
                    set_pairing_window(true);
                }
            }
        }
        if (pressed && !long_press_handled && now - pressed_at >= pdMS_TO_TICKS(PAIRING_BUTTON_LONG_PRESS_MS)) {
            long_press_handled = true;
            set_pairing_window(false);
            clear_all_bonds();
        }
        if (s_pairing_window_open && s_pairing_window_deadline &&
            (int32_t)(now - s_pairing_window_deadline) >= 0) {
            set_pairing_window(false);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

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
    ble_hs_cfg.store_status_cb = reject_bond_store_overflow;
    nimble_port_freertos_init(nimble_host_task);
    if (xTaskCreate(pairing_button_task, "pairing_button", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
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

#include "usb_host_inspector.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "hid_mouse_parser.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

static const char *TAG = "usb_inspector";

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
} hid_event_t;

static QueueHandle_t s_hid_events;
static usb_mouse_event_callback_t s_mouse_callback;
static void *s_mouse_callback_arg;

static void print_wide_ascii(const char *label, const wchar_t *value)
{
    char text[HID_STR_DESC_MAX_LENGTH + 1] = {0};
    size_t out = 0;
    for (size_t i = 0; i < HID_STR_DESC_MAX_LENGTH && value[i] != L'\0'; ++i) {
        wchar_t ch = value[i];
        text[out++] = (ch >= 0x20 && ch <= 0x7e) ? (char)ch : '?';
    }
    ESP_LOGI(TAG, "%s: %s", label, out ? text : "<not provided>");
}

static void dump_bytes(const char *label, const uint8_t *data, size_t length)
{
    ESP_LOGI(TAG, "%s (%u bytes):", label, (unsigned)length);
    for (size_t offset = 0; offset < length; offset += 16) {
        char line[16 * 3 + 1] = {0};
        size_t used = 0;
        size_t end = (offset + 16 < length) ? offset + 16 : length;
        for (size_t i = offset; i < end; ++i) {
            used += (size_t)snprintf(line + used, sizeof(line) - used,
                                     "%02X%s", data[i], (i + 1 < end) ? " " : "");
        }
        ESP_LOGI(TAG, "  %04X: %s", (unsigned)offset, line);
    }
}

static void hid_interface_callback(hid_host_device_handle_t handle,
                                   const hid_host_interface_event_t event,
                                   void *arg)
{
    (void)arg;
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        uint8_t report[256];
        size_t report_length = 0;
        esp_err_t err = hid_host_device_get_raw_input_report_data(
            handle, report, sizeof(report), &report_length);
        if (err == ESP_OK) {
            MouseEvent mouse = {0};
            hid_mouse_parser_t *parser = arg;
            if (parser && hid_mouse_parser_parse(parser, report, report_length, &mouse)) {
                ESP_LOGD(TAG, "MouseEvent dx=%d dy=%d wheel=%d h_wheel=%d buttons=0x%02X",
                         mouse.dx, mouse.dy, mouse.wheel, mouse.horizontal_wheel, mouse.buttons);
                if (s_mouse_callback) s_mouse_callback(&mouse, s_mouse_callback_arg);
            } else {
                dump_bytes("Raw input report", report, report_length);
            }
        } else {
            ESP_LOGW(TAG, "Could not read input report: %s", esp_err_to_name(err));
        }
    } else if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
        ESP_LOGW(TAG, "HID transfer error");
    } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "HID interface disconnected; closing interface");
        free(arg);
        esp_err_t err = hid_host_device_close(handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HID close after disconnect: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "HID interface event %d", (int)event);
    }
}

static void hid_driver_callback(hid_host_device_handle_t handle,
                                const hid_host_driver_event_t event,
                                void *arg)
{
    (void)arg;
    const hid_event_t queued = {.handle = handle, .event = event};
    if (s_hid_events && xQueueSend(s_hid_events, &queued, 0) != pdTRUE) {
        ESP_LOGW(TAG, "HID event queue full; connection event dropped");
    }
}

static void inspect_connected_interface(hid_host_device_handle_t handle)
{
    hid_host_dev_params_t params = {0};
    hid_host_dev_info_t info = {0};
    ESP_ERROR_CHECK(hid_host_device_get_params(handle, &params));
    ESP_LOGI(TAG, "HID interface found: USB address=%u interface=%u subclass=0x%02X protocol=0x%02X",
             params.addr, params.iface_num, params.sub_class, params.proto);

    hid_mouse_parser_t *parser = calloc(1, sizeof(*parser));
    if (!parser) {
        ESP_LOGE(TAG, "Out of memory for HID parser");
        return;
    }
    esp_err_t err = hid_host_device_open(handle, &(hid_host_device_config_t){
        .callback = hid_interface_callback,
        .callback_arg = parser,
    });
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open interface %u: %s", params.iface_num, esp_err_to_name(err));
        return;
    }

    if (hid_host_get_device_info(handle, &info) == ESP_OK) {
        ESP_LOGI(TAG, "VID:PID = %04" PRIX16 ":%04" PRIX16, info.VID, info.PID);
        print_wide_ascii("Manufacturer", info.iManufacturer);
        print_wide_ascii("Product", info.iProduct);
        print_wide_ascii("Serial number", info.iSerialNumber);
    } else {
        ESP_LOGW(TAG, "Device strings unavailable");
    }

    size_t descriptor_length = 0;
    uint8_t *descriptor = hid_host_get_report_descriptor(handle, &descriptor_length);
    if (descriptor && descriptor_length) {
        dump_bytes("HID report descriptor", descriptor, descriptor_length);
        err = hid_mouse_parser_init(parser, descriptor, descriptor_length);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Standard mouse parser ready (%u mapped fields)",
                     (unsigned)parser->field_count);
        } else {
            ESP_LOGI(TAG, "Interface is not a supported standard mouse (%s)", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "HID report descriptor unavailable");
    }

    ESP_LOGW(TAG, "The public usb_host_hid API does not expose endpoint descriptors; "
                  "this build logs HID interface metadata and report descriptors.");

    err = hid_host_device_start(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start input reports on interface %u: %s",
                 params.iface_num, esp_err_to_name(err));
        (void)hid_host_device_close(handle);
    } else {
        ESP_LOGI(TAG, "Listening for raw reports on interface %u", params.iface_num);
    }
}

void usb_host_inspector_set_mouse_callback(usb_mouse_event_callback_t callback, void *arg)
{
    s_mouse_callback = callback;
    s_mouse_callback_arg = arg;
}

static void hid_event_task(void *arg)
{
    (void)arg;
    hid_event_t event;
    for (;;) {
        if (xQueueReceive(s_hid_events, &event, portMAX_DELAY) == pdTRUE &&
            event.event == HID_HOST_DRIVER_EVENT_CONNECTED) {
            inspect_connected_interface(event.handle);
        }
    }
}

static void usb_library_task(void *arg)
{
    TaskHandle_t startup_waiter = (TaskHandle_t)arg;
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    ESP_ERROR_CHECK(usb_host_install(&config));
    xTaskNotifyGive(startup_waiter);

    for (;;) {
        uint32_t event_flags = 0;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "USB library event handler: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t usb_host_inspector_start(void)
{
    s_hid_events = xQueueCreate(8, sizeof(hid_event_t));
    if (!s_hid_events) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(hid_event_task, "hid_events", 4096, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_hid_events);
        s_hid_events = NULL;
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    if (xTaskCreate(usb_library_task, "usb_library", 4096, caller, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));

    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = tskNO_AFFINITY,
        .callback = hid_driver_callback,
        .callback_arg = NULL,
    };
    return hid_host_install(&hid_config);
}

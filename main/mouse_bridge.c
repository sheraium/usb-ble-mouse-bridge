#include "mouse_bridge.h"

#include "ble_hid_output.h"
#include "usb_host_inspector.h"

#include <inttypes.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define MOUSE_EVENT_QUEUE_LENGTH 32

static const char *TAG = "mouse_bridge";
static QueueHandle_t s_mouse_events;
static uint32_t s_dropped_events;

static void on_usb_mouse_event(const MouseEvent *event, void *arg)
{
    (void)arg;
    if (xQueueSend(s_mouse_events, event, 0) != pdTRUE) {
        ++s_dropped_events;
        if ((s_dropped_events & 0x1F) == 1) {
            ESP_LOGW(TAG, "Mouse event queue full; dropped %" PRIu32 " events",
                     s_dropped_events);
        }
    }
}

static void mouse_output_task(void *arg)
{
    (void)arg;
    MouseEvent event;
    for (;;) {
        if (xQueueReceive(s_mouse_events, &event, portMAX_DELAY) == pdTRUE) {
            ble_hid_output_send(&event);
        }
    }
}

esp_err_t mouse_bridge_start(void)
{
    s_mouse_events = xQueueCreate(MOUSE_EVENT_QUEUE_LENGTH, sizeof(MouseEvent));
    if (!s_mouse_events) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(mouse_output_task, "mouse_output", 3072, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_mouse_events);
        s_mouse_events = NULL;
        return ESP_ERR_NO_MEM;
    }

    usb_host_inspector_set_mouse_callback(on_usb_mouse_event, NULL);
    return usb_host_inspector_start();
}

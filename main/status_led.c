#include "status_led.h"

#include <string.h>

#include "driver/rmt_encoder.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Goouuu ESP32-S3 N16R8 dual-USB-C board's onboard WS2812 RGB LED.
#define STATUS_LED_GPIO GPIO_NUM_38
#define RMT_RESOLUTION_HZ 10000000
#define WS2812_DATA_BITS 24
#define WS2812_TOTAL_SYMBOLS (WS2812_DATA_BITS + 1)
#define STATUS_LED_ACTIVITY_MS 180

static const char *TAG = "status_led";
static rmt_channel_handle_t s_led_channel;
static rmt_encoder_handle_t s_led_encoder;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ble_connected;
static bool s_receiver_connected;
static uint32_t s_faults;
static TickType_t s_activity_until;

static size_t ws2812_encode(const void *data, size_t data_size,
                            size_t symbols_written, size_t symbols_free,
                            rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    (void)arg;
    if (data_size != 3 || symbols_written >= WS2812_TOTAL_SYMBOLS) {
        *done = true;
        return 0;
    }

    const uint8_t *grb = data;
    size_t count = WS2812_TOTAL_SYMBOLS - symbols_written;
    if (count > symbols_free) count = symbols_free;

    for (size_t i = 0; i < count; ++i) {
        size_t position = symbols_written + i;
        if (position == WS2812_DATA_BITS) {
            // 50 us low reset/latch interval.
            symbols[i] = (rmt_symbol_word_t){
                .level0 = 0, .duration0 = 250,
                .level1 = 0, .duration1 = 250,
            };
            continue;
        }

        bool bit = (grb[position / 8] & (1u << (7 - (position % 8)))) != 0;
        symbols[i] = bit
            ? (rmt_symbol_word_t){.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3}
            : (rmt_symbol_word_t){.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9};
    }

    *done = (symbols_written + count) == WS2812_TOTAL_SYMBOLS;
    return count;
}

static esp_err_t led_write(uint8_t red, uint8_t green, uint8_t blue)
{
    // WS2812 uses GRB byte order.
    uint8_t grb[] = {green, red, blue};
    const rmt_transmit_config_t tx_config = {.loop_count = 0};
    ESP_RETURN_ON_ERROR(rmt_transmit(s_led_channel, s_led_encoder, grb, sizeof(grb), &tx_config),
                        TAG, "RGB LED transmit failed");
    return rmt_tx_wait_all_done(s_led_channel, pdMS_TO_TICKS(100));
}

static void color_for_tick(TickType_t now, uint8_t rgb[3])
{
    bool ble_connected;
    bool receiver_connected;
    bool activity;
    uint32_t faults;

    portENTER_CRITICAL(&s_state_mux);
    ble_connected = s_ble_connected;
    receiver_connected = s_receiver_connected;
    faults = s_faults;
    activity = ble_connected && receiver_connected &&
               (int32_t)(s_activity_until - now) > 0;
    portEXIT_CRITICAL(&s_state_mux);

    if (faults) {
        bool on = ((now / pdMS_TO_TICKS(250)) & 1u) == 0;
        rgb[0] = on ? 64 : 0;
        rgb[1] = 0;
        rgb[2] = 0;
    } else if (!ble_connected) {
        bool on = ((now / pdMS_TO_TICKS(500)) & 1u) == 0;
        rgb[0] = 0;
        rgb[1] = 0;
        rgb[2] = on ? 56 : 0;
    } else if (!receiver_connected) {
        bool on = ((now / pdMS_TO_TICKS(500)) & 1u) == 0;
        rgb[0] = on ? 48 : 0;
        rgb[1] = on ? 18 : 0;
        rgb[2] = 0;
    } else if (activity) {
        bool on = ((now / pdMS_TO_TICKS(40)) & 1u) == 0;
        rgb[0] = 0;
        rgb[1] = on ? 80 : 0;
        rgb[2] = on ? 88 : 0;
    } else {
        rgb[0] = 0;
        rgb[1] = 80;
        rgb[2] = 0;
    }
}

static void status_led_task(void *arg)
{
    (void)arg;
    uint8_t previous[3] = {UINT8_MAX, UINT8_MAX, UINT8_MAX};
    uint8_t current[3];
    for (;;) {
        color_for_tick(xTaskGetTickCount(), current);
        if (memcmp(current, previous, sizeof(current)) != 0) {
            esp_err_t err = led_write(current[0], current[1], current[2]);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Could not update RGB LED: %s", esp_err_to_name(err));
            } else {
                memcpy(previous, current, sizeof(previous));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t status_led_start(void)
{
    const rmt_tx_channel_config_t channel_config = {
        .gpio_num = STATUS_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&channel_config, &s_led_channel), TAG,
                        "Cannot create RGB LED RMT channel");

    const rmt_simple_encoder_config_t encoder_config = {
        .callback = ws2812_encode,
        .min_chunk_size = WS2812_TOTAL_SYMBOLS,
    };
    ESP_RETURN_ON_ERROR(rmt_new_simple_encoder(&encoder_config, &s_led_encoder), TAG,
                        "Cannot create RGB LED encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(s_led_channel), TAG, "Cannot enable RGB LED RMT channel");

    if (xTaskCreate(status_led_task, "status_led", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "RGB status LED ready on GPIO%d", STATUS_LED_GPIO);
    return ESP_OK;
}

void status_led_set_ble_connected(bool connected)
{
    portENTER_CRITICAL(&s_state_mux);
    s_ble_connected = connected;
    portEXIT_CRITICAL(&s_state_mux);
}

void status_led_set_receiver_connected(bool connected)
{
    portENTER_CRITICAL(&s_state_mux);
    s_receiver_connected = connected;
    portEXIT_CRITICAL(&s_state_mux);
}

void status_led_set_fault(status_led_fault_t fault, bool active)
{
    portENTER_CRITICAL(&s_state_mux);
    if (active) s_faults |= fault;
    else s_faults &= ~((uint32_t)fault);
    portEXIT_CRITICAL(&s_state_mux);
}

void status_led_mouse_activity(void)
{
    portENTER_CRITICAL(&s_state_mux);
    s_activity_until = xTaskGetTickCount() + pdMS_TO_TICKS(STATUS_LED_ACTIVITY_MS);
    portEXIT_CRITICAL(&s_state_mux);
}

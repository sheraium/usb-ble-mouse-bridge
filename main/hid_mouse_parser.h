#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mouse_event.h"

typedef struct {
    uint16_t bit_offset;
    uint8_t bit_size;
    int32_t logical_min;
    int32_t logical_max;
    uint16_t usage_page;
    uint16_t usage;
    uint8_t report_id;
    bool relative;
} hid_mouse_field_t;

typedef struct {
    hid_mouse_field_t fields[16];
    size_t field_count;
    uint16_t report_bits[256];
    bool uses_report_ids;
    bool mouse_collection;
} hid_mouse_parser_t;

esp_err_t hid_mouse_parser_init(hid_mouse_parser_t *parser,
                                const uint8_t *descriptor, size_t length);
bool hid_mouse_parser_parse(const hid_mouse_parser_t *parser,
                            const uint8_t *report, size_t length,
                            MouseEvent *event);

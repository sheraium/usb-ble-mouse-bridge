#include "hid_mouse_parser.h"

#include <string.h>

#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01
#define HID_USAGE_PAGE_BUTTON          0x09
#define HID_USAGE_PAGE_CONSUMER        0x0C
#define HID_USAGE_MOUSE                0x02
#define HID_USAGE_X                    0x30
#define HID_USAGE_Y                    0x31
#define HID_USAGE_WHEEL                0x38
#define HID_USAGE_AC_PAN               0x0238

typedef struct {
    uint16_t usage_page;
    uint16_t usage_min;
    uint16_t usage_max;
    uint32_t report_size;
    uint32_t report_count;
    int32_t logical_min;
    int32_t logical_max;
    uint8_t report_id;
    bool relative;
    bool constant;
} hid_global_t;

static uint32_t item_value(const uint8_t *data, size_t size)
{
    uint32_t v = 0;
    for (size_t i = 0; i < size; ++i) v |= (uint32_t)data[i] << (8 * i);
    return v;
}

static int32_t sign_extend(uint32_t value, size_t size)
{
    if (size == 0) return 0;
    if (size >= 4) return (int32_t)value;
    uint32_t sign = 1u << (size * 8 - 1);
    return (int32_t)((value ^ sign) - sign);
}

static int32_t sign_extend_bits(uint32_t value, uint8_t bit_size)
{
    if (!bit_size || bit_size >= 32) return (int32_t)value;
    uint32_t sign = 1u << (bit_size - 1);
    return (int32_t)((value ^ sign) - sign);
}

esp_err_t hid_mouse_parser_init(hid_mouse_parser_t *p,
                                const uint8_t *desc, size_t len)
{
    if (!p || !desc || !len) return ESP_ERR_INVALID_ARG;
    memset(p, 0, sizeof(*p));
    hid_global_t g = {0};
    hid_global_t stack[4];
    size_t stack_depth = 0;
    uint16_t local_usage[32];
    size_t local_count = 0;
    uint8_t collection_depth = 0;
    uint8_t mouse_collection_depth = 0;
    size_t offset = 0;

    while (offset < len) {
        uint8_t prefix = desc[offset++];
        if (prefix == 0xFE) {
            if (offset + 2 > len) return ESP_ERR_INVALID_SIZE;
            size_t n = desc[offset]; offset += 2;
            if (offset + n > len) return ESP_ERR_INVALID_SIZE;
            offset += n;
            continue;
        }
        size_t n = prefix & 0x03;
        if (n == 3) n = 4;
        if (offset + n > len) return ESP_ERR_INVALID_SIZE;
        uint32_t value = item_value(desc + offset, n);
        offset += n;
        uint8_t type = (prefix >> 2) & 0x03;
        uint8_t tag = (prefix >> 4) & 0x0F;

        if (type == 1) { // Global
            switch (tag) {
            case 0: g.usage_page = (uint16_t)value; break;
            case 1: g.logical_min = sign_extend(value, n); break;
            case 2: g.logical_max = g.logical_min < 0 ? sign_extend(value, n) : (int32_t)value; break;
            case 7: g.report_size = value; break;
            case 8:
                if (!value || value > 255) return ESP_ERR_INVALID_ARG;
                g.report_id = (uint8_t)value; p->uses_report_ids = true; break;
            case 9: g.report_count = value; break;
            case 10:
                if (stack_depth >= sizeof(stack)/sizeof(stack[0])) return ESP_ERR_NO_MEM;
                stack[stack_depth++] = g; break;
            case 11:
                if (!stack_depth) return ESP_ERR_INVALID_STATE;
                g = stack[--stack_depth]; break;
            default: break;
            }
        } else if (type == 2) { // Local
            if (tag == 0 && local_count < sizeof(local_usage)/sizeof(local_usage[0])) {
                local_usage[local_count++] = (uint16_t)value;
            } else if (tag == 1) {
                g.usage_min = (uint16_t)value;
            } else if (tag == 2) {
                g.usage_max = (uint16_t)value;
            }
        } else if (type == 0) { // Main
            if (tag == 10) { // Collection
                uint16_t usage = local_count ? local_usage[0] : g.usage_min;
                uint16_t page = g.usage_page;
                if (local_count && usage > 0xFF) {
                    page = usage >> 8;
                    usage &= 0xFF;
                }
                if (collection_depth < UINT8_MAX) ++collection_depth;
                if (value == 1 && page == HID_USAGE_PAGE_GENERIC_DESKTOP && usage == HID_USAGE_MOUSE) {
                    mouse_collection_depth = collection_depth;
                    p->mouse_collection = true;
                }
            } else if (tag == 8) { // Input
                bool constant = (value & 0x01) != 0;
                bool relative = (value & 0x04) != 0;
                uint32_t bits = g.report_size * g.report_count;
                if (bits > UINT16_MAX || p->report_bits[g.report_id] + bits > UINT16_MAX)
                    return ESP_ERR_INVALID_SIZE;
                uint16_t base = p->report_bits[g.report_id];
                if (mouse_collection_depth && collection_depth >= mouse_collection_depth &&
                    !constant && g.report_size && g.report_size <= 32) {
                    for (uint32_t i = 0; i < g.report_count && p->field_count < 16; ++i) {
                        uint16_t usage = i < local_count ? local_usage[i] : (uint16_t)(g.usage_min + i);
                        uint16_t page = g.usage_page;
                        if (usage > 0xFF && page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
                            page = usage >> 8;
                            usage &= 0xFF;
                        }
                        if ((page == HID_USAGE_PAGE_BUTTON && usage <= 8) ||
                            (page == HID_USAGE_PAGE_GENERIC_DESKTOP &&
                             (usage == HID_USAGE_X || usage == HID_USAGE_Y ||
                              usage == HID_USAGE_WHEEL)) ||
                            (page == HID_USAGE_PAGE_CONSUMER && usage == HID_USAGE_AC_PAN)) {
                            hid_mouse_field_t *f = &p->fields[p->field_count++];
                            *f = (hid_mouse_field_t){
                                .bit_offset = (uint16_t)(base + i * g.report_size),
                                .bit_size = (uint8_t)g.report_size,
                                .logical_min = g.logical_min, .logical_max = g.logical_max,
                                .usage_page = page, .usage = usage,
                                .report_id = g.report_id, .relative = relative,
                            };
                        }
                    }
                }
                p->report_bits[g.report_id] = (uint16_t)(base + bits);
            } else if (tag == 12) { // End Collection
                if (mouse_collection_depth && mouse_collection_depth == collection_depth)
                    mouse_collection_depth = 0;
                if (collection_depth) --collection_depth;
            }
            local_count = 0;
            g.usage_min = g.usage_max = 0;
        }
    }
    return p->mouse_collection && p->field_count ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static int32_t extract_bits(const uint8_t *data, size_t len,
                            uint16_t bit_offset, uint8_t bit_size, bool is_signed)
{
    if (!bit_size || bit_size > 32 || bit_offset + bit_size > len * 8) return 0;
    uint32_t value = 0;
    for (uint8_t i = 0; i < bit_size; ++i) {
        size_t bit = bit_offset + i;
        if (data[bit / 8] & (1u << (bit % 8))) value |= 1u << i;
    }
    return is_signed ? sign_extend_bits(value, bit_size) : (int32_t)value;
}

bool hid_mouse_parser_parse(const hid_mouse_parser_t *p,
                            const uint8_t *report, size_t len,
                            MouseEvent *event)
{
    if (!p || !report || !len || !event) return false;
    memset(event, 0, sizeof(*event));
    uint8_t report_id = 0;
    size_t skip = 0;
    if (p->uses_report_ids) { report_id = report[0]; skip = 1; }
    if (skip >= len) return false;
    for (size_t i = 0; i < p->field_count; ++i) {
        const hid_mouse_field_t *f = &p->fields[i];
        if (f->report_id != report_id) continue;
        int32_t v = extract_bits(report + skip, len - skip, f->bit_offset,
                                 f->bit_size, f->logical_min < 0);
        if (f->usage_page == HID_USAGE_PAGE_BUTTON) {
            if (v && f->usage >= 1 && f->usage <= 8) event->buttons |= (uint8_t)(1u << (f->usage - 1));
        } else if (f->usage == HID_USAGE_X) event->dx = (int16_t)v;
        else if (f->usage == HID_USAGE_Y) event->dy = (int16_t)v;
        else if (f->usage == HID_USAGE_WHEEL) event->wheel = (int8_t)v;
        else if (f->usage_page == HID_USAGE_PAGE_CONSUMER && f->usage == HID_USAGE_AC_PAN)
            event->horizontal_wheel = (int8_t)v;
    }
    return true;
}

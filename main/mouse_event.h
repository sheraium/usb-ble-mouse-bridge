#pragma once

#include <stdint.h>

typedef struct {
    int16_t dx;
    int16_t dy;
    int8_t wheel;
    int8_t horizontal_wheel;
    uint8_t buttons;
} MouseEvent;

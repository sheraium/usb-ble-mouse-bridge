#pragma once

#include "esp_err.h"
#include "mouse_event.h"

typedef void (*usb_mouse_event_callback_t)(const MouseEvent *event, void *arg);

/** Start USB Host and HID class drivers. Returns after the host is ready. */
esp_err_t usb_host_inspector_start(void);
void usb_host_inspector_set_mouse_callback(usb_mouse_event_callback_t callback, void *arg);

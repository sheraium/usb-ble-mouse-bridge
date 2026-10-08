idf_component_register(
    SRCS "app_main.c" "mouse_bridge.c" "status_led.c" "usb_host_inspector.c" "hid_mouse_parser.c" "mouse_event.c" "ble_hid_output.c" "esp_hid_gap.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES usb_host_hid esp_hid nvs_flash esp_driver_rmt
)

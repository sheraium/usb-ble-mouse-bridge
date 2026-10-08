# ESP32-S3 USB-to-BLE Mouse Bridge

The firmware starts a USB Host HID class driver and a BLE HID mouse. A
descriptor-driven parser extracts standard mouse buttons, X/Y, and both wheel
axes from mouse reports, normalizes them to `MouseEvent`, then sends them to the
paired BLE HID host. Unsupported HID interfaces continue to print raw input
reports for inspection.

## Requirements

- ESP-IDF 6.1 with the `esp32s3` target installed (verified build environment)
- Component Manager access to `espressif/usb_host_hid` 1.2.1
- ESP-IDF `esp_hid` component with NimBLE HID service enabled by defaults
- USB OTG host port wired for host operation and powered VBUS
- The expected board has 16 MB flash; `sdkconfig.defaults` selects that size.

## Build

```sh
cd usb_ble_mouse_bridge
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py set-target esp32s3
idf.py build
```

## Flash and monitor

Replace `/dev/cu.usbmodemXXXX` with the serial port shown by `ls /dev/cu.*`.
Use the board's programming USB-C port for flashing and serial monitoring.

```sh
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Exit the monitor with `Ctrl+]`.

The ESP-IDF `usb_host_hid` API reports the HID interface number, subclass,
protocol, VID/PID, strings, report descriptor, and input data. Its public HID
handle does not expose USB endpoint descriptors, so this firmware does not log
endpoint metadata.

## Current bridge behavior

Flash and monitor the board, connect **ESP32-S3 Mouse** from macOS System
Settings → Bluetooth, then plug the receiver into the board's native USB-OTG
host port. The serial log shows normalized mouse events. USB input is sent over
BLE when the Mac is connected. The parser is generic for standard HID mouse
fields and forwards up to eight standard buttons, including side buttons, plus
vertical and horizontal wheel values. It does not interpret vendor-specific
Logitech HID++ features.

## Hardware verification

The firmware has been exercised on the target ESP32-S3 board with a Logitech
Unifying receiver and two different Logitech mouse receivers. Mouse movement,
left/right/middle clicks, vertical scrolling, and the M325S previous/next
buttons were verified through macOS Bluetooth. The receiver was unplugged and
reconnected without rebooting the board. Horizontal scrolling is parsed and
forwarded when exposed by the USB HID report descriptor; verify it with each
receiver/mouse combination.

The firmware does not implement Logitech HID++ features. Endpoint descriptor
inspection and board-specific VBUS electrical characterization are not part of
the current public USB HID API/firmware path.

## License

This project is distributed under the MIT License; see `LICENSE`.

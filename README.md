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

## RGB status LED

On the Goouuu ESP32-S3 N16R8 dual-USB-C board, the onboard WS2812 RGB LED is
driven through GPIO48 in this build. Goouuu N16R8 board revisions vary; some
revisions may use GPIO38 instead.

- Slow blue blink: waiting for a BLE host connection.
- Solid green: BLE connected and a standard USB mouse interface is present.
- Fast cyan pulses: mouse reports are being forwarded over BLE; returns to
  solid green after activity stops.
- Slow orange blink: BLE connected, but no supported USB mouse interface is
  present.
- Pulsing red: startup failure or repeated USB/BLE report errors.
- Fast blue blink: the 60-second new-device pairing window is open.
- Fast red blink for about one second: all saved bonds were cleared.

## BLE pairing controls

The BOOT button (GPIO0) controls new pairing. A short press opens pairing for
60 seconds; a newly bonded computer closes the window. Previously bonded
computers can reconnect without opening it. Holding BOOT for 5 seconds clears
all saved bonds and disconnects the current BLE host; it does not open pairing.
Press BOOT briefly afterward to pair again. Up to three computers are retained.
When all three slots are occupied, new devices are rejected and existing bonds
are not automatically evicted. RESET only restarts the board and keeps bonds.

## License

This project is distributed under the MIT License; see `LICENSE`.

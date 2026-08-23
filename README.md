# ESP32-C5 CAN FD Gateway

An ESP-IDF application for the ESP32-C5 that bridges two on-chip TWAI-FD
controllers to a Wi-Fi network over UDP. The firmware also provides a small
I2C status panel, RTC timekeeping, queue/drop diagnostics, and an optional
high-rate CAN FD traffic generator for bus testing.

## Features

- Two independent CAN FD channels using the ESP32-C5 TWAI peripherals.
- 1 Mbit/s arbitration phase and 5 Mbit/s data phase.
- CAN FD payloads up to 64 bytes.
- Wi-Fi station mode with automatic reconnect.
- UDP output for received CAN frames and UDP input for CAN transmission.
- OLED status display, DS3231MZ RTC, and PCF8574 I/O expander on one I2C bus.
- Large PSRAM-backed receive pool with separate logging and routing queues.
- CAN health monitoring and automatic recovery after repeated error states.
- Built-in burst traffic generator for development and stress testing.

## Hardware

The firmware targets an **ESP32-C5** development board or custom board with:

- Two external CAN FD transceivers. The ESP32-C5 GPIOs are logic-level TWAI
  signals; they must not be connected directly to a CAN bus.
- A 3.3 V I2C bus with pull-ups.
- Optional I2C OLED, DS3231MZ RTC, and PCF8574 modules.
- PSRAM enabled and available to the application. The receive pool allocates
  approximately 1.1 MiB from PSRAM at startup.

See [PINOUT.md](PINOUT.md) for the complete connection table and electrical
notes.

## Data Flow

```text
CAN transceiver 1 -> ESP32-C5 TWAI node 1 --+
                                             +-> receive pool -> UDP host
CAN transceiver 2 -> ESP32-C5 TWAI node 2 --+

UDP command host -> ESP32-C5 -> selected TWAI node -> CAN transceiver
```

Each received CAN frame is copied into a shared pool and made available to the
UDP sender and routing/diagnostic path. Frames are sent to the host in batches
of up to 20 records, or after a short flush timeout.

## UDP Interface

The current network values are compile-time definitions in `main/main.c`:

| Setting | Current value | Meaning |
| --- | --- | --- |
| Destination host | `192.168.178.61` | Computer receiving CAN frames |
| TX port | `3333` | ESP32-C5 to host |
| RX port | `3334` | Host to ESP32-C5 |

The UDP payload is a packed binary structure. A single record contains:

| Field | Size | Description |
| --- | ---: | --- |
| `timestamp` | 4 bytes | Milliseconds from ESP timer startup |
| `node_id` | 1 byte | `1` or `2` |
| `id` | 4 bytes | CAN identifier; bit 31 marks an extended identifier |
| `dlc` | 1 byte | CAN FD DLC code |
| `data` | 64 bytes | CAN payload storage; unused bytes are unspecified |

The host-to-device command uses the same field layout. Set `node_id` to `1` or
`2` to select the CAN channel. The firmware bounds the DLC before transmission.
The protocol is intentionally simple and currently has no version, sequence,
endianness, or integrity field. A host implementation must use the target's
little-endian packed representation.

## Traffic Generator

The generator is enabled by default in `main/main.c`. Every two seconds it
attempts a burst of 1,000 64-byte CAN FD frames on each channel, using IDs
`0x55` and `0x66`, respectively. Payload bytes alternate between `0xAA` and
`0x55`.

Disable it by changing `enable_test_generator` to `false` before building. The
generator is intended for a correctly terminated test bus with a receiving
node; it should not be enabled on an unprepared live network.

## Display and Diagnostics

When an OLED is detected at I2C address `0x3C`, it displays the device IP,
RTC time, receive/transmit rates, and dropped-frame count. The serial monitor
prints one diagnostic line per second with frame rates, queue drops, generator
transmissions, UDP transmissions, and CAN error counters.

The DS3231MZ uses address `0x68`; the PCF8574 uses address `0x20`. RTC time is
initialized to a compile-time default after a detected power loss.

## Prerequisites

- ESP-IDF **6.0.2** (or a compatible ESP-IDF 6.x release with ESP32-C5 TWAI-FD
  support).
- ESP-IDF tools installed and exported in the shell.
- An ESP32-C5 board with PSRAM configured and two CAN FD transceivers.
- A Wi-Fi access point using WPA2-PSK.

## Configure Wi-Fi

Create `main/secrets.h` locally. This file is ignored by Git:

```c
#pragma once

#define WIFI_SSID "your-network-name"
#define WIFI_PASS "your-network-password"
```

Never commit real credentials. If credentials have been exposed publicly,
rotate the Wi-Fi password before publishing the repository.

## Build and Flash

From an ESP-IDF PowerShell or command prompt:

```powershell
idf.py set-target esp32c5
idf.py build
idf.py -p COM_PORT flash monitor
```

Replace `COM_PORT` with the serial port for the board, for example `COM7`.
The generated `build/` directory and local `sdkconfig` are intentionally
ignored and should not be uploaded to GitHub.

## Project Layout

```text
main/                      Application entry point and network configuration
components/twai_manager/   Reusable TWAI/TWAI-FD node manager and recovery
components/twai_fd_stress/ Standalone dual-channel stress-test component
components/c_oled/         SSD1306-style I2C OLED helper
components/ds3231mz/       DS3231MZ RTC driver
components/pcf8574/        PCF8574 I/O-expander driver
components/sdcard_service/ Optional SD-card service and default SPI mapping
```

## Current Limitations

- Wi-Fi credentials, UDP destination, and CAN bit rates are compile-time
  settings rather than menuconfig options.
- UDP is connectionless and unauthenticated; use it only on a trusted network
  or add an application-level security layer.
- The CAN bus requires external transceivers, correct common ground, and
  termination at the two physical ends of each bus.
- The SD-card component is present, but it is not mounted by the current
  `app_main`.

## License

No license has been selected for this repository yet. Add a license before
accepting external contributions or redistributing the project.

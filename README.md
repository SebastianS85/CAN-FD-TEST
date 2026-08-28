# ESP32-C5 CAN FD Gateway

An ESP-IDF application for the ESP32-C5 that bridges two on-chip TWAI-FD
controllers to a Wi-Fi-connected host over a socket-based connection. The
firmware also provides a small I2C status panel, RTC timekeeping, queue/drop
statistics, and an optional high-rate CAN FD traffic generator for bus testing.
The host-side Python client used to receive and monitor frames is located in
`can_analyser.py`. The SD-card raw log decoder is located in `can_sd_decode.py`.

## Features

- Two independent CAN FD channels using the ESP32-C5 TWAI peripherals.
- 1 Mbit/s arbitration phase and 5 Mbit/s data phase.
- CAN FD payloads up to 64 bytes.
- Wi-Fi station mode with automatic reconnect.
- Socket-based communication for received CAN frames and host-to-device CAN TX.
- Python client for monitoring and analysis in `can_analyser.py`.
- OLED status display, DS3231MZ RTC, and PCF8574 I/O expander on one I2C bus.
- Large PSRAM-backed receive pool with separate logging and routing queues.
- CAN health monitoring and automatic recovery after repeated error states.
- Built-in burst traffic generator for development and stress testing.
- SD-card data currently written as raw binary log files; decode helper is in `can_sd_decode.py`.

## Hardware
<img width="711" height="624" alt="image" src="https://github.com/user-attachments/assets/fa8cd841-c782-47a9-b497-dfd896d87813" />

The firmware targets an **ESP32-C5** development board or custom board with:

- Two external CAN FD transceivers. The ESP32-C5 GPIOs are logic-level TWAI
  signals; they must not be connected directly to a CAN bus.
- A 3.3 V I2C bus with pull-ups.
- Optional I2C OLED, DS3231MZ RTC, and PCF8574 modules.
- PSRAM enabled and available to the application. The receive pool allocates
  approximately 1.1 MiB from PSRAM at startup.

See [PINOUT.md](PINOUT.md) for the complete connection table and electrical
notes.
## Hardware Availability

The first batch of the ESP32-C5 Dual Isolated CAN-FD board is available for purchase at the following stores:

* [Get it on Lectronz](https://lectronz.com/products/esp32-c5-dual-isolated-can-fd-board-first-batch)
* [Get it on Tindie](https://www.tindie.com/products/smuqdev/esp32-c5-dual-isolated-can-fd-board-first-batch/)
# ESP32-C5 CAN FD Gateway Pinout

This table reflects the GPIO assignments currently compiled in
`main/main.c`. GPIO numbers are ESP32-C5 chip GPIO numbers, not physical pin
numbers on a particular development board. Check the board schematic before
wiring.

## Active Connections

| Function | ESP32-C5 GPIO | Direction | Connection |
| --- | ---: | --- | --- |
| I2C SDA | GPIO0 | Bidirectional | OLED, DS3231MZ, and PCF8574 SDA |
| I2C SCL | GPIO1 | Output | OLED, DS3231MZ, and PCF8574 SCL |
| CAN node 1 TX | GPIO4 | Output | Transceiver 1 TXD / TX input |
| CAN node 1 RX | GPIO5 | Input | Transceiver 1 RXD / RX output |
| CAN node 2 TX | GPIO23 | Output | Transceiver 2 TXD / TX input |
| CAN node 2 RX | GPIO24 | Input | Transceiver 2 RXD / RX output |

## I2C Devices

All devices share GPIO0/GPIO1 and require compatible 3.3 V logic levels.

| Device | I2C address | Bus speed | Required signals |
| --- | ---: | ---: | --- |
| OLED | `0x3C` | Up to 400 kHz | SDA, SCL, 3.3 V, GND |
| DS3231MZ | `0x68` | 400 kHz | SDA, SCL, 3.3 V, GND |
| PCF8574 | `0x20` | 400 kHz | SDA, SCL, 3.3 V, GND |

The application enables the ESP32-C5 internal I2C pull-ups. External pull-ups
appropriate for the bus capacitance are still recommended for reliable 400 kHz
operation.

## CAN Bus Wiring

The ESP32-C5 TWAI pins are digital controller pins. Each channel needs an
external CAN or CAN FD transceiver:

```text
ESP32-C5 CAN1 TX (GPIO4)  -> Transceiver 1 TXD
ESP32-C5 CAN1 RX (GPIO5)  <- Transceiver 1 RXD
Transceiver 1 CANH/CANL   <-> CAN bus 1

ESP32-C5 CAN2 TX (GPIO23) -> Transceiver 2 TXD
ESP32-C5 CAN2 RX (GPIO24) <- Transceiver 2 RXD
Transceiver 2 CANH/CANL   <-> CAN bus 2
```

Connect the ESP32-C5 and transceiver grounds together. Use 3.3 V-compatible
transceivers unless level shifting is provided. Install 120 ohm termination at
the two physical ends of each CAN bus, not at every node. Confirm that the
transceiver supports CAN FD at the configured 5 Mbit/s data phase.

## CAN Configuration

| Channel | TX | RX | Arbitration | Data phase |
| --- | ---: | ---: | ---: | ---: |
| Node 1 | GPIO4 | GPIO5 | 1 Mbit/s | 5 Mbit/s |
| Node 2 | GPIO23 | GPIO24 | 1 Mbit/s | 5 Mbit/s |

Both channels use CAN FD frames with bit-rate switching enabled. The firmware
expects an FD-capable bus and transceiver when `TWAI_USE_FD_FRAMES` is `1`.

## Optional SD-Card Mapping

The `sdcard_service` component defines this default SPI mapping, but the
current application does not initialize or mount the SD card. Treat these
signals as reserved for a future SD-card integration:

| SD-card SPI signal | ESP32-C5 GPIO |
| --- | ---: |
| MOSI | GPIO8 |
| MISO | GPIO9 |
| SCLK | GPIO10 |
| CS | GPIO6 |

## Data Flow

```text
CAN transceiver 1 -> ESP32-C5 TWAI node 1 --+
                                             +-> receive pool -> socket host client
CAN transceiver 2 -> ESP32-C5 TWAI node 2 --+

Socket client / host app -> ESP32-C5 -> selected TWAI node -> CAN transceiver
```

Each received CAN frame is copied into a shared pool and made available to the
socket-based sender and the routing/diagnostic path. Frames are sent to the host
in batches of up to 20 records, or after a short flush timeout.

## Socket Interface

The current network values are compile-time definitions in `main/main.c`:

| Setting | Current value | Meaning |
| --- | --- | --- |
| Destination host | `192.168.178.61` | Computer receiving CAN frames |
| TX port | `3333` | ESP32-C5 to host |
| RX port | `3334` | Host to ESP32-C5 |

The socket payload is a packed binary structure. A single record contains:

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

The Python client used for monitoring is `can_analyser.py`; it connects to the
ESP32 through the socket interface and decodes the incoming binary payloads.

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
can_analyser.py            Host-side Python client for socket monitoring
can_sd_decode.py           Decoder for raw binary SD-card CAN logs
```

## Current Limitations

- Wi-Fi credentials, socket destination, and CAN bit rates are compile-time
  settings rather than menuconfig options.
- The socket link is unauthenticated; use it only on a trusted network or add
  an application-level security layer.
- The CAN bus requires external transceivers, correct common ground, and
  termination at the two physical ends of each bus.
- The SD-card data is currently written in binary format and is decoded with the
  helper script in `can_sd_decode.py`.
- The SD-card component is present, but it is not mounted by the current
  `app_main`.



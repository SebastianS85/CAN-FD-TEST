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

Do not connect another peripheral to these pins if SD-card support will be
enabled later without first checking the board design and SPI host settings.

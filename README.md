# Inspector 001 RT-Thread

Inspector 001 is an embedded inspection terminal built on RT-Thread for the ALIENTEK Explorer STM32F407 V3 development board. It integrates identity verification, device identification, camera capture, local record storage, and MQTT upload into one board-level workflow.

The current firmware has been validated on real hardware with NFC, RFID, OV2640 camera, TF card storage, ESP8266 networking, MQTT publishing, LCD display, and touch interaction.

## Features

- NFC identity verification with PN532 over software I2C
- RFID device verification with RC522 over software SPI
- OV2640 camera preview and snapshot capture with DCMI + DMA
- TF card storage through SDIO
- CSV inspection records and BMP photo output
- ESP8266 networking through RT-Thread `at_device`
- MQTT upload through Paho MQTT
- Touch-driven inspection flow on the LCD
- Shared-pin handling between OV2640 DCMI and SDIO storage

## Hardware Platform

| Item | Configuration |
| --- | --- |
| MCU board | ALIENTEK Explorer STM32F407 V3 |
| RTOS | RT-Thread |
| IDE | RT-Thread Studio |
| Display | 4.3-inch LCD with GT9xxx touch |
| Camera | OV2640 |
| NFC | PN532 |
| RFID | RC522 |
| Storage | TF card over SDIO |
| Network | ESP8266 on ATK MODULE / UART3 |

## Main Pin Assignments

| Peripheral | Pins |
| --- | --- |
| PN532 NFC | SCL=PE3, SDA=PE2, I2C address 0x24 |
| RC522 RFID | SCK=PA5, MISO=PA6, MOSI=PA7, CS=PA4, RST=PC4 |
| OV2640 SCCB/control | XCLK=PA8, SCL=PD6, SDA=PD7, PWDN=PG9, RESET=PG15 |
| OV2640 DCMI | PA4, PA6, PB6, PB7, PC6, PC7, PC8, PC9, PC11, PE5, PE6 |
| TF card | SDIO |
| ESP8266 | UART3, PB10/PB11 through ATK MODULE |

OV2640 and TF card share part of the board routing. The firmware switches ownership by using DCMI during camera capture, then releasing the shared lines back to SDIO before saving the photo.

## Inspection Flow

1. Show the welcome screen after boot.
2. Wait for a registered NFC card.
3. Enter RFID verification after NFC succeeds.
4. Enter camera preview after RFID succeeds.
5. Capture a photo from the touch UI.
6. Save the photo to the TF card.
7. Append an inspection record to CSV.
8. Connect WiFi and publish the record through MQTT.
9. Let the operator continue with another device or finish the inspection round.

## Storage Layout

The TF card is mounted at `/sdcard`. Runtime data is stored under:

```text
/sdcard/INSPECT/
├── DEVICES.CSV
├── RECORDS.CSV
└── PHOTOS/
    ├── INS0001.BMP
    └── INS0002.BMP
```

`RECORDS.CSV` uses a sequence number to distinguish inspection records. The same sequence number is used in the CSV record, BMP filename, and MQTT JSON payload.

## MQTT

ESP8266 is registered as the RT-Thread network device `esp0`. MQTT publishing uses the Paho MQTT package.

Default configuration is kept in source-level macros:

- WiFi SSID/password: `APP_ESP_DEFAULT_WIFI_SSID`, `APP_ESP_DEFAULT_WIFI_PASSWORD`
- MQTT URI: `APP_MQTT_DEFAULT_URI`
- MQTT topic: `APP_MQTT_DEFAULT_TOPIC`

Default topic:

```text
rtt_to_atk/explorer/test
```

## Useful FinSH Commands

The normal inspection flow is automatic. These commands are kept for board bring-up and diagnosis:

```text
APP
APP sd
APP sd rescan
APP esp status
APP esp join <ssid> <password>
APP mqtt status
APP mqtt start
APP mqtt pub <topic> <message>
APP nfc
APP rfid
APP ov live
```

## Build

Open `inspector_001_explorer_v3` with RT-Thread Studio and build the Debug configuration.

The project can also be built with the generated Debug Makefile when the RT-Thread Studio toolchain is available locally:

```powershell
cd inspector_001_explorer_v3\Debug
$env:PATH='E:\RT_ThreadStudioIDE\RT-ThreadStudio\repo\Extract\ToolChain_Support_Packages\ARM\GNU_Tools_for_ARM_Embedded_Processors\5.4.1\bin;E:\RT_ThreadStudioIDE\RT-ThreadStudio\platform\env_released\env\tools\bin;' + $env:PATH
mingw32-make all
```

Last verified local build:

```text
text=296672 data=2420 bss=26088 dec=325180
```

## Notes

- The ESP8266 firmware used during validation does not support `AT+CIPDNS?`; this produces a warning from `at_device`. The application sets DNS locally and waits for stable `esp0` link/IP state before starting MQTT.
- LCD display direction is configured for the validated physical mounting direction, with matching touch-coordinate mapping.
- Build outputs, local temporary files, IDE runtime data, and large vendor reference material are intentionally ignored by Git.

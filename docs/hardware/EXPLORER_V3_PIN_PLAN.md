# Explorer V3 Pin Plan

## Scope

This file records Explorer STM32F407 V3-specific pin assignments before peripheral code is migrated.

## LCD 4.3-Inch TFTLCD

Source baseline:

- Explorer V3 IO allocation table
- ALIENTEK HAL TFTLCD MCU-screen example
- 4.3-inch NT35510 material directory

| Signal | MCU pin | Mode | Status |
|---|---|---|---|
| LCD_D0 | PD14 | FSMC_D0 | used |
| LCD_D1 | PD15 | FSMC_D1 | used |
| LCD_D2 | PD0 | FSMC_D2 | used |
| LCD_D3 | PD1 | FSMC_D3 | used |
| LCD_D4 | PE7 | FSMC_D4 | used |
| LCD_D5 | PE8 | FSMC_D5 | used |
| LCD_D6 | PE9 | FSMC_D6 | used |
| LCD_D7 | PE10 | FSMC_D7 | used |
| LCD_D8 | PE11 | FSMC_D8 | used |
| LCD_D9 | PE12 | FSMC_D9 | used |
| LCD_D10 | PE13 | FSMC_D10 | used |
| LCD_D11 | PE14 | FSMC_D11 | used |
| LCD_D12 | PE15 | FSMC_D12 | used |
| LCD_D13 | PD8 | FSMC_D13 | used |
| LCD_D14 | PD9 | FSMC_D14 | used |
| LCD_D15 | PD10 | FSMC_D15 | used |
| LCD_RD | PD4 | FSMC_NOE | used |
| LCD_WR | PD5 | FSMC_NWE | used |
| LCD_RS | PF12 | FSMC_A6 | used |
| LCD_CS | PG12 | FSMC_NE4 | used |
| LCD_BL | PB15 | GPIO output, high active | used |

Address model:

- Command register: `0x6C00007E`
- Data register: `0x6C000080`
- FSMC bank: NE4
- RS address line: A6
- Bus width: 16-bit
- First controller target: NT35510, 480x800 portrait

## 4.3-Inch GT9xxx Touch

Source baseline:

- Explorer V3 IO allocation table
- ALIENTEK HAL touch-screen example
- 4.3-inch NT35510 material directory with GT91x/GT9xxx controller references

| Signal | MCU pin | Status |
|---|---|---|
| GT9xxx SCL / T_SCK | PB0 | used by software I2C |
| GT9xxx SDA / T_MOSI | PF11 | used by software I2C |
| GT9xxx INT / T_PEN | PB1 | used as input |
| GT9xxx RST / T_CS | PC13 | used as reset output |
| T_MISO | PB2 | reserved for resistive touch path, not used by GT9xxx |

Driver model:

- Current firmware uses an application-layer GT9xxx software-I2C implementation.
- It does not require RT-Thread `RT_USING_I2C`.
- It does not modify CubeMX generated files.

## RC522 RFID

Source baseline:

- Explorer V3 IO allocation table
- Explorer V3.5 schematic
- Current verified board peripherals: LCD, GT9xxx touch, SDIO SD card, RTC

Current wiring target:

| Signal | MCU pin | Mode | Status |
|---|---|---|---|
| RC522 SCK | PA5 | GPIO output, software SPI clock | previously verified, pending re-verification in new mix |
| RC522 MISO | PG6 | GPIO input, software SPI MISO | pending re-verification |
| RC522 MOSI | PA7 | GPIO output, software SPI MOSI | previously verified, pending re-verification in new mix |
| RC522 SDA / CS | PG7 | GPIO output, active low | pending re-verification |
| RC522 RST | PC4 | GPIO output | previously verified, pending re-verification in new mix |
| RC522 IRQ | not connected | unused | verified |
| RC522 VCC | 3.3V / VDD3.3 | power | verified |
| RC522 GND | GND | ground | verified |

Selection notes:

- The selected physical area remains the same external IO header side used for the previous RC522 wiring, to keep the module close to the same 3.3 V/GND area and avoid moving to the opposite header side.
- The firmware uses GPIO software SPI for RC522. It does not require RT-Thread SPI device attachment for this RC522 path.
- The previous verified wiring `PA5/PA6/PA7/PA4/PC4` is no longer the preferred plan because `PA4` and `PA6` are required by OV2640 DCMI.
- The current wiring intentionally releases:
  - `PA4` for `OV_HREF / DCMI_HSYNC`;
  - `PA6` for `OV_PCLK / DCMI_PIXCLK`;
- `PA5/PA7/PC4` are retained from the already verified RC522 wiring to minimize variables.
- `PG6/PG7` are selected for the two moved signals because the Explorer V3 IO table marks them as WIRELESS interface pins that are independent when the WIRELESS socket is unused.
- The rejected `PC4/PC5` control-pin attempt failed at request stage and `PC4/PC5` are physically connected to the onboard YT8512C Ethernet PHY.
- The rejected `PB13/PC2/PC3/PB12/PA5` attempt still failed and those pins are tied to ES8388 audio/TPAD paths, so it is no longer the preferred plan.
- If WIRELESS, Ethernet, or OV camera is enabled later, this RC522 wiring must be reviewed before use.
- Do not connect RC522 `IRQ` for the first verification.

Verification status:

- Board-side verification passed with the previous GPIO software-SPI firmware on `PA5/PA6/PA7/PA4/PC4`.
- No-card run:
  - `version=0x12`
  - `step=no card`
- Card-present run:
  - `version=0x12`
  - `atqa=04 00`
  - `uid_len=4`
  - `uid=3464C2F0`
- The new `PA5/PG6/PA7/PG7/PC4` wiring still needs board-side re-verification with:

```text
APP rfid
```

## PN532 NFC

Source baseline:

- Old Spark-1 `BSP_PN532.c` / `BSP_PN532.h` protocol flow
- Explorer V3 IO allocation table
- Explorer V3.5 schematic `P8` / `P9` IO header layout
- Current verified board peripherals: LCD, GT9xxx touch, SDIO SD card, RTC, RC522 RFID

I2C wiring candidates:

| Signal | MCU pin | Mode | Status |
|---|---|---|---|
| PN532 SCL | PE3 | GPIO open-drain style, software I2C clock | verified |
| PN532 SDA | PE2 | GPIO open-drain style, software I2C data | verified |
| PN532 SCL | PB8 | board IIC SCL / RT-Thread `i2c1` bus | fallback if this physical position is acceptable |
| PN532 SDA | PB9 | board IIC SDA / RT-Thread `i2c1` bus | fallback if this physical position is acceptable |
| PN532 IRQ | not connected | unused for first verification | candidate |
| PN532 RSTO / RSTPDN | not connected | unused for first verification | candidate |
| PN532 VCC | 3.3V / VDD3.3 | power | candidate |
| PN532 GND | GND | ground | candidate |

Selection notes:

- The old Spark-1 firmware used software I2C on `PE4=SCL` and `PE5=SDA`; those pins are not copied to Explorer V3.
- The current PN532 module is treated as an I2C-only wiring target because only `GND/VCC/SDA/SCL` are used.
- The first priority is physical placement on the same external IO header side as the verified RC522 wiring.
- The first implemented pair `PC0=SCL` and `PA0=SDA` was rejected after board-side diagnostics because `PC0` stayed low after bus release.
- The current preferred same-side pair is `PE3=SCL` and `PE2=SDA`:
  - both pins are in the same `P8` external IO header area as the verified RC522 wiring;
  - `PE3` is connected to `KEY1` and can be used as GPIO when the key is not pressed;
  - `PE2` is connected to `KEY2` and can be used as GPIO when the key is not pressed;
  - neither pin is used by the verified LCD, GT9xxx touch, SDIO SD card, RTC, or RC522 paths;
  - neither pin consumes the SPI2 group (`PB13/PC2/PC3`) or the board IIC group (`PB8/PB9`).
- If the `PB8/PB9` position on the external IO header area is physically acceptable, it remains a fallback board-level IIC bus:
  - `PB8 = IIC_SCL`
  - `PB9 = IIC_SDA`
  - board pull-ups already exist on these lines.
- In this BSP, `BSP_USING_I2C1` registers RT-Thread bus `i2c1` through `drv_soft_i2c.c` on `PB8/PB9`. It is not the STM32 HAL hardware I2C peripheral, but it is the board-designated IIC bus.
- Hardware I2C2 on `PF1/PF0` is not selected for this stage because those pins are currently mapped as FSMC address lines in CubeMX metadata and changing them risks disturbing the current FSMC/LCD/SRAM configuration area.
- `PC2/PC3` are not selected for PN532 I2C because they are currently mapped as SPI2 pins in CubeMX metadata and should not be consumed for a two-wire I2C-only NFC module while other same-side GPIO candidates exist.
- `PE5/PE6` are not selected because they are DCMI camera pins and camera migration may become relevant later.
- Do not use PN532 `IRQ` or reset control for the first verification unless the module fails to wake reliably with power cycling and bus recovery.
- PN532 module mode switches or solder jumpers must be set to I2C mode before firmware verification.

Firmware status:

- `APP_Pn532.c` implements a PN532 I2C bring-up path using software I2C on `PE3/PE2`.
- It follows the old Spark-1 PN532 protocol sequence as a reference:
  - bus init and recovery;
  - I2C address probe at `0x24`;
  - `GetFirmwareVersion`;
  - `SAMConfiguration`;
  - `InListPassiveTarget`;
  - UID conversion to uppercase hexadecimal text.
- FinSH command:

```text
APP nfc
```

- Board-side verification passed:
  - idle bus level: `SCL=1`, `SDA=1`;
  - address scan found PN532 at `0x24`;
  - firmware bytes: `32 01 06 07`;
  - card-present result: `uid_len=7`, `uid=04C35D91410289`.

## ESP-WROOM-5V2L WiFi Module

Source baseline:

- User-provided `ESP-WROOM-5V2L_Datasheet__CN.pdf`
- Explorer V3 IO allocation table
- Explorer V3.5 schematic page `RS232`
- Explorer V3 ATK MODULE / USART3 wiring already enabled in the current project

Current wiring target:

| Signal | Explorer V3 resource | ESP-WROOM-5V2L pin | Status |
|---|---|---|---|
| UART TX from MCU | PB10 / USART3_TX / GBC_RX | RXD_VCC | verified |
| UART RX to MCU | PB11 / USART3_RX / GBC_TX | TXD_VCC | verified |
| Power | ATK MODULE VCC5 | VIN | verified |
| Ground | ATK MODULE GND | GND | verified |
| Enable | module default pull-up or external high | EN | optional |
| Boot mode | leave floating/high for normal boot | BOOT / GPIO0 | candidate |

Selection notes:

- The current active WiFi module plan is ESP-WROOM-5V2L, not DT-06.
- The module uses ESP8266-class UART AT communication.
- `VIN` should use the module board's 5 V input path when connected through the Explorer V3 ATK MODULE socket.
- `RXD_VCC` is the module receive pin, so it connects to MCU transmit `PB10/USART3_TX`.
- `TXD_VCC` is the module transmit pin, so it connects to MCU receive `PB11/USART3_RX`.
- `PB10/PB11` are routed through Explorer V3 jumper block `P2`.
- `P2` selects whether USART3 is connected to the RS232 `COM3` connector or the `ATK-MODULE` connector:
  - `PB10 / USART3_TX` can connect to RS232 `COM3 RX` or ATK-MODULE `RXD`;
  - `PB11 / USART3_RX` can connect to RS232 `COM3 TX` or ATK-MODULE `TXD`;
  - if the `P2` jumpers are removed, `PB10/PB11` are independent and will not reach the ATK-MODULE socket.
- For ESP-WROOM-5V2L on the ATK-MODULE socket, both `P2` jumpers must be placed on the ATK-MODULE / `GBC_RX/GBC_TX` side, not the RS232 `COM3` side.
- ATK-MODULE `RXD/TXD` are named from the external module's perspective:
  - ESP `RXD_VCC` connects to ATK-MODULE `RXD / GBC_RX`;
  - ESP `TXD_VCC` connects to ATK-MODULE `TXD / GBC_TX`;
  - do not cross `RX/TX` again when wiring through the ATK-MODULE header.
- `BOOT/GPIO0` must not be pulled low during normal operation; low boot selects UART download mode.
- First firmware verification only proves UART AT command/response and AT firmware presence. WiFi join, SAL/netdev/lwIP, and MQTT should be planned after this link is stable.
- Current `APP esp` diagnostic tries `115200 8N1` first and `9600 8N1` as fallback, then prints `AT+GMR` firmware version information after `AT` succeeds.

Verification status:

- Board-side UART link was verified after correcting the ATK-MODULE wiring to `RXD_VCC -> GBC_RX` and `TXD_VCC -> GBC_TX`.
- The previous timeout was caused by applying another RX/TX cross at the module header even though the ATK-MODULE header is already named from the external module side.

Software ownership rule:

- The physical link is `USART3 / uart3` routed by `P2` to the ATK-MODULE header.
- Board/BSP menus may describe this resource as `COM3`, but the current hardware path is not the external RS232 COM3 connector.
- `APP esp <AT command>` is only a low-level UART/AT diagnostic command.
- After RT-Thread `at_device` is enabled for ESP8266, `uart3` should be considered owned by the AT device stack during normal network operation.
- Do not use `APP esp` as the WiFi/MQTT business path. Formal networking should use `at_device`, SAL/netdev, and Paho MQTT.

## DT-06 WiFi-TTL Module

Status:

- Evaluated from `C19949084_WIFI模块_DT-06_规格书_WJ978051.PDF`.
- Not selected for the current active WiFi path after user decision.
- Main reason: DT-06 defaults to transparent-transmission firmware and a web-configuration workflow, while the current plan returns to the first ESP-WROOM-5V2L AT module.

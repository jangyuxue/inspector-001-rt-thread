# LCD Touch Software Interface

## Current Firmware Project

`C:/Users/dengsikai/Desktop/RTT_TO_ATK/inspector_001_explorer_v3`

## Baseline

- Build: zero errors, verified by user before LCD migration.
- Download: verified by user before LCD migration.
- FinSH: verified by user before LCD migration.
- Current base application: PF9 LED heartbeat.

## LCD Software Entry

The first LCD implementation is application-layer code under `applications/`.

Files:

- `applications/APP_Display.c`
- `applications/APP_Display.h`
- `applications/APP_Font.c`
- `applications/APP_Font.h`
- `applications/APP_Touch.c`
- `applications/APP_Touch.h`

FinSH command:

```text
APP lcd
APP touch
```

## Current Design Notes

- The LCD is driven directly through the Explorer V3 FSMC TFTLCD interface.
- The code does not depend on RT-Thread `BSP_USING_LCD`.
- The code does not modify CubeMX generated files.
- The first controller target is NT35510 on the 4.3-inch screen.
- Full arbitrary Chinese rendering is not included yet; only the demo phrase is embedded.

## Responsibility Boundary

- Firmware-side LCD and touch implementation is maintained in this project.
- Build, download, serial terminal observation, and real board display/touch phenomenon verification are performed externally by the hardware bring-up owner.
- This file records firmware entry points, code boundary, and used board resources. It does not define the board-side verification flow.

## Touch Boundary

The first touch implementation targets the 4.3-inch GT9xxx capacitive touch path used by the Explorer V3 TFTLCD connector.

Current command:

```text
APP touch
```

The command initializes the GT9xxx software-I2C path, reads the product ID, reads one touch status frame, prints the point count and coordinates, then returns.

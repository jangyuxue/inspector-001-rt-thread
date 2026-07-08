# Inspector 001 RT-Thread

这是一个基于 RT-Thread 的 STM32F407 巡检终端固件仓库。工程运行在正点原子 Explorer STM32F407 V3 开发板上，集成 LCD 触摸屏、NFC、RFID、OV2640 摄像头、TF 卡、ESP8266 和 MQTT。

固件上电后自动执行巡检流程，也保留 FinSH 命令用于单模块调试。

## 功能

- LCD 显示巡检页面和状态信息。
- GT9xxx 触摸屏用于页面按钮操作。
- PN532 读取 NFC UID，用于身份或物品验证。
- RC522 读取 RFID UID，用于设备验证。
- 未登记 UID 支持现场注册名称。
- OV2640 摄像头预览和拍照。
- TF 卡保存设备登记表、巡检记录和 BMP 照片。
- ESP8266 通过 UART3 接入 RT-Thread `at_device`。
- Paho MQTT 发布巡检结果，可用 MQTTX 接收验证。

## 自动流程

```text
上电
  -> Welcome
  -> NFC 验证
  -> RFID 验证
  -> 摄像头预览
  -> 触摸拍照
  -> 保存照片和 CSV 记录到 TF 卡
  -> WiFi 联网
  -> MQTT 上报
  -> 继续下一台设备 / 结束本轮巡检
```

## 硬件

| 模块 | 说明 |
| --- | --- |
| 开发板 | 正点原子 Explorer STM32F407 V3 |
| MCU | STM32F407ZG |
| RTOS | RT-Thread 4.0.5 |
| IDE | RT-Thread Studio |
| 显示 | 4.3 inch LCD |
| 触摸 | GT9xxx |
| NFC | PN532 |
| RFID | RC522 |
| 摄像头 | OV2640 |
| 存储 | TF card / SDIO |
| 网络 | ESP8266 AT firmware / UART3 / ATK MODULE |
| MQTT | Paho MQTT |

## 引脚

| 外设 | 引脚 |
| --- | --- |
| PN532 | SCL=PE3, SDA=PE2, I2C address=0x24 |
| RC522 | SCK=PA5, MISO=PA6, MOSI=PA7, CS=PA4, RST=PC4 |
| OV2640 control | XCLK=PA8, SCCB_SCL=PD6, SCCB_SDA=PD7, PWDN=PG9, RESET=PG15 |
| OV2640 DCMI | PA4, PA6, PB6, PB7, PC6, PC7, PC8, PC9, PC11, PE5, PE6 |
| TF card SDIO | CMD=PD2, D0=PC8, D1=PC9, D2=PC10, D3=PC11, CK=PC12 |
| ESP8266 | UART3, PB10/PB11 |
| GT9xxx touch | SCL=PB0, SDA=PF11, RST=PC13, INT=PB1 |

共享引脚：

| 引脚 | 共享外设 |
| --- | --- |
| PA4, PA6 | RC522 / OV2640 DCMI |
| PC8, PC9, PC11 | TF card SDIO / OV2640 DCMI |

当前代码按顺序使用共享外设：RFID 验证完成后进入摄像头，拍照完成后恢复 SDIO 并写入 TF 卡。

## 目录

```text
.
|-- README.md
`-- inspector_001_explorer_v3/
    |-- applications/      应用层代码
    |-- board/             板级配置和链接脚本
    |-- libraries/         STM32 HAL 与 RT-Thread HAL 驱动
    |-- packages/          at_device、Paho MQTT 等软件包
    |-- rt-thread/         RT-Thread 内核与组件
    |-- .project           RT-Thread Studio 工程文件
    |-- .cproject          CDT 构建配置
    |-- .config            RT-Thread 配置
    |-- rtconfig.h
    |-- rtconfig.py
    `-- SConstruct
```

主要应用文件：

| 文件 | 作用 |
| --- | --- |
| `APP_Flow.c` | 自动巡检流程、卡片注册、记录生成、MQTT 上报 |
| `APP_Display.c` | LCD 初始化、页面绘制、按钮和图像显示 |
| `APP_Touch.c` | GT9xxx 触摸读取 |
| `APP_Pn532.c` | PN532 NFC 读取 |
| `APP_Rc522.c` | RC522 RFID 读取 |
| `APP_Ov.c` | OV2640 初始化、预览、拍照和 BMP 保存 |
| `APP_SdDiag.c` | TF 卡挂载、重扫和读写诊断 |
| `APP_Esp8266.c` | ESP8266 注册、联网和 AT 调试 |
| `APP_Mqtt.c` | MQTT 启动、订阅、发布和停止 |

## 导入和编译

克隆仓库：

```powershell
git clone https://github.com/jangyuxue/inspector-001-rt-thread.git
```

在 RT-Thread Studio 中导入下面这个目录：

```text
inspector-001-rt-thread/inspector_001_explorer_v3
```

然后使用 `Debug` 配置编译并下载到开发板。

不要把仓库根目录作为 RT-Thread Studio 工程导入。

## 运行数据

TF 卡挂载路径：

```text
/sdcard
```

巡检数据目录：

```text
/sdcard/INSPECT/
|-- DEVICES.CSV
|-- RECORDS.CSV
`-- PHOTOS/
    |-- INS0001.BMP
    |-- INS0002.BMP
    `-- ...
```

`DEVICES.CSV` 保存已登记 UID：

```csv
type,uid,name
NFC,04C35D91410289,item001
RFID,0463D09D410289,device001
```

`RECORDS.CSV` 保存巡检结果：

```csv
seq,tick,nfc_uid,nfc_name,rfid_uid,rfid_name,photo_path,photo_status,photo_ret
1,123456,04C35D91410289,item001,0463D09D410289,device001,/sdcard/INSPECT/PHOTOS/INS0001.BMP,PHOTO_OK,0
```

## MQTT 配置

| 配置 | 位置 |
| --- | --- |
| WiFi SSID | `inspector_001_explorer_v3/.config` 中的 `CONFIG_ESP8266_SAMPLE_WIFI_SSID` |
| WiFi password | `inspector_001_explorer_v3/.config` 中的 `CONFIG_ESP8266_SAMPLE_WIFI_PASSWORD` |
| MQTT URI | `APP_Mqtt.c` 中的 `APP_MQTT_DEFAULT_URI` |
| MQTT topic | `APP_Mqtt.c` 中的 `APP_MQTT_DEFAULT_TOPIC`，`APP_Flow.c` 中的 `APP_FLOW_MQTT_TOPIC` |

当前默认值：

```text
WiFi SSID: ESP
WiFi password: 12345678
MQTT URI: tcp://broker.emqx.io:1883
MQTT topic: rtt_to_atk/explorer/test
```

## FinSH 命令

查看命令列表：

```text
APP
```

常用调试命令：

```text
APP flow status
APP flow add <name>
APP flow reset

APP sd
APP sd rescan

APP nfc
APP rfid
APP ov live

APP esp status
APP esp up
APP esp join <ssid> <password>
APP esp ping <host>
APP esp <AT command>

APP mqtt status
APP mqtt start [tcp://host:1883]
APP mqtt sub [topic]
APP mqtt pub [topic] [message]
APP mqtt stop
```

单模块验证顺序：

```text
APP sd
APP nfc
APP rfid
APP ov live
APP esp join ESP 12345678
APP mqtt start
APP mqtt pub rtt_to_atk/explorer/test hello
```

## 说明

- `Debug/`、`Release/`、对象文件和固件产物不提交。
- RT-Thread Studio 工程目录是 `inspector_001_explorer_v3`。
- ESP8266 固件如果不支持 `AT+CIPDNS?`，运行时可能出现对应 warning；当前代码使用本地 DNS 和 `AT+CIPDOMAIN` 处理域名解析。
- 修改共享引脚相关代码后，需要重新验证 RC522、OV2640 和 TF 卡写入。

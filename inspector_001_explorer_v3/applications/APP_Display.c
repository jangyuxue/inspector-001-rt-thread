#include <rtthread.h>
#include <rtdevice.h>
#include <finsh.h>
#include <board.h>
#include <string.h>

#include "APP_Display.h"
#include "APP_Esp8266.h"
#include "APP_Flow.h"
#include "APP_Font.h"
#include "APP_Mqtt.h"
#include "APP_Ov.h"
#include "APP_Pn532.h"
#include "APP_Rc522.h"

extern int APP_SdDiag_Test(int argc, char **argv);

/* 4.3 寸 NT35510 LCD 当前使用竖屏坐标：宽 480，高 800。 */
#define APP_LCD_WIDTH              480U
#define APP_LCD_HEIGHT             800U
#define APP_LCD_FSMC_BANK_INDEX    3U

/* FSMC 8080 并口映射地址：REG 写命令，RAM 写像素数据。 */
#define APP_LCD_REG_ADDR           ((uint32_t)0x6C00007E)
#define APP_LCD_RAM_ADDR           ((uint32_t)0x6C000080)
#define APP_LCD_REG                (*((volatile uint16_t *)APP_LCD_REG_ADDR))
#define APP_LCD_RAM                (*((volatile uint16_t *)APP_LCD_RAM_ADDR))

/* RGB565 常用颜色。 */
#define APP_LCD_WHITE              0xFFFFU
#define APP_LCD_BLACK              0x0000U
#define APP_LCD_RED                0xF800U
#define APP_LCD_GREEN              0x07E0U
#define APP_LCD_BLUE               0x001FU
#define APP_LCD_YELLOW             0xFFE0U
#define APP_LCD_CYAN               0x07FFU
#define APP_LCD_NAVY               0x0010U
#define APP_LCD_GRAY               0x8410U
#define APP_LCD_LIGHT_GRAY         0xC618U
#define APP_LCD_ORANGE             0xFD20U
#define APP_LCD_MAGENTA            0xF81FU

/* LCD 演示线程参数。 */
#define APP_LCD_THREAD_STACK_SIZE  2048U
#define APP_LCD_THREAD_PRIORITY    25U
#define APP_LCD_THREAD_TICK        10U

/* 触摸演示刷新参数：空闲时低频扫描，按下后提高跟踪频率。 */

/* 触摸状态区域位置，局部刷新时只更新该矩形，避免整屏清屏闪烁。 */

/* 摄像头预览页布局。 */
#define APP_LCD_CAMERA_PREVIEW_X   104U
#define APP_LCD_CAMERA_PREVIEW_Y   128U
#define APP_LCD_CAMERA_PREVIEW_W   272U
#define APP_LCD_CAMERA_PREVIEW_H   224U
#define APP_LCD_CAMERA_BUTTON_X    80U
#define APP_LCD_CAMERA_BUTTON_Y    392U
#define APP_LCD_CAMERA_BUTTON_W    320U
#define APP_LCD_CAMERA_BUTTON_H    72U
#define APP_LCD_CAMERA_EXIT_X      80U
#define APP_LCD_CAMERA_EXIT_Y      492U
#define APP_LCD_CAMERA_EXIT_W      320U
#define APP_LCD_CAMERA_EXIT_H      72U
#define APP_LCD_CAMERA_STATUS_X    80U
#define APP_LCD_CAMERA_STATUS_Y    608U
#define APP_LCD_CAMERA_STATUS_W    320U
#define APP_LCD_CAMERA_STATUS_H    56U

/* Card identity page: status and button are updated independently. */
#define APP_LCD_NFC_STATUS_X       40U
#define APP_LCD_NFC_STATUS_Y       192U
#define APP_LCD_NFC_STATUS_W       400U
#define APP_LCD_NFC_STATUS_H       190U
#define APP_LCD_NFC_BUTTON_X       80U
#define APP_LCD_NFC_BUTTON_Y       520U
#define APP_LCD_NFC_BUTTON_W       320U
#define APP_LCD_NFC_BUTTON_H       72U
#define APP_LCD_CARD_BUTTON_CLEAR_X 40U
#define APP_LCD_CARD_BUTTON_CLEAR_W 400U
#define APP_LCD_CARD_REGISTER_X    50U
#define APP_LCD_CARD_REGISTER_W    180U
#define APP_LCD_CARD_RESCAN_X      250U
#define APP_LCD_CARD_RESCAN_W      180U
#define APP_LCD_NEXT_BUTTON_X      50U
#define APP_LCD_NEXT_BUTTON_Y      520U
#define APP_LCD_NEXT_BUTTON_W      180U
#define APP_LCD_NEXT_BUTTON_H      76U
#define APP_LCD_FINISH_BUTTON_X    250U
#define APP_LCD_FINISH_BUTTON_Y    520U
#define APP_LCD_FINISH_BUTTON_W    180U
#define APP_LCD_FINISH_BUTTON_H    76U

typedef struct
{
    uint16_t reg;
    uint16_t value;
} APP_LcdRegValue;

static uint8_t app_lcd_ready;
static uint16_t app_lcd_id;
static struct rt_thread app_lcd_thread_obj;
ALIGN(RT_ALIGN_SIZE)
static uint8_t app_lcd_thread_stack[APP_LCD_THREAD_STACK_SIZE];
static uint8_t app_lcd_thread_started;

static int app_lcd_hw_init(void);

static const APP_LcdRegValue app_nt35510_init_seq[] =
{
    { 0xF000, 0x55 }, { 0xF001, 0xAA }, { 0xF002, 0x52 }, { 0xF003, 0x08 },
    { 0xF004, 0x01 }, { 0xB600, 0x34 }, { 0xB601, 0x34 }, { 0xB602, 0x34 },
    { 0xB000, 0x0D }, { 0xB001, 0x0D }, { 0xB002, 0x0D }, { 0xB700, 0x24 },
    { 0xB701, 0x24 }, { 0xB702, 0x24 }, { 0xB100, 0x0D }, { 0xB101, 0x0D },
    { 0xB102, 0x0D }, { 0xB800, 0x24 }, { 0xB801, 0x24 }, { 0xB802, 0x24 },
    { 0xB200, 0x00 }, { 0xB900, 0x24 }, { 0xB901, 0x24 }, { 0xB902, 0x24 },
    { 0xB300, 0x05 }, { 0xB301, 0x05 }, { 0xB302, 0x05 }, { 0xBA00, 0x34 },
    { 0xBA01, 0x34 }, { 0xBA02, 0x34 }, { 0xB500, 0x0B }, { 0xB501, 0x0B },
    { 0xB502, 0x0B }, { 0xBC00, 0x00 }, { 0xBC01, 0xA3 }, { 0xBC02, 0x00 },
    { 0xBD00, 0x00 }, { 0xBD01, 0xA3 }, { 0xBD02, 0x00 }, { 0xBE00, 0x00 },
    { 0xBE01, 0x37 },

    { 0xD100, 0x00 }, { 0xD101, 0x37 }, { 0xD102, 0x00 }, { 0xD103, 0x53 },
    { 0xD104, 0x00 }, { 0xD105, 0x79 }, { 0xD106, 0x00 }, { 0xD107, 0x97 },
    { 0xD108, 0x00 }, { 0xD109, 0xB1 }, { 0xD10A, 0x00 }, { 0xD10B, 0xD5 },
    { 0xD10C, 0x00 }, { 0xD10D, 0xF4 }, { 0xD10E, 0x01 }, { 0xD10F, 0x23 },
    { 0xD110, 0x01 }, { 0xD111, 0x49 }, { 0xD112, 0x01 }, { 0xD113, 0x87 },
    { 0xD114, 0x01 }, { 0xD115, 0xB6 }, { 0xD116, 0x02 }, { 0xD117, 0x00 },
    { 0xD118, 0x02 }, { 0xD119, 0x3B }, { 0xD11A, 0x02 }, { 0xD11B, 0x3D },
    { 0xD11C, 0x02 }, { 0xD11D, 0x75 }, { 0xD11E, 0x02 }, { 0xD11F, 0xB1 },
    { 0xD120, 0x02 }, { 0xD121, 0xD5 }, { 0xD122, 0x03 }, { 0xD123, 0x09 },
    { 0xD124, 0x03 }, { 0xD125, 0x28 }, { 0xD126, 0x03 }, { 0xD127, 0x52 },
    { 0xD128, 0x03 }, { 0xD129, 0x6B }, { 0xD12A, 0x03 }, { 0xD12B, 0x8D },
    { 0xD12C, 0x03 }, { 0xD12D, 0xA2 }, { 0xD12E, 0x03 }, { 0xD12F, 0xBB },
    { 0xD130, 0x03 }, { 0xD131, 0xC1 }, { 0xD132, 0x03 }, { 0xD133, 0xC1 },

    { 0xD200, 0x00 }, { 0xD201, 0x37 }, { 0xD202, 0x00 }, { 0xD203, 0x53 },
    { 0xD204, 0x00 }, { 0xD205, 0x79 }, { 0xD206, 0x00 }, { 0xD207, 0x97 },
    { 0xD208, 0x00 }, { 0xD209, 0xB1 }, { 0xD20A, 0x00 }, { 0xD20B, 0xD5 },
    { 0xD20C, 0x00 }, { 0xD20D, 0xF4 }, { 0xD20E, 0x01 }, { 0xD20F, 0x23 },
    { 0xD210, 0x01 }, { 0xD211, 0x49 }, { 0xD212, 0x01 }, { 0xD213, 0x87 },
    { 0xD214, 0x01 }, { 0xD215, 0xB6 }, { 0xD216, 0x02 }, { 0xD217, 0x00 },
    { 0xD218, 0x02 }, { 0xD219, 0x3B }, { 0xD21A, 0x02 }, { 0xD21B, 0x3D },
    { 0xD21C, 0x02 }, { 0xD21D, 0x75 }, { 0xD21E, 0x02 }, { 0xD21F, 0xB1 },
    { 0xD220, 0x02 }, { 0xD221, 0xD5 }, { 0xD222, 0x03 }, { 0xD223, 0x09 },
    { 0xD224, 0x03 }, { 0xD225, 0x28 }, { 0xD226, 0x03 }, { 0xD227, 0x52 },
    { 0xD228, 0x03 }, { 0xD229, 0x6B }, { 0xD22A, 0x03 }, { 0xD22B, 0x8D },
    { 0xD22C, 0x03 }, { 0xD22D, 0xA2 }, { 0xD22E, 0x03 }, { 0xD22F, 0xBB },
    { 0xD230, 0x03 }, { 0xD231, 0xC1 }, { 0xD232, 0x03 }, { 0xD233, 0xC1 },

    { 0xD300, 0x00 }, { 0xD301, 0x37 }, { 0xD302, 0x00 }, { 0xD303, 0x53 },
    { 0xD304, 0x00 }, { 0xD305, 0x79 }, { 0xD306, 0x00 }, { 0xD307, 0x97 },
    { 0xD308, 0x00 }, { 0xD309, 0xB1 }, { 0xD30A, 0x00 }, { 0xD30B, 0xD5 },
    { 0xD30C, 0x00 }, { 0xD30D, 0xF4 }, { 0xD30E, 0x01 }, { 0xD30F, 0x23 },
    { 0xD310, 0x01 }, { 0xD311, 0x49 }, { 0xD312, 0x01 }, { 0xD313, 0x87 },
    { 0xD314, 0x01 }, { 0xD315, 0xB6 }, { 0xD316, 0x02 }, { 0xD317, 0x00 },
    { 0xD318, 0x02 }, { 0xD319, 0x3B }, { 0xD31A, 0x02 }, { 0xD31B, 0x3D },
    { 0xD31C, 0x02 }, { 0xD31D, 0x75 }, { 0xD31E, 0x02 }, { 0xD31F, 0xB1 },
    { 0xD320, 0x02 }, { 0xD321, 0xD5 }, { 0xD322, 0x03 }, { 0xD323, 0x09 },
    { 0xD324, 0x03 }, { 0xD325, 0x28 }, { 0xD326, 0x03 }, { 0xD327, 0x52 },
    { 0xD328, 0x03 }, { 0xD329, 0x6B }, { 0xD32A, 0x03 }, { 0xD32B, 0x8D },
    { 0xD32C, 0x03 }, { 0xD32D, 0xA2 }, { 0xD32E, 0x03 }, { 0xD32F, 0xBB },
    { 0xD330, 0x03 }, { 0xD331, 0xC1 }, { 0xD332, 0x03 }, { 0xD333, 0xC1 },

    { 0xD400, 0x00 }, { 0xD401, 0x37 }, { 0xD402, 0x00 }, { 0xD403, 0x53 },
    { 0xD404, 0x00 }, { 0xD405, 0x79 }, { 0xD406, 0x00 }, { 0xD407, 0x97 },
    { 0xD408, 0x00 }, { 0xD409, 0xB1 }, { 0xD40A, 0x00 }, { 0xD40B, 0xD5 },
    { 0xD40C, 0x00 }, { 0xD40D, 0xF4 }, { 0xD40E, 0x01 }, { 0xD40F, 0x23 },
    { 0xD410, 0x01 }, { 0xD411, 0x49 }, { 0xD412, 0x01 }, { 0xD413, 0x87 },
    { 0xD414, 0x01 }, { 0xD415, 0xB6 }, { 0xD416, 0x02 }, { 0xD417, 0x00 },
    { 0xD418, 0x02 }, { 0xD419, 0x3B }, { 0xD41A, 0x02 }, { 0xD41B, 0x3D },
    { 0xD41C, 0x02 }, { 0xD41D, 0x75 }, { 0xD41E, 0x02 }, { 0xD41F, 0xB1 },
    { 0xD420, 0x02 }, { 0xD421, 0xD5 }, { 0xD422, 0x03 }, { 0xD423, 0x09 },
    { 0xD424, 0x03 }, { 0xD425, 0x28 }, { 0xD426, 0x03 }, { 0xD427, 0x52 },
    { 0xD428, 0x03 }, { 0xD429, 0x6B }, { 0xD42A, 0x03 }, { 0xD42B, 0x8D },
    { 0xD42C, 0x03 }, { 0xD42D, 0xA2 }, { 0xD42E, 0x03 }, { 0xD42F, 0xBB },
    { 0xD430, 0x03 }, { 0xD431, 0xC1 }, { 0xD432, 0x03 }, { 0xD433, 0xC1 },

    { 0xD500, 0x00 }, { 0xD501, 0x37 }, { 0xD502, 0x00 }, { 0xD503, 0x53 },
    { 0xD504, 0x00 }, { 0xD505, 0x79 }, { 0xD506, 0x00 }, { 0xD507, 0x97 },
    { 0xD508, 0x00 }, { 0xD509, 0xB1 }, { 0xD50A, 0x00 }, { 0xD50B, 0xD5 },
    { 0xD50C, 0x00 }, { 0xD50D, 0xF4 }, { 0xD50E, 0x01 }, { 0xD50F, 0x23 },
    { 0xD510, 0x01 }, { 0xD511, 0x49 }, { 0xD512, 0x01 }, { 0xD513, 0x87 },
    { 0xD514, 0x01 }, { 0xD515, 0xB6 }, { 0xD516, 0x02 }, { 0xD517, 0x00 },
    { 0xD518, 0x02 }, { 0xD519, 0x3B }, { 0xD51A, 0x02 }, { 0xD51B, 0x3D },
    { 0xD51C, 0x02 }, { 0xD51D, 0x75 }, { 0xD51E, 0x02 }, { 0xD51F, 0xB1 },
    { 0xD520, 0x02 }, { 0xD521, 0xD5 }, { 0xD522, 0x03 }, { 0xD523, 0x09 },
    { 0xD524, 0x03 }, { 0xD525, 0x28 }, { 0xD526, 0x03 }, { 0xD527, 0x52 },
    { 0xD528, 0x03 }, { 0xD529, 0x6B }, { 0xD52A, 0x03 }, { 0xD52B, 0x8D },
    { 0xD52C, 0x03 }, { 0xD52D, 0xA2 }, { 0xD52E, 0x03 }, { 0xD52F, 0xBB },
    { 0xD530, 0x03 }, { 0xD531, 0xC1 }, { 0xD532, 0x03 }, { 0xD533, 0xC1 },

    { 0xD600, 0x00 }, { 0xD601, 0x37 }, { 0xD602, 0x00 }, { 0xD603, 0x53 },
    { 0xD604, 0x00 }, { 0xD605, 0x79 }, { 0xD606, 0x00 }, { 0xD607, 0x97 },
    { 0xD608, 0x00 }, { 0xD609, 0xB1 }, { 0xD60A, 0x00 }, { 0xD60B, 0xD5 },
    { 0xD60C, 0x00 }, { 0xD60D, 0xF4 }, { 0xD60E, 0x01 }, { 0xD60F, 0x23 },
    { 0xD610, 0x01 }, { 0xD611, 0x49 }, { 0xD612, 0x01 }, { 0xD613, 0x87 },
    { 0xD614, 0x01 }, { 0xD615, 0xB6 }, { 0xD616, 0x02 }, { 0xD617, 0x00 },
    { 0xD618, 0x02 }, { 0xD619, 0x3B }, { 0xD61A, 0x02 }, { 0xD61B, 0x3D },
    { 0xD61C, 0x02 }, { 0xD61D, 0x75 }, { 0xD61E, 0x02 }, { 0xD61F, 0xB1 },
    { 0xD620, 0x02 }, { 0xD621, 0xD5 }, { 0xD622, 0x03 }, { 0xD623, 0x09 },
    { 0xD624, 0x03 }, { 0xD625, 0x28 }, { 0xD626, 0x03 }, { 0xD627, 0x52 },
    { 0xD628, 0x03 }, { 0xD629, 0x6B }, { 0xD62A, 0x03 }, { 0xD62B, 0x8D },
    { 0xD62C, 0x03 }, { 0xD62D, 0xA2 }, { 0xD62E, 0x03 }, { 0xD62F, 0xBB },
    { 0xD630, 0x03 }, { 0xD631, 0xC1 }, { 0xD632, 0x03 }, { 0xD633, 0xC1 },

    { 0xF000, 0x55 }, { 0xF001, 0xAA }, { 0xF002, 0x52 }, { 0xF003, 0x08 },
    { 0xF004, 0x00 }, { 0xB500, 0x50 }, { 0xB000, 0x00 }, { 0xB001, 0x05 },
    { 0xB002, 0x02 }, { 0xB003, 0x05 }, { 0xB004, 0x02 }, { 0xB600, 0x08 },
    { 0xB500, 0x50 }, { 0xB100, 0xCC }, { 0xB101, 0x00 }, { 0xB600, 0x05 },
    { 0xB700, 0x00 }, { 0xB701, 0x00 }, { 0xB800, 0x01 }, { 0xB801, 0x05 },
    { 0xB802, 0x05 }, { 0xB803, 0x05 }, { 0xBC00, 0x02 }, { 0xBC01, 0x00 },
    { 0xBC02, 0x00 }, { 0xCC00, 0x03 }, { 0xCC01, 0x00 }, { 0xCC02, 0x00 },
    { 0xBD00, 0x01 }, { 0xBD01, 0x84 }, { 0xBD02, 0x07 }, { 0xBD03, 0x31 },
    { 0xBD04, 0x00 }, { 0xBA00, 0x01 }, { 0xFF00, 0xAA }, { 0xFF01, 0x55 },
    { 0xFF02, 0x25 }, { 0xFF03, 0x01 }, { 0x3600, 0xC0 }, { 0x3500, 0x00 },
    { 0x3A00, 0x05 },
};

static void app_lcd_write_reg(uint16_t reg, uint16_t data)
{
    APP_LCD_REG = reg;
    APP_LCD_RAM = data;
}

static void app_lcd_write_cmd(uint16_t reg)
{
    APP_LCD_REG = reg;
}

static void app_lcd_write_data(uint16_t data)
{
    APP_LCD_RAM = data;
}

static uint16_t app_lcd_read_data(void)
{
    volatile uint16_t value;
    value = APP_LCD_RAM;
    return value;
}

static void app_lcd_backlight(uint8_t on)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_15, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void app_lcd_gpio_init(void)
{
    GPIO_InitTypeDef gpio;

    __HAL_RCC_FSMC_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF12_FSMC;

    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5 |
               GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_14 |
               GPIO_PIN_15;
    HAL_GPIO_Init(GPIOD, &gpio);

    gpio.Pin = GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 |
               GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 |
               GPIO_PIN_15;
    HAL_GPIO_Init(GPIOE, &gpio);

    gpio.Pin = GPIO_PIN_12;
    HAL_GPIO_Init(GPIOF, &gpio);

    gpio.Pin = GPIO_PIN_12;
    HAL_GPIO_Init(GPIOG, &gpio);

    gpio.Pin = GPIO_PIN_15;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);
    app_lcd_backlight(0);
}

static void app_lcd_fsmc_timing(uint8_t addset, uint8_t datast, uint8_t write)
{
    uint32_t value;

    value = ((uint32_t)(addset & 0x0F)) |
            ((uint32_t)(datast & 0xFF) << 8) |
            (0U << 28);

    if (write)
    {
        FSMC_Bank1E->BWTR[APP_LCD_FSMC_BANK_INDEX] = value;
    }
    else
    {
        FSMC_Bank1->BTCR[APP_LCD_FSMC_BANK_INDEX * 2U + 1U] = value;
    }
}

static void app_lcd_fsmc_init(void)
{
    FSMC_Bank1->BTCR[APP_LCD_FSMC_BANK_INDEX * 2U] =
        FSMC_BCR4_MBKEN | FSMC_BCR4_MWID_0 | FSMC_BCR4_WREN | FSMC_BCR4_EXTMOD;

    app_lcd_fsmc_timing(15, 60, 0);
    app_lcd_fsmc_timing(9, 9, 1);
}

static uint16_t app_lcd_read_id(void)
{
    uint16_t id;

    app_lcd_write_reg(0xF000, 0x0055);
    app_lcd_write_reg(0xF001, 0x00AA);
    app_lcd_write_reg(0xF002, 0x0052);
    app_lcd_write_reg(0xF003, 0x0008);
    app_lcd_write_reg(0xF004, 0x0001);

    app_lcd_write_cmd(0xC500);
    id = app_lcd_read_data();
    id <<= 8;
    app_lcd_write_cmd(0xC501);
    id |= app_lcd_read_data();

    return id;
}

static void app_lcd_nt35510_init(void)
{
    unsigned int i;

    for (i = 0; i < sizeof(app_nt35510_init_seq) / sizeof(app_nt35510_init_seq[0]); i++)
    {
        app_lcd_write_reg(app_nt35510_init_seq[i].reg, app_nt35510_init_seq[i].value);
    }

    app_lcd_write_cmd(0x1100);
    rt_thread_mdelay(120);
    app_lcd_write_cmd(0x2900);
    rt_thread_mdelay(10);
    app_lcd_write_cmd(0x2C00);
}

static void app_lcd_set_addr(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    app_lcd_write_cmd(0x2A00);
    app_lcd_write_data((uint16_t)(x0 >> 8));
    app_lcd_write_cmd(0x2A01);
    app_lcd_write_data((uint16_t)(x0 & 0xFFU));
    app_lcd_write_cmd(0x2A02);
    app_lcd_write_data((uint16_t)(x1 >> 8));
    app_lcd_write_cmd(0x2A03);
    app_lcd_write_data((uint16_t)(x1 & 0xFFU));

    app_lcd_write_cmd(0x2B00);
    app_lcd_write_data((uint16_t)(y0 >> 8));
    app_lcd_write_cmd(0x2B01);
    app_lcd_write_data((uint16_t)(y0 & 0xFFU));
    app_lcd_write_cmd(0x2B02);
    app_lcd_write_data((uint16_t)(y1 >> 8));
    app_lcd_write_cmd(0x2B03);
    app_lcd_write_data((uint16_t)(y1 & 0xFFU));
}

static void app_lcd_write_ram_prepare(void)
{
    app_lcd_write_cmd(0x2C00);
}

static void app_lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint32_t count;
    uint32_t i;
    uint16_t x1;
    uint16_t y1;

    if (x >= APP_LCD_WIDTH || y >= APP_LCD_HEIGHT || w == 0 || h == 0)
    {
        return;
    }

    x1 = x + w - 1U;
    y1 = y + h - 1U;

    if (x1 >= APP_LCD_WIDTH)
    {
        x1 = APP_LCD_WIDTH - 1U;
    }

    if (y1 >= APP_LCD_HEIGHT)
    {
        y1 = APP_LCD_HEIGHT - 1U;
    }

    count = (uint32_t)(x1 - x + 1U) * (uint32_t)(y1 - y + 1U);
    app_lcd_set_addr(x, y, x1, y1);
    app_lcd_write_ram_prepare();

    for (i = 0; i < count; i++)
    {
        app_lcd_write_data(color);
    }
}

static void app_lcd_clear(uint16_t color)
{
    app_lcd_fill_rect(0, 0, APP_LCD_WIDTH, APP_LCD_HEIGHT, color);
}

static void app_lcd_draw_ascii_char(uint16_t x, uint16_t y, char ch, uint8_t scale, uint16_t fg, uint16_t bg)
{
    const uint8_t *rows;
    uint16_t w;
    uint16_t h;
    uint16_t x1;
    uint16_t y1;
    uint16_t px;
    uint16_t py;

    if (scale == 0U || x >= APP_LCD_WIDTH || y >= APP_LCD_HEIGHT)
    {
        return;
    }

    rows = APP_Font_FindAscii5x7(ch);
    w = (uint16_t)(6U * scale);
    h = (uint16_t)(7U * scale);
    x1 = (uint16_t)(x + w - 1U);
    y1 = (uint16_t)(y + h - 1U);

    if (x1 >= APP_LCD_WIDTH)
    {
        x1 = APP_LCD_WIDTH - 1U;
    }
    if (y1 >= APP_LCD_HEIGHT)
    {
        y1 = APP_LCD_HEIGHT - 1U;
    }

    app_lcd_set_addr(x, y, x1, y1);
    app_lcd_write_ram_prepare();

    for (py = 0; py <= (uint16_t)(y1 - y); py++)
    {
        uint8_t row = (uint8_t)(py / scale);

        for (px = 0; px <= (uint16_t)(x1 - x); px++)
        {
            uint8_t col = (uint8_t)(px / scale);
            uint16_t color = bg;

            if (col < 5U && (rows[row] & (uint8_t)(1U << (4U - col))))
            {
                color = fg;
            }

            app_lcd_write_data(color);
        }
    }
}

static void app_lcd_draw_ascii(uint16_t x, uint16_t y, const char *text, uint8_t scale, uint16_t fg, uint16_t bg)
{
    while (*text != '\0')
    {
        app_lcd_draw_ascii_char(x, y, *text, scale, fg, bg);
        x = (uint16_t)(x + 6U * scale);
        text++;
    }
}

static void app_lcd_draw_ascii_field(uint16_t x,
                                     uint16_t y,
                                     uint16_t w,
                                     const char *text,
                                     uint8_t scale,
                                     uint16_t fg,
                                     uint16_t bg)
{
    char field[32];
    rt_size_t i;
    rt_size_t max_chars = w / (6U * scale);

    if (max_chars >= sizeof(field))
    {
        max_chars = sizeof(field) - 1U;
    }

    rt_memset(field, ' ', max_chars);
    field[max_chars] = '\0';
    if (text != RT_NULL)
    {
        for (i = 0; i < max_chars && text[i] != '\0'; i++)
        {
            field[i] = text[i];
        }
    }

    app_lcd_draw_ascii(x, y, field, scale, fg, bg);
}

static void app_lcd_draw_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color)
{
    app_lcd_fill_rect(x, y, w, 1U, color);
}

static void app_lcd_draw_vline(uint16_t x, uint16_t y, uint16_t h, uint16_t color)
{
    app_lcd_fill_rect(x, y, 1U, h, color);
}

static void app_lcd_draw_rect_frame(uint16_t x,
                                    uint16_t y,
                                    uint16_t w,
                                    uint16_t h,
                                    uint8_t thickness,
                                    uint16_t color)
{
    uint8_t i;

    for (i = 0; i < thickness; i++)
    {
        app_lcd_draw_hline((uint16_t)(x + i),
                           (uint16_t)(y + i),
                           (uint16_t)(w - 2U * i),
                           color);
        app_lcd_draw_hline((uint16_t)(x + i),
                           (uint16_t)(y + h - 1U - i),
                           (uint16_t)(w - 2U * i),
                           color);
        app_lcd_draw_vline((uint16_t)(x + i),
                           (uint16_t)(y + i),
                           (uint16_t)(h - 2U * i),
                           color);
        app_lcd_draw_vline((uint16_t)(x + w - 1U - i),
                           (uint16_t)(y + i),
                           (uint16_t)(h - 2U * i),
                           color);
    }
}

static int app_lcd_show_rgb565_image(unsigned short x,
                                     unsigned short y,
                                     unsigned short src_w,
                                     unsigned short src_h,
                                     unsigned char scale,
                                     const unsigned short *pixels,
                                     unsigned char swap_bytes,
                                     unsigned char mirror_x,
                                     unsigned char mirror_y,
                                     unsigned char row_phase,
                                     unsigned char row_stride)
{
    uint16_t out_w;
    uint16_t out_h;
    uint16_t src_x;
    uint16_t src_y;

    if ((pixels == RT_NULL) || (src_w == 0U) || (src_h == 0U))
    {
        return -RT_EINVAL;
    }

    if (scale == 0U)
    {
        scale = 1U;
    }
    if (row_stride == 0U)
    {
        row_stride = 1U;
    }
    row_phase = (uint8_t)(row_phase % row_stride);

    out_w = (uint16_t)(src_w * scale);
    out_h = (uint16_t)(src_h * scale);
    if ((x >= APP_LCD_WIDTH) || (y >= APP_LCD_HEIGHT) ||
        ((uint32_t)x + out_w > APP_LCD_WIDTH) ||
        ((uint32_t)y + out_h > APP_LCD_HEIGHT))
    {
        return -RT_EINVAL;
    }

    if (app_lcd_hw_init() != RT_EOK)
    {
        return -RT_ERROR;
    }

    for (src_y = row_phase; src_y < src_h; src_y = (uint16_t)(src_y + row_stride))
    {
        uint16_t read_y = src_y;
        const unsigned short *row;
        uint8_t repeat_y;

        if (mirror_y != 0U)
        {
            read_y = (uint16_t)(src_h - 1U - src_y);
        }
        row = &pixels[(uint32_t)read_y * src_w];

        for (repeat_y = 0U; repeat_y < scale; repeat_y++)
        {
            uint16_t out_y = (uint16_t)(y + (uint16_t)(src_y * scale) + repeat_y);

            if (row_stride == 1U && src_y == 0U && repeat_y == 0U)
            {
                app_lcd_set_addr(x, y, (uint16_t)(x + out_w - 1U), (uint16_t)(y + out_h - 1U));
            }
            else
            {
                app_lcd_set_addr(x, out_y, (uint16_t)(x + out_w - 1U), out_y);
            }
            app_lcd_write_ram_prepare();

            for (src_x = 0; src_x < src_w; src_x++)
            {
                uint16_t index = src_x;
                uint16_t color;
                uint8_t repeat_x;

                if (mirror_x != 0U)
                {
                    index = (uint16_t)(src_w - 1U - src_x);
                }

                color = row[index];
                if (swap_bytes != 0U)
                {
                    color = (uint16_t)((color << 8) | (color >> 8));
                }

                for (repeat_x = 0U; repeat_x < scale; repeat_x++)
                {
                    app_lcd_write_data(color);
                }
            }
        }
    }

    return RT_EOK;
}

int APP_Display_ShowRgb565ImageMirrorX(unsigned short x,
                                       unsigned short y,
                                       unsigned short src_w,
                                       unsigned short src_h,
                                       unsigned char scale,
                                       const unsigned short *pixels,
                                       unsigned char swap_bytes,
                                       unsigned char row_phase,
                                       unsigned char row_stride)
{
    return app_lcd_show_rgb565_image(x, y, src_w, src_h, scale, pixels, swap_bytes, 1U, 0U, row_phase, row_stride);
}

int APP_Display_ShowRgb565ImageRotate180(unsigned short x,
                                         unsigned short y,
                                         unsigned short src_w,
                                         unsigned short src_h,
                                         unsigned char scale,
                                         const unsigned short *pixels,
                                         unsigned char swap_bytes,
                                         unsigned char row_phase,
                                         unsigned char row_stride)
{
    return app_lcd_show_rgb565_image(x, y, src_w, src_h, scale, pixels, swap_bytes, 1U, 1U, row_phase, row_stride);
}

int APP_Display_EnterCameraPage(void)
{
    int ret;

    ret = app_lcd_hw_init();
    if (ret != RT_EOK)
    {
        return ret;
    }

    app_lcd_clear(APP_LCD_BLACK);
    app_lcd_fill_rect(0, 0, APP_LCD_WIDTH, 64U, APP_LCD_NAVY);
    app_lcd_draw_ascii(16U, 12U, "OV2640 CAMERA", 3U, APP_LCD_WHITE, APP_LCD_NAVY);

    app_lcd_fill_rect(APP_LCD_CAMERA_PREVIEW_X,
                      APP_LCD_CAMERA_PREVIEW_Y,
                      APP_LCD_CAMERA_PREVIEW_W,
                      APP_LCD_CAMERA_PREVIEW_H,
                      APP_LCD_GRAY);
    app_lcd_draw_rect_frame(APP_LCD_CAMERA_PREVIEW_X,
                            APP_LCD_CAMERA_PREVIEW_Y,
                            APP_LCD_CAMERA_PREVIEW_W,
                            APP_LCD_CAMERA_PREVIEW_H,
                            3U,
                            APP_LCD_CYAN);

    app_lcd_fill_rect(APP_LCD_CAMERA_BUTTON_X,
                      APP_LCD_CAMERA_BUTTON_Y,
                      APP_LCD_CAMERA_BUTTON_W,
                      APP_LCD_CAMERA_BUTTON_H,
                      APP_LCD_ORANGE);
    app_lcd_draw_rect_frame(APP_LCD_CAMERA_BUTTON_X,
                            APP_LCD_CAMERA_BUTTON_Y,
                            APP_LCD_CAMERA_BUTTON_W,
                            APP_LCD_CAMERA_BUTTON_H,
                            3U,
                            APP_LCD_WHITE);
    app_lcd_draw_ascii(APP_LCD_CAMERA_BUTTON_X + 88U,
                       APP_LCD_CAMERA_BUTTON_Y + 26U,
                       "SNAPSHOT",
                       3U,
                       APP_LCD_BLACK,
                       APP_LCD_ORANGE);

    app_lcd_fill_rect(APP_LCD_CAMERA_EXIT_X,
                      APP_LCD_CAMERA_EXIT_Y,
                      APP_LCD_CAMERA_EXIT_W,
                      APP_LCD_CAMERA_EXIT_H,
                      APP_LCD_LIGHT_GRAY);
    app_lcd_draw_rect_frame(APP_LCD_CAMERA_EXIT_X,
                            APP_LCD_CAMERA_EXIT_Y,
                            APP_LCD_CAMERA_EXIT_W,
                            APP_LCD_CAMERA_EXIT_H,
                            3U,
                            APP_LCD_WHITE);
    app_lcd_draw_ascii(APP_LCD_CAMERA_EXIT_X + 124U,
                       APP_LCD_CAMERA_EXIT_Y + 26U,
                       "EXIT",
                       3U,
                       APP_LCD_BLACK,
                       APP_LCD_LIGHT_GRAY);

    app_lcd_fill_rect(APP_LCD_CAMERA_STATUS_X,
                      APP_LCD_CAMERA_STATUS_Y,
                      APP_LCD_CAMERA_STATUS_W,
                      APP_LCD_CAMERA_STATUS_H,
                      APP_LCD_LIGHT_GRAY);
    app_lcd_draw_rect_frame(APP_LCD_CAMERA_STATUS_X,
                            APP_LCD_CAMERA_STATUS_Y,
                            APP_LCD_CAMERA_STATUS_W,
                            APP_LCD_CAMERA_STATUS_H,
                            2U,
                            APP_LCD_CYAN);
    APP_Display_CameraStatus("LIVE");

    return RT_EOK;
}

void APP_Display_LeaveCameraPage(void)
{
}

void APP_Display_CameraStatus(const char *status)
{
    if (app_lcd_hw_init() != RT_EOK)
    {
        return;
    }

    app_lcd_fill_rect((uint16_t)(APP_LCD_CAMERA_STATUS_X + 8U),
                      (uint16_t)(APP_LCD_CAMERA_STATUS_Y + 10U),
                      (uint16_t)(APP_LCD_CAMERA_STATUS_W - 16U),
                      (uint16_t)(APP_LCD_CAMERA_STATUS_H - 20U),
                      APP_LCD_LIGHT_GRAY);
    app_lcd_draw_ascii_field((uint16_t)(APP_LCD_CAMERA_STATUS_X + 18U),
                             (uint16_t)(APP_LCD_CAMERA_STATUS_Y + 18U),
                             (uint16_t)(APP_LCD_CAMERA_STATUS_W - 36U),
                             status,
                             2U,
                             APP_LCD_BLACK,
                             APP_LCD_LIGHT_GRAY);
}

int APP_Display_ShowFlowPage(const char *title,
                             const char *line1,
                             const char *line2,
                             const char *line3,
                             const char *line4)
{
    const char *lines[4];
    uint16_t y = 122U;
    rt_size_t i;

    if (app_lcd_hw_init() != RT_EOK)
    {
        return -RT_ERROR;
    }

    lines[0] = line1;
    lines[1] = line2;
    lines[2] = line3;
    lines[3] = line4;

    app_lcd_clear(APP_LCD_BLACK);
    app_lcd_fill_rect(0, 0, APP_LCD_WIDTH, 76U, APP_LCD_NAVY);
    app_lcd_draw_ascii_field(18U,
                             20U,
                             (uint16_t)(APP_LCD_WIDTH - 36U),
                             (title != RT_NULL) ? title : "INSPECTOR",
                             3U,
                             APP_LCD_WHITE,
                             APP_LCD_NAVY);

    app_lcd_fill_rect(28U, 106U, (uint16_t)(APP_LCD_WIDTH - 56U), 308U, APP_LCD_GRAY);
    app_lcd_draw_rect_frame(28U, 106U, (uint16_t)(APP_LCD_WIDTH - 56U), 308U, 2U, APP_LCD_CYAN);

    for (i = 0; i < 4U; i++)
    {
        if ((lines[i] != RT_NULL) && (lines[i][0] != '\0'))
        {
            app_lcd_draw_ascii_field(52U,
                                     y,
                                     (uint16_t)(APP_LCD_WIDTH - 104U),
                                     lines[i],
                                     2U,
                                     APP_LCD_WHITE,
                                     APP_LCD_GRAY);
        }
        y = (uint16_t)(y + 64U);
    }

    return RT_EOK;
}

int APP_Display_ShowWelcomePage(void)
{
    if (app_lcd_hw_init() != RT_EOK)
    {
        return -RT_ERROR;
    }

    app_lcd_clear(APP_LCD_BLACK);
    app_lcd_fill_rect(0U, 0U, APP_LCD_WIDTH, APP_LCD_HEIGHT, APP_LCD_NAVY);
    app_lcd_draw_ascii(84U, 344U, "WELCOME", 6U, APP_LCD_WHITE, APP_LCD_NAVY);
    app_lcd_draw_ascii(116U, 430U, "INSPECTOR", 3U, APP_LCD_CYAN, APP_LCD_NAVY);

    return RT_EOK;
}

static int app_lcd_show_card_page(const char *title, const char *hint)
{
    if (app_lcd_hw_init() != RT_EOK)
    {
        return -RT_ERROR;
    }

    app_lcd_clear(APP_LCD_BLACK);
    app_lcd_fill_rect(0U, 0U, APP_LCD_WIDTH, 82U, APP_LCD_NAVY);
    app_lcd_draw_ascii_field(26U, 22U, 420U, title, 3U, APP_LCD_WHITE, APP_LCD_NAVY);
    app_lcd_draw_ascii_field(46U, 126U, 388U, hint, 3U, APP_LCD_CYAN, APP_LCD_BLACK);

    app_lcd_fill_rect(APP_LCD_NFC_STATUS_X,
                      APP_LCD_NFC_STATUS_Y,
                      APP_LCD_NFC_STATUS_W,
                      APP_LCD_NFC_STATUS_H,
                      APP_LCD_GRAY);
    app_lcd_draw_rect_frame(APP_LCD_NFC_STATUS_X,
                            APP_LCD_NFC_STATUS_Y,
                            APP_LCD_NFC_STATUS_W,
                            APP_LCD_NFC_STATUS_H,
                            2U,
                            APP_LCD_CYAN);
    APP_Display_NfcStatus("WAITING CARD", "", "");
    APP_Display_NfcRegisterButton(0);

    return RT_EOK;
}

int APP_Display_ShowNfcPage(void)
{
    return app_lcd_show_card_page("NFC VERIFY", "PLACE NFC CARD");
}

int APP_Display_ShowRfidPage(void)
{
    return app_lcd_show_card_page("RFID VERIFY", "PLACE RFID CARD");
}

static void app_lcd_card_status(const char *line1, const char *line2, const char *line3)
{
    if (app_lcd_hw_init() != RT_EOK)
    {
        return;
    }

    app_lcd_fill_rect((uint16_t)(APP_LCD_NFC_STATUS_X + 12U),
                      (uint16_t)(APP_LCD_NFC_STATUS_Y + 12U),
                      (uint16_t)(APP_LCD_NFC_STATUS_W - 24U),
                      (uint16_t)(APP_LCD_NFC_STATUS_H - 24U),
                      APP_LCD_GRAY);
    app_lcd_draw_ascii_field((uint16_t)(APP_LCD_NFC_STATUS_X + 24U),
                             (uint16_t)(APP_LCD_NFC_STATUS_Y + 28U),
                             (uint16_t)(APP_LCD_NFC_STATUS_W - 48U),
                             (line1 != RT_NULL) ? line1 : "",
                             2U,
                             APP_LCD_WHITE,
                             APP_LCD_GRAY);
    app_lcd_draw_ascii_field((uint16_t)(APP_LCD_NFC_STATUS_X + 24U),
                             (uint16_t)(APP_LCD_NFC_STATUS_Y + 82U),
                             (uint16_t)(APP_LCD_NFC_STATUS_W - 48U),
                             (line2 != RT_NULL) ? line2 : "",
                             2U,
                             APP_LCD_WHITE,
                             APP_LCD_GRAY);
    app_lcd_draw_ascii_field((uint16_t)(APP_LCD_NFC_STATUS_X + 24U),
                             (uint16_t)(APP_LCD_NFC_STATUS_Y + 136U),
                             (uint16_t)(APP_LCD_NFC_STATUS_W - 48U),
                             (line3 != RT_NULL) ? line3 : "",
                             2U,
                             APP_LCD_WHITE,
                             APP_LCD_GRAY);
}

void APP_Display_NfcStatus(const char *line1, const char *line2, const char *line3)
{
    app_lcd_card_status(line1, line2, line3);
}

void APP_Display_RfidStatus(const char *line1, const char *line2, const char *line3)
{
    app_lcd_card_status(line1, line2, line3);
}

static void app_lcd_card_register_button(int visible)
{
    if (app_lcd_hw_init() != RT_EOK)
    {
        return;
    }

    if (visible)
    {
        app_lcd_fill_rect(APP_LCD_CARD_REGISTER_X,
                          APP_LCD_NFC_BUTTON_Y,
                          APP_LCD_CARD_REGISTER_W,
                          APP_LCD_NFC_BUTTON_H,
                          APP_LCD_ORANGE);
        app_lcd_draw_rect_frame(APP_LCD_CARD_REGISTER_X,
                                APP_LCD_NFC_BUTTON_Y,
                                APP_LCD_CARD_REGISTER_W,
                                APP_LCD_NFC_BUTTON_H,
                                3U,
                                APP_LCD_WHITE);
        app_lcd_draw_ascii((uint16_t)(APP_LCD_CARD_REGISTER_X + 30U),
                           (uint16_t)(APP_LCD_NFC_BUTTON_Y + 28U),
                           "REGISTER",
                           2U,
                           APP_LCD_BLACK,
                           APP_LCD_ORANGE);

        app_lcd_fill_rect(APP_LCD_CARD_RESCAN_X,
                          APP_LCD_NFC_BUTTON_Y,
                          APP_LCD_CARD_RESCAN_W,
                          APP_LCD_NFC_BUTTON_H,
                          APP_LCD_LIGHT_GRAY);
        app_lcd_draw_rect_frame(APP_LCD_CARD_RESCAN_X,
                                APP_LCD_NFC_BUTTON_Y,
                                APP_LCD_CARD_RESCAN_W,
                                APP_LCD_NFC_BUTTON_H,
                                3U,
                                APP_LCD_WHITE);
        app_lcd_draw_ascii((uint16_t)(APP_LCD_CARD_RESCAN_X + 48U),
                           (uint16_t)(APP_LCD_NFC_BUTTON_Y + 28U),
                           "RESCAN",
                           2U,
                           APP_LCD_BLACK,
                           APP_LCD_LIGHT_GRAY);
    }
    else
    {
        app_lcd_fill_rect(APP_LCD_CARD_BUTTON_CLEAR_X,
                          APP_LCD_NFC_BUTTON_Y,
                          APP_LCD_CARD_BUTTON_CLEAR_W,
                          APP_LCD_NFC_BUTTON_H,
                          APP_LCD_BLACK);
    }
}

void APP_Display_NfcRegisterButton(int visible)
{
    app_lcd_card_register_button(visible);
}

void APP_Display_RfidRegisterButton(int visible)
{
    app_lcd_card_register_button(visible);
}

static int app_lcd_card_register_hit(unsigned short x, unsigned short y)
{
    return (x >= APP_LCD_CARD_REGISTER_X) &&
           (x < (APP_LCD_CARD_REGISTER_X + APP_LCD_CARD_REGISTER_W)) &&
           (y >= APP_LCD_NFC_BUTTON_Y) &&
           (y < (APP_LCD_NFC_BUTTON_Y + APP_LCD_NFC_BUTTON_H));
}

static int app_lcd_card_rescan_hit(unsigned short x, unsigned short y)
{
    return (x >= APP_LCD_CARD_RESCAN_X) &&
           (x < (APP_LCD_CARD_RESCAN_X + APP_LCD_CARD_RESCAN_W)) &&
           (y >= APP_LCD_NFC_BUTTON_Y) &&
           (y < (APP_LCD_NFC_BUTTON_Y + APP_LCD_NFC_BUTTON_H));
}

int APP_Display_NfcRegisterHit(unsigned short x, unsigned short y)
{
    return app_lcd_card_register_hit(x, y);
}

int APP_Display_RfidRegisterHit(unsigned short x, unsigned short y)
{
    return app_lcd_card_register_hit(x, y);
}

int APP_Display_NfcRescanHit(unsigned short x, unsigned short y)
{
    return app_lcd_card_rescan_hit(x, y);
}

int APP_Display_RfidRescanHit(unsigned short x, unsigned short y)
{
    return app_lcd_card_rescan_hit(x, y);
}

int APP_Display_ShowResultChoicePage(const char *line1,
                                     const char *line2,
                                     const char *line3)
{
    int ret;

    ret = APP_Display_ShowFlowPage("UPLOAD DONE",
                                   line1,
                                   line2,
                                   line3,
                                   "SELECT NEXT STEP");
    if (ret != RT_EOK)
    {
        return ret;
    }

    app_lcd_fill_rect(APP_LCD_NEXT_BUTTON_X,
                      APP_LCD_NEXT_BUTTON_Y,
                      APP_LCD_NEXT_BUTTON_W,
                      APP_LCD_NEXT_BUTTON_H,
                      APP_LCD_ORANGE);
    app_lcd_draw_rect_frame(APP_LCD_NEXT_BUTTON_X,
                            APP_LCD_NEXT_BUTTON_Y,
                            APP_LCD_NEXT_BUTTON_W,
                            APP_LCD_NEXT_BUTTON_H,
                            3U,
                            APP_LCD_WHITE);
    app_lcd_draw_ascii((uint16_t)(APP_LCD_NEXT_BUTTON_X + 30U),
                       (uint16_t)(APP_LCD_NEXT_BUTTON_Y + 28U),
                       "CONTINUE",
                       2U,
                       APP_LCD_BLACK,
                       APP_LCD_ORANGE);

    app_lcd_fill_rect(APP_LCD_FINISH_BUTTON_X,
                      APP_LCD_FINISH_BUTTON_Y,
                      APP_LCD_FINISH_BUTTON_W,
                      APP_LCD_FINISH_BUTTON_H,
                      APP_LCD_LIGHT_GRAY);
    app_lcd_draw_rect_frame(APP_LCD_FINISH_BUTTON_X,
                            APP_LCD_FINISH_BUTTON_Y,
                            APP_LCD_FINISH_BUTTON_W,
                            APP_LCD_FINISH_BUTTON_H,
                            3U,
                            APP_LCD_WHITE);
    app_lcd_draw_ascii((uint16_t)(APP_LCD_FINISH_BUTTON_X + 46U),
                       (uint16_t)(APP_LCD_FINISH_BUTTON_Y + 28U),
                       "FINISH",
                       2U,
                       APP_LCD_BLACK,
                       APP_LCD_LIGHT_GRAY);

    return RT_EOK;
}

int APP_Display_ResultContinueHit(unsigned short x, unsigned short y)
{
    return (x >= APP_LCD_NEXT_BUTTON_X) &&
           (x < (APP_LCD_NEXT_BUTTON_X + APP_LCD_NEXT_BUTTON_W)) &&
           (y >= APP_LCD_NEXT_BUTTON_Y) &&
           (y < (APP_LCD_NEXT_BUTTON_Y + APP_LCD_NEXT_BUTTON_H));
}

int APP_Display_ResultFinishHit(unsigned short x, unsigned short y)
{
    return (x >= APP_LCD_FINISH_BUTTON_X) &&
           (x < (APP_LCD_FINISH_BUTTON_X + APP_LCD_FINISH_BUTTON_W)) &&
           (y >= APP_LCD_FINISH_BUTTON_Y) &&
           (y < (APP_LCD_FINISH_BUTTON_Y + APP_LCD_FINISH_BUTTON_H));
}

static int app_lcd_hw_init(void)
{
    if (app_lcd_ready)
    {
        return RT_EOK;
    }

    app_lcd_gpio_init();
    app_lcd_fsmc_init();
    rt_thread_mdelay(50);

    app_lcd_id = app_lcd_read_id();
    rt_kprintf("APP lcd: read id 0x%04x\r\n", app_lcd_id);

    app_lcd_nt35510_init();
    app_lcd_fsmc_timing(2, 2, 1);
    app_lcd_backlight(1);
    app_lcd_ready = 1;

    return RT_EOK;
}

static void APP_Display_ThreadEntry(void *parameter)
{
    (void)parameter;

    if (app_lcd_hw_init() != RT_EOK)
    {
        rt_kprintf("APP lcd: auto init failed\r\n");
        return;
    }

    rt_kprintf("APP lcd: auto init ok, id=0x%04x\r\n", app_lcd_id);
}

static int APP_Display_AutoStart(void)
{
    rt_err_t ret;

    if (app_lcd_thread_started)
    {
        return RT_EOK;
    }

    ret = rt_thread_init(&app_lcd_thread_obj,
                         "lcd_app",
                         APP_Display_ThreadEntry,
                         RT_NULL,
                         app_lcd_thread_stack,
                         sizeof(app_lcd_thread_stack),
                         APP_LCD_THREAD_PRIORITY,
                         APP_LCD_THREAD_TICK);
    if (ret != RT_EOK)
    {
        rt_kprintf("APP lcd: auto thread init failed ret=%d\r\n", ret);
        return ret;
    }

    ret = rt_thread_startup(&app_lcd_thread_obj);
    if (ret != RT_EOK)
    {
        rt_thread_detach(&app_lcd_thread_obj);
        rt_kprintf("APP lcd: auto thread startup failed ret=%d\r\n", ret);
        return ret;
    }

    app_lcd_thread_started = 1U;
    return RT_EOK;
}
INIT_APP_EXPORT(APP_Display_AutoStart);

typedef int (*app_command_handler_t)(int argc, char **argv);

typedef struct
{
    const char *name;
    const char *usage;
    app_command_handler_t handler;
} app_command_entry_t;

static const app_command_entry_t app_command_table[] =
{
    {"flow", "APP flow [start|status|add|reset]",     APP_Flow_Test},
    {"esp",  "APP esp <AT command>|status|join|ping", APP_Esp8266_Test},
    {"mqtt", "APP mqtt [status|start|pub|stop]",      APP_Mqtt_Test},
    {"ov",   "APP ov live",                           APP_Ov_Test},
    {"sd",   "APP sd",                                APP_SdDiag_Test},
    {"rfid", "APP rfid",                              APP_Rc522_Test},
    {"nfc",  "APP nfc",                               APP_Pn532_Test},
};

static void app_command_print_usage(void)
{
    rt_size_t i;

    rt_kprintf("Usage:\r\n");
    for (i = 0; i < (sizeof(app_command_table) / sizeof(app_command_table[0])); i++)
    {
        rt_kprintf("  %s\r\n", app_command_table[i].usage);
    }
}

static int APP_Command(int argc, char **argv)
{
    rt_size_t i;

    if (argc >= 2)
    {
        for (i = 0; i < (sizeof(app_command_table) / sizeof(app_command_table[0])); i++)
        {
            if (rt_strcmp(argv[1], app_command_table[i].name) == 0)
            {
                return app_command_table[i].handler(argc - 1, argv + 1);
            }
        }
    }

    app_command_print_usage();
    return RT_EOK;
}
MSH_CMD_EXPORT_ALIAS(APP_Command, APP, Explorer application commands);

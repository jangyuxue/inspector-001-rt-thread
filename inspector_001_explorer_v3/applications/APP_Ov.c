#include <rtthread.h>
#include <rthw.h>
#include <rtdevice.h>
#include <dfs_posix.h>
#include <board.h>
#include <string.h>

#include "APP_Display.h"
#include "APP_Ov.h"
#include "APP_Touch.h"

/* Explorer V3 OV2640 最小探针阶段只验证 XCLK、PWDN、RESET 和 SCCB。 */
#define APP_OV_SCCB_DELAY_US         5U
#define APP_OV_BOOT_DELAY_MS         3U
#define APP_OV_RESET_LOW_MS          10U
#define APP_OV_RESET_HIGH_MS         10U
#define APP_OV_PREVIEW_WIDTH         200U
#define APP_OV_PREVIEW_HEIGHT        160U
#define APP_OV_PREVIEW_SCALE         1U
#define APP_OV_CAPTURE_WORDS         ((APP_OV_PREVIEW_WIDTH * APP_OV_PREVIEW_HEIGHT) / 2U)
#define APP_OV_CAPTURE_BYTES         (APP_OV_CAPTURE_WORDS * sizeof(rt_uint32_t))
#define APP_OV_CAPTURE_TIMEOUT_MS    1000U
#define APP_OV_LIVE_THREAD_STACK     4096U
#define APP_OV_LIVE_THREAD_PRIORITY  22U
#define APP_OV_LIVE_THREAD_TICK      10U
#define APP_OV_LIVE_ROW_STRIDE       2U
#define APP_OV_SDIO_SETTLE_MS        20U
#define APP_OV_WRITE_CHUNK_SIZE      4096U
#define APP_OV_PHOTO_ROOT            "/sdcard/INSPECT"
#define APP_OV_PHOTO_DIR             "/sdcard/INSPECT/PHOTOS"
#define APP_OV_PHOTO_PATH_LEN        64U

/* 摄像头页面触摸区域，需要和 APP_Display.c 的页面布局一致。 */
#define APP_OV_LIVE_BUTTON_X         80U
#define APP_OV_LIVE_BUTTON_Y         392U
#define APP_OV_LIVE_BUTTON_W         320U
#define APP_OV_LIVE_BUTTON_H         72U
#define APP_OV_EXIT_BUTTON_X         80U
#define APP_OV_EXIT_BUTTON_Y         492U
#define APP_OV_EXIT_BUTTON_W         320U
#define APP_OV_EXIT_BUTTON_H         72U
#define APP_OV_LIVE_PREVIEW_X        140U
#define APP_OV_LIVE_PREVIEW_Y        150U

/* 板载 OV 摄像头接口固定引脚。 */
#define APP_OV_XCLK_PORT             GPIOA
#define APP_OV_XCLK_PIN              GPIO_PIN_8
#define APP_OV_SCL_PORT              GPIOD
#define APP_OV_SCL_PIN               GPIO_PIN_6
#define APP_OV_SDA_PORT              GPIOD
#define APP_OV_SDA_PIN               GPIO_PIN_7
#define APP_OV_PWDN_PORT             GPIOG
#define APP_OV_PWDN_PIN              GPIO_PIN_9
#define APP_OV_RESET_PORT            GPIOG
#define APP_OV_RESET_PIN             GPIO_PIN_15

/* OV2640 SCCB 7 位地址是 0x30，对应总线写地址 0x60、读地址 0x61。 */
#define APP_OV_SCCB_ADDR             0x30U
#define APP_OV_SENSOR_BANK_SEL       0xFFU
#define APP_OV_SENSOR_CLKRC          0x11U
#define APP_OV_SENSOR_COM7           0x12U
#define APP_OV_SENSOR_MIDH           0x1CU
#define APP_OV_SENSOR_MIDL           0x1DU
#define APP_OV_SENSOR_PIDH           0x0AU
#define APP_OV_SENSOR_PIDL           0x0BU

typedef struct
{
    rt_uint8_t reg;
    rt_uint8_t val;
} app_ov_reg_val_t;

typedef enum
{
    APP_OV_TOUCH_NONE = 0,
    APP_OV_TOUCH_SNAPSHOT,
    APP_OV_TOUCH_EXIT,
} app_ov_touch_action_t;

typedef enum
{
    APP_OV_BUS_OWNER_SDIO = 0,
    APP_OV_BUS_OWNER_CAMERA,
} app_ov_bus_owner_t;

/* OV2640 SVGA 基础初始化表，来源于正点原子 Explorer F407 OV2640 例程并按本工程命名适配。 */
static const app_ov_reg_val_t app_ov_init_svga_cfg[] =
{
    {0xFF, 0x00}, {0x2C, 0xFF}, {0x2E, 0xDF}, {0xFF, 0x01},
    {0x3C, 0x32}, {0x11, 0x00}, {0x09, 0x02}, {0x04, 0xA8},
    {0x13, 0xE5}, {0x14, 0x48}, {0x2C, 0x0C}, {0x33, 0x78},
    {0x3A, 0x33}, {0x3B, 0xFB}, {0x3E, 0x00}, {0x43, 0x11},
    {0x16, 0x10}, {0x39, 0x92}, {0x35, 0xDA}, {0x22, 0x1A},
    {0x37, 0xC3}, {0x23, 0x00}, {0x34, 0xC0}, {0x36, 0x1A},
    {0x06, 0x88}, {0x07, 0xC0}, {0x0D, 0x87}, {0x0E, 0x41},
    {0x4C, 0x00}, {0x48, 0x00}, {0x5B, 0x00}, {0x42, 0x03},
    {0x4A, 0x81}, {0x21, 0x99}, {0x24, 0x40}, {0x25, 0x38},
    {0x26, 0x82}, {0x5C, 0x00}, {0x63, 0x00}, {0x46, 0x22},
    {0x0C, 0x3C}, {0x61, 0x70}, {0x62, 0x80}, {0x7C, 0x05},
    {0x20, 0x80}, {0x28, 0x30}, {0x6C, 0x00}, {0x6D, 0x80},
    {0x6E, 0x00}, {0x70, 0x02}, {0x71, 0x94}, {0x73, 0xC1},
    {0x3D, 0x34}, {0x5A, 0x57}, {0x12, 0x40}, {0x17, 0x11},
    {0x18, 0x43}, {0x19, 0x00}, {0x1A, 0x4B}, {0x32, 0x09},
    {0x37, 0xC0}, {0x4F, 0xCA}, {0x50, 0xA8}, {0x5A, 0x23},
    {0x6D, 0x00}, {0x3D, 0x38}, {0xFF, 0x00}, {0xE5, 0x7F},
    {0xF9, 0xC0}, {0x41, 0x24}, {0xE0, 0x14}, {0x76, 0xFF},
    {0x33, 0xA0}, {0x42, 0x20}, {0x43, 0x18}, {0x4C, 0x00},
    {0x87, 0xD5}, {0x88, 0x3F}, {0xD7, 0x03}, {0xD9, 0x10},
    {0xD3, 0x82}, {0xC8, 0x08}, {0xC9, 0x80}, {0x7C, 0x00},
    {0x7D, 0x00}, {0x7C, 0x03}, {0x7D, 0x48}, {0x7D, 0x48},
    {0x7C, 0x08}, {0x7D, 0x20}, {0x7D, 0x10}, {0x7D, 0x0E},
    {0x90, 0x00}, {0x91, 0x0E}, {0x91, 0x1A}, {0x91, 0x31},
    {0x91, 0x5A}, {0x91, 0x69}, {0x91, 0x75}, {0x91, 0x7E},
    {0x91, 0x88}, {0x91, 0x8F}, {0x91, 0x96}, {0x91, 0xA3},
    {0x91, 0xAF}, {0x91, 0xC4}, {0x91, 0xD7}, {0x91, 0xE8},
    {0x91, 0x20}, {0x92, 0x00}, {0x93, 0x06}, {0x93, 0xE3},
    {0x93, 0x05}, {0x93, 0x05}, {0x93, 0x00}, {0x93, 0x04},
    {0x93, 0x00}, {0x93, 0x00}, {0x93, 0x00}, {0x93, 0x00},
    {0x93, 0x00}, {0x93, 0x00}, {0x93, 0x00}, {0x96, 0x00},
    {0x97, 0x08}, {0x97, 0x19}, {0x97, 0x02}, {0x97, 0x0C},
    {0x97, 0x24}, {0x97, 0x30}, {0x97, 0x28}, {0x97, 0x26},
    {0x97, 0x02}, {0x97, 0x98}, {0x97, 0x80}, {0x97, 0x00},
    {0x97, 0x00}, {0xC3, 0xED}, {0xA4, 0x00}, {0xA8, 0x00},
    {0xC5, 0x11}, {0xC6, 0x51}, {0xBF, 0x80}, {0xC7, 0x10},
    {0xB6, 0x66}, {0xB8, 0xA5}, {0xB7, 0x64}, {0xB9, 0x7C},
    {0xB3, 0xAF}, {0xB4, 0x97}, {0xB5, 0xFF}, {0xB0, 0xC5},
    {0xB1, 0x94}, {0xB2, 0x0F}, {0xC4, 0x5C}, {0xC0, 0x64},
    {0xC1, 0x4B}, {0x8C, 0x00}, {0x86, 0x3D}, {0x50, 0x00},
    {0x51, 0xC8}, {0x52, 0x96}, {0x53, 0x00}, {0x54, 0x00},
    {0x55, 0x00}, {0x5A, 0xC8}, {0x5B, 0x96}, {0x5C, 0x00},
    {0xD3, 0x02}, {0xC3, 0xED}, {0x7F, 0x00}, {0xDA, 0x09},
    {0xE5, 0x1F}, {0xE1, 0x67}, {0xE0, 0x00}, {0xDD, 0x7F},
    {0x05, 0x00},
};

/* DCMI 第一阶段只验证有帧进入 RAM，因此先使用无需压缩解析的 RGB565 输出。 */
static const app_ov_reg_val_t app_ov_rgb565_cfg[] =
{
    {0xFF, 0x00}, {0xDA, 0x09}, {0xD7, 0x03}, {0xDF, 0x02},
    {0x33, 0xA0}, {0x3C, 0x00}, {0xE1, 0x67}, {0xFF, 0x01},
    {0xE0, 0x00}, {0xE1, 0x00}, {0xE5, 0x00}, {0xD7, 0x00},
    {0xDA, 0x00}, {0xE0, 0x00},
};

static DCMI_HandleTypeDef app_ov_dcmi;
static DMA_HandleTypeDef app_ov_dma;
static volatile rt_bool_t app_ov_frame_done = RT_FALSE;
static volatile rt_bool_t app_ov_dma_done = RT_FALSE;
static volatile rt_bool_t app_ov_error = RT_FALSE;
static rt_uint32_t *app_ov_capture_buf = RT_NULL;
static rt_thread_t app_ov_live_thread;
static volatile rt_bool_t app_ov_live_running = RT_FALSE;
static volatile app_ov_bus_owner_t app_ov_bus_owner = APP_OV_BUS_OWNER_SDIO;

static const char *app_ov_last_step = "idle";
static rt_err_t app_ov_last_result = RT_EOK;

extern void APP_SdDiag_RestorePins(void);
extern int APP_SdDiag_EnsureMounted(void);

static void app_ov_bus_idle(void);

static void app_ov_set_result(const char *step, rt_err_t result)
{
    app_ov_last_step = step;
    app_ov_last_result = result;
}

static rt_err_t app_ov_capture_buffer_acquire(void)
{
    if (app_ov_capture_buf != RT_NULL)
    {
        return RT_EOK;
    }

    app_ov_capture_buf = (rt_uint32_t *)rt_malloc_align(APP_OV_CAPTURE_BYTES, 4U);
    if (app_ov_capture_buf == RT_NULL)
    {
        app_ov_set_result("capture buffer malloc", -RT_ENOMEM);
        rt_kprintf("APP ov: capture buffer malloc failed bytes=%u\r\n",
                   (unsigned int)APP_OV_CAPTURE_BYTES);
        return -RT_ENOMEM;
    }

    rt_kprintf("APP ov: capture buffer allocated bytes=%u\r\n",
               (unsigned int)APP_OV_CAPTURE_BYTES);
    return RT_EOK;
}

static void app_ov_capture_buffer_release(void)
{
    if (app_ov_capture_buf == RT_NULL)
    {
        return;
    }

    rt_free_align(app_ov_capture_buf);
    app_ov_capture_buf = RT_NULL;
    rt_kprintf("APP ov: capture buffer released\r\n");
}

static void app_ov_delay_us(rt_uint32_t us)
{
    rt_hw_us_delay(us);
}

static void app_ov_gpio_output_pp(GPIO_TypeDef *port, rt_uint16_t pin)
{
    GPIO_InitTypeDef gpio;

    gpio.Pin = pin;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(port, &gpio);
}

static void app_ov_gpio_input_pullup(GPIO_TypeDef *port, rt_uint16_t pin)
{
    GPIO_InitTypeDef gpio;

    gpio.Pin = pin;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(port, &gpio);
}

static void app_ov_scl(rt_base_t level)
{
    HAL_GPIO_WritePin(APP_OV_SCL_PORT, APP_OV_SCL_PIN, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void app_ov_sda(rt_base_t level)
{
    HAL_GPIO_WritePin(APP_OV_SDA_PORT, APP_OV_SDA_PIN, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static rt_base_t app_ov_read_sda(void)
{
    return (HAL_GPIO_ReadPin(APP_OV_SDA_PORT, APP_OV_SDA_PIN) == GPIO_PIN_SET) ? PIN_HIGH : PIN_LOW;
}

static void app_ov_sda_input(void)
{
    app_ov_gpio_input_pullup(APP_OV_SDA_PORT, APP_OV_SDA_PIN);
}

static void app_ov_sda_output(void)
{
    app_ov_gpio_output_pp(APP_OV_SDA_PORT, APP_OV_SDA_PIN);
}

static void app_ov_sccb_idle(void)
{
    app_ov_sda_output();
    app_ov_sda(PIN_HIGH);
    app_ov_scl(PIN_HIGH);
    app_ov_delay_us(APP_OV_SCCB_DELAY_US);
}

static void app_ov_sccb_start(void)
{
    app_ov_sda_output();
    app_ov_sda(PIN_HIGH);
    app_ov_scl(PIN_HIGH);
    app_ov_delay_us(APP_OV_SCCB_DELAY_US);
    app_ov_sda(PIN_LOW);
    app_ov_delay_us(APP_OV_SCCB_DELAY_US);
    app_ov_scl(PIN_LOW);
}

static void app_ov_sccb_stop(void)
{
    app_ov_sda_output();
    app_ov_sda(PIN_LOW);
    app_ov_delay_us(APP_OV_SCCB_DELAY_US);
    app_ov_scl(PIN_HIGH);
    app_ov_delay_us(APP_OV_SCCB_DELAY_US);
    app_ov_sda(PIN_HIGH);
    app_ov_delay_us(APP_OV_SCCB_DELAY_US);
}

static void app_ov_sccb_write_byte(rt_uint8_t data)
{
    rt_uint8_t bit;

    app_ov_sda_output();
    for (bit = 0; bit < 8; bit++)
    {
        app_ov_sda((data & 0x80U) ? PIN_HIGH : PIN_LOW);
        app_ov_delay_us(APP_OV_SCCB_DELAY_US);
        app_ov_scl(PIN_HIGH);
        app_ov_delay_us(APP_OV_SCCB_DELAY_US);
        app_ov_scl(PIN_LOW);
        data <<= 1;
    }

    app_ov_sda_input();
    app_ov_delay_us(APP_OV_SCCB_DELAY_US);
    app_ov_scl(PIN_HIGH);
    app_ov_delay_us(APP_OV_SCCB_DELAY_US);
    (void)app_ov_read_sda();
    app_ov_scl(PIN_LOW);
    app_ov_sda_output();
    app_ov_sda(PIN_HIGH);
}

static rt_err_t app_ov_sccb_read_byte(rt_uint8_t *data, rt_bool_t ack)
{
    rt_uint8_t bit;
    rt_uint8_t value = 0;

    if (data == RT_NULL)
    {
        return -RT_EINVAL;
    }

    app_ov_sda_input();
    for (bit = 0; bit < 8; bit++)
    {
        value <<= 1;
        app_ov_delay_us(APP_OV_SCCB_DELAY_US);
        app_ov_scl(PIN_HIGH);
        app_ov_delay_us(APP_OV_SCCB_DELAY_US);
        if (app_ov_read_sda() == PIN_HIGH)
        {
            value |= 0x01U;
        }
        app_ov_scl(PIN_LOW);
    }

    app_ov_sda_output();
    app_ov_sda(ack ? PIN_LOW : PIN_HIGH);
    app_ov_delay_us(APP_OV_SCCB_DELAY_US);
    app_ov_scl(PIN_HIGH);
    app_ov_delay_us(APP_OV_SCCB_DELAY_US);
    app_ov_scl(PIN_LOW);
    app_ov_sda(PIN_HIGH);
    *data = value;
    return RT_EOK;
}

static rt_err_t app_ov_write_reg(rt_uint8_t reg, rt_uint8_t val)
{
    app_ov_sccb_start();
    app_ov_sccb_write_byte((APP_OV_SCCB_ADDR << 1) | 0x00U);
    app_ov_sccb_write_byte(reg);
    app_ov_sccb_write_byte(val);
    app_ov_sccb_stop();
    return RT_EOK;
}

static rt_err_t app_ov_read_reg(rt_uint8_t reg, rt_uint8_t *val)
{
    rt_err_t ret;

    if (val == RT_NULL)
    {
        return -RT_EINVAL;
    }

    app_ov_sccb_start();
    app_ov_sccb_write_byte((APP_OV_SCCB_ADDR << 1) | 0x00U);
    app_ov_sccb_write_byte(reg);
    app_ov_sccb_stop();
    app_ov_sccb_start();
    app_ov_sccb_write_byte((APP_OV_SCCB_ADDR << 1) | 0x01U);
    ret = app_ov_sccb_read_byte(val, RT_FALSE);
    app_ov_sccb_stop();
    return ret;
}

static rt_err_t app_ov_write_table(const app_ov_reg_val_t *table, rt_size_t count, const char *step)
{
    rt_size_t i;
    rt_err_t ret;

    if ((table == RT_NULL) || (step == RT_NULL))
    {
        return -RT_EINVAL;
    }

    for (i = 0; i < count; i++)
    {
        ret = app_ov_write_reg(table[i].reg, table[i].val);
        if (ret != RT_EOK)
        {
            app_ov_set_result(step, ret);
            return ret;
        }
    }

    return RT_EOK;
}

static void app_ov_xclk_init(void)
{
    /* HSE=8 MHz，处于 OV2640 允许的 XCLK 输入范围内，先用于可靠探针。 */
    HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_HSE, RCC_MCODIV_1);
}

static void app_ov_dcmi_gpio_config(GPIO_TypeDef *port, rt_uint32_t pins)
{
    GPIO_InitTypeDef gpio;

    gpio.Pin = pins;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF13_DCMI;
    HAL_GPIO_Init(port, &gpio);
}

static void app_ov_sdio_gpio_restore(void)
{
    APP_SdDiag_RestorePins();
    app_ov_bus_owner = APP_OV_BUS_OWNER_SDIO;
}

static void app_ov_release_bus_to_sdio(void)
{
    __HAL_DCMI_DISABLE_IT(&app_ov_dcmi, DCMI_IT_FRAME | DCMI_IT_ERR | DCMI_IT_OVR);
    HAL_DCMI_Stop(&app_ov_dcmi);
    HAL_DMA_Abort(&app_ov_dma);
    HAL_DMA_DeInit(&app_ov_dma);

    HAL_NVIC_DisableIRQ(DCMI_IRQn);
    HAL_NVIC_ClearPendingIRQ(DCMI_IRQn);
    HAL_NVIC_DisableIRQ(DMA2_Stream1_IRQn);
    HAL_NVIC_ClearPendingIRQ(DMA2_Stream1_IRQn);

    __HAL_RCC_DCMI_FORCE_RESET();
    __HAL_RCC_DCMI_RELEASE_RESET();
    __HAL_RCC_DCMI_CLK_DISABLE();

    app_ov_bus_idle();
    app_ov_sdio_gpio_restore();
    rt_thread_mdelay(APP_OV_SDIO_SETTLE_MS);
}

static void app_ov_dcmi_gpio_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    app_ov_dcmi_gpio_config(GPIOA, GPIO_PIN_4 | GPIO_PIN_6);
    app_ov_dcmi_gpio_config(GPIOB, GPIO_PIN_6 | GPIO_PIN_7);
    app_ov_dcmi_gpio_config(GPIOC, GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_11);
    app_ov_dcmi_gpio_config(GPIOE, GPIO_PIN_5 | GPIO_PIN_6);

    app_ov_bus_owner = APP_OV_BUS_OWNER_CAMERA;
}

static void app_ov_dcmi_msp_init(void)
{
    __HAL_RCC_DCMI_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();

    app_ov_dcmi_gpio_init();

    app_ov_dma.Instance = DMA2_Stream1;
    app_ov_dma.Init.Channel = DMA_CHANNEL_1;
    app_ov_dma.Init.Direction = DMA_PERIPH_TO_MEMORY;
    app_ov_dma.Init.PeriphInc = DMA_PINC_DISABLE;
    app_ov_dma.Init.MemInc = DMA_MINC_ENABLE;
    app_ov_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    app_ov_dma.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
    app_ov_dma.Init.Mode = DMA_CIRCULAR;
    app_ov_dma.Init.Priority = DMA_PRIORITY_HIGH;
    app_ov_dma.Init.FIFOMode = DMA_FIFOMODE_ENABLE;
    app_ov_dma.Init.FIFOThreshold = DMA_FIFO_THRESHOLD_HALFFULL;
    app_ov_dma.Init.MemBurst = DMA_MBURST_SINGLE;
    app_ov_dma.Init.PeriphBurst = DMA_PBURST_SINGLE;
    __HAL_LINKDMA(&app_ov_dcmi, DMA_Handle, app_ov_dma);

    HAL_DMA_DeInit(&app_ov_dma);
    HAL_DMA_Init(&app_ov_dma);

    HAL_NVIC_SetPriority(DCMI_IRQn, 2, 2);
    HAL_NVIC_EnableIRQ(DCMI_IRQn);
    HAL_NVIC_SetPriority(DMA2_Stream1_IRQn, 2, 3);
    HAL_NVIC_EnableIRQ(DMA2_Stream1_IRQn);
}

static rt_err_t app_ov_dcmi_init(void)
{
    HAL_StatusTypeDef hal_ret;

    rt_memset(&app_ov_dcmi, 0, sizeof(app_ov_dcmi));
    rt_memset(&app_ov_dma, 0, sizeof(app_ov_dma));

    app_ov_dcmi.Instance = DCMI;
    app_ov_dcmi.Init.SynchroMode = DCMI_SYNCHRO_HARDWARE;
    app_ov_dcmi.Init.PCKPolarity = DCMI_PCKPOLARITY_RISING;
    app_ov_dcmi.Init.VSPolarity = DCMI_VSPOLARITY_LOW;
    app_ov_dcmi.Init.HSPolarity = DCMI_HSPOLARITY_LOW;
    app_ov_dcmi.Init.CaptureRate = DCMI_CR_ALL_FRAME;
    app_ov_dcmi.Init.ExtendedDataMode = DCMI_EXTEND_DATA_8B;
    app_ov_dcmi.Init.JPEGMode = DCMI_JPEG_DISABLE;

    hal_ret = HAL_DCMI_Init(&app_ov_dcmi);
    if (hal_ret != HAL_OK)
    {
        app_ov_set_result("dcmi init", -RT_ERROR);
        return -RT_ERROR;
    }

    return RT_EOK;
}

static rt_uint32_t app_ov_photo_seq = 0U;

static void app_ov_put_le16(rt_uint8_t *buf, rt_uint16_t value)
{
    buf[0] = (rt_uint8_t)(value & 0xFFU);
    buf[1] = (rt_uint8_t)(value >> 8);
}

static void app_ov_put_le32(rt_uint8_t *buf, rt_uint32_t value)
{
    buf[0] = (rt_uint8_t)(value & 0xFFU);
    buf[1] = (rt_uint8_t)((value >> 8) & 0xFFU);
    buf[2] = (rt_uint8_t)((value >> 16) & 0xFFU);
    buf[3] = (rt_uint8_t)((value >> 24) & 0xFFU);
}

static int app_ov_write_all(int fd, const void *buf, rt_size_t len)
{
    const rt_uint8_t *p = (const rt_uint8_t *)buf;
    rt_size_t done = 0;
    int ret;

    while (done < len)
    {
        rt_set_errno(0);
        ret = write(fd, p + done, len - done);
        if (ret <= 0)
        {
            rt_kprintf("APP ov save: write failed ret=%d done=%u len=%u errno=%d\r\n",
                       ret,
                       (unsigned int)done,
                       (unsigned int)len,
                       rt_get_errno());
            return -RT_ERROR;
        }
        done += (rt_size_t)ret;
    }

    return RT_EOK;
}

static rt_err_t app_ov_flush_chunk(int fd, rt_uint8_t *chunk, rt_size_t *used)
{
    rt_err_t ret;

    if ((chunk == RT_NULL) || (used == RT_NULL))
    {
        return -RT_EINVAL;
    }

    if (*used == 0U)
    {
        return RT_EOK;
    }

    ret = app_ov_write_all(fd, chunk, *used);
    if (ret == RT_EOK)
    {
        *used = 0U;
    }

    return ret;
}

static rt_err_t app_ov_append_chunk(int fd,
                                    rt_uint8_t *chunk,
                                    rt_size_t *used,
                                    const void *buf,
                                    rt_size_t len)
{
    const rt_uint8_t *src = (const rt_uint8_t *)buf;

    if ((chunk == RT_NULL) || (used == RT_NULL) || (buf == RT_NULL))
    {
        return -RT_EINVAL;
    }

    while (len > 0U)
    {
        rt_size_t room = APP_OV_WRITE_CHUNK_SIZE - *used;
        rt_size_t copy = (len < room) ? len : room;

        rt_memcpy(&chunk[*used], src, copy);
        *used += copy;
        src += copy;
        len -= copy;

        if (*used == APP_OV_WRITE_CHUNK_SIZE)
        {
            rt_err_t ret = app_ov_flush_chunk(fd, chunk, used);
            if (ret != RT_EOK)
            {
                return ret;
            }
        }
    }

    return RT_EOK;
}

static rt_err_t app_ov_make_photo_path(char *path, rt_size_t path_len)
{
    if ((path == RT_NULL) || (path_len == 0U))
    {
        return -RT_EINVAL;
    }

    app_ov_photo_seq++;
    rt_snprintf(path, path_len, "%s/DBG%08lx.BMP",
                APP_OV_PHOTO_DIR,
                (unsigned long)((rt_uint32_t)rt_tick_get() + app_ov_photo_seq));
    return RT_EOK;
}

static rt_err_t app_ov_save_bmp565(const char *path,
                                   const rt_uint16_t *pixels,
                                   rt_uint16_t width,
                                   rt_uint16_t height)
{
    rt_uint8_t header[66];
    rt_uint8_t padding[2] = {0U, 0U};
    static rt_uint8_t chunk[APP_OV_WRITE_CHUNK_SIZE];
    rt_uint16_t row[APP_OV_PREVIEW_WIDTH];
    rt_size_t chunk_used = 0U;
    rt_uint32_t raw_row_bytes;
    rt_uint32_t file_row_bytes;
    rt_uint32_t image_bytes;
    int fd;
    int y;

    if ((path == RT_NULL) || (pixels == RT_NULL) ||
        (width == 0U) || (height == 0U) || (width > APP_OV_PREVIEW_WIDTH))
    {
        return -RT_EINVAL;
    }

    raw_row_bytes = (rt_uint32_t)width * 2U;
    file_row_bytes = (raw_row_bytes + 3U) & ~3UL;
    image_bytes = file_row_bytes * height;

    rt_memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    app_ov_put_le32(&header[2], (rt_uint32_t)sizeof(header) + image_bytes);
    app_ov_put_le32(&header[10], sizeof(header));
    app_ov_put_le32(&header[14], 40U);
    app_ov_put_le32(&header[18], width);
    app_ov_put_le32(&header[22], height);
    app_ov_put_le16(&header[26], 1U);
    app_ov_put_le16(&header[28], 16U);
    app_ov_put_le32(&header[30], 3U);
    app_ov_put_le32(&header[34], image_bytes);
    app_ov_put_le32(&header[54], 0x0000F800U);
    app_ov_put_le32(&header[58], 0x000007E0U);
    app_ov_put_le32(&header[62], 0x0000001FU);

    rt_set_errno(0);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0)
    {
        rt_kprintf("APP ov save: image open failed path=%s fd=%d errno=%d\r\n",
                   path,
                   fd,
                   rt_get_errno());
        return -RT_ERROR;
    }

    if (app_ov_append_chunk(fd, chunk, &chunk_used, header, sizeof(header)) != RT_EOK)
    {
        close(fd);
        rt_kprintf("APP ov save: header write failed\r\n");
        return -RT_ERROR;
    }

    for (y = (int)height - 1; y >= 0; y--)
    {
        rt_uint16_t x;
        const rt_uint16_t *src = &pixels[(rt_uint32_t)y * width];

        for (x = 0U; x < width; x++)
        {
            row[x] = src[width - 1U - x];
        }

        if (app_ov_append_chunk(fd, chunk, &chunk_used, row, raw_row_bytes) != RT_EOK)
        {
            close(fd);
            rt_kprintf("APP ov save: pixel row write failed y=%d\r\n", y);
            return -RT_ERROR;
        }

        if (file_row_bytes > raw_row_bytes)
        {
            if (app_ov_append_chunk(fd,
                                    chunk,
                                    &chunk_used,
                                    padding,
                                    file_row_bytes - raw_row_bytes) != RT_EOK)
            {
                close(fd);
                rt_kprintf("APP ov save: padding write failed y=%d\r\n", y);
                return -RT_ERROR;
            }
        }
    }

    if (app_ov_flush_chunk(fd, chunk, &chunk_used) != RT_EOK)
    {
        close(fd);
        rt_kprintf("APP ov save: final flush failed\r\n");
        return -RT_ERROR;
    }

    rt_set_errno(0);
    if (close(fd) < 0)
    {
        rt_kprintf("APP ov save: image close failed errno=%d\r\n", rt_get_errno());
        return -RT_ERROR;
    }

    return RT_EOK;
}

static rt_err_t app_ov_ensure_photo_dir(void)
{
    int mk_ret;

    rt_set_errno(0);
    mk_ret = mkdir(APP_OV_PHOTO_ROOT, 0);
    if ((mk_ret < 0) && (rt_get_errno() != -EEXIST))
    {
        rt_kprintf("APP ov save: mkdir root failed ret=%d errno=%d\r\n", mk_ret, rt_get_errno());
        return -RT_ERROR;
    }

    rt_set_errno(0);
    mk_ret = mkdir(APP_OV_PHOTO_DIR, 0);
    if ((mk_ret < 0) && (rt_get_errno() != -EEXIST))
    {
        rt_kprintf("APP ov save: mkdir photo failed ret=%d errno=%d\r\n", mk_ret, rt_get_errno());
        return -RT_ERROR;
    }

    return RT_EOK;
}

static rt_err_t app_ov_save_current_frame(const char *target_path, char *path, rt_size_t path_len)
{
    rt_err_t ret;

    if (app_ov_capture_buf == RT_NULL)
    {
        rt_kprintf("APP ov save: no frame buffer\r\n");
        return -RT_ERROR;
    }

    if (APP_SdDiag_EnsureMounted() != RT_EOK)
    {
        rt_kprintf("APP ov save: sd mount failed\r\n");
        return -RT_ERROR;
    }

    ret = app_ov_ensure_photo_dir();
    if (ret != RT_EOK)
    {
        return ret;
    }

    if ((target_path != RT_NULL) && (target_path[0] != '\0'))
    {
        if ((path == RT_NULL) || (path_len == 0U) || (rt_strlen(target_path) >= path_len))
        {
            return -RT_EINVAL;
        }
        rt_strncpy(path, target_path, path_len);
        path[path_len - 1U] = '\0';
    }
    else
    {
        ret = app_ov_make_photo_path(path, path_len);
        if (ret != RT_EOK)
        {
            return ret;
        }
    }

    return app_ov_save_bmp565(path,
                              (const rt_uint16_t *)app_ov_capture_buf,
                              APP_OV_PREVIEW_WIDTH,
                              APP_OV_PREVIEW_HEIGHT);
}

static rt_err_t app_ov_capture_once(rt_bool_t dcmi_ready)
{
    rt_tick_t start_tick;
    HAL_StatusTypeDef hal_ret;
    rt_err_t ret = RT_EOK;

    if (app_ov_capture_buf == RT_NULL)
    {
        app_ov_set_result("capture no buffer", -RT_ENOMEM);
        return -RT_ENOMEM;
    }

    rt_memset(app_ov_capture_buf, 0xA5, APP_OV_CAPTURE_BYTES);
    app_ov_frame_done = RT_FALSE;
    app_ov_dma_done = RT_FALSE;
    app_ov_error = RT_FALSE;

    if ((dcmi_ready == RT_FALSE) && (app_ov_dcmi_init() != RT_EOK))
    {
        return -RT_ERROR;
    }

    __HAL_DCMI_ENABLE_IT(&app_ov_dcmi, DCMI_IT_FRAME | DCMI_IT_ERR | DCMI_IT_OVR);
    hal_ret = HAL_DCMI_Start_DMA(&app_ov_dcmi,
                                 DCMI_MODE_SNAPSHOT,
                                 (rt_uint32_t)app_ov_capture_buf,
                                 APP_OV_CAPTURE_WORDS);
    if (hal_ret != HAL_OK)
    {
        app_ov_set_result("dcmi start dma", -RT_ERROR);
        ret = -RT_ERROR;
        goto cleanup;
    }

    start_tick = rt_tick_get();
    while ((app_ov_frame_done == RT_FALSE) && (app_ov_error == RT_FALSE))
    {
        if ((rt_tick_get() - start_tick) > rt_tick_from_millisecond(APP_OV_CAPTURE_TIMEOUT_MS))
        {
            app_ov_set_result("capture timeout", -RT_ETIMEOUT);
            ret = -RT_ETIMEOUT;
            goto cleanup;
        }
        rt_thread_mdelay(1);
    }

    if (app_ov_error != RT_FALSE)
    {
        app_ov_set_result("capture error", -RT_ERROR);
        ret = -RT_ERROR;
        goto cleanup;
    }

    app_ov_set_result("capture ok", RT_EOK);

cleanup:
    HAL_DCMI_Stop(&app_ov_dcmi);
    if (dcmi_ready == RT_FALSE)
    {
        app_ov_release_bus_to_sdio();
    }

    return ret;
}

static void app_ov_gpio_init(void)
{
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    app_ov_gpio_output_pp(APP_OV_SCL_PORT, APP_OV_SCL_PIN);
    app_ov_gpio_output_pp(APP_OV_SDA_PORT, APP_OV_SDA_PIN);
    app_ov_gpio_output_pp(APP_OV_PWDN_PORT, APP_OV_PWDN_PIN);
    app_ov_gpio_output_pp(APP_OV_RESET_PORT, APP_OV_RESET_PIN);

    HAL_GPIO_WritePin(APP_OV_SCL_PORT, APP_OV_SCL_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(APP_OV_SDA_PORT, APP_OV_SDA_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(APP_OV_PWDN_PORT, APP_OV_PWDN_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(APP_OV_RESET_PORT, APP_OV_RESET_PIN, GPIO_PIN_SET);
}

static void app_ov_bus_idle(void)
{
    __HAL_RCC_GPIOG_CLK_ENABLE();

    app_ov_gpio_output_pp(APP_OV_PWDN_PORT, APP_OV_PWDN_PIN);
    app_ov_gpio_output_pp(APP_OV_RESET_PORT, APP_OV_RESET_PIN);

    /* OV2640 不工作时进入掉电/复位态，避免继续驱动与 SDIO 共享的 DCMI 数据线。 */
    HAL_GPIO_WritePin(APP_OV_PWDN_PORT, APP_OV_PWDN_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(APP_OV_RESET_PORT, APP_OV_RESET_PIN, GPIO_PIN_RESET);
}

static int app_ov_board_idle_init(void)
{
    /* 启动诊断打点：INIT_BOARD 是最早的应用层初始化阶段，调度器尚未启动，
       此处点亮 PF9（板载 LED0）用于定位“启动卡死”发生在哪一层。
       判断方法（烧录后只看 PF9 这颗 LED）：
         - PF9 始终不亮：卡死发生在 INIT_BOARD 之前（rt_hw_board_init / 时钟 / 堆栈 / 烧录地址 / 向量表）。
         - PF9 常亮但不闪：卡死发生在 INIT_BOARD 之后、main 之前（INIT_DEVICE / COMPONENT / ENV / APP 某个入口）。
         - PF9 正常 500ms 闪烁：main 已执行，启动并未卡死，现象另找原因。 */
    GPIO_InitTypeDef diag_gpio;

    app_ov_bus_idle();

    __HAL_RCC_GPIOF_CLK_ENABLE();
    diag_gpio.Pin = GPIO_PIN_9;
    diag_gpio.Mode = GPIO_MODE_OUTPUT_PP;
    diag_gpio.Pull = GPIO_NOPULL;
    diag_gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOF, &diag_gpio);
    HAL_GPIO_WritePin(GPIOF, GPIO_PIN_9, GPIO_PIN_SET);

    return RT_EOK;
}
INIT_BOARD_EXPORT(app_ov_board_idle_init);

static void app_ov_power_up(void)
{
    HAL_GPIO_WritePin(APP_OV_PWDN_PORT, APP_OV_PWDN_PIN, GPIO_PIN_RESET);
    rt_thread_mdelay(APP_OV_BOOT_DELAY_MS);

    HAL_GPIO_WritePin(APP_OV_RESET_PORT, APP_OV_RESET_PIN, GPIO_PIN_RESET);
    rt_thread_mdelay(APP_OV_RESET_LOW_MS);
    HAL_GPIO_WritePin(APP_OV_RESET_PORT, APP_OV_RESET_PIN, GPIO_PIN_SET);
    rt_thread_mdelay(APP_OV_RESET_HIGH_MS);
}

static rt_err_t app_ov_probe_once(rt_uint16_t *mid, rt_uint16_t *pid)
{
    rt_uint8_t h = 0;
    rt_uint8_t l = 0;
    rt_err_t ret;

    if ((mid == RT_NULL) || (pid == RT_NULL))
    {
        return -RT_EINVAL;
    }

    ret = app_ov_write_reg(APP_OV_SENSOR_BANK_SEL, 0x01U);
    if (ret != RT_EOK)
    {
        app_ov_set_result("select sensor bank", ret);
        return ret;
    }

    ret = app_ov_read_reg(APP_OV_SENSOR_MIDH, &h);
    if (ret != RT_EOK)
    {
        app_ov_set_result("read midh", ret);
        return ret;
    }
    ret = app_ov_read_reg(APP_OV_SENSOR_MIDL, &l);
    if (ret != RT_EOK)
    {
        app_ov_set_result("read midl", ret);
        return ret;
    }
    *mid = ((rt_uint16_t)h << 8) | l;

    ret = app_ov_read_reg(APP_OV_SENSOR_PIDH, &h);
    if (ret != RT_EOK)
    {
        app_ov_set_result("read pidh", ret);
        return ret;
    }
    ret = app_ov_read_reg(APP_OV_SENSOR_PIDL, &l);
    if (ret != RT_EOK)
    {
        app_ov_set_result("read pidl", ret);
        return ret;
    }
    *pid = ((rt_uint16_t)h << 8) | l;

    app_ov_set_result("probe ok", RT_EOK);
    return RT_EOK;
}

static rt_err_t app_ov_set_output_speed(rt_uint8_t clk_div, rt_uint8_t pclk_div)
{
    rt_err_t ret;

    ret = app_ov_write_reg(APP_OV_SENSOR_BANK_SEL, 0x00U);
    if (ret != RT_EOK)
    {
        app_ov_set_result("set pclk bank", ret);
        return ret;
    }

    ret = app_ov_write_reg(0xD3U, pclk_div);
    if (ret != RT_EOK)
    {
        app_ov_set_result("set pclk div", ret);
        return ret;
    }

    ret = app_ov_write_reg(APP_OV_SENSOR_BANK_SEL, 0x01U);
    if (ret != RT_EOK)
    {
        app_ov_set_result("set clk bank", ret);
        return ret;
    }

    ret = app_ov_write_reg(APP_OV_SENSOR_CLKRC, clk_div);
    if (ret != RT_EOK)
    {
        app_ov_set_result("set clk div", ret);
        return ret;
    }

    return RT_EOK;
}

static rt_err_t app_ov_set_output_size(rt_uint16_t width, rt_uint16_t height)
{
    rt_uint16_t output_width;
    rt_uint16_t output_height;
    rt_uint8_t zmhh;
    rt_err_t ret;

    if (((width & 0x03U) != 0U) || ((height & 0x03U) != 0U))
    {
        return -RT_EINVAL;
    }

    output_width = width >> 2;
    output_height = height >> 2;
    zmhh = ((rt_uint8_t)(output_width >> 8) & 0x03U) | ((rt_uint8_t)(output_height >> 6) & 0x04U);

    ret = app_ov_write_reg(APP_OV_SENSOR_BANK_SEL, 0x00U);
    if (ret != RT_EOK)
    {
        app_ov_set_result("size bank", ret);
        return ret;
    }

    ret = app_ov_write_reg(0xE0U, 0x04U);
    if (ret == RT_EOK) ret = app_ov_write_reg(0x5AU, (rt_uint8_t)(output_width & 0x00FFU));
    if (ret == RT_EOK) ret = app_ov_write_reg(0x5BU, (rt_uint8_t)(output_height & 0x00FFU));
    if (ret == RT_EOK) ret = app_ov_write_reg(0x5CU, zmhh);
    if (ret == RT_EOK) ret = app_ov_write_reg(0xE0U, 0x00U);

    if (ret != RT_EOK)
    {
        app_ov_set_result("set output size", ret);
        return ret;
    }

    return RT_EOK;
}

static rt_err_t app_ov_capture_sensor_init(rt_uint16_t width, rt_uint16_t height)
{
    rt_err_t ret;

    ret = app_ov_write_reg(APP_OV_SENSOR_BANK_SEL, 0x01U);
    if (ret == RT_EOK)
    {
        ret = app_ov_write_reg(APP_OV_SENSOR_COM7, 0x80U);
    }
    if (ret != RT_EOK)
    {
        app_ov_set_result("capture sensor reset", ret);
        return ret;
    }
    rt_thread_mdelay(50);

    ret = app_ov_write_table(app_ov_init_svga_cfg,
                             sizeof(app_ov_init_svga_cfg) / sizeof(app_ov_init_svga_cfg[0]),
                             "capture svga cfg");
    if (ret != RT_EOK)
    {
        return ret;
    }

    ret = app_ov_write_table(app_ov_rgb565_cfg,
                             sizeof(app_ov_rgb565_cfg) / sizeof(app_ov_rgb565_cfg[0]),
                             "capture rgb565 cfg");
    if (ret != RT_EOK)
    {
        return ret;
    }

    ret = app_ov_set_output_size(width, height);
    if (ret != RT_EOK)
    {
        return ret;
    }

    ret = app_ov_set_output_speed(0x00U, 0x04U);
    if (ret != RT_EOK)
    {
        return ret;
    }

    app_ov_set_result("capture sensor init ok", RT_EOK);
    return RT_EOK;
}

static rt_err_t app_ov_prepare_capture(rt_uint16_t width,
                                       rt_uint16_t height,
                                       rt_uint16_t *mid,
                                       rt_uint16_t *pid)
{
    rt_err_t ret;

    ret = app_ov_probe_once(mid, pid);
    if (ret == RT_EOK)
    {
        ret = app_ov_capture_sensor_init(width, height);
    }

    return ret;
}

static void app_ov_dump_pin_plan(void)
{
    rt_kprintf("APP ov: pins XCLK=PA8 SCL=PD6 SDA=PD7 PWDN=PG9 RESET=PG15\r\n");
    rt_kprintf("APP ov: DCMI PA4 PA6 PB6 PB7 PC6 PC7 PC8 PC9 PC11 PE5 PE6\r\n");
    rt_kprintf("APP ov: shared PC8 PC9 PC11 owner=%s\r\n",
               (app_ov_bus_owner == APP_OV_BUS_OWNER_CAMERA) ? "OV" : "SDIO");
}

static void app_ov_prepare(void)
{
    app_ov_gpio_init();
    app_ov_xclk_init();
    app_ov_sccb_idle();
    app_ov_power_up();
}

static app_ov_touch_action_t app_ov_live_get_touch_action(const APP_TouchState *state)
{
    if ((state == RT_NULL) || (state->count == 0U))
    {
        return APP_OV_TOUCH_NONE;
    }

    if ((state->x[0] >= APP_OV_LIVE_BUTTON_X) &&
        (state->x[0] < (APP_OV_LIVE_BUTTON_X + APP_OV_LIVE_BUTTON_W)) &&
        (state->y[0] >= APP_OV_LIVE_BUTTON_Y) &&
        (state->y[0] < (APP_OV_LIVE_BUTTON_Y + APP_OV_LIVE_BUTTON_H)))
    {
        return APP_OV_TOUCH_SNAPSHOT;
    }

    if ((state->x[0] >= APP_OV_EXIT_BUTTON_X) &&
        (state->x[0] < (APP_OV_EXIT_BUTTON_X + APP_OV_EXIT_BUTTON_W)) &&
        (state->y[0] >= APP_OV_EXIT_BUTTON_Y) &&
        (state->y[0] < (APP_OV_EXIT_BUTTON_Y + APP_OV_EXIT_BUTTON_H)))
    {
        return APP_OV_TOUCH_EXIT;
    }

    return APP_OV_TOUCH_NONE;
}

static void app_ov_live_hold(rt_uint32_t hold_ms)
{
    rt_uint32_t elapsed = 0;

    while (elapsed < hold_ms)
    {
        rt_thread_mdelay(50);
        elapsed += 50U;
    }
}

static rt_err_t app_ov_live_preview_loop(rt_uint16_t mid,
                                         rt_uint16_t pid,
                                         const char *target_path,
                                         char *saved_path,
                                         rt_size_t saved_path_size,
                                         rt_bool_t exit_after_snapshot)
{
    APP_TouchState touch;
    rt_err_t ret;
    int lcd_ret;
    rt_uint32_t frame_count = 0;
    rt_bool_t dcmi_active = RT_FALSE;
    rt_bool_t snapshot_armed = RT_FALSE;

    if ((saved_path != RT_NULL) && (saved_path_size > 0U))
    {
        saved_path[0] = '\0';
    }

    ret = APP_Display_EnterCameraPage();
    if (ret != RT_EOK)
    {
        return ret;
    }

    if (APP_Touch_Init() != RT_EOK)
    {
        APP_Display_CameraStatus("TOUCH INIT FAILED");
        rt_thread_mdelay(500);
        ret = -RT_ERROR;
        goto exit_page;
    }
    APP_Touch_ResetState();

    APP_Display_CameraStatus("LIVE");
    rt_kprintf("APP ov live: mid=0x%04x pid=0x%04x snapshot x=%u y=%u w=%u h=%u exit x=%u y=%u w=%u h=%u\r\n",
               mid,
               pid,
               APP_OV_LIVE_BUTTON_X,
               APP_OV_LIVE_BUTTON_Y,
               APP_OV_LIVE_BUTTON_W,
               APP_OV_LIVE_BUTTON_H,
               APP_OV_EXIT_BUTTON_X,
               APP_OV_EXIT_BUTTON_Y,
               APP_OV_EXIT_BUTTON_W,
               APP_OV_EXIT_BUTTON_H);

    ret = app_ov_dcmi_init();
    if (ret != RT_EOK)
    {
        APP_Display_CameraStatus("DCMI INIT FAILED");
        rt_thread_mdelay(500);
        goto exit_page;
    }
    dcmi_active = RT_TRUE;

    while (1)
    {
        app_ov_touch_action_t action = APP_OV_TOUCH_NONE;

        ret = app_ov_capture_once(RT_TRUE);
        if (ret != RT_EOK)
        {
            APP_Display_CameraStatus("CAPTURE FAILED");
            rt_kprintf("APP ov live: capture failed step=%s result=%d\r\n",
                       app_ov_last_step,
                       app_ov_last_result);
            rt_thread_mdelay(500);
            goto exit_page;
        }

        lcd_ret = APP_Display_ShowRgb565ImageRotate180(APP_OV_LIVE_PREVIEW_X,
                                                       APP_OV_LIVE_PREVIEW_Y,
                                                       APP_OV_PREVIEW_WIDTH,
                                                       APP_OV_PREVIEW_HEIGHT,
                                                       APP_OV_PREVIEW_SCALE,
                                                       (const unsigned short *)app_ov_capture_buf,
                                                       0U,
                                                       0U,
                                                       1U);
        if (lcd_ret != RT_EOK)
        {
            APP_Display_CameraStatus("LCD FAILED");
            rt_kprintf("APP ov live: lcd failed ret=%d\r\n", lcd_ret);
            rt_thread_mdelay(500);
            ret = lcd_ret;
            goto exit_page;
        }

        frame_count++;

        if (APP_Touch_Read(&touch) == RT_EOK)
        {
            if ((touch.count == 0U) || (touch.released != 0U))
            {
                snapshot_armed = RT_TRUE;
                action = APP_OV_TOUCH_NONE;
            }
            else if (snapshot_armed != RT_FALSE)
            {
                action = app_ov_live_get_touch_action(&touch);
            }
        }

        if (action == APP_OV_TOUCH_SNAPSHOT)
        {
            rt_err_t save_ret;
            char photo_path[APP_OV_PHOTO_PATH_LEN];

            snapshot_armed = RT_FALSE;
            APP_Display_CameraStatus("SNAPSHOT");

            app_ov_release_bus_to_sdio();
            dcmi_active = RT_FALSE;

            APP_Display_CameraStatus("SAVING");
            save_ret = app_ov_save_current_frame(target_path, photo_path, sizeof(photo_path));
            if (save_ret == RT_EOK)
            {
                APP_Display_CameraStatus("SAVED");
                if ((saved_path != RT_NULL) && (saved_path_size > 0U))
                {
                    rt_strncpy(saved_path, photo_path, saved_path_size);
                    saved_path[saved_path_size - 1U] = '\0';
                }
                rt_kprintf("APP ov live: snapshot saved path=%s frame=%u\r\n",
                           photo_path,
                           frame_count);
            }
            else
            {
                APP_Display_CameraStatus("SAVE FAILED");
                rt_kprintf("APP ov live: snapshot save failed ret=%d frame=%u\r\n",
                           save_ret,
                           frame_count);
            }

            app_ov_live_hold(700U);
            if (exit_after_snapshot != RT_FALSE)
            {
                ret = save_ret;
                break;
            }

            APP_Display_CameraStatus("LIVE");
            ret = app_ov_prepare_capture(APP_OV_PREVIEW_WIDTH,
                                         APP_OV_PREVIEW_HEIGHT,
                                         &mid,
                                         &pid);
            if (ret != RT_EOK)
            {
                APP_Display_CameraStatus("OV REINIT FAILED");
                rt_thread_mdelay(500);
                goto exit_page;
            }

            ret = app_ov_dcmi_init();
            if (ret != RT_EOK)
            {
                APP_Display_CameraStatus("DCMI REINIT FAILED");
                rt_thread_mdelay(500);
                goto exit_page;
            }
            dcmi_active = RT_TRUE;
        }
        else if (action == APP_OV_TOUCH_EXIT)
        {
            APP_Display_CameraStatus("EXIT");
            rt_thread_mdelay(200);
            break;
        }
    }

    APP_Display_CameraStatus("STOPPED");
    rt_thread_mdelay(200);
    ret = RT_EOK;

exit_page:
    if (dcmi_active != RT_FALSE)
    {
        app_ov_release_bus_to_sdio();
        dcmi_active = RT_FALSE;
    }

    APP_Display_LeaveCameraPage();
    rt_kprintf("APP ov live: stopped frame=%u\r\n", frame_count);
    return ret;
}

static void app_ov_live_thread_entry(void *parameter)
{
    rt_uint16_t mid = 0;
    rt_uint16_t pid = 0;
    rt_err_t ret;

    (void)parameter;

    ret = app_ov_capture_buffer_acquire();
    if (ret != RT_EOK)
    {
        app_ov_live_running = RT_FALSE;
        app_ov_live_thread = RT_NULL;
        return;
    }

    app_ov_prepare();

    ret = app_ov_prepare_capture(APP_OV_PREVIEW_WIDTH,
                                 APP_OV_PREVIEW_HEIGHT,
                                 &mid,
                                 &pid);

    app_ov_dump_pin_plan();
    if (ret == RT_EOK)
    {
        ret = app_ov_live_preview_loop(mid, pid, RT_NULL, RT_NULL, 0U, RT_FALSE);
    }
    else
    {
        rt_kprintf("APP ov live: init failed step=%s result=%d mid=0x%04x pid=0x%04x\r\n",
                   app_ov_last_step,
                   app_ov_last_result,
                   mid,
                   pid);
    }

    app_ov_capture_buffer_release();
    app_ov_live_running = RT_FALSE;
    app_ov_live_thread = RT_NULL;
}

static rt_err_t app_ov_live_start(void)
{
    rt_err_t ret;

    if (app_ov_live_running != RT_FALSE)
    {
        rt_kprintf("APP ov live: already running\r\n");
        return RT_EOK;
    }

    app_ov_live_thread = rt_thread_create("ov_live",
                                          app_ov_live_thread_entry,
                                          RT_NULL,
                                          APP_OV_LIVE_THREAD_STACK,
                                          APP_OV_LIVE_THREAD_PRIORITY,
                                          APP_OV_LIVE_THREAD_TICK);
    if (app_ov_live_thread == RT_NULL)
    {
        rt_kprintf("APP ov live: thread create failed\r\n");
        return -RT_ENOMEM;
    }

    app_ov_live_running = RT_TRUE;
    ret = rt_thread_startup(app_ov_live_thread);
    if (ret != RT_EOK)
    {
        rt_thread_delete(app_ov_live_thread);
        app_ov_live_thread = RT_NULL;
        app_ov_live_running = RT_FALSE;
        rt_kprintf("APP ov live: thread startup failed ret=%d\r\n", ret);
        return ret;
    }

    rt_kprintf("APP ov live: started\r\n");
    return RT_EOK;
}

rt_err_t APP_Ov_PreviewAndSave(char *path, rt_size_t path_size)
{
    return APP_Ov_PreviewAndSaveAs(RT_NULL, path, path_size);
}

rt_err_t APP_Ov_PreviewAndSaveAs(const char *target_path, char *saved_path, rt_size_t saved_path_size)
{
    rt_uint16_t mid = 0;
    rt_uint16_t pid = 0;
    rt_err_t ret;

    if ((saved_path == RT_NULL) || (saved_path_size == 0U))
    {
        return -RT_EINVAL;
    }
    saved_path[0] = '\0';

    if (app_ov_live_running != RT_FALSE)
    {
        rt_kprintf("APP ov preview: live already running\r\n");
        return -RT_EBUSY;
    }

    app_ov_live_running = RT_TRUE;
    ret = app_ov_capture_buffer_acquire();
    if (ret == RT_EOK)
    {
        app_ov_prepare();
        ret = app_ov_prepare_capture(APP_OV_PREVIEW_WIDTH,
                                     APP_OV_PREVIEW_HEIGHT,
                                     &mid,
                                     &pid);
    }

    app_ov_dump_pin_plan();
    if (ret == RT_EOK)
    {
        ret = app_ov_live_preview_loop(mid, pid, target_path, saved_path, saved_path_size, RT_TRUE);
    }
    else
    {
        rt_kprintf("APP ov preview: init failed step=%s result=%d mid=0x%04x pid=0x%04x\r\n",
                   app_ov_last_step,
                   app_ov_last_result,
                   mid,
                   pid);
    }

    app_ov_capture_buffer_release();
    app_ov_live_running = RT_FALSE;
    return ret;
}

int APP_Ov_Test(int argc, char **argv)
{
    if ((argc >= 2) && (rt_strcmp(argv[1], "live") == 0))
    {
        return app_ov_live_start();
    }

    rt_kprintf("Usage: APP ov live\r\n");
    return -RT_EINVAL;
}

void HAL_DCMI_MspInit(DCMI_HandleTypeDef *hdcmi)
{
    if (hdcmi == &app_ov_dcmi)
    {
        app_ov_dcmi_msp_init();
    }
}

void HAL_DCMI_FrameEventCallback(DCMI_HandleTypeDef *hdcmi)
{
    if (hdcmi == &app_ov_dcmi)
    {
        app_ov_frame_done = RT_TRUE;
    }
}

void HAL_DCMI_ErrorCallback(DCMI_HandleTypeDef *hdcmi)
{
    if (hdcmi == &app_ov_dcmi)
    {
        app_ov_error = RT_TRUE;
    }
}

void DCMI_IRQHandler(void)
{
    rt_interrupt_enter();
    HAL_DCMI_IRQHandler(&app_ov_dcmi);
    rt_interrupt_leave();
}

void DMA2_Stream1_IRQHandler(void)
{
    rt_interrupt_enter();
    HAL_DMA_IRQHandler(&app_ov_dma);
    app_ov_dma_done = RT_TRUE;
    rt_interrupt_leave();
}

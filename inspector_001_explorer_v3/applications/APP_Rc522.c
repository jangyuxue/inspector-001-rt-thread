#include "APP_Rc522.h"

#include <rtdevice.h>
#include <board.h>
#include <string.h>

/* RC522 当前使用软件 SPI，保持已经验证通过的接线不变。 */
#define APP_RC522_SCK_PORT       GPIOA          /**< RC522 SCK 所在 GPIO 端口。 */
#define APP_RC522_SCK_PIN        GPIO_PIN_5     /**< RC522 SCK 引脚。 */
#define APP_RC522_MISO_PORT      GPIOA          /**< RC522 MISO 所在 GPIO 端口。 */
#define APP_RC522_MISO_PIN       GPIO_PIN_6     /**< RC522 MISO 引脚。 */
#define APP_RC522_MOSI_PORT      GPIOA          /**< RC522 MOSI 所在 GPIO 端口。 */
#define APP_RC522_MOSI_PIN       GPIO_PIN_7     /**< RC522 MOSI 引脚。 */
#define APP_RC522_CS_PORT        GPIOA          /**< RC522 SDA/CS 所在 GPIO 端口。 */
#define APP_RC522_CS_PIN         GPIO_PIN_4     /**< RC522 SDA/CS 引脚，低电平选中。 */
#define APP_RC522_RST_PORT       GPIOC          /**< RC522 RST 所在 GPIO 端口。 */
#define APP_RC522_RST_PIN        GPIO_PIN_4     /**< RC522 RST 引脚。 */
#define APP_RC522_SPI_DELAY_US   2U             /**< 软件 SPI 半周期延时，单位 us。 */

/* RC522 常用寄存器地址，只保留当前寻卡/防冲突流程需要的寄存器。 */
#define RC522_REG_COMMAND        0x01
#define RC522_REG_COMM_IE_N      0x02
#define RC522_REG_COMM_IRQ       0x04
#define RC522_REG_DIV_IRQ        0x05
#define RC522_REG_ERROR          0x06
#define RC522_REG_FIFO_DATA      0x09
#define RC522_REG_FIFO_LEVEL     0x0A
#define RC522_REG_CONTROL        0x0C
#define RC522_REG_BIT_FRAMING    0x0D
#define RC522_REG_MODE           0x11
#define RC522_REG_TX_CONTROL     0x14
#define RC522_REG_TX_ASK         0x15
#define RC522_REG_T_MODE         0x2A
#define RC522_REG_T_PRESCALER    0x2B
#define RC522_REG_T_RELOAD_H     0x2C
#define RC522_REG_T_RELOAD_L     0x2D
#define RC522_REG_CRC_RESULT_H   0x21
#define RC522_REG_CRC_RESULT_L   0x22
#define RC522_REG_VERSION        0x37

/* RC522 内部命令字。 */
#define RC522_CMD_IDLE           0x00
#define RC522_CMD_CALC_CRC       0x03
#define RC522_CMD_TRANSCEIVE     0x0C
#define RC522_CMD_SOFT_RESET     0x0F

/* ISO14443A 卡片侧命令和级联标志。 */
#define PICC_CMD_REQA            0x26
#define PICC_CMD_SEL_CL1         0x93
#define PICC_CMD_SEL_CL2         0x95
#define PICC_CASCADE_TAG         0x88
#define PICC_SAK_CASCADE         0x04

static rt_bool_t app_rc522_bus_ready = RT_FALSE;

static void app_rc522_set_diag(APP_Rc522Diag *diag,
                               const char *step,
                               rt_err_t result,
                               rt_uint8_t version,
                               const rt_uint8_t atqa[2],
                               rt_uint8_t uid_len)
{
    if (diag == RT_NULL)
    {
        return;
    }

    diag->step = step;
    diag->result = result;
    diag->version = version;
    diag->uid_len = uid_len;
    if (atqa != RT_NULL)
    {
        diag->atqa[0] = atqa[0];
        diag->atqa[1] = atqa[1];
    }
}

static void app_rc522_uid_to_hex(const rt_uint8_t *uid,
                                 rt_uint8_t uid_len,
                                 char *out,
                                 rt_size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";
    rt_size_t i;
    rt_size_t pos = 0;

    if ((uid == RT_NULL) || (out == RT_NULL) || (out_size == 0))
    {
        return;
    }

    for (i = 0; (i < uid_len) && ((pos + 2) < out_size); i++)
    {
        out[pos++] = hex[(uid[i] >> 4) & 0x0F];
        out[pos++] = hex[uid[i] & 0x0F];
    }
    out[pos] = '\0';
}

static void app_rc522_board_gpio_init(void)
{
    GPIO_InitTypeDef gpio;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;

    gpio.Pin = APP_RC522_SCK_PIN | APP_RC522_MOSI_PIN | APP_RC522_CS_PIN;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = APP_RC522_RST_PIN;
    HAL_GPIO_Init(GPIOC, &gpio);

    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    gpio.Pin = APP_RC522_MISO_PIN;
    HAL_GPIO_Init(GPIOA, &gpio);

    HAL_GPIO_WritePin(APP_RC522_SCK_PORT, APP_RC522_SCK_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(APP_RC522_MOSI_PORT, APP_RC522_MOSI_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(APP_RC522_CS_PORT, APP_RC522_CS_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(APP_RC522_RST_PORT, APP_RC522_RST_PIN, GPIO_PIN_SET);
}

static void app_rc522_spi_delay(void)
{
    rt_hw_us_delay(APP_RC522_SPI_DELAY_US);
}

static rt_uint8_t app_rc522_soft_spi_xfer(rt_uint8_t data)
{
    rt_uint8_t i;
    rt_uint8_t rx = 0;

    for (i = 0; i < 8; i++)
    {
        HAL_GPIO_WritePin(APP_RC522_SCK_PORT, APP_RC522_SCK_PIN, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(APP_RC522_MOSI_PORT,
                          APP_RC522_MOSI_PIN,
                          (data & 0x80) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        app_rc522_spi_delay();

        HAL_GPIO_WritePin(APP_RC522_SCK_PORT, APP_RC522_SCK_PIN, GPIO_PIN_SET);
        app_rc522_spi_delay();
        rx <<= 1;
        if (HAL_GPIO_ReadPin(APP_RC522_MISO_PORT, APP_RC522_MISO_PIN) == GPIO_PIN_SET)
        {
            rx |= 0x01;
        }
        data <<= 1;
    }

    HAL_GPIO_WritePin(APP_RC522_SCK_PORT, APP_RC522_SCK_PIN, GPIO_PIN_RESET);
    app_rc522_spi_delay();
    return rx;
}

static rt_uint8_t app_rc522_read_reg(rt_uint8_t reg)
{
    rt_uint8_t value;

    if (!app_rc522_bus_ready)
    {
        return 0;
    }

    HAL_GPIO_WritePin(APP_RC522_CS_PORT, APP_RC522_CS_PIN, GPIO_PIN_RESET);
    app_rc522_soft_spi_xfer(((reg << 1) & 0x7E) | 0x80);
    value = app_rc522_soft_spi_xfer(0x00);
    HAL_GPIO_WritePin(APP_RC522_CS_PORT, APP_RC522_CS_PIN, GPIO_PIN_SET);
    return value;
}

static void app_rc522_write_reg(rt_uint8_t reg, rt_uint8_t value)
{
    if (!app_rc522_bus_ready)
    {
        return;
    }

    HAL_GPIO_WritePin(APP_RC522_CS_PORT, APP_RC522_CS_PIN, GPIO_PIN_RESET);
    app_rc522_soft_spi_xfer((reg << 1) & 0x7E);
    app_rc522_soft_spi_xfer(value);
    HAL_GPIO_WritePin(APP_RC522_CS_PORT, APP_RC522_CS_PIN, GPIO_PIN_SET);
}

static void app_rc522_set_bit(rt_uint8_t reg, rt_uint8_t mask)
{
    app_rc522_write_reg(reg, app_rc522_read_reg(reg) | mask);
}

static void app_rc522_clear_bit(rt_uint8_t reg, rt_uint8_t mask)
{
    app_rc522_write_reg(reg, app_rc522_read_reg(reg) & (rt_uint8_t)(~mask));
}

static void app_rc522_reset(void)
{
    HAL_GPIO_WritePin(APP_RC522_RST_PORT, APP_RC522_RST_PIN, GPIO_PIN_RESET);
    rt_thread_mdelay(20);
    HAL_GPIO_WritePin(APP_RC522_RST_PORT, APP_RC522_RST_PIN, GPIO_PIN_SET);
    rt_thread_mdelay(50);

    app_rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_SOFT_RESET);
    rt_thread_mdelay(50);
}

static void app_rc522_antenna_on(void)
{
    if ((app_rc522_read_reg(RC522_REG_TX_CONTROL) & 0x03) != 0x03)
    {
        app_rc522_set_bit(RC522_REG_TX_CONTROL, 0x03);
    }
}

void APP_Rc522_InitChip(void)
{
    app_rc522_reset();
    app_rc522_write_reg(RC522_REG_T_MODE, 0x8D);
    app_rc522_write_reg(RC522_REG_T_PRESCALER, 0x3E);
    app_rc522_write_reg(RC522_REG_T_RELOAD_L, 30);
    app_rc522_write_reg(RC522_REG_T_RELOAD_H, 0);
    app_rc522_write_reg(RC522_REG_TX_ASK, 0x40);
    app_rc522_write_reg(RC522_REG_MODE, 0x3D);
    app_rc522_antenna_on();
}

rt_uint8_t APP_Rc522_ReadVersion(void)
{
    return app_rc522_read_reg(RC522_REG_VERSION);
}

static rt_err_t app_rc522_transceive(const rt_uint8_t *send_data,
                                     rt_uint8_t send_len,
                                     rt_uint8_t *recv_data,
                                     rt_uint8_t *recv_len,
                                     rt_uint16_t *recv_bits)
{
    rt_uint8_t irq;
    rt_uint8_t fifo_len;
    rt_uint8_t last_bits;
    rt_uint16_t retry;
    rt_uint8_t i;

    if ((send_data == RT_NULL) || (send_len == 0) ||
        (recv_data == RT_NULL) || (recv_len == RT_NULL) || (recv_bits == RT_NULL))
    {
        return -RT_EINVAL;
    }

    app_rc522_write_reg(RC522_REG_COMM_IE_N, 0x77 | 0x80);
    app_rc522_clear_bit(RC522_REG_COMM_IRQ, 0x80);
    app_rc522_set_bit(RC522_REG_FIFO_LEVEL, 0x80);
    app_rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_IDLE);

    for (i = 0; i < send_len; i++)
    {
        app_rc522_write_reg(RC522_REG_FIFO_DATA, send_data[i]);
    }

    app_rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_TRANSCEIVE);
    app_rc522_set_bit(RC522_REG_BIT_FRAMING, 0x80);

    retry = 2000;
    do
    {
        irq = app_rc522_read_reg(RC522_REG_COMM_IRQ);
        retry--;
    }
    while ((retry != 0) && ((irq & 0x30) == 0) && ((irq & 0x01) == 0));

    app_rc522_clear_bit(RC522_REG_BIT_FRAMING, 0x80);

    if ((retry == 0) || (irq & 0x01))
    {
        return -RT_ETIMEOUT;
    }
    if (app_rc522_read_reg(RC522_REG_ERROR) & 0x1B)
    {
        return -RT_ERROR;
    }

    fifo_len = app_rc522_read_reg(RC522_REG_FIFO_LEVEL);
    last_bits = app_rc522_read_reg(RC522_REG_CONTROL) & 0x07;
    *recv_bits = (last_bits != 0) ? (rt_uint16_t)((fifo_len - 1) * 8 + last_bits) :
                                    (rt_uint16_t)(fifo_len * 8);
    if (fifo_len > *recv_len)
    {
        fifo_len = *recv_len;
    }
    for (i = 0; i < fifo_len; i++)
    {
        recv_data[i] = app_rc522_read_reg(RC522_REG_FIFO_DATA);
    }
    *recv_len = fifo_len;
    return RT_EOK;
}

rt_err_t APP_Rc522_Request(rt_uint8_t atqa[2])
{
    rt_uint8_t cmd = PICC_CMD_REQA;
    rt_uint8_t recv_len = 2;
    rt_uint16_t recv_bits = 0;
    rt_err_t result;

    if (atqa == RT_NULL)
    {
        return -RT_EINVAL;
    }

    app_rc522_write_reg(RC522_REG_BIT_FRAMING, 0x07);
    result = app_rc522_transceive(&cmd, 1, atqa, &recv_len, &recv_bits);
    app_rc522_write_reg(RC522_REG_BIT_FRAMING, 0x00);
    if (result != RT_EOK)
    {
        return result;
    }
    return (recv_bits == 0x10) ? RT_EOK : -RT_ERROR;
}

static rt_err_t app_rc522_calc_crc(const rt_uint8_t *data, rt_uint8_t len, rt_uint8_t crc[2])
{
    rt_uint16_t retry;
    rt_uint8_t irq;
    rt_uint8_t i;

    if ((data == RT_NULL) || (len == 0) || (crc == RT_NULL))
    {
        return -RT_EINVAL;
    }

    app_rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_IDLE);
    app_rc522_write_reg(RC522_REG_DIV_IRQ, 0x04);
    app_rc522_set_bit(RC522_REG_FIFO_LEVEL, 0x80);
    for (i = 0; i < len; i++)
    {
        app_rc522_write_reg(RC522_REG_FIFO_DATA, data[i]);
    }
    app_rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_CALC_CRC);

    retry = 2000;
    do
    {
        irq = app_rc522_read_reg(RC522_REG_DIV_IRQ);
        retry--;
    }
    while ((retry != 0) && ((irq & 0x04) == 0));

    app_rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_IDLE);
    if (retry == 0)
    {
        return -RT_ETIMEOUT;
    }
    crc[0] = app_rc522_read_reg(RC522_REG_CRC_RESULT_L);
    crc[1] = app_rc522_read_reg(RC522_REG_CRC_RESULT_H);
    return RT_EOK;
}

static rt_err_t app_rc522_select_level(rt_uint8_t sel_cmd, rt_uint8_t *uid_part, rt_bool_t *cascade)
{
    rt_uint8_t cmd[9];
    rt_uint8_t recv[3] = {0};
    rt_uint8_t recv_len = sizeof(recv);
    rt_uint16_t recv_bits = 0;
    rt_err_t result;

    if ((uid_part == RT_NULL) || (cascade == RT_NULL))
    {
        return -RT_EINVAL;
    }

    cmd[0] = sel_cmd;
    cmd[1] = 0x70;
    cmd[2] = uid_part[0];
    cmd[3] = uid_part[1];
    cmd[4] = uid_part[2];
    cmd[5] = uid_part[3];
    cmd[6] = uid_part[0] ^ uid_part[1] ^ uid_part[2] ^ uid_part[3];
    result = app_rc522_calc_crc(cmd, 7, &cmd[7]);
    if (result != RT_EOK)
    {
        return result;
    }

    app_rc522_write_reg(RC522_REG_BIT_FRAMING, 0x00);
    result = app_rc522_transceive(cmd, sizeof(cmd), recv, &recv_len, &recv_bits);
    if ((result != RT_EOK) || (recv_len < 1) || (recv_bits < 8))
    {
        return -RT_ERROR;
    }
    *cascade = (recv[0] & PICC_SAK_CASCADE) ? RT_TRUE : RT_FALSE;
    return RT_EOK;
}

static rt_err_t app_rc522_anticoll_level(rt_uint8_t sel_cmd, rt_uint8_t uid_part[4])
{
    rt_uint8_t cmd[2] = {sel_cmd, 0x20};
    rt_uint8_t recv[5] = {0};
    rt_uint8_t recv_len = sizeof(recv);
    rt_uint16_t recv_bits = 0;
    rt_uint8_t bcc;
    rt_err_t result;

    if (uid_part == RT_NULL)
    {
        return -RT_EINVAL;
    }

    app_rc522_write_reg(RC522_REG_BIT_FRAMING, 0x00);
    result = app_rc522_transceive(cmd, 2, recv, &recv_len, &recv_bits);
    if ((result != RT_EOK) || (recv_len < 5) || (recv_bits != 40))
    {
        return -RT_ERROR;
    }

    bcc = recv[0] ^ recv[1] ^ recv[2] ^ recv[3];
    if (bcc != recv[4])
    {
        return -RT_ERROR;
    }
    rt_memcpy(uid_part, recv, 4);
    return RT_EOK;
}

rt_err_t APP_Rc522_ReadUID(rt_uint8_t *uid, rt_uint8_t *uid_len)
{
    rt_uint8_t cl1[4] = {0};
    rt_uint8_t cl2[4] = {0};
    rt_bool_t cascade = RT_FALSE;
    rt_err_t result;

    if ((uid == RT_NULL) || (uid_len == RT_NULL))
    {
        return -RT_EINVAL;
    }

    result = app_rc522_anticoll_level(PICC_CMD_SEL_CL1, cl1);
    if (result != RT_EOK)
    {
        return result;
    }
    result = app_rc522_select_level(PICC_CMD_SEL_CL1, cl1, &cascade);
    if (result != RT_EOK)
    {
        return result;
    }

    if (cl1[0] != PICC_CASCADE_TAG)
    {
        rt_memcpy(uid, cl1, 4);
        *uid_len = 4;
        return RT_EOK;
    }
    if (!cascade)
    {
        return -RT_ERROR;
    }

    result = app_rc522_anticoll_level(PICC_CMD_SEL_CL2, cl2);
    if (result != RT_EOK)
    {
        return result;
    }
    result = app_rc522_select_level(PICC_CMD_SEL_CL2, cl2, &cascade);
    if (result != RT_EOK)
    {
        return result;
    }

    uid[0] = cl1[1];
    uid[1] = cl1[2];
    uid[2] = cl1[3];
    uid[3] = cl2[0];
    uid[4] = cl2[1];
    uid[5] = cl2[2];
    uid[6] = cl2[3];
    *uid_len = 7;
    return RT_EOK;
}

rt_err_t APP_Rc522_ReadUIDTextEx(char *uid_text, rt_size_t size, APP_Rc522Diag *diag)
{
    rt_uint8_t version;
    rt_uint8_t atqa[2] = {0};
    rt_uint8_t uid[APP_RC522_UID_MAX_LEN] = {0};
    rt_uint8_t uid_len = 0;
    rt_err_t result;

    if ((uid_text == RT_NULL) || (size == 0))
    {
        app_rc522_set_diag(diag, "invalid args", -RT_EINVAL, 0, RT_NULL, 0);
        return -RT_EINVAL;
    }

    uid_text[0] = '\0';
    if (diag != RT_NULL)
    {
        rt_memset(diag, 0, sizeof(*diag));
    }

    result = APP_Rc522_Prepare();
    if (result != RT_EOK)
    {
        app_rc522_set_diag(diag, "spi prepare failed", result, 0, RT_NULL, 0);
        return result;
    }

    APP_Rc522_InitChip();
    version = APP_Rc522_ReadVersion();
    if ((version == 0x00) || (version == 0xFF))
    {
        app_rc522_set_diag(diag, "invalid version", -RT_ERROR, version, RT_NULL, 0);
        return -RT_ERROR;
    }

    result = APP_Rc522_Request(atqa);
    if (result != RT_EOK)
    {
        if (result == -RT_ETIMEOUT)
        {
            app_rc522_set_diag(diag, "no card", -RT_ETIMEOUT, version, atqa, 0);
            return -RT_ETIMEOUT;
        }
        app_rc522_set_diag(diag, "request failed", result, version, atqa, 0);
        return result;
    }

    result = APP_Rc522_ReadUID(uid, &uid_len);
    if (result != RT_EOK)
    {
        app_rc522_set_diag(diag, "uid read failed", result, version, atqa, 0);
        return result;
    }

    app_rc522_uid_to_hex(uid, uid_len, uid_text, size);
    app_rc522_set_diag(diag, "uid ok", RT_EOK, version, atqa, uid_len);
    return RT_EOK;
}

rt_err_t APP_Rc522_ReadUIDText(char *uid_text, rt_size_t size)
{
    return APP_Rc522_ReadUIDTextEx(uid_text, size, RT_NULL);
}

rt_err_t APP_Rc522_Prepare(void)
{
    /* OV2640 拍照会临时复用 PA4/PA6，每次读卡前都恢复 RC522 软 SPI 引脚。 */
    app_rc522_board_gpio_init();
    app_rc522_bus_ready = RT_TRUE;
    return RT_EOK;
}

int APP_Rc522_Test(int argc, char **argv)
{
    char uid[APP_RC522_UID_MAX_LEN * 2 + 1];
    APP_Rc522Diag diag;
    rt_err_t result;

    (void)argc;
    (void)argv;

    result = APP_Rc522_ReadUIDTextEx(uid, sizeof(uid), &diag);

    rt_kprintf("APP rfid: bus=gpio-soft-spi mode=0 delay=%uus\r\n", APP_RC522_SPI_DELAY_US);
    rt_kprintf("APP rfid: pins SCK=PA5 MISO=PA6 MOSI=PA7 CS=PA4 RST=PC4\r\n");
    rt_kprintf("APP rfid: step=%s result=%d version=0x%02x atqa=%02x %02x uid_len=%u\r\n",
               diag.step ? diag.step : "none",
               diag.result,
               diag.version,
               diag.atqa[0],
               diag.atqa[1],
               diag.uid_len);

    if (result == RT_EOK)
    {
        rt_kprintf("APP rfid: uid=%s\r\n", uid);
    }
    else if (diag.step && rt_strcmp(diag.step, "no card") == 0)
    {
        rt_kprintf("APP rfid: no card\r\n");
    }
    else
    {
        rt_kprintf("APP rfid: failed\r\n");
    }

    return result;
}

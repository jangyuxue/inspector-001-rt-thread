#include "APP_Pn532.h"

#include <rtdevice.h>
#include <rthw.h>
#include <board.h>
#include <string.h>

/** PN532 软件 I2C 的 SCL 端口，已在探索者 V3 右侧排针验证。 */
#define APP_PN532_SCL_PORT                 GPIOE
/** PN532 软件 I2C 的 SCL 引脚。 */
#define APP_PN532_SCL_PIN                  GPIO_PIN_3
/** PN532 软件 I2C 的 SDA 端口，已在探索者 V3 右侧排针验证。 */
#define APP_PN532_SDA_PORT                 GPIOE
/** PN532 软件 I2C 的 SDA 引脚。 */
#define APP_PN532_SDA_PIN                  GPIO_PIN_2
/** PN532 I2C 模式下的 7 位地址。 */
#define APP_PN532_I2C_ADDR                 0x24
/** 软件 I2C 半周期延时；杜邦线连接时保持保守速度更稳定。 */
#define APP_PN532_I2C_DELAY_US             50
/** PN532 拉低 SCL 进行时钟延展时的最长等待时间。 */
#define APP_PN532_I2C_STRETCH_TIMEOUT_US   1000
/** 当前 PN532 命令和响应使用的最大帧缓存长度。 */
#define APP_PN532_FRAME_MAX                80
/** 单个 PN532 命令事务的最大重试次数。 */
#define APP_PN532_COMMAND_RETRY_MAX        3

/** PN532 普通信息帧格式常量。 */
#define PN532_PREAMBLE                     0x00
#define PN532_STARTCODE1                   0x00
#define PN532_STARTCODE2                   0xFF
#define PN532_POSTAMBLE                    0x00
#define PN532_HOSTTOPN532                  0xD4
#define PN532_PN532TOHOST                  0xD5
#define PN532_I2C_READY                    0x01
#define PN532_CMD_GET_FIRMWARE_VERSION     0x02
#define PN532_CMD_SAM_CONFIGURATION        0x14
#define PN532_CMD_INLIST_PASSIVE_TARGET    0x4A

static const char *pn532_last_step = "not started";
static rt_err_t pn532_last_result = RT_EOK;
static rt_int32_t pn532_last_nack_index = -2;
static rt_uint8_t pn532_last_nack_byte = 0x00;
static rt_uint8_t pn532_last_retry = 0;

static void pn532_i2c_start(void);
static void pn532_i2c_stop(void);
static rt_err_t pn532_i2c_write_byte(rt_uint8_t data);
static rt_err_t pn532_scl_wait_high(void);
static void pn532_get_diag(APP_Pn532Diag *diag);

static void pn532_gpio_input_pullup(GPIO_TypeDef *port, rt_uint16_t pin)
{
    GPIO_InitTypeDef gpio;

    gpio.Pin = pin;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(port, &gpio);
}

static void pn532_gpio_output_low(GPIO_TypeDef *port, rt_uint16_t pin)
{
    GPIO_InitTypeDef gpio;

    HAL_GPIO_WritePin(port, pin, GPIO_PIN_RESET);

    gpio.Pin = pin;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(port, &gpio);
}

static void pn532_set_result(const char *step, rt_err_t result)
{
    pn532_last_step = step;
    pn532_last_result = result;
}

static void pn532_i2c_delay(void)
{
    rt_hw_us_delay(APP_PN532_I2C_DELAY_US);
}

static void pn532_scl_low(void)
{
    pn532_gpio_output_low(APP_PN532_SCL_PORT, APP_PN532_SCL_PIN);
    pn532_i2c_delay();
}

static void pn532_scl_release(void)
{
    pn532_gpio_input_pullup(APP_PN532_SCL_PORT, APP_PN532_SCL_PIN);
    pn532_i2c_delay();
}

static void pn532_sda_low(void)
{
    pn532_gpio_output_low(APP_PN532_SDA_PORT, APP_PN532_SDA_PIN);
    pn532_i2c_delay();
}

static void pn532_sda_release(void)
{
    pn532_gpio_input_pullup(APP_PN532_SDA_PORT, APP_PN532_SDA_PIN);
    pn532_i2c_delay();
}

static int pn532_read_scl(void)
{
    return (HAL_GPIO_ReadPin(APP_PN532_SCL_PORT, APP_PN532_SCL_PIN) == GPIO_PIN_SET) ? PIN_HIGH : PIN_LOW;
}

static int pn532_read_sda(void)
{
    return (HAL_GPIO_ReadPin(APP_PN532_SDA_PORT, APP_PN532_SDA_PIN) == GPIO_PIN_SET) ? PIN_HIGH : PIN_LOW;
}

static void pn532_scl_write(rt_base_t value)
{
    if (value == PIN_LOW)
    {
        pn532_scl_low();
    }
    else
    {
        pn532_scl_release();
    }
}

static rt_err_t pn532_scl_high_wait(void)
{
    pn532_scl_release();
    return pn532_scl_wait_high();
}

static void pn532_sda_write(rt_base_t value)
{
    if (value == PIN_LOW)
    {
        pn532_sda_low();
    }
    else
    {
        pn532_sda_release();
    }
}

/** @brief 初始化 PN532 软件 I2C 引脚，并让总线进入释放状态。 */
static void pn532_init(void)
{
    __HAL_RCC_GPIOE_CLK_ENABLE();

    pn532_scl_release();
    pn532_sda_release();
    rt_thread_mdelay(5);
}

/** @brief 通过手动输出 SCL 脉冲，恢复可能被从设备拉住的软件 I2C 总线。 */
static void pn532_recover(void)
{
    int i;

    pn532_sda_release();
    for (i = 0; i < 9; i++)
    {
        pn532_scl_release();
        pn532_scl_low();
    }

    pn532_i2c_stop();
    rt_thread_mdelay(5);
}

static void pn532_i2c_force_idle(void)
{
    pn532_sda_low();
    pn532_scl_release();
    rt_thread_mdelay(1);
    pn532_sda_release();
    pn532_scl_release();
    rt_thread_mdelay(2);
}

static rt_err_t pn532_scl_wait_high(void)
{
    rt_uint32_t elapsed = 0;

    while (pn532_read_scl() == PIN_LOW)
    {
        if (elapsed >= APP_PN532_I2C_STRETCH_TIMEOUT_US)
        {
            pn532_set_result("i2c scl stretch timeout", -RT_ETIMEOUT);
            return -RT_ETIMEOUT;
        }

        rt_hw_us_delay(10);
        elapsed += 10;
    }

    return RT_EOK;
}

/** @brief 探测一个 7 位 I2C 地址是否有 ACK 响应。 */
static rt_err_t pn532_probe(rt_uint8_t addr)
{
    rt_err_t result;

    pn532_i2c_start();
    result = pn532_i2c_write_byte((addr << 1) | 0x00);
    pn532_i2c_stop();

    pn532_set_result((result == RT_EOK) ? "probe ok" : "probe no ack", result);
    return result;
}

static void pn532_i2c_start(void)
{
    pn532_sda_write(PIN_HIGH);
    pn532_scl_high_wait();
    pn532_sda_write(PIN_LOW);
    pn532_scl_write(PIN_LOW);
}

static void pn532_i2c_stop(void)
{
    pn532_sda_write(PIN_LOW);
    pn532_scl_high_wait();
    pn532_sda_write(PIN_HIGH);
}

static rt_err_t pn532_i2c_write_byte(rt_uint8_t data)
{
    int i;
    rt_base_t ack;

    pn532_sda_write(PIN_HIGH);
    for (i = 0; i < 8; i++)
    {
        pn532_sda_write((data & 0x80) ? PIN_HIGH : PIN_LOW);
        if (pn532_scl_high_wait() != RT_EOK)
        {
            pn532_scl_write(PIN_LOW);
            return -RT_ERROR;
        }
        pn532_scl_write(PIN_LOW);
        data <<= 1;
    }

    pn532_sda_write(PIN_HIGH);
    if (pn532_scl_high_wait() != RT_EOK)
    {
        pn532_scl_write(PIN_LOW);
        return -RT_ERROR;
    }
    ack = pn532_read_sda();
    pn532_scl_write(PIN_LOW);
    pn532_sda_write(PIN_HIGH);

    return (ack == PIN_LOW) ? RT_EOK : -RT_ERROR;
}

static rt_uint8_t pn532_i2c_read_byte(rt_bool_t ack)
{
    int i;
    rt_uint8_t data = 0;

    pn532_sda_write(PIN_HIGH);
    for (i = 0; i < 8; i++)
    {
        data <<= 1;
        if (pn532_scl_high_wait() != RT_EOK)
        {
            pn532_scl_write(PIN_LOW);
            return data;
        }
        if (pn532_read_sda() == PIN_HIGH)
        {
            data |= 0x01;
        }
        pn532_scl_write(PIN_LOW);
    }

    pn532_sda_write(ack ? PIN_LOW : PIN_HIGH);
    pn532_scl_high_wait();
    pn532_scl_write(PIN_LOW);
    pn532_sda_write(PIN_HIGH);

    return data;
}

static rt_err_t pn532_i2c_write(const rt_uint8_t *data, rt_size_t len)
{
    rt_size_t i;

    pn532_set_result("i2c write", RT_EOK);
    pn532_last_nack_index = -2;
    pn532_last_nack_byte = 0x00;
    if ((data == RT_NULL) || (len == 0))
    {
        pn532_set_result("i2c write invalid args", -RT_EINVAL);
        return -RT_EINVAL;
    }

    pn532_i2c_start();
    if (pn532_i2c_write_byte((APP_PN532_I2C_ADDR << 1) | 0x00) != RT_EOK)
    {
        pn532_i2c_stop();
        pn532_set_result("i2c write address no ack", -RT_ERROR);
        return -RT_ERROR;
    }

    for (i = 0; i < len; i++)
    {
        if (pn532_i2c_write_byte(data[i]) != RT_EOK)
        {
            pn532_i2c_stop();
            pn532_set_result("i2c write data no ack", -RT_ERROR);
            pn532_last_nack_index = (rt_int32_t)i;
            pn532_last_nack_byte = data[i];
            return -RT_ERROR;
        }
    }

    pn532_i2c_stop();
    return RT_EOK;
}

/** @brief 从 PN532 软件 I2C 总线读取指定长度数据。 */
static rt_err_t pn532_i2c_read(rt_uint8_t *data, rt_size_t len)
{
    rt_size_t i;

    pn532_set_result("i2c read", RT_EOK);
    if ((data == RT_NULL) || (len == 0))
    {
        pn532_set_result("i2c read invalid args", -RT_EINVAL);
        return -RT_EINVAL;
    }

    pn532_i2c_start();
    if (pn532_i2c_write_byte((APP_PN532_I2C_ADDR << 1) | 0x01) != RT_EOK)
    {
        pn532_i2c_stop();
        pn532_set_result("i2c read address no ack", -RT_ERROR);
        return -RT_ERROR;
    }

    for (i = 0; i < len; i++)
    {
        data[i] = pn532_i2c_read_byte((i + 1) < len);
    }

    pn532_i2c_stop();
    return RT_EOK;
}

/** @brief 轮询 PN532 I2C 状态字，等待模块准备好响应数据。 */
static rt_err_t pn532_wait_ready(rt_int32_t timeout_ms)
{
    rt_uint8_t status;
    rt_err_t result;
    rt_int32_t elapsed = 0;

    pn532_set_result("wait pn532 ready", RT_EOK);
    while (elapsed <= timeout_ms)
    {
        result = pn532_i2c_read(&status, 1);
        if (result != RT_EOK)
        {
            return result;
        }

        if (status == PN532_I2C_READY)
        {
            return RT_EOK;
        }
        rt_thread_mdelay(10);
        elapsed += 10;
    }

    pn532_set_result("wait pn532 ready timeout", -RT_ETIMEOUT);
    return -RT_ETIMEOUT;
}

/** @brief 按 PN532 普通信息帧格式封装并发送命令。 */
static rt_err_t pn532_write_command(const rt_uint8_t *cmd, rt_uint8_t cmd_len)
{
    rt_uint8_t frame[APP_PN532_FRAME_MAX];
    rt_uint8_t len;
    rt_uint8_t sum;
    rt_uint8_t i;
    rt_uint8_t pos = 0;

    pn532_set_result("build command frame", RT_EOK);
    if ((cmd == RT_NULL) || (cmd_len == 0) || ((cmd_len + 8) > sizeof(frame)))
    {
        pn532_set_result("command frame invalid args", -RT_EINVAL);
        return -RT_EINVAL;
    }

    len = cmd_len + 1;
    frame[pos++] = PN532_PREAMBLE;
    frame[pos++] = PN532_STARTCODE1;
    frame[pos++] = PN532_STARTCODE2;
    frame[pos++] = len;
    frame[pos++] = (rt_uint8_t)(0x100 - len);
    frame[pos++] = PN532_HOSTTOPN532;
    sum = PN532_HOSTTOPN532;

    for (i = 0; i < cmd_len; i++)
    {
        frame[pos++] = cmd[i];
        sum = (rt_uint8_t)(sum + cmd[i]);
    }

    frame[pos++] = (rt_uint8_t)(0x100 - sum);
    frame[pos++] = PN532_POSTAMBLE;

    return pn532_i2c_write(frame, pos);
}

/** @brief 读取并校验 PN532 ACK 帧，确认命令已被模块接收。 */
static rt_err_t pn532_read_ack(void)
{
    rt_uint8_t ack[7];
    rt_err_t result;
    static const rt_uint8_t expected_ack[7] = {PN532_I2C_READY, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

    pn532_set_result("read ack wait ready", RT_EOK);
    result = pn532_wait_ready(1000);
    if (result != RT_EOK)
    {
        return result;
    }

    pn532_set_result("read ack bytes", RT_EOK);
    if (pn532_i2c_read(ack, sizeof(ack)) != RT_EOK)
    {
        pn532_set_result("read ack i2c failed", -RT_ERROR);
        return -RT_ERROR;
    }

    if (rt_memcmp(ack, expected_ack, sizeof(expected_ack)) != 0)
    {
        pn532_set_result("ack frame invalid", -RT_ERROR);
        return -RT_ERROR;
    }

    return RT_EOK;
}

/** @brief 读取 PN532 响应帧，并完成帧头、长度、命令码和校验和检查。 */
static rt_err_t pn532_read_response(rt_uint8_t expected_cmd,
                                    rt_uint8_t *payload,
                                    rt_uint8_t *payload_len,
                                    rt_int32_t timeout_ms)
{
    rt_uint8_t raw[APP_PN532_FRAME_MAX];
    rt_uint8_t frame_len;
    rt_uint8_t data_len;
    rt_uint8_t sum;
    rt_uint8_t i;
    rt_err_t result;

    pn532_set_result("read response", RT_EOK);
    if ((payload == RT_NULL) || (payload_len == RT_NULL))
    {
        pn532_set_result("response invalid args", -RT_EINVAL);
        return -RT_EINVAL;
    }

    result = pn532_wait_ready(timeout_ms);
    if (result != RT_EOK)
    {
        return result;
    }

    pn532_set_result("read response bytes", RT_EOK);
    if (pn532_i2c_read(raw, sizeof(raw)) != RT_EOK)
    {
        pn532_set_result("read response i2c failed", -RT_ERROR);
        return -RT_ERROR;
    }

    if ((raw[0] != PN532_I2C_READY) ||
        (raw[1] != PN532_PREAMBLE) ||
        (raw[2] != PN532_STARTCODE1) ||
        (raw[3] != PN532_STARTCODE2))
    {
        pn532_set_result("response header invalid", -RT_ERROR);
        return -RT_ERROR;
    }

    frame_len = raw[4];
    if (((rt_uint8_t)(frame_len + raw[5])) != 0)
    {
        pn532_set_result("response length checksum invalid", -RT_ERROR);
        return -RT_ERROR;
    }

    if ((frame_len < 2) || ((rt_size_t)(frame_len + 8) > sizeof(raw)))
    {
        pn532_set_result("response length invalid", -RT_ERROR);
        return -RT_ERROR;
    }

    if ((raw[6] != PN532_PN532TOHOST) || (raw[7] != (rt_uint8_t)(expected_cmd + 1)))
    {
        pn532_set_result("response command invalid", -RT_ERROR);
        return -RT_ERROR;
    }

    sum = 0;
    for (i = 0; i < frame_len; i++)
    {
        sum = (rt_uint8_t)(sum + raw[6 + i]);
    }
    sum = (rt_uint8_t)(sum + raw[6 + frame_len]);
    if (sum != 0)
    {
        pn532_set_result("response data checksum invalid", -RT_ERROR);
        return -RT_ERROR;
    }

    data_len = frame_len - 2;
    if (data_len > *payload_len)
    {
        pn532_set_result("response payload buffer too small", -RT_EFULL);
        return -RT_EFULL;
    }

    rt_memcpy(payload, &raw[8], data_len);
    *payload_len = data_len;

    return RT_EOK;
}

/** @brief 执行一次完整 PN532 命令事务，失败时进行有限次数总线恢复和重试。 */
static rt_err_t pn532_command(const rt_uint8_t *cmd,
                              rt_uint8_t cmd_len,
                              rt_uint8_t expected_cmd,
                              rt_uint8_t *payload,
                              rt_uint8_t *payload_len,
                              rt_int32_t timeout_ms)
{
    rt_err_t result = -RT_ERROR;
    rt_uint8_t attempt;
    rt_uint8_t payload_capacity;

    if (payload_len == RT_NULL)
    {
        pn532_set_result("command invalid args", -RT_EINVAL);
        return -RT_EINVAL;
    }

    payload_capacity = *payload_len;

    for (attempt = 0; attempt < APP_PN532_COMMAND_RETRY_MAX; attempt++)
    {
        pn532_last_retry = attempt;
        *payload_len = payload_capacity;

        if (attempt > 0)
        {
            pn532_recover();
            pn532_i2c_force_idle();
            rt_thread_mdelay(20);
        }

        result = pn532_write_command(cmd, cmd_len);
        if (result != RT_EOK)
        {
            continue;
        }

        result = pn532_read_ack();
        if (result != RT_EOK)
        {
            continue;
        }

        result = pn532_read_response(expected_cmd, payload, payload_len, timeout_ms);
        if (result == RT_EOK)
        {
            return RT_EOK;
        }
    }

    pn532_last_result = result;
    return result;
}

/** @brief 读取 PN532 固件版本字节，用于确认模块通信正常。 */
static rt_err_t pn532_get_firmware(rt_uint8_t fw[4])
{
    rt_uint8_t cmd[] = {PN532_CMD_GET_FIRMWARE_VERSION};
    rt_uint8_t payload[8];
    rt_uint8_t payload_len = sizeof(payload);
    rt_err_t result;

    if (fw == RT_NULL)
    {
        pn532_set_result("firmware invalid args", -RT_EINVAL);
        return -RT_EINVAL;
    }

    result = pn532_command(cmd,
                           sizeof(cmd),
                           PN532_CMD_GET_FIRMWARE_VERSION,
                           payload,
                           &payload_len,
                           1000);
    if (result != RT_EOK)
    {
        return result;
    }

    if (payload_len < 4)
    {
        pn532_set_result("firmware payload too short", -RT_ERROR);
        return -RT_ERROR;
    }

    rt_memcpy(fw, payload, 4);
    pn532_set_result("firmware ok", RT_EOK);
    return RT_EOK;
}

/** @brief 配置 PN532 为普通 SAM 模式，后续才能进行被动寻卡。 */
static rt_err_t pn532_sam_config(void)
{
    rt_uint8_t cmd[] = {PN532_CMD_SAM_CONFIGURATION, 0x01, 0x14, 0x01};
    rt_uint8_t payload[4];
    rt_uint8_t payload_len = sizeof(payload);
    rt_err_t result;

    result = pn532_command(cmd,
                           sizeof(cmd),
                           PN532_CMD_SAM_CONFIGURATION,
                           payload,
                           &payload_len,
                           1000);
    if (result == RT_EOK)
    {
        pn532_set_result("sam config ok", RT_EOK);
    }
    return result;
}

/** @brief 寻找一张 ISO14443A 被动卡，并复制其 UID。 */
static rt_err_t pn532_read_uid(rt_uint8_t *uid, rt_uint8_t *uid_len)
{
    rt_uint8_t cmd[] = {PN532_CMD_INLIST_PASSIVE_TARGET, 0x01, 0x00};
    rt_uint8_t payload[32];
    rt_uint8_t payload_len = sizeof(payload);
    rt_uint8_t len;
    rt_err_t result;

    if ((uid == RT_NULL) || (uid_len == RT_NULL))
    {
        pn532_set_result("uid invalid args", -RT_EINVAL);
        return -RT_EINVAL;
    }

    result = pn532_command(cmd,
                           sizeof(cmd),
                           PN532_CMD_INLIST_PASSIVE_TARGET,
                           payload,
                           &payload_len,
                           1500);
    if (result != RT_EOK)
    {
        return result;
    }

    if ((payload_len < 7) || (payload[0] == 0))
    {
        pn532_set_result("no card", -RT_ETIMEOUT);
        return -RT_ETIMEOUT;
    }

    len = payload[5];
    if ((len == 0) || (len > APP_PN532_UID_MAX_LEN) || ((rt_uint8_t)(6 + len) > payload_len))
    {
        pn532_set_result("uid payload invalid", -RT_ERROR);
        return -RT_ERROR;
    }

    rt_memcpy(uid, &payload[6], len);
    *uid_len = len;
    pn532_set_result("uid ok", RT_EOK);
    return RT_EOK;
}

static void pn532_uid_to_hex(const rt_uint8_t *uid, rt_uint8_t uid_len, char *out, rt_size_t out_size)
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

/**
 * @brief 读取一张 PN532 NFC 卡 UID，并格式化为大写十六进制字符串。
 */
rt_err_t APP_Pn532_ReadUIDTextEx(char *uid_text, rt_size_t size, APP_Pn532Diag *diag)
{
    rt_uint8_t uid[APP_PN532_UID_MAX_LEN] = {0};
    rt_uint8_t uid_len = 0;
    rt_uint8_t fw[4] = {0};
    rt_err_t result;

    if ((uid_text == RT_NULL) || (size == 0))
    {
        pn532_set_result("read uid text invalid args", -RT_EINVAL);
        return -RT_EINVAL;
    }
    uid_text[0] = '\0';

    pn532_init();
    pn532_recover();
    result = pn532_probe(APP_PN532_I2C_ADDR);
    if (result != RT_EOK)
    {
        goto out;
    }

    pn532_recover();
    rt_thread_mdelay(20);
    result = pn532_get_firmware(fw);
    if (result != RT_EOK)
    {
        goto out;
    }

    result = pn532_sam_config();
    if (result != RT_EOK)
    {
        goto out;
    }

    result = pn532_read_uid(uid, &uid_len);
    if (result != RT_EOK)
    {
        goto out;
    }

    pn532_uid_to_hex(uid, uid_len, uid_text, size);

out:
    if (diag != RT_NULL)
    {
        pn532_get_diag(diag);
        diag->result = result;
        diag->firmware[0] = fw[0];
        diag->firmware[1] = fw[1];
        diag->firmware[2] = fw[2];
        diag->firmware[3] = fw[3];
        diag->uid_len = uid_len;
    }
    return result;
}

/**
 * @brief 读取一张 PN532 NFC 卡 UID，不返回诊断信息。
 */
rt_err_t APP_Pn532_ReadUIDText(char *uid_text, rt_size_t size)
{
    return APP_Pn532_ReadUIDTextEx(uid_text, size, RT_NULL);
}

/** @brief 拷贝最近一次内部诊断状态，方便 FinSH 输出具体失败步骤。 */
static void pn532_get_diag(APP_Pn532Diag *diag)
{
    if (diag == RT_NULL)
    {
        return;
    }

    diag->step = pn532_last_step;
    diag->result = pn532_last_result;
    diag->nack_index = pn532_last_nack_index;
    diag->nack_byte = pn532_last_nack_byte;
    diag->retry = pn532_last_retry;
}

/**
 * @brief FinSH 命令 `APP nfc` 的实现。
 */
int APP_Pn532_Test(int argc, char **argv)
{
    char uid[APP_PN532_UID_MAX_LEN * 2 + 1];
    APP_Pn532Diag diag;
    rt_err_t result;
    int idle_scl;
    int idle_sda;

    (void)argc;
    (void)argv;

    rt_memset(&diag, 0, sizeof(diag));
    pn532_init();
    pn532_recover();
    idle_scl = pn532_read_scl();
    idle_sda = pn532_read_sda();

    result = APP_Pn532_ReadUIDTextEx(uid, sizeof(uid), &diag);

    rt_kprintf("APP nfc: bus=gpio-soft-i2c addr=0x%02x delay=%uus\r\n",
               APP_PN532_I2C_ADDR,
               APP_PN532_I2C_DELAY_US);
    rt_kprintf("APP nfc: pins SCL=PE3 SDA=PE2 IRQ=NC RST=NC\r\n");
    rt_kprintf("APP nfc: idle SCL=%d SDA=%d\r\n",
               idle_scl,
               idle_sda);
    rt_kprintf("APP nfc: step=%s result=%d retry=%u nack_index=%d nack_byte=0x%02x\r\n",
               diag.step ? diag.step : "none",
               diag.result,
               diag.retry,
               diag.nack_index,
               diag.nack_byte);
    rt_kprintf("APP nfc: fw=%02x %02x %02x %02x uid_len=%u\r\n",
               diag.firmware[0],
               diag.firmware[1],
               diag.firmware[2],
               diag.firmware[3],
               diag.uid_len);

    if (result == RT_EOK)
    {
        rt_kprintf("APP nfc: uid=%s\r\n", uid);
    }
    else if (diag.step && rt_strcmp(diag.step, "no card") == 0)
    {
        rt_kprintf("APP nfc: no card\r\n");
    }
    else
    {
        rt_kprintf("APP nfc: failed\r\n");
    }

    return result;
}

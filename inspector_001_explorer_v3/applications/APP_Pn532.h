#ifndef APP_PN532_H
#define APP_PN532_H

#include <rtthread.h>

/** PN532 被动寻卡命令返回的最大 UID 字节数。 */
#define APP_PN532_UID_MAX_LEN    10

/**
 * @brief PN532 单次读卡过程的诊断信息。
 */
typedef struct
{
    const char *step;        /**< 最后停留的总线或协议步骤。 */
    rt_err_t result;         /**< 最终返回值，使用 RT-Thread 风格错误码。 */
    rt_int32_t nack_index;   /**< I2C 写入时未应答的数据下标；负数表示不适用。 */
    rt_uint8_t nack_byte;    /**< I2C 写入时未应答的数据值。 */
    rt_uint8_t retry;        /**< 最后一次命令重试序号。 */
    rt_uint8_t firmware[4];  /**< PN532 固件版本响应字节。 */
    rt_uint8_t uid_len;      /**< 读取到的 UID 字节数。 */
} APP_Pn532Diag;

/**
 * @brief 读取一张 NFC 卡 UID，并返回大写十六进制字符串和诊断信息。
 */
rt_err_t APP_Pn532_ReadUIDTextEx(char *uid_text, rt_size_t size, APP_Pn532Diag *diag);

/**
 * @brief 读取一张 NFC 卡 UID，并返回大写十六进制字符串。
 */
rt_err_t APP_Pn532_ReadUIDText(char *uid_text, rt_size_t size);

/**
 * @brief FinSH 命令 `APP nfc` 的测试入口。
 */
int APP_Pn532_Test(int argc, char **argv);

#endif

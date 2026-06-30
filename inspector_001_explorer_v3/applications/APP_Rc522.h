#ifndef APP_RC522_H
#define APP_RC522_H

#include <rtthread.h>

/** RC522 读卡流程支持的最大 UID 字节数。 */
#define APP_RC522_UID_MAX_LEN    10

/**
 * @brief RC522 单次读卡过程的诊断信息。
 */
typedef struct
{
    const char *step;       /**< 最后停留的协议步骤。 */
    rt_err_t result;        /**< 最终返回值，使用 RT-Thread 风格错误码。 */
    rt_uint8_t version;     /**< RC522 VersionReg 寄存器值。 */
    rt_uint8_t atqa[2];     /**< ISO14443A 寻卡返回的 ATQA 字节。 */
    rt_uint8_t uid_len;     /**< 读取到的 UID 字节数。 */
} APP_Rc522Diag;

/** @brief 配置软件 SPI 引脚，并准备 RC522 片选和复位引脚。 */
rt_err_t APP_Rc522_Prepare(void);
/** @brief 复位 RC522，并初始化 ISO14443A 寻卡所需寄存器。 */
void APP_Rc522_InitChip(void);
/** @brief 读取 RC522 VersionReg 寄存器值。 */
rt_uint8_t APP_Rc522_ReadVersion(void);
/** @brief 执行一次 ISO14443A 寻卡，并返回 ATQA。 */
rt_err_t APP_Rc522_Request(rt_uint8_t atqa[2]);
/** @brief 读取一张卡的二进制 UID。 */
rt_err_t APP_Rc522_ReadUID(rt_uint8_t *uid, rt_uint8_t *uid_len);
/** @brief 读取一张卡 UID，并返回大写十六进制字符串和诊断信息。 */
rt_err_t APP_Rc522_ReadUIDTextEx(char *uid_text, rt_size_t size, APP_Rc522Diag *diag);
/** @brief 读取一张卡 UID，并返回大写十六进制字符串。 */
rt_err_t APP_Rc522_ReadUIDText(char *uid_text, rt_size_t size);
/** @brief FinSH 命令 `APP rfid` 的测试入口。 */
int APP_Rc522_Test(int argc, char **argv);

#endif

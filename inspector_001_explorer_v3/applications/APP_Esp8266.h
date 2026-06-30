#ifndef APP_ESP8266_H__
#define APP_ESP8266_H__

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FinSH 命令 `APP esp`：发送用户输入内容并自动追加 CRLF。
 */
rt_err_t APP_Esp8266_JoinDefault(void);
rt_err_t APP_Esp8266_WaitReady(rt_uint32_t timeout_ms);
int APP_Esp8266_Test(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif

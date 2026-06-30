#ifndef APP_MQTT_H
#define APP_MQTT_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FinSH 命令 `APP mqtt` 的 MQTT 验证入口。
 */
rt_err_t APP_Mqtt_StartDefault(void);
rt_err_t APP_Mqtt_PublishText(const char *topic, const char *message);
rt_err_t APP_Mqtt_Stop(void);
rt_bool_t APP_Mqtt_IsConnected(void);
int APP_Mqtt_Test(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif

#ifndef APP_FLOW_H
#define APP_FLOW_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

rt_err_t APP_Flow_Start(void);
int APP_Flow_Test(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif

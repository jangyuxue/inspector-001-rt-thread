#ifndef APP_TOUCH_H__
#define APP_TOUCH_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum touch points returned by one GT9xxx sample. */
#define APP_TOUCH_MAX_POINTS    10U

/** @brief One GT9xxx touch sample in LCD pixel coordinates. */
typedef struct
{
    uint8_t count;
    uint8_t pressed;
    uint8_t released;
    uint8_t changed;
    uint16_t x[APP_TOUCH_MAX_POINTS];
    uint16_t y[APP_TOUCH_MAX_POINTS];
} APP_TouchState;

int APP_Touch_Init(void);
void APP_Touch_ResetState(void);
int APP_Touch_Read(APP_TouchState *state);
int APP_Touch_WaitEvent(int32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif

#ifndef APP_FONT_H__
#define APP_FONT_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Find the built-in 5x7 ASCII bitmap. */
const uint8_t *APP_Font_FindAscii5x7(char ch);

#ifdef __cplusplus
}
#endif

#endif

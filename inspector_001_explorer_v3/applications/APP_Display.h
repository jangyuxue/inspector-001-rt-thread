#ifndef APP_DISPLAY_H__
#define APP_DISPLAY_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Show a mirrored RGB565 image on LCD.
 */
int APP_Display_ShowRgb565ImageMirrorX(unsigned short x,
                                       unsigned short y,
                                       unsigned short src_w,
                                       unsigned short src_h,
                                       unsigned char scale,
                                       const unsigned short *pixels,
                                       unsigned char swap_bytes,
                                       unsigned char row_phase,
                                       unsigned char row_stride);

int APP_Display_ShowRgb565ImageRotate180(unsigned short x,
                                         unsigned short y,
                                         unsigned short src_w,
                                         unsigned short src_h,
                                         unsigned char scale,
                                         const unsigned short *pixels,
                                         unsigned char swap_bytes,
                                         unsigned char row_phase,
                                         unsigned char row_stride);

/** @brief Enter the camera preview page and pause the normal touch UI. */
int APP_Display_EnterCameraPage(void);

/** @brief Leave the camera preview page and allow normal LCD UI refresh. */
void APP_Display_LeaveCameraPage(void);

/** @brief Update the camera page status text. */
void APP_Display_CameraStatus(const char *status);

/** @brief Draw the main automatic flow page. */
int APP_Display_ShowFlowPage(const char *title,
                             const char *line1,
                             const char *line2,
                             const char *line3,
                             const char *line4);

/** @brief Draw the power-on welcome page. */
int APP_Display_ShowWelcomePage(void);

/** @brief Draw the NFC identity page. */
int APP_Display_ShowNfcPage(void);

/** @brief Draw the RFID identity page. */
int APP_Display_ShowRfidPage(void);

/** @brief Update the NFC page status area only. */
void APP_Display_NfcStatus(const char *line1, const char *line2, const char *line3);

/** @brief Update the RFID page status area only. */
void APP_Display_RfidStatus(const char *line1, const char *line2, const char *line3);

/** @brief Show or hide the NFC register button area only. */
void APP_Display_NfcRegisterButton(int visible);

/** @brief Show or hide the RFID register button area only. */
void APP_Display_RfidRegisterButton(int visible);

/** @brief Return non-zero when a touch point is inside the NFC register button. */
int APP_Display_NfcRegisterHit(unsigned short x, unsigned short y);

/** @brief Return non-zero when a touch point is inside the RFID register button. */
int APP_Display_RfidRegisterHit(unsigned short x, unsigned short y);

/** @brief Return non-zero when a touch point is inside the NFC rescan button. */
int APP_Display_NfcRescanHit(unsigned short x, unsigned short y);

/** @brief Return non-zero when a touch point is inside the RFID rescan button. */
int APP_Display_RfidRescanHit(unsigned short x, unsigned short y);

/** @brief Draw the final result page with CONTINUE and FINISH choices. */
int APP_Display_ShowResultChoicePage(const char *line1,
                                     const char *line2,
                                     const char *line3);

/** @brief Return non-zero when a touch point is inside the CONTINUE button. */
int APP_Display_ResultContinueHit(unsigned short x, unsigned short y);

/** @brief Return non-zero when a touch point is inside the FINISH button. */
int APP_Display_ResultFinishHit(unsigned short x, unsigned short y);

#ifdef __cplusplus
}
#endif

#endif

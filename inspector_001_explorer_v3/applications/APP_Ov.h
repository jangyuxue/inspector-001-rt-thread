#ifndef APP_OV_H
#define APP_OV_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FinSH 命令 `APP ov` 的 OV2640 调试入口。
 */
rt_err_t APP_Ov_PreviewAndSave(char *path, rt_size_t path_size);
rt_err_t APP_Ov_PreviewAndSaveAs(const char *target_path, char *saved_path, rt_size_t saved_path_size);
int APP_Ov_Test(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif

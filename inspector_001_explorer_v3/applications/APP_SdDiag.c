#include <rtthread.h>

#ifndef APP_SD_DIAG_ENABLE
#define APP_SD_DIAG_ENABLE 1
#endif

#if APP_SD_DIAG_ENABLE
#include <rtdevice.h>
#include <dfs_fs.h>
#include <dfs_posix.h>
#include <board.h>

#define APP_SD_PATH       "/sdcard"
#define APP_SD_TEST_DIR   "/sdcard/CODXDIR"
#define APP_SD_TEST_FILE  "/sdcard/CODX.TXT"
#define APP_SD_ENSURE_RETRY_MAX       3
#define APP_SD_ENSURE_WAIT_MS         1200

extern void stm32_mmcsd_change(void);

void APP_SdDiag_RestorePins(void);

static void app_sd_print_errno(const char *tag, int ret)
{
    rt_kprintf("APP sd: %-14s ret=%d errno=%d\r\n", tag, ret, rt_get_errno());
}

static void app_sd_print_mount(void)
{
    struct dfs_filesystem *fs;
    rt_device_t dev;

    dev = rt_device_find("sd0");
    if (dev == RT_NULL)
    {
        rt_kprintf("APP sd: device sd0 not found\r\n");
    }
    else
    {
        rt_kprintf("APP sd: device sd0 found, type=%d flag=0x%04x open=0x%04x\r\n",
                   dev->type,
                   dev->flag,
                   dev->open_flag);
    }

    fs = dfs_filesystem_lookup(APP_SD_PATH);
    if (fs == RT_NULL)
    {
        rt_kprintf("APP sd: filesystem lookup %s failed\r\n", APP_SD_PATH);
        return;
    }

    rt_kprintf("APP sd: fs path=%s type=%s dev=%s\r\n",
               fs->path ? fs->path : "(null)",
               (fs->ops && fs->ops->name) ? fs->ops->name : "(null)",
               fs->dev_id ? fs->dev_id->parent.name : "(null)");
}

static rt_bool_t app_sd_is_mounted(void)
{
    struct dfs_filesystem *fs;

    fs = dfs_filesystem_lookup(APP_SD_PATH);
    if (fs == RT_NULL || fs->path == RT_NULL || fs->ops == RT_NULL || fs->ops->name == RT_NULL)
    {
        return RT_FALSE;
    }

    return (rt_strcmp(fs->path, APP_SD_PATH) == 0) && (rt_strcmp(fs->ops->name, "elm") == 0);
}

static int app_sd_mount_if_needed(void)
{
    int ret;

    if (app_sd_is_mounted())
    {
        rt_kprintf("APP sd: /sdcard already mounted\r\n");
        return RT_EOK;
    }

    if (rt_device_find("sd0") == RT_NULL)
    {
        rt_kprintf("APP sd: sd0 not found, skip mount\r\n");
        return -RT_ENOSYS;
    }

    rt_set_errno(0);
    ret = dfs_mount("sd0", APP_SD_PATH, "elm", 0, 0);
    rt_kprintf("APP sd: mount sd0      ret=%d errno=%d\r\n", ret, rt_get_errno());

    return ret == 0 ? RT_EOK : -RT_ERROR;
}

int APP_SdDiag_EnsureMounted(void)
{
    int retry;

    if (app_sd_is_mounted())
    {
        return RT_EOK;
    }

    for (retry = 0; retry < APP_SD_ENSURE_RETRY_MAX; retry++)
    {
        APP_SdDiag_RestorePins();

        if (rt_device_find("sd0") == RT_NULL)
        {
            stm32_mmcsd_change();
        }

        rt_thread_mdelay(APP_SD_ENSURE_WAIT_MS);

        if (app_sd_mount_if_needed() == RT_EOK)
        {
            return RT_EOK;
        }
    }

    return -RT_ERROR;
}

static void app_sd_test_statfs(void)
{
    struct statfs stat;
    int ret;

    rt_memset(&stat, 0, sizeof(stat));
    rt_set_errno(0);
    ret = dfs_statfs(APP_SD_PATH, &stat);
    app_sd_print_errno("statfs", ret);
    if (ret == 0)
    {
        rt_kprintf("APP sd: blocks=%d free=%d bsize=%d\r\n",
                   stat.f_blocks,
                   stat.f_bfree,
                   stat.f_bsize);
    }
}

static void app_sd_test_mkdir(void)
{
    int ret;

    rt_set_errno(0);
    ret = mkdir(APP_SD_TEST_DIR, 0);
    if (ret < 0 && rt_get_errno() == -EEXIST)
    {
        rt_kprintf("APP sd: %-14s ret=0 errno=%d (exists)\r\n", "mkdir", rt_get_errno());
        return;
    }

    app_sd_print_errno("mkdir", ret);
}

static void app_sd_test_file(void)
{
    const char text[] = "hello sd\r\n";
    char buf[sizeof(text)];
    int fd;
    int ret;

    rt_set_errno(0);
    unlink(APP_SD_TEST_FILE);
    rt_kprintf("APP sd: unlink old errno=%d\r\n", rt_get_errno());

    rt_set_errno(0);
    fd = open(APP_SD_TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0);
    app_sd_print_errno("open write", fd);
    if (fd < 0)
    {
        return;
    }

    rt_set_errno(0);
    ret = write(fd, text, sizeof(text) - 1);
    app_sd_print_errno("write", ret);

    rt_set_errno(0);
    ret = close(fd);
    app_sd_print_errno("close write", ret);

    rt_memset(buf, 0, sizeof(buf));
    rt_set_errno(0);
    fd = open(APP_SD_TEST_FILE, O_RDONLY, 0);
    app_sd_print_errno("open read", fd);
    if (fd < 0)
    {
        return;
    }

    rt_set_errno(0);
    ret = read(fd, buf, sizeof(buf) - 1);
    app_sd_print_errno("read", ret);
    if (ret > 0)
    {
        buf[ret] = '\0';
        rt_kprintf("APP sd: read data='%s'\r\n", buf);
    }

    rt_set_errno(0);
    ret = close(fd);
    app_sd_print_errno("close read", ret);
}

void APP_SdDiag_RestorePins(void)
{
    GPIO_InitTypeDef gpio;

    __HAL_RCC_SDIO_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF12_SDIO;

    /* SDIO 的 CMD/DAT0~DAT3 保持上拉，CLK 保持无上拉。 */
    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11;
    HAL_GPIO_Init(GPIOC, &gpio);

    gpio.Pin = GPIO_PIN_2;
    HAL_GPIO_Init(GPIOD, &gpio);

    gpio.Pull = GPIO_NOPULL;
    gpio.Pin = GPIO_PIN_12;
    HAL_GPIO_Init(GPIOC, &gpio);
}

static void app_sd_print_pin_level(void)
{
    rt_kprintf("APP sd: pins CMD=PD2:%d D0=PC8:%d D1=PC9:%d D2=PC10:%d D3=PC11:%d CK=PC12:%d\r\n",
               HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_2) == GPIO_PIN_SET ? 1 : 0,
               HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_8) == GPIO_PIN_SET ? 1 : 0,
               HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_9) == GPIO_PIN_SET ? 1 : 0,
               HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_10) == GPIO_PIN_SET ? 1 : 0,
               HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_11) == GPIO_PIN_SET ? 1 : 0,
               HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_12) == GPIO_PIN_SET ? 1 : 0);
}

static int app_sd_rescan(void)
{
    if (rt_device_find("sd0") != RT_NULL)
    {
        rt_kprintf("APP sd: sd0 already exists\r\n");
        app_sd_mount_if_needed();
        app_sd_print_mount();
        return RT_EOK;
    }

    rt_kprintf("APP sd: restore SDIO pull-up pins and rescan\r\n");
    APP_SdDiag_RestorePins();
    app_sd_print_pin_level();
    stm32_mmcsd_change();
    rt_thread_mdelay(1500);
    app_sd_mount_if_needed();
    app_sd_print_mount();

    return RT_EOK;
}

int APP_SdDiag_Test(int argc, char **argv)
{
    if ((argc >= 2) && (rt_strcmp(argv[1], "rescan") == 0))
    {
        return app_sd_rescan();
    }

    rt_kprintf("APP sd: diag start\r\n");
    APP_SdDiag_EnsureMounted();
    app_sd_print_mount();
    app_sd_test_statfs();
    app_sd_test_mkdir();
    app_sd_test_file();
    rt_kprintf("APP sd: diag end\r\n");

    return RT_EOK;
}
#else
int APP_SdDiag_Test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    rt_kprintf("APP sd: diagnostic disabled, set APP_SD_DIAG_ENABLE to 1 to enable\r\n");
    return RT_EOK;
}
#endif

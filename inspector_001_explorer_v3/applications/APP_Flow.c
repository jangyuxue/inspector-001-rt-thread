#include "APP_Flow.h"

#include "APP_Display.h"
#include "APP_Mqtt.h"
#include "APP_Ov.h"
#include "APP_Pn532.h"
#include "APP_Rc522.h"
#include "APP_Touch.h"

#include <dfs_posix.h>
#include <rtthread.h>
#include <stdio.h>
#include <string.h>

#define APP_FLOW_THREAD_STACK      4096U
#define APP_FLOW_THREAD_PRIORITY   24U
#define APP_FLOW_THREAD_TICK       10U
#define APP_FLOW_WELCOME_MS        2000U
#define APP_FLOW_SCAN_MS           250U
#define APP_FLOW_ROOT_DIR          "/sdcard/INSPECT"
#define APP_FLOW_PHOTO_DIR         "/sdcard/INSPECT/PHOTOS"
#define APP_FLOW_DEVICE_FILE       "/sdcard/INSPECT/DEVICES.CSV"
#define APP_FLOW_RECORD_FILE       "/sdcard/INSPECT/RECORDS.CSV"
#define APP_FLOW_DEVICE_HEADER     "type,uid,name\r\n"
#define APP_FLOW_RECORD_HEADER     "seq,tick,nfc_uid,nfc_name,rfid_uid,rfid_name,photo_path,photo_status,photo_ret\r\n"
#define APP_FLOW_LINE_SIZE         128U
#define APP_FLOW_RECORD_LINE_SIZE  192U
#define APP_FLOW_TYPE_SIZE         8U
#define APP_FLOW_UID_SIZE          24U
#define APP_FLOW_NAME_SIZE         32U
#define APP_FLOW_PHOTO_PATH_SIZE   64U
#define APP_FLOW_MQTT_TOPIC        "rtt_to_atk/explorer/test"
#define APP_FLOW_MQTT_MSG_SIZE     192U

typedef struct
{
    char type[APP_FLOW_TYPE_SIZE];
    char uid[APP_FLOW_UID_SIZE];
    char name[APP_FLOW_NAME_SIZE];
} app_flow_device_t;

typedef struct
{
    char type[APP_FLOW_TYPE_SIZE];
    char uid[APP_FLOW_UID_SIZE];
    rt_bool_t valid;
    rt_bool_t button_pressed;
} app_flow_pending_t;

typedef enum
{
    APP_FLOW_STAGE_NFC = 0,
    APP_FLOW_STAGE_RFID,
    APP_FLOW_STAGE_CHOICE
} app_flow_stage_t;

extern int APP_SdDiag_EnsureMounted(void);

static struct rt_thread app_flow_thread;
static rt_uint8_t app_flow_stack[APP_FLOW_THREAD_STACK];
static rt_bool_t app_flow_started;
static app_flow_pending_t app_flow_pending;
static app_flow_device_t app_flow_nfc_device;
static rt_bool_t app_flow_nfc_verified;
static app_flow_stage_t app_flow_stage = APP_FLOW_STAGE_NFC;
static rt_uint32_t app_flow_record_seq;
static rt_bool_t app_flow_record_seq_loaded;
static rt_bool_t app_flow_clear_last_uid;

static rt_err_t app_flow_readline(int fd, char *line, rt_size_t size)
{
    char ch;
    rt_size_t pos = 0U;

    while (read(fd, &ch, 1) == 1)
    {
        if (ch == '\n')
        {
            break;
        }
        if ((ch != '\r') && (pos + 1U < size))
        {
            line[pos++] = ch;
        }
    }

    if (pos == 0U)
    {
        return -RT_EEMPTY;
    }

    line[pos] = '\0';
    return RT_EOK;
}

static rt_err_t app_flow_parse_device(const char *line, app_flow_device_t *dev)
{
    rt_memset(dev, 0, sizeof(*dev));
    if (sscanf(line, "%7[^,],%23[^,],%31[^\r\n]", dev->type, dev->uid, dev->name) != 3)
    {
        return -RT_ERROR;
    }

    return RT_EOK;
}

static rt_err_t app_flow_mkdir_if_needed(const char *path)
{
    int ret;

    rt_set_errno(0);
    ret = mkdir(path, 0);
    if ((ret < 0) && (rt_get_errno() != -EEXIST))
    {
        rt_kprintf("APP flow: mkdir %s failed ret=%d errno=%d\r\n", path, ret, rt_get_errno());
        return -RT_ERROR;
    }

    return RT_EOK;
}

static rt_err_t app_flow_ensure_storage(void)
{
    if (APP_SdDiag_EnsureMounted() != RT_EOK)
    {
        return -RT_ERROR;
    }

    if (app_flow_mkdir_if_needed(APP_FLOW_ROOT_DIR) != RT_EOK)
    {
        return -RT_ERROR;
    }

    return app_flow_mkdir_if_needed(APP_FLOW_PHOTO_DIR);
}

static rt_err_t app_flow_ensure_file_header(const char *path, const char *header)
{
    struct stat st;
    int fd;
    rt_size_t len;

    if ((path == RT_NULL) || (header == RT_NULL))
    {
        return -RT_EINVAL;
    }

    if ((stat(path, &st) == 0) && (st.st_size > 0))
    {
        return RT_EOK;
    }

    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0);
    if (fd < 0)
    {
        rt_kprintf("APP flow: open header file failed path=%s errno=%d\r\n", path, rt_get_errno());
        return -RT_ERROR;
    }

    len = rt_strlen(header);
    if (write(fd, header, len) != (int)len)
    {
        close(fd);
        rt_kprintf("APP flow: write header failed path=%s errno=%d\r\n", path, rt_get_errno());
        return -RT_ERROR;
    }

    close(fd);
    return RT_EOK;
}

static rt_err_t app_flow_open_registry(int *fd)
{
    if (app_flow_ensure_storage() != RT_EOK)
    {
        return -RT_ERROR;
    }

    *fd = open(APP_FLOW_DEVICE_FILE, O_RDONLY, 0);
    return (*fd >= 0) ? RT_EOK : -RT_ENOSYS;
}

static rt_err_t app_flow_find_device(const char *type, const char *uid, app_flow_device_t *dev)
{
    int fd;
    char line[APP_FLOW_LINE_SIZE];
    app_flow_device_t cur;
    rt_err_t ret;

    ret = app_flow_open_registry(&fd);
    if (ret != RT_EOK)
    {
        return ret;
    }

    while (app_flow_readline(fd, line, sizeof(line)) == RT_EOK)
    {
        if ((app_flow_parse_device(line, &cur) == RT_EOK) &&
            (rt_strcmp(cur.type, type) == 0) &&
            (rt_strcmp(cur.uid, uid) == 0))
        {
            *dev = cur;
            close(fd);
            return RT_EOK;
        }
    }

    close(fd);
    return -RT_ENOSYS;
}

static rt_bool_t app_flow_name_exists(const char *name)
{
    int fd;
    char line[APP_FLOW_LINE_SIZE];
    app_flow_device_t cur;

    if (app_flow_open_registry(&fd) != RT_EOK)
    {
        return RT_FALSE;
    }

    while (app_flow_readline(fd, line, sizeof(line)) == RT_EOK)
    {
        if ((app_flow_parse_device(line, &cur) == RT_EOK) && (rt_strcmp(cur.name, name) == 0))
        {
            close(fd);
            return RT_TRUE;
        }
    }

    close(fd);
    return RT_FALSE;
}

static rt_err_t app_flow_append_device(const char *type, const char *uid, const char *name)
{
    int fd;
    char line[APP_FLOW_LINE_SIZE];
    rt_size_t len;

    if ((type == RT_NULL) || (uid == RT_NULL) || (name == RT_NULL) ||
        (type[0] == '\0') || (uid[0] == '\0') || (name[0] == '\0'))
    {
        return -RT_EINVAL;
    }

    if (app_flow_ensure_storage() != RT_EOK)
    {
        return -RT_ERROR;
    }
    if (app_flow_ensure_file_header(APP_FLOW_DEVICE_FILE, APP_FLOW_DEVICE_HEADER) != RT_EOK)
    {
        return -RT_ERROR;
    }

    fd = open(APP_FLOW_DEVICE_FILE, O_WRONLY | O_CREAT | O_APPEND, 0);
    if (fd < 0)
    {
        return -RT_ERROR;
    }

    rt_snprintf(line, sizeof(line), "%s,%s,%s\r\n", type, uid, name);
    len = rt_strlen(line);
    if (write(fd, line, len) != (int)len)
    {
        close(fd);
        return -RT_ERROR;
    }

    close(fd);
    return RT_EOK;
}

static void app_flow_load_record_seq(void)
{
    int fd;
    char line[APP_FLOW_LINE_SIZE];
    unsigned long seq;

    if (app_flow_record_seq_loaded != RT_FALSE)
    {
        return;
    }

    app_flow_record_seq_loaded = RT_TRUE;
    app_flow_record_seq = 0U;

    if (app_flow_ensure_storage() != RT_EOK)
    {
        return;
    }

    fd = open(APP_FLOW_RECORD_FILE, O_RDONLY, 0);
    if (fd < 0)
    {
        return;
    }

    while (app_flow_readline(fd, line, sizeof(line)) == RT_EOK)
    {
        if ((sscanf(line, "%lu,", &seq) == 1) && (seq > app_flow_record_seq))
        {
            app_flow_record_seq = (rt_uint32_t)seq;
        }
    }

    close(fd);
}

static rt_uint32_t app_flow_next_record_seq(void)
{
    app_flow_load_record_seq();
    app_flow_record_seq++;
    return app_flow_record_seq;
}

static void app_flow_make_photo_path(rt_uint32_t seq, char *path, rt_size_t path_size)
{
    if ((path == RT_NULL) || (path_size == 0U))
    {
        return;
    }

    rt_snprintf(path, path_size, "%s/INS%04lu.BMP",
                APP_FLOW_PHOTO_DIR,
                (unsigned long)seq);
}

static rt_err_t app_flow_append_record(const app_flow_device_t *nfc,
                                       const app_flow_device_t *rfid,
                                       rt_uint32_t seq,
                                       const char *photo_path,
                                       rt_err_t photo_result)
{
    int fd;
    char line[APP_FLOW_RECORD_LINE_SIZE];
    rt_size_t len;

    if ((nfc == RT_NULL) || (rfid == RT_NULL) || (photo_path == RT_NULL))
    {
        return -RT_EINVAL;
    }
    if (app_flow_ensure_storage() != RT_EOK)
    {
        return -RT_ERROR;
    }
    if (app_flow_ensure_file_header(APP_FLOW_RECORD_FILE, APP_FLOW_RECORD_HEADER) != RT_EOK)
    {
        return -RT_ERROR;
    }

    rt_snprintf(line,
                sizeof(line),
                "%lu,%lu,%s,%s,%s,%s,%s,%s,%d\r\n",
                (unsigned long)seq,
                (unsigned long)rt_tick_get(),
                nfc->uid,
                nfc->name,
                rfid->uid,
                rfid->name,
                photo_path[0] ? photo_path : "-",
                (photo_result == RT_EOK) ? "PHOTO_OK" : "PHOTO_FAIL",
                photo_result);

    fd = open(APP_FLOW_RECORD_FILE, O_WRONLY | O_CREAT | O_APPEND, 0);
    if (fd < 0)
    {
        rt_kprintf("APP flow: open record failed errno=%d\r\n", rt_get_errno());
        return -RT_ERROR;
    }

    len = rt_strlen(line);
    if (write(fd, line, len) != (int)len)
    {
        close(fd);
        rt_kprintf("APP flow: write record failed errno=%d\r\n", rt_get_errno());
        return -RT_ERROR;
    }

    close(fd);
    rt_kprintf("APP flow: record seq=%lu photo=%s ret=%d\r\n",
               (unsigned long)seq,
               photo_path[0] ? photo_path : "-",
               photo_result);
    return RT_EOK;
}

static const char *app_flow_stage_type(void)
{
    return (app_flow_stage == APP_FLOW_STAGE_NFC) ? "NFC" : "RFID";
}

static void app_flow_show_done_page(const app_flow_device_t *dev)
{
    char nfc_name_line[44];
    char nfc_uid_line[40];
    char rfid_name_line[44];
    char rfid_uid_line[40];

    if (app_flow_nfc_verified)
    {
        rt_snprintf(nfc_name_line, sizeof(nfc_name_line), "NFC %s", app_flow_nfc_device.name);
        rt_snprintf(nfc_uid_line, sizeof(nfc_uid_line), "NFC UID %s", app_flow_nfc_device.uid);
    }
    else
    {
        rt_snprintf(nfc_name_line, sizeof(nfc_name_line), "NFC VERIFIED");
        rt_snprintf(nfc_uid_line, sizeof(nfc_uid_line), "NFC UID UNKNOWN");
    }

    rt_snprintf(rfid_name_line, sizeof(rfid_name_line), "RFID %s", dev->name);
    rt_snprintf(rfid_uid_line, sizeof(rfid_uid_line), "RFID UID %s", dev->uid);
    APP_Display_ShowFlowPage("VERIFY OK", nfc_name_line, nfc_uid_line, rfid_name_line, rfid_uid_line);
    rt_kprintf("APP flow: verify ok, nfc uid=%s name=%s, rfid uid=%s name=%s\r\n",
               app_flow_nfc_verified ? app_flow_nfc_device.uid : "(unknown)",
               app_flow_nfc_verified ? app_flow_nfc_device.name : "(unknown)",
               dev->uid,
               dev->name);
}

static rt_err_t app_flow_publish_record(const app_flow_device_t *rfid,
                                        rt_uint32_t seq,
                                        const char *photo_path,
                                        rt_err_t photo_ret,
                                        rt_err_t record_ret)
{
    char message[APP_FLOW_MQTT_MSG_SIZE];
    rt_err_t ret;

    if ((rfid == RT_NULL) || (photo_path == RT_NULL) || (app_flow_nfc_verified == RT_FALSE))
    {
        return -RT_EINVAL;
    }

    APP_Display_ShowFlowPage("NETWORK", "JOIN WIFI", "START MQTT", "", "");
    ret = APP_Mqtt_StartDefault();
    if (ret != RT_EOK)
    {
        rt_kprintf("APP flow: mqtt start failed ret=%d\r\n", ret);
        return ret;
    }

    APP_Display_ShowFlowPage("NETWORK", "MQTT ONLINE", "PUBLISH RECORD", "", "");
    rt_snprintf(message,
                sizeof(message),
                "{\"seq\":%lu,\"nfc\":\"%s\",\"nfc_name\":\"%s\",\"rfid\":\"%s\",\"rfid_name\":\"%s\",\"photo\":\"%s\",\"photo_ret\":%d,\"record_ret\":%d}",
                (unsigned long)seq,
                app_flow_nfc_device.uid,
                app_flow_nfc_device.name,
                rfid->uid,
                rfid->name,
                photo_path[0] ? photo_path : "-",
                photo_ret,
                record_ret);

    ret = APP_Mqtt_PublishText(APP_FLOW_MQTT_TOPIC, message);
    rt_kprintf("APP flow: mqtt publish ret=%d topic=%s\r\n", ret, APP_FLOW_MQTT_TOPIC);
    if (ret == RT_EOK)
    {
        APP_Mqtt_Stop();
    }
    return ret;
}

static void app_flow_show_choice(rt_uint32_t seq, rt_err_t photo_ret, rt_err_t record_ret, rt_err_t mqtt_ret)
{
    char line1[44];
    char line2[44];
    char line3[44];

    rt_snprintf(line1, sizeof(line1), "SEQ %lu %s",
                (unsigned long)seq,
                (photo_ret == RT_EOK) ? "PHOTO OK" : "PHOTO FAIL");
    rt_snprintf(line2, sizeof(line2), "%s",
                (record_ret == RT_EOK) ? "RECORD SAVED" : "RECORD FAILED");
    rt_snprintf(line3, sizeof(line3), "%s",
                (mqtt_ret == RT_EOK) ? "MQTT UPLOADED" : "MQTT FAILED");

    APP_Display_ShowResultChoicePage(line1, line2, line3);
    app_flow_stage = APP_FLOW_STAGE_CHOICE;
}

static void app_flow_finish_business(const app_flow_device_t *rfid)
{
    char target_path[APP_FLOW_PHOTO_PATH_SIZE];
    char photo_path[APP_FLOW_PHOTO_PATH_SIZE];
    rt_uint32_t seq;
    rt_err_t photo_ret;
    rt_err_t record_ret;
    rt_err_t mqtt_ret;

    if ((rfid == RT_NULL) || (app_flow_nfc_verified == RT_FALSE))
    {
        return;
    }

    seq = app_flow_next_record_seq();
    app_flow_make_photo_path(seq, target_path, sizeof(target_path));

    APP_Display_ShowFlowPage("CAPTURE", "NFC AND RFID PASS", "TOUCH SNAPSHOT", "", "");
    rt_thread_mdelay(500);
    photo_ret = APP_Ov_PreviewAndSaveAs(target_path, photo_path, sizeof(photo_path));
    if (photo_path[0] == '\0')
    {
        rt_strncpy(photo_path, target_path, sizeof(photo_path));
        photo_path[sizeof(photo_path) - 1U] = '\0';
    }

    record_ret = app_flow_append_record(&app_flow_nfc_device, rfid, seq, photo_path, photo_ret);
    mqtt_ret = app_flow_publish_record(rfid, seq, photo_path, photo_ret, record_ret);
    app_flow_show_choice(seq, photo_ret, record_ret, mqtt_ret);
}

static void app_flow_show_stage_page(void)
{
    if (app_flow_stage == APP_FLOW_STAGE_NFC)
    {
        APP_Display_ShowNfcPage();
    }
    else
    {
        APP_Display_ShowRfidPage();
    }
}

static void app_flow_stage_status(const char *line1, const char *line2, const char *line3)
{
    if (app_flow_stage == APP_FLOW_STAGE_NFC)
    {
        APP_Display_NfcStatus(line1, line2, line3);
    }
    else
    {
        APP_Display_RfidStatus(line1, line2, line3);
    }
}

static void app_flow_stage_register_button(rt_bool_t visible)
{
    if (app_flow_stage == APP_FLOW_STAGE_NFC)
    {
        APP_Display_NfcRegisterButton(visible);
    }
    else
    {
        APP_Display_RfidRegisterButton(visible);
    }
}

static rt_bool_t app_flow_stage_register_hit(uint16_t x, uint16_t y)
{
    if (app_flow_stage == APP_FLOW_STAGE_NFC)
    {
        return APP_Display_NfcRegisterHit(x, y) ? RT_TRUE : RT_FALSE;
    }

    return APP_Display_RfidRegisterHit(x, y) ? RT_TRUE : RT_FALSE;
}

static rt_bool_t app_flow_stage_rescan_hit(uint16_t x, uint16_t y)
{
    if (app_flow_stage == APP_FLOW_STAGE_NFC)
    {
        return APP_Display_NfcRescanHit(x, y) ? RT_TRUE : RT_FALSE;
    }

    return APP_Display_RfidRescanHit(x, y) ? RT_TRUE : RT_FALSE;
}

static void app_flow_rescan_pending(void)
{
    rt_kprintf("APP flow: rescan %s card\r\n", app_flow_stage_type());
    app_flow_pending.valid = RT_FALSE;
    app_flow_pending.button_pressed = RT_FALSE;
    app_flow_clear_last_uid = RT_TRUE;
    app_flow_show_stage_page();
}

static void app_flow_set_pending(const char *type, const char *uid)
{
    char uid_line[40];

    rt_strncpy(app_flow_pending.type, type, sizeof(app_flow_pending.type));
    rt_strncpy(app_flow_pending.uid, uid, sizeof(app_flow_pending.uid));
    app_flow_pending.type[sizeof(app_flow_pending.type) - 1U] = '\0';
    app_flow_pending.uid[sizeof(app_flow_pending.uid) - 1U] = '\0';
    app_flow_pending.valid = RT_TRUE;
    app_flow_pending.button_pressed = RT_FALSE;

    rt_snprintf(uid_line, sizeof(uid_line), "UID %s", app_flow_pending.uid);
    app_flow_stage_status("PLEASE REGISTER", uid_line, "REGISTER OR RESCAN");
    app_flow_stage_register_button(RT_TRUE);
    rt_kprintf("APP flow: unknown %s uid=%s, touch REGISTER then use APP flow add <name>, or touch RESCAN\r\n",
               app_flow_pending.type,
               app_flow_pending.uid);
}

static void app_flow_show_known(const app_flow_device_t *dev)
{
    char uid_line[40];
    char name_line[44];
    char ok_line[24];

    rt_snprintf(name_line, sizeof(name_line), "NAME %s", dev->name);
    rt_snprintf(uid_line, sizeof(uid_line), "UID %s", dev->uid);
    rt_snprintf(ok_line, sizeof(ok_line), "%s VERIFIED", dev->type);
    app_flow_stage_status(ok_line, name_line, uid_line);
    app_flow_stage_register_button(RT_FALSE);
    rt_kprintf("APP flow: %s verified uid=%s name=%s\r\n", dev->type, dev->uid, dev->name);
}

static void app_flow_handle_known(const app_flow_device_t *dev)
{
    app_flow_show_known(dev);

    if (app_flow_stage == APP_FLOW_STAGE_NFC)
    {
        app_flow_nfc_device = *dev;
        app_flow_nfc_verified = RT_TRUE;
        rt_thread_mdelay(900);
        app_flow_stage = APP_FLOW_STAGE_RFID;
        app_flow_show_stage_page();
        rt_kprintf("APP flow: NFC ok, enter RFID verify\r\n");
    }
    else if (app_flow_stage == APP_FLOW_STAGE_RFID)
    {
        rt_thread_mdelay(900);
        app_flow_stage = APP_FLOW_STAGE_CHOICE;
        app_flow_show_done_page(dev);
        rt_thread_mdelay(700);
        app_flow_finish_business(dev);
    }
}

static void app_flow_poll_register_button(void)
{
    APP_TouchState state;

    if (!app_flow_pending.valid)
    {
        return;
    }

    if (APP_Touch_Read(&state) != RT_EOK || state.count == 0U)
    {
        return;
    }

    if (app_flow_stage_rescan_hit(state.x[0], state.y[0]))
    {
        app_flow_rescan_pending();
        return;
    }

    if (!app_flow_pending.button_pressed && app_flow_stage_register_hit(state.x[0], state.y[0]))
    {
        app_flow_pending.button_pressed = RT_TRUE;
        app_flow_stage_status("INPUT NAME IN MSH", "APP flow add <name>", app_flow_pending.uid);
        rt_kprintf("APP flow: input %s name with APP flow add <name>\r\n", app_flow_pending.type);
    }
}

static void app_flow_back_to_nfc(void)
{
    app_flow_pending.valid = RT_FALSE;
    app_flow_pending.button_pressed = RT_FALSE;
    app_flow_nfc_verified = RT_FALSE;
    rt_memset(&app_flow_nfc_device, 0, sizeof(app_flow_nfc_device));
    app_flow_stage = APP_FLOW_STAGE_NFC;
    app_flow_show_stage_page();
}

static void app_flow_poll_choice_button(void)
{
    APP_TouchState state;

    if (APP_Touch_Read(&state) != RT_EOK || state.count == 0U)
    {
        return;
    }

    if (APP_Display_ResultContinueHit(state.x[0], state.y[0]))
    {
        app_flow_pending.valid = RT_FALSE;
        app_flow_pending.button_pressed = RT_FALSE;
        app_flow_stage = APP_FLOW_STAGE_RFID;
        app_flow_show_stage_page();
        rt_kprintf("APP flow: continue, back to RFID verify\r\n");
    }
    else if (APP_Display_ResultFinishHit(state.x[0], state.y[0]))
    {
        APP_Display_ShowFlowPage("INSPECTION END", "TASK FINISHED", "BACK TO NFC", "", "");
        rt_kprintf("APP flow: finish, back to NFC after 2s\r\n");
        rt_thread_mdelay(2000);
        app_flow_back_to_nfc();
    }
}

static void app_flow_thread_entry(void *parameter)
{
    char uid[APP_FLOW_UID_SIZE];
    char last_uid[APP_FLOW_UID_SIZE] = {0};
    const char *type;
    app_flow_device_t dev;
    rt_err_t ret;

    (void)parameter;

    APP_Display_ShowWelcomePage();
    rt_thread_mdelay(APP_FLOW_WELCOME_MS);
    app_flow_stage = APP_FLOW_STAGE_NFC;
    app_flow_nfc_verified = RT_FALSE;
    rt_memset(&app_flow_nfc_device, 0, sizeof(app_flow_nfc_device));
    app_flow_show_stage_page();
    APP_Touch_Init();

    while (1)
    {
        if (app_flow_stage == APP_FLOW_STAGE_CHOICE)
        {
            app_flow_poll_choice_button();
            rt_thread_mdelay(APP_FLOW_SCAN_MS);
            continue;
        }

        if (app_flow_pending.valid)
        {
            app_flow_poll_register_button();
            rt_thread_mdelay(APP_FLOW_SCAN_MS);
            continue;
        }

        if (app_flow_clear_last_uid)
        {
            last_uid[0] = '\0';
            app_flow_clear_last_uid = RT_FALSE;
        }

        type = app_flow_stage_type();
        if (((app_flow_stage == APP_FLOW_STAGE_NFC) &&
             (APP_Pn532_ReadUIDText(uid, sizeof(uid)) != RT_EOK)) ||
            ((app_flow_stage == APP_FLOW_STAGE_RFID) &&
             (APP_Rc522_ReadUIDText(uid, sizeof(uid)) != RT_EOK)))
        {
            last_uid[0] = '\0';
            rt_thread_mdelay(APP_FLOW_SCAN_MS);
            continue;
        }

        if (rt_strcmp(uid, last_uid) == 0)
        {
            rt_thread_mdelay(APP_FLOW_SCAN_MS);
            continue;
        }
        rt_strncpy(last_uid, uid, sizeof(last_uid));
        last_uid[sizeof(last_uid) - 1U] = '\0';

        ret = app_flow_find_device(type, uid, &dev);
        if (ret == RT_EOK)
        {
            app_flow_handle_known(&dev);
            last_uid[0] = '\0';
        }
        else if (ret == -RT_ENOSYS)
        {
            app_flow_set_pending(type, uid);
        }
        else
        {
            app_flow_stage_status("SD NOT READY", "CHECK SDCARD", uid);
            app_flow_stage_register_button(RT_FALSE);
            rt_kprintf("APP flow: registry read failed ret=%d\r\n", ret);
        }

        rt_thread_mdelay(APP_FLOW_SCAN_MS);
    }
}

rt_err_t APP_Flow_Start(void)
{
    rt_err_t ret;

    if (app_flow_started)
    {
        return RT_EOK;
    }

    ret = rt_thread_init(&app_flow_thread,
                         "app_flow",
                         app_flow_thread_entry,
                         RT_NULL,
                         app_flow_stack,
                         sizeof(app_flow_stack),
                         APP_FLOW_THREAD_PRIORITY,
                         APP_FLOW_THREAD_TICK);
    if (ret == RT_EOK)
    {
        ret = rt_thread_startup(&app_flow_thread);
    }
    if (ret != RT_EOK)
    {
        rt_kprintf("APP flow: thread start failed ret=%d\r\n", ret);
        return ret;
    }

    app_flow_started = RT_TRUE;
    return RT_EOK;
}

static int app_flow_auto_start(void)
{
    return APP_Flow_Start();
}
INIT_APP_EXPORT(app_flow_auto_start);

static rt_err_t app_flow_add_pending(int argc, char **argv)
{
    char name[APP_FLOW_NAME_SIZE] = {0};
    rt_size_t used = 0U;
    int i;
    rt_err_t ret;

    if (!app_flow_pending.valid)
    {
        rt_kprintf("APP flow: no pending card uid\r\n");
        return -RT_ERROR;
    }
    if (!app_flow_pending.button_pressed)
    {
        app_flow_stage_status("TOUCH REGISTER", "BEFORE INPUT", app_flow_pending.uid);
        rt_kprintf("APP flow: touch REGISTER button first\r\n");
        return -RT_ERROR;
    }

    for (i = 2; i < argc; i++)
    {
        rt_size_t len = rt_strlen(argv[i]);

        if ((used + len + (used ? 1U : 0U)) >= sizeof(name))
        {
            app_flow_stage_status("NAME TOO LONG", "PLEASE RENAME", app_flow_pending.uid);
            return -RT_EINVAL;
        }
        if (used)
        {
            name[used++] = '_';
        }
        rt_memcpy(name + used, argv[i], len);
        used += len;
    }

    if (name[0] == '\0')
    {
        app_flow_stage_status("EMPTY NAME", "PLEASE RENAME", app_flow_pending.uid);
        return -RT_EINVAL;
    }

    if (app_flow_name_exists(name))
    {
        app_flow_stage_status("SAME NAME", "PLEASE RENAME", app_flow_pending.uid);
        rt_kprintf("APP flow: same name, please rename\r\n");
        return -RT_EINVAL;
    }

    ret = app_flow_append_device(app_flow_pending.type, app_flow_pending.uid, name);
    if (ret == RT_EOK)
    {
        app_flow_device_t dev;

        rt_strncpy(dev.type, app_flow_pending.type, sizeof(dev.type));
        rt_strncpy(dev.uid, app_flow_pending.uid, sizeof(dev.uid));
        rt_strncpy(dev.name, name, sizeof(dev.name));
        dev.type[sizeof(dev.type) - 1U] = '\0';
        dev.uid[sizeof(dev.uid) - 1U] = '\0';
        dev.name[sizeof(dev.name) - 1U] = '\0';

        app_flow_pending.valid = RT_FALSE;
        app_flow_pending.button_pressed = RT_FALSE;
        app_flow_handle_known(&dev);
    }
    else
    {
        app_flow_stage_status("REGISTER FAILED", "CHECK SDCARD", app_flow_pending.uid);
    }

    return ret;
}

int APP_Flow_Test(int argc, char **argv)
{
    if ((argc >= 2) && (rt_strcmp(argv[1], "start") == 0))
    {
        return APP_Flow_Start();
    }
    if ((argc >= 2) && (rt_strcmp(argv[1], "add") == 0))
    {
        return (argc >= 3) ? app_flow_add_pending(argc, argv) : -RT_EINVAL;
    }
    if ((argc >= 2) && (rt_strcmp(argv[1], "status") == 0))
    {
        rt_kprintf("APP flow: started=%d pending=%d type=%s uid=%s button=%d\r\n",
                   app_flow_started,
                   app_flow_pending.valid,
                   app_flow_pending.type,
                   app_flow_pending.uid,
                   app_flow_pending.button_pressed);
        return RT_EOK;
    }
    if ((argc >= 2) && (rt_strcmp(argv[1], "reset") == 0))
    {
        app_flow_back_to_nfc();
        return RT_EOK;
    }

    rt_kprintf("Usage: APP flow [start|status|add <name>|reset]\r\n");
    return RT_EOK;
}

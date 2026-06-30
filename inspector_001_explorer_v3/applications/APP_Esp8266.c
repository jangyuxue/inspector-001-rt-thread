#include "APP_Esp8266.h"

#include <rtthread.h>
#include <string.h>

#ifdef RT_USING_NETDEV
#include <arpa/inet.h>
#include <netdev.h>
#endif

#ifdef AT_USING_CLIENT
#include <at.h>
#endif
#ifdef AT_DEVICE_USING_ESP8266
#include <at_device_esp8266.h>
#endif

/** ESP8266 鎺ュ湪 Explorer V3 鐨?ATK MODULE锛屽搴?RT-Thread 涓插彛璁惧 uart3銆?*/
#define APP_ESP_UART_NAME          "uart3"
#define APP_ESP_DEVICE_NAME        "esp0"
/** 鍗曟潯 FinSH 鍙戦€佸懡浠ょ殑鏈€澶ч暱搴︼紝涓嶅寘鍚嚜鍔ㄨ拷鍔犵殑 CRLF銆?*/
#define APP_ESP_TX_CMD_SIZE        192U
/** AT client 鍝嶅簲缂撳啿锛岄潤鎬佸垎閰嶏紝閬垮厤 FinSH 璋冭瘯鍛戒护寮曞叆鍫嗙敵璇枫€?*/
#define APP_ESP_AT_RESP_SIZE       4096U
/** AT client 鎵ц鍗曟潯鍛戒护鐨勬渶闀跨瓑寰呮椂闂淬€?*/
#define APP_ESP_AT_TIMEOUT_MS      10000U
#define APP_ESP_WIFI_RESP_SIZE     1024U
#define APP_ESP_DNS0               "8.8.8.8"
#define APP_ESP_DNS1               "114.114.114.114"
#define APP_ESP_JOIN_WAIT_MS       20000U
#define APP_ESP_READY_POLL_MS      300U
#define APP_ESP_READY_REFRESH_MS   1200U
#define APP_ESP_READY_STABLE_HITS  3U

#ifndef APP_ESP_DEFAULT_WIFI_SSID
#define APP_ESP_DEFAULT_WIFI_SSID  ESP8266_SAMPLE_WIFI_SSID
#endif

#ifndef APP_ESP_DEFAULT_WIFI_PASSWORD
#define APP_ESP_DEFAULT_WIFI_PASSWORD ESP8266_SAMPLE_WIFI_PASSWORD
#endif

#ifdef AT_DEVICE_USING_ESP8266
static struct at_device_esp8266 app_esp8266 =
{
    APP_ESP_DEVICE_NAME,
    APP_ESP_UART_NAME,
    APP_ESP_DEFAULT_WIFI_SSID,
    APP_ESP_DEFAULT_WIFI_PASSWORD,
    ESP8266_SAMPLE_RECV_BUFF_LEN,
};

/**
 * @brief 娉ㄥ唽椤圭洰绾?ESP8266 缃戠粶璁惧锛屾浛浠ｅ畼鏂?sample 鑷姩娉ㄥ唽璺緞銆? */
static rt_err_t app_esp8266_register(void)
{
    struct at_device_esp8266 *esp8266 = &app_esp8266;

    if (at_device_get_by_name(AT_DEVICE_NAMETYPE_DEVICE, APP_ESP_DEVICE_NAME) != RT_NULL)
    {
        rt_kprintf("APP esp: %s already registered\r\n", APP_ESP_DEVICE_NAME);
        return RT_EOK;
    }

    rt_kprintf("APP esp: register %s on %s\r\n", APP_ESP_DEVICE_NAME, APP_ESP_UART_NAME);

    return at_device_register(&(esp8266->device),
                              esp8266->device_name,
                              esp8266->client_name,
                              AT_DEVICE_CLASS_ESP8266,
                              (void *)esp8266);
}

static rt_err_t app_esp_ensure_registered(void)
{
    return app_esp8266_register();
}
#else
static rt_err_t app_esp_ensure_registered(void)
{
    return -RT_ENOSYS;
}
#endif

#if defined(AT_DEVICE_USING_ESP8266)
static struct at_device *app_esp_get_at_device(void)
{
    struct at_device *device;

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_DEVICE, APP_ESP_DEVICE_NAME);
    if (device == RT_NULL)
    {
        rt_kprintf("APP esp: %s not registered yet\r\n", APP_ESP_DEVICE_NAME);
    }

    return device;
}
#endif

#if defined(AT_USING_CLIENT) && defined(RT_USING_NETDEV)
static void app_esp_apply_default_dns(struct netdev *netdev)
{
    ip_addr_t dns_addr;

    if (netdev == RT_NULL)
    {
        return;
    }

    if (inet_aton(APP_ESP_DNS0, &dns_addr))
    {
        netdev_low_level_set_dns_server(netdev, 0, &dns_addr);
    }
    if (inet_aton(APP_ESP_DNS1, &dns_addr))
    {
        netdev_low_level_set_dns_server(netdev, 1, &dns_addr);
    }

    rt_kprintf("APP esp: local dns set dns0=%s dns1=%s\r\n", APP_ESP_DNS0, APP_ESP_DNS1);
}

static rt_bool_t app_esp_netdev_ready(struct netdev *netdev)
{
    const char *ip;

    if (netdev == RT_NULL)
    {
        return RT_FALSE;
    }

    ip = inet_ntoa(netdev->ip_addr);
    return (netdev_is_up(netdev) &&
            netdev_is_link_up(netdev) &&
            (rt_strcmp(ip, "0.0.0.0") != 0)) ? RT_TRUE : RT_FALSE;
}

static void app_esp_refresh_netdev_info(struct at_device *device)
{
    struct at_response *resp;
    struct netdev *netdev;
    char ip[32] = {0};
    char gateway[32] = {0};
    char netmask[32] = {0};
    ip_addr_t ip_addr;
    const char *resp_expr = "%*[^\"]\"%[^\"]\"";

    if ((device == RT_NULL) || (device->client == RT_NULL) || (device->netdev == RT_NULL))
    {
        return;
    }

    netdev = device->netdev;
    resp = at_create_resp(256, 0, rt_tick_from_millisecond(1000));
    if (resp == RT_NULL)
    {
        rt_kprintf("APP esp: no memory for netdev refresh\r\n");
        return;
    }

    if (at_obj_exec_cmd(device->client, resp, "AT+CIPSTA?") != RT_EOK)
    {
        rt_kprintf("APP esp: AT+CIPSTA? failed, keep current netdev info\r\n");
        at_delete_resp(resp);
        return;
    }

    if ((at_resp_parse_line_args_by_kw(resp, "ip", resp_expr, ip) <= 0) ||
        (at_resp_parse_line_args_by_kw(resp, "gateway", resp_expr, gateway) <= 0) ||
        (at_resp_parse_line_args_by_kw(resp, "netmask", resp_expr, netmask) <= 0))
    {
        rt_kprintf("APP esp: parse AT+CIPSTA? failed\r\n");
        at_delete_resp(resp);
        return;
    }

    if (inet_aton(ip, &ip_addr))
    {
        netdev_low_level_set_ipaddr(netdev, &ip_addr);
    }
    if (inet_aton(gateway, &ip_addr))
    {
        netdev_low_level_set_gw(netdev, &ip_addr);
    }
    if (inet_aton(netmask, &ip_addr))
    {
        netdev_low_level_set_netmask(netdev, &ip_addr);
    }

    netdev_low_level_set_status(netdev, RT_TRUE);
    if (rt_strcmp(ip, "0.0.0.0") != 0)
    {
        netdev_low_level_set_link_status(netdev, RT_TRUE);
    }
    app_esp_apply_default_dns(netdev);
    netdev_set_default(netdev);

    rt_kprintf("APP esp: netdev refreshed ip=%s gw=%s mask=%s\r\n", ip, gateway, netmask);
    at_delete_resp(resp);
}

rt_err_t APP_Esp8266_WaitReady(rt_uint32_t timeout_ms)
{
    struct at_device *device;
    struct netdev *netdev;
    rt_uint32_t waited = 0U;
    rt_uint32_t refresh_wait = APP_ESP_READY_REFRESH_MS;
    rt_uint8_t stable_hits = 0U;

    if (timeout_ms == 0U)
    {
        timeout_ms = APP_ESP_JOIN_WAIT_MS;
    }

    if (app_esp_ensure_registered() != RT_EOK)
    {
        return -RT_ERROR;
    }

    device = app_esp_get_at_device();
    if ((device == RT_NULL) || (device->netdev == RT_NULL))
    {
        return -RT_ERROR;
    }

    netdev = device->netdev;
    while (waited <= timeout_ms)
    {
        if (!app_esp_netdev_ready(netdev) && (refresh_wait >= APP_ESP_READY_REFRESH_MS))
        {
            app_esp_refresh_netdev_info(device);
            refresh_wait = 0U;
        }

        if (app_esp_netdev_ready(netdev))
        {
            stable_hits++;
            if (stable_hits >= APP_ESP_READY_STABLE_HITS)
            {
                app_esp_apply_default_dns(netdev);
                netdev_set_default(netdev);
                rt_kprintf("APP esp: ready stable ip=%s\r\n", inet_ntoa(netdev->ip_addr));
                return RT_EOK;
            }
        }
        else
        {
            stable_hits = 0U;
        }

        rt_thread_mdelay(APP_ESP_READY_POLL_MS);
        waited += APP_ESP_READY_POLL_MS;
        refresh_wait += APP_ESP_READY_POLL_MS;
    }

    rt_kprintf("APP esp: ready timeout up=%d link=%d ip=%s\r\n",
               netdev_is_up(netdev),
               netdev_is_link_up(netdev),
               inet_ntoa(netdev->ip_addr));
    return -RT_ETIMEOUT;
}
#else
rt_err_t APP_Esp8266_WaitReady(rt_uint32_t timeout_ms)
{
    (void)timeout_ms;
    return -RT_ENOSYS;
}
#endif

#ifdef RT_USING_NETDEV
static void app_esp_print_netdev_status(void)
{
    struct netdev *netdev = netdev_get_by_name(APP_ESP_DEVICE_NAME);

    if (netdev == RT_NULL)
    {
        rt_kprintf("APP esp: netdev %s not found\r\n", APP_ESP_DEVICE_NAME);
        return;
    }

    rt_kprintf("APP esp: netdev=%s flags=0x%04x", netdev->name, netdev->flags);
    rt_kprintf(" up=%d", netdev_is_up(netdev));
    rt_kprintf(" link=%d", netdev_is_link_up(netdev));
    rt_kprintf(" internet=%d", netdev_is_internet_up(netdev));
    rt_kprintf(" dhcp=%d\r\n", netdev_is_dhcp_enabled(netdev));
    rt_kprintf("APP esp: ip=%s\r\n", inet_ntoa(netdev->ip_addr));
    rt_kprintf("APP esp: gw=%s\r\n", inet_ntoa(netdev->gw));
    rt_kprintf("APP esp: mask=%s\r\n", inet_ntoa(netdev->netmask));
    rt_kprintf("APP esp: dns0=%s\r\n", inet_ntoa(netdev->dns_servers[0]));
}
#endif

static rt_err_t app_esp_print_status(void)
{
#ifdef AT_USING_CLIENT
    at_client_t client = at_client_get(APP_ESP_UART_NAME);

    rt_kprintf("APP esp: at client %s %s\r\n",
               APP_ESP_UART_NAME,
               (client != RT_NULL) ? "ready" : "not found");
#endif

#ifdef AT_DEVICE_USING_ESP8266
    {
        struct at_device *device = at_device_get_by_name(AT_DEVICE_NAMETYPE_DEVICE, APP_ESP_DEVICE_NAME);

        if (device != RT_NULL)
        {
            rt_kprintf("APP esp: device=%s init=%d client=%s\r\n",
                       device->name,
                       device->is_init,
                       (device->client != RT_NULL) ? "ready" : "null");
        }
        else
        {
            rt_kprintf("APP esp: device %s not registered\r\n", APP_ESP_DEVICE_NAME);
        }
    }
#endif

#ifdef RT_USING_NETDEV
    app_esp_print_netdev_status();
#endif

    return RT_EOK;
}

static rt_err_t app_esp_net_up(void)
{
#if defined(AT_DEVICE_USING_ESP8266) && defined(RT_USING_NETDEV)
    struct at_device *device;

    if (app_esp_ensure_registered() != RT_EOK)
    {
        return -RT_ERROR;
    }

    device = app_esp_get_at_device();

    if ((device == RT_NULL) || (device->netdev == RT_NULL))
    {
        return -RT_ERROR;
    }

    rt_kprintf("APP esp: netdev_set_up(%s)\r\n", device->netdev->name);
    return netdev_set_up(device->netdev);
#else
    rt_kprintf("APP esp: at_device/netdev is not enabled\r\n");
    return -RT_ENOSYS;
#endif
}

static rt_err_t app_esp_join_wifi(const char *ssid, const char *password)
{
#if defined(AT_DEVICE_USING_ESP8266) && defined(AT_USING_CLIENT)
    struct at_device *device;
    at_response_t resp;
    rt_err_t wait_ret;
    int ret;

    if ((ssid == RT_NULL) || (password == RT_NULL))
    {
        return -RT_EINVAL;
    }

    if (app_esp_ensure_registered() != RT_EOK)
    {
        return -RT_ERROR;
    }

    device = app_esp_get_at_device();
    if ((device == RT_NULL) || (device->client == RT_NULL))
    {
        return -RT_ERROR;
    }

#ifdef RT_USING_NETDEV
    if (app_esp_netdev_ready(device->netdev))
    {
        app_esp_apply_default_dns(device->netdev);
        netdev_set_default(device->netdev);
        rt_kprintf("APP esp: already joined ip=%s\r\n", inet_ntoa(device->netdev->ip_addr));
        return RT_EOK;
    }
#endif

    resp = at_create_resp(APP_ESP_WIFI_RESP_SIZE, 0, 20 * RT_TICK_PER_SECOND);
    if (resp == RT_NULL)
    {
        rt_kprintf("APP esp: no memory for wifi response\r\n");
        return -RT_ENOMEM;
    }

    rt_kprintf("APP esp: join ssid=%s\r\n", ssid);
    ret = at_obj_exec_cmd(device->client, resp, "AT+CWJAP=\"%s\",\"%s\"", ssid, password);
    at_delete_resp(resp);

    if (ret != RT_EOK)
    {
        rt_kprintf("APP esp: join command ret=%d, wait async ready\r\n", ret);
    }

    wait_ret = APP_Esp8266_WaitReady(APP_ESP_JOIN_WAIT_MS);
    if (wait_ret == RT_EOK)
    {
        rt_kprintf("APP esp: join ok\r\n");
        return RT_EOK;
    }

    rt_kprintf("APP esp: join failed ret=%d wait=%d\r\n", ret, wait_ret);
    return (ret != RT_EOK) ? ret : wait_ret;
#else
    rt_kprintf("APP esp: at_device esp8266 is not enabled\r\n");
    return -RT_ENOSYS;
#endif
}

rt_err_t APP_Esp8266_JoinDefault(void)
{
#if defined(AT_DEVICE_USING_ESP8266)
    rt_err_t ret;

    ret = app_esp_net_up();
    if ((ret != RT_EOK) && (ret != -RT_ENOSYS))
    {
        rt_kprintf("APP esp: net up guard failed ret=%d\r\n", ret);
    }

    rt_kprintf("APP esp: mqtt guard AT+CWJAP=\"%s\",\"%s\"\r\n",
               APP_ESP_DEFAULT_WIFI_SSID,
               APP_ESP_DEFAULT_WIFI_PASSWORD);
    return app_esp_join_wifi(APP_ESP_DEFAULT_WIFI_SSID, APP_ESP_DEFAULT_WIFI_PASSWORD);
#else
    rt_kprintf("APP esp: default join requires AT_DEVICE_USING_ESP8266\r\n");
    return -RT_ENOSYS;
#endif
}

static rt_err_t app_esp_ping_raw(const char *host)
{
#ifdef AT_USING_CLIENT
    at_client_t client;
    at_response_t resp;
    int ret;
    rt_size_t i;

    if (host == RT_NULL)
    {
        return -RT_EINVAL;
    }

    if (app_esp_ensure_registered() != RT_EOK)
    {
        return -RT_ERROR;
    }

    client = at_client_get(APP_ESP_UART_NAME);
    if (client == RT_NULL)
    {
        rt_kprintf("APP esp: AT client %s not found\r\n", APP_ESP_UART_NAME);
        return -RT_ERROR;
    }

    resp = at_create_resp(256, 0, rt_tick_from_millisecond(5000));
    if (resp == RT_NULL)
    {
        rt_kprintf("APP esp: no memory for ping response\r\n");
        return -RT_ENOMEM;
    }

    rt_kprintf("APP esp ping: AT+PING=\"%s\"\r\n", host);
    ret = at_obj_exec_cmd(client, resp, "AT+PING=\"%s\"", host);
    if (resp->line_counts > 0U)
    {
        rt_kprintf("APP esp ping rx:\r\n");
        for (i = 1U; i <= resp->line_counts; i++)
        {
            const char *line = at_resp_get_line(resp, i);
            if (line != RT_NULL)
            {
                rt_kprintf("%s\r\n", line);
            }
        }
    }
    else
    {
        rt_kprintf("APP esp ping rx: empty\r\n");
    }

#ifdef RT_USING_NETDEV
    if (ret == RT_EOK)
    {
        struct netdev *netdev = netdev_get_by_name(APP_ESP_DEVICE_NAME);
        if (netdev != RT_NULL)
        {
            netdev_low_level_set_internet_status(netdev, RT_TRUE);
        }
    }
#endif

    if (ret != RT_EOK)
    {
        rt_kprintf("APP esp ping: failed ret=%d\r\n", ret);
    }

    at_delete_resp(resp);
    return ret;
#else
    rt_kprintf("APP esp: AT client is not enabled\r\n");
    return -RT_ENOSYS;
#endif
}





/**
 * @brief 鎶?`APP esp` 鍚庨潰鐨勫弬鏁伴噸鏂版嫾鎴愪竴鏉?AT 鍛戒护銆? */
static rt_err_t app_esp_build_cmd(int argc, char **argv, char *line, rt_size_t size)
{
    rt_size_t used = 0U;
    int i;

    if ((argc <= 0) || (argv == RT_NULL) || (line == RT_NULL) || (size == 0U))
    {
        return -RT_EINVAL;
    }

    for (i = 0; i < argc; i++)
    {
        rt_size_t len = rt_strlen(argv[i]);

        if ((used + len + 1U) >= size)
        {
            rt_kprintf("APP esp: command too long\r\n");
            return -RT_EINVAL;
        }

        if (i > 0)
        {
            line[used++] = ' ';
        }

        rt_memcpy(&line[used], argv[i], len);
        used += len;
    }

    line[used] = '\0';
    return RT_EOK;
}

#ifdef AT_USING_CLIENT
/**
 * @brief at_device 宸叉帴绠?uart3 鏃讹紝閫氳繃 AT client 鍙戦€佸懡浠わ紝閬垮厤閲嶅鎵撳紑涓插彛銆? */
static rt_err_t app_esp_send_by_at_client(const char *line)
{
    static struct at_response resp;
    static char resp_buf[APP_ESP_AT_RESP_SIZE];
    at_client_t client;
    int ret;
    rt_size_t i;

    if (app_esp_ensure_registered() != RT_EOK)
    {
        return -RT_ENOSYS;
    }

    client = at_client_get(APP_ESP_UART_NAME);
    if (client == RT_NULL)
    {
        return -RT_ENOSYS;
    }

    rt_memset(&resp, 0, sizeof(resp));
    rt_memset(resp_buf, 0, sizeof(resp_buf));
    resp.buf = resp_buf;
    resp.buf_size = sizeof(resp_buf);
    resp.line_num = 0U;
    resp.timeout = rt_tick_from_millisecond(APP_ESP_AT_TIMEOUT_MS);

    rt_kprintf("APP esp: using RT-Thread AT client on %s\r\n", APP_ESP_UART_NAME);
    rt_kprintf("APP esp tx: %s <CRLF>\r\n", line);

    ret = at_obj_exec_cmd(client, &resp, "%s", line);
    if (resp.line_counts > 0U)
    {
        rt_kprintf("APP esp rx:\r\n");
        for (i = 1U; i <= resp.line_counts; i++)
        {
            const char *text = at_resp_get_line(&resp, i);
            if (text != RT_NULL)
            {
                rt_kprintf("%s\r\n", text);
            }
        }
    }
    else
    {
        rt_kprintf("APP esp rx: empty\r\n");
    }

    if (ret != RT_EOK)
    {
        rt_kprintf("APP esp: AT client command failed ret=%d\r\n", ret);
        return ret;
    }

    return RT_EOK;
}
#endif







/**
 * @brief 鍙戦€佺敤鎴疯緭鍏ュ唴瀹癸紝鑷姩杩藉姞鐪熸鐨?CRLF锛屼笉闇€瑕佹墜鍔ㄨ緭鍏?\r\n銆? *
 * FinSH 浼氭寜绌烘牸鎷嗗垎鍙傛暟锛岃繖閲屼細鎶?`APP esp` 鍚庨潰鐨勬墍鏈夊弬鏁扮敤
 * 涓€涓┖鏍奸噸鏂版嫾鍥炰竴琛岋紝鐒跺悗鍙戠粰 ESP8266銆? */
static int app_esp_send_line(int argc, char **argv)
{
    char line[APP_ESP_TX_CMD_SIZE + 1U];
    rt_err_t ret;

    ret = app_esp_build_cmd(argc, argv, line, sizeof(line));
    if (ret != RT_EOK)
    {
        rt_kprintf("Usage: APP esp <text>\r\n");
        return ret;
    }

#ifdef AT_USING_CLIENT
    ret = app_esp_send_by_at_client(line);
    return ret;
#else
    rt_kprintf("APP esp: AT client is not enabled\r\n");
    return -RT_ENOSYS;
#endif
}

/**
 * @brief 鎵撳嵃 ESP8266 涓插彛鍙戦€佸懡浠ゅ府鍔┿€? */
static void app_esp_print_usage(void)
{
    rt_kprintf("Usage:\r\n");
    rt_kprintf("  APP esp status\r\n");
    rt_kprintf("  APP esp up\r\n");
    rt_kprintf("  APP esp join <ssid> <password>\r\n");
    rt_kprintf("  APP esp ping <host>\r\n");
    rt_kprintf("  APP esp <AT command>\r\n");
    rt_kprintf("Examples:\r\n");
    rt_kprintf("  APP esp join ESP 12345678\r\n");
    rt_kprintf("  APP esp ping 8.8.8.8\r\n");
    rt_kprintf("  APP esp AT\r\n");
    rt_kprintf("  APP esp AT+GMR\r\n");
    rt_kprintf("  APP esp AT+CIFSR\r\n");
    rt_kprintf("  APP esp AT+CWJAP=\"ssid\",\"password\"\r\n");
}

/**
 * @brief FinSH 鍛戒护鍏ュ彛锛氭妸 `APP esp` 鍚庨潰鐨勫唴瀹硅ˉ CRLF 鍚庡彂鍒?ESP8266銆? */
int APP_Esp8266_Test(int argc, char **argv)
{
    if (argc <= 1)
    {
        app_esp_print_usage();
        return RT_EOK;
    }

    if (rt_strcmp(argv[1], "status") == 0)
    {
        return app_esp_print_status();
    }

    if (rt_strcmp(argv[1], "up") == 0)
    {
        return app_esp_net_up();
    }

    if (rt_strcmp(argv[1], "join") == 0)
    {
        if (argc != 4)
        {
            rt_kprintf("Usage: APP esp join <ssid> <password>\r\n");
            return -RT_EINVAL;
        }

        return app_esp_join_wifi(argv[2], argv[3]);
    }

    if (rt_strcmp(argv[1], "ping") == 0)
    {
        if (argc != 3)
        {
            rt_kprintf("Usage: APP esp ping <host>\r\n");
            return -RT_EINVAL;
        }

        return app_esp_ping_raw(argv[2]);
    }

    return app_esp_send_line(argc - 1, argv + 1);
}


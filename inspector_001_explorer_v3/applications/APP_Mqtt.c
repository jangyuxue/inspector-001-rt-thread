#include "APP_Mqtt.h"
#include "APP_Esp8266.h"

#include <rtthread.h>
#include <string.h>

#ifdef PKG_USING_PAHOMQTT
#ifdef RT_USING_NETDEV
#include <arpa/inet.h>
#include <netdev.h>
#endif
#ifdef AT_USING_CLIENT
#include <at.h>
#endif
#include "paho_mqtt.h"
#endif

/** 默认 MQTT Broker，优先用于 MQTTX 公共测试流程。 */
#define APP_MQTT_DEFAULT_URI        "tcp://broker.emqx.io:1883"
/** 默认发布和订阅主题，MQTTX 端使用同一个 topic 即可观察消息。 */
#define APP_MQTT_DEFAULT_TOPIC      "rtt_to_atk/explorer/test"
/** 不带消息参数时发送的默认内容。 */
#define APP_MQTT_DEFAULT_MESSAGE    "hello from explorer"
/** MQTT 收发缓冲大小，保持 1 KB，和 Paho 官方示例一致。 */
#define APP_MQTT_BUFFER_SIZE        1024U
/** URI 最大长度，支持替换成 IP 形式 broker。 */
#define APP_MQTT_URI_SIZE           96U
/** Topic 最大长度，覆盖当前验证阶段需求。 */
#define APP_MQTT_TOPIC_SIZE         96U
/** 单条发布消息最大长度，FinSH 调试阶段不做大包传输。 */
#define APP_MQTT_MESSAGE_SIZE       160U
/** Client ID 最大长度，避免公共 broker 上重复冲突。 */
#define APP_MQTT_CLIENT_ID_SIZE     32U
#define APP_MQTT_NETDEV_NAME        "esp0"
#define APP_MQTT_NET_READY_MS       20000U
#define APP_MQTT_ONLINE_TIMEOUT_MS  30000U
#define APP_MQTT_ONLINE_POLL_MS     200U

#ifdef PKG_USING_PAHOMQTT
static MQTTClient app_mqtt_client;
static rt_bool_t app_mqtt_started = RT_FALSE;
static char app_mqtt_uri[APP_MQTT_URI_SIZE] = APP_MQTT_DEFAULT_URI;
static char app_mqtt_client_id[APP_MQTT_CLIENT_ID_SIZE];
static char app_mqtt_topic[APP_MQTT_TOPIC_SIZE] = APP_MQTT_DEFAULT_TOPIC;

static rt_err_t app_mqtt_copy_text(char *dst, rt_size_t dst_size, const char *src);

static rt_err_t app_mqtt_check_network(void)
{
#ifdef RT_USING_NETDEV
    struct netdev *netdev = netdev_get_by_name(APP_MQTT_NETDEV_NAME);
    const char *ip;

    if (netdev == RT_NULL)
    {
        rt_kprintf("APP mqtt: netdev %s not found\r\n", APP_MQTT_NETDEV_NAME);
        return -RT_ERROR;
    }

    ip = inet_ntoa(netdev->ip_addr);
    if (!netdev_is_up(netdev) || !netdev_is_link_up(netdev) || (rt_strcmp(ip, "0.0.0.0") == 0))
    {
        rt_kprintf("APP mqtt: network not ready up=%d link=%d ip=%s\r\n",
                   netdev_is_up(netdev),
                   netdev_is_link_up(netdev),
                   ip);
        rt_kprintf("APP mqtt: run APP esp join <ssid> <password> first\r\n");
        return -RT_ERROR;
    }
#endif

    return RT_EOK;
}

static rt_bool_t app_mqtt_ip_is_valid(const char *ip)
{
    if ((ip == RT_NULL) || (ip[0] == '\0'))
    {
        return RT_FALSE;
    }

    if ((rt_strcmp(ip, "0.0.0.0") == 0) || (rt_strlen(ip) < 7U))
    {
        return RT_FALSE;
    }

    return RT_TRUE;
}

static rt_bool_t app_mqtt_uri_is_ip(const char *uri)
{
    const char *host;
    const char *p;

    if (uri == RT_NULL)
    {
        return RT_FALSE;
    }

    host = strstr(uri, "tcp://");
    if (host != uri)
    {
        return RT_FALSE;
    }

    host += 6;
    for (p = host; (*p != '\0') && (*p != ':'); p++)
    {
        if (((*p < '0') || (*p > '9')) && (*p != '.'))
        {
            return RT_FALSE;
        }
    }

    return (p > host) ? RT_TRUE : RT_FALSE;
}

static rt_err_t app_mqtt_uri_to_host_port(const char *uri,
                                          char *host,
                                          rt_size_t host_size,
                                          char *port,
                                          rt_size_t port_size)
{
    const char *host_begin;
    const char *colon;
    rt_size_t host_len;
    rt_size_t port_len;

    if ((uri == RT_NULL) || (host == RT_NULL) || (port == RT_NULL))
    {
        return -RT_EINVAL;
    }

    if (strstr(uri, "tcp://") != uri)
    {
        return -RT_EINVAL;
    }

    host_begin = uri + 6;
    colon = strstr(host_begin, ":");
    if (colon == RT_NULL)
    {
        return -RT_EINVAL;
    }

    host_len = (rt_size_t)(colon - host_begin);
    port_len = rt_strlen(colon + 1);
    if ((host_len == 0U) || (host_len >= host_size) || (port_len == 0U) || (port_len >= port_size))
    {
        return -RT_EINVAL;
    }

    rt_memcpy(host, host_begin, host_len);
    host[host_len] = '\0';
    rt_memcpy(port, colon + 1, port_len + 1U);
    return RT_EOK;
}

static rt_err_t app_mqtt_resolve_domain(const char *host, char *ip, rt_size_t ip_size)
{
#ifdef AT_USING_CLIENT
    at_client_t client;
    at_response_t resp;
    int ret;

    if ((host == RT_NULL) || (ip == RT_NULL) || (ip_size < 16U))
    {
        return -RT_EINVAL;
    }

    client = at_client_get("uart3");
    if (client == RT_NULL)
    {
        rt_kprintf("APP mqtt: AT client uart3 not ready\r\n");
        return -RT_ERROR;
    }

    resp = at_create_resp(256, 0, rt_tick_from_millisecond(10000));
    if (resp == RT_NULL)
    {
        rt_kprintf("APP mqtt: no memory for DNS response\r\n");
        return -RT_ENOMEM;
    }

    ret = at_obj_exec_cmd(client, resp, "AT+CIPDOMAIN=\"%s\"", host);
    if (ret != RT_EOK)
    {
        rt_kprintf("APP mqtt: AT+CIPDOMAIN failed ret=%d\r\n", ret);
        at_delete_resp(resp);
        return -RT_ERROR;
    }

    /* ESP8266 AT 固件常见两种返回：+CIPDOMAIN:1.2.3.4 或 +CIPDOMAIN:"1.2.3.4"。 */
    if ((at_resp_parse_line_args_by_kw(resp, "+CIPDOMAIN:", "+CIPDOMAIN:%15s", ip) <= 0) &&
        (at_resp_parse_line_args_by_kw(resp, "+CIPDOMAIN:", "+CIPDOMAIN:\"%15[^\"]\"", ip) <= 0))
    {
        rt_kprintf("APP mqtt: parse DNS response failed\r\n");
        at_delete_resp(resp);
        return -RT_ERROR;
    }

    at_delete_resp(resp);

    if (!app_mqtt_ip_is_valid(ip))
    {
        rt_kprintf("APP mqtt: DNS %s -> %s invalid\r\n", host, ip);
        return -RT_ERROR;
    }

    rt_kprintf("APP mqtt: DNS %s -> %s\r\n", host, ip);
    return RT_EOK;
#else
    (void)host;
    (void)ip;
    (void)ip_size;
    rt_kprintf("APP mqtt: AT client is not enabled\r\n");
    return -RT_ENOSYS;
#endif
}

static rt_err_t app_mqtt_prepare_uri(void)
{
    char host[64];
    char port[8];
    char ip[16] = {0};
    char new_uri[APP_MQTT_URI_SIZE];
    rt_err_t ret;

    if (app_mqtt_uri_is_ip(app_mqtt_uri))
    {
        return RT_EOK;
    }

    ret = app_mqtt_uri_to_host_port(app_mqtt_uri, host, sizeof(host), port, sizeof(port));
    if (ret != RT_EOK)
    {
        rt_kprintf("APP mqtt: bad uri, use tcp://host:1883\r\n");
        return ret;
    }

    ret = app_mqtt_resolve_domain(host, ip, sizeof(ip));
    if (ret != RT_EOK)
    {
        rt_kprintf("APP mqtt: use APP esp AT+CIPDOMAIN=\"%s\" or start with tcp://<broker-ip>:1883\r\n", host);
        return ret;
    }

    rt_snprintf(new_uri, sizeof(new_uri), "tcp://%s:%s", ip, port);
    ret = app_mqtt_copy_text(app_mqtt_uri, sizeof(app_mqtt_uri), new_uri);
    if (ret != RT_EOK)
    {
        rt_kprintf("APP mqtt: resolved uri too long\r\n");
        return ret;
    }

    rt_kprintf("APP mqtt: use resolved uri=%s\r\n", app_mqtt_uri);
    return RT_EOK;
}

static rt_err_t app_mqtt_copy_text(char *dst, rt_size_t dst_size, const char *src)
{
    rt_size_t len;

    if ((dst == RT_NULL) || (src == RT_NULL) || (dst_size == 0U))
    {
        return -RT_EINVAL;
    }

    len = rt_strlen(src);
    if (len >= dst_size)
    {
        return -RT_EINVAL;
    }

    rt_memcpy(dst, src, len + 1U);
    return RT_EOK;
}

static rt_err_t app_mqtt_build_message(int argc, char **argv, int start, char *buf, rt_size_t size)
{
    int i;
    rt_size_t used = 0U;

    if ((buf == RT_NULL) || (size == 0U) || (start >= argc))
    {
        return -RT_EINVAL;
    }

    buf[0] = '\0';
    for (i = start; i < argc; i++)
    {
        rt_size_t len = rt_strlen(argv[i]);
        rt_size_t need = len + ((i > start) ? 1U : 0U);

        if ((used + need) >= size)
        {
            return -RT_EINVAL;
        }

        if (i > start)
        {
            buf[used++] = ' ';
        }
        rt_memcpy(&buf[used], argv[i], len);
        used += len;
        buf[used] = '\0';
    }

    return RT_EOK;
}

static void app_mqtt_print_message(MQTTClient *client, MessageData *msg_data)
{
    (void)client;

    if ((msg_data == RT_NULL) || (msg_data->topicName == RT_NULL) || (msg_data->message == RT_NULL))
    {
        return;
    }

    rt_kprintf("APP mqtt rx: topic=%.*s payload=%.*s\r\n",
               msg_data->topicName->lenstring.len,
               msg_data->topicName->lenstring.data,
               (int)msg_data->message->payloadlen,
               (const char *)msg_data->message->payload);
}

static void app_mqtt_connect_callback(MQTTClient *client)
{
    (void)client;
    rt_kprintf("APP mqtt: connect callback\r\n");
}

static void app_mqtt_online_callback(MQTTClient *client)
{
    (void)client;
    rt_kprintf("APP mqtt: online\r\n");
}

static void app_mqtt_offline_callback(MQTTClient *client)
{
    (void)client;
    rt_kprintf("APP mqtt: offline\r\n");
}

static rt_err_t app_mqtt_wait_online(rt_uint32_t timeout_ms)
{
    rt_uint32_t waited = 0U;

    while (waited < timeout_ms)
    {
        if (app_mqtt_client.isconnected)
        {
            return RT_EOK;
        }

        rt_thread_mdelay(APP_MQTT_ONLINE_POLL_MS);
        waited += APP_MQTT_ONLINE_POLL_MS;
    }

    rt_kprintf("APP mqtt: online wait timeout started=%d connected=%d\r\n",
               app_mqtt_started,
               app_mqtt_client.isconnected);
    return -RT_ETIMEOUT;
}

static void app_mqtt_print_status(void)
{
    rt_kprintf("APP mqtt: uri=%s\r\n", app_mqtt_uri);
    rt_kprintf("APP mqtt: topic=%s\r\n", app_mqtt_topic);
    rt_kprintf("APP mqtt: started=%d connected=%d\r\n",
               app_mqtt_started,
               app_mqtt_client.isconnected);
}

static rt_err_t app_mqtt_stop_client(rt_bool_t print_not_started)
{
    int ret;

    if (!app_mqtt_started)
    {
        if (print_not_started)
        {
            rt_kprintf("APP mqtt: not started\r\n");
        }
        app_mqtt_client.isconnected = 0;
        return RT_EOK;
    }

    ret = paho_mqtt_stop(&app_mqtt_client);
    rt_kprintf("APP mqtt: stop ret=%d\r\n", ret);
    app_mqtt_started = RT_FALSE;
    rt_memset(&app_mqtt_client, 0, sizeof(app_mqtt_client));
    return ret;
}

static int app_mqtt_start(int argc, char **argv)
{
    int ret;
    int timeout_ms = 10000;
    int reconnect_ms = 5000;
    int keepalive_s = 30;
    int publish_block = 1;
    MQTTPacket_connectData condata = MQTTPacket_connectData_initializer;

    ret = APP_Esp8266_JoinDefault();
    if (ret != RT_EOK)
    {
        rt_kprintf("APP mqtt: wifi guard failed ret=%d\r\n", ret);
        return ret;
    }

    ret = APP_Esp8266_WaitReady(APP_MQTT_NET_READY_MS);
    if (ret != RT_EOK)
    {
        rt_kprintf("APP mqtt: esp ready guard failed ret=%d\r\n", ret);
        return ret;
    }

    if (app_mqtt_started)
    {
        if (app_mqtt_client.isconnected)
        {
            rt_kprintf("APP mqtt: already online\r\n");
            return RT_EOK;
        }

        rt_kprintf("APP mqtt: previous client offline, restart\r\n");
        app_mqtt_stop_client(RT_FALSE);
        rt_thread_mdelay(500);
    }

    if ((argc >= 3) && (app_mqtt_copy_text(app_mqtt_uri, sizeof(app_mqtt_uri), argv[2]) != RT_EOK))
    {
        rt_kprintf("APP mqtt: uri too long\r\n");
        return -RT_EINVAL;
    }

    ret = app_mqtt_check_network();
    if (ret != RT_EOK)
    {
        return ret;
    }

    ret = app_mqtt_prepare_uri();
    if (ret != RT_EOK)
    {
        return ret;
    }

    rt_memset(&app_mqtt_client, 0, sizeof(app_mqtt_client));
    rt_snprintf(app_mqtt_client_id, sizeof(app_mqtt_client_id), "atk_%u", (unsigned int)rt_tick_get());

    app_mqtt_client.uri = app_mqtt_uri;
    rt_memcpy(&app_mqtt_client.condata, &condata, sizeof(condata));
    app_mqtt_client.condata.clientID.cstring = app_mqtt_client_id;
    app_mqtt_client.condata.keepAliveInterval = keepalive_s;
    app_mqtt_client.condata.cleansession = 1;

    app_mqtt_client.buf_size = APP_MQTT_BUFFER_SIZE;
    app_mqtt_client.readbuf_size = APP_MQTT_BUFFER_SIZE;
    app_mqtt_client.buf = rt_calloc(1, app_mqtt_client.buf_size);
    app_mqtt_client.readbuf = rt_calloc(1, app_mqtt_client.readbuf_size);
    if ((app_mqtt_client.buf == RT_NULL) || (app_mqtt_client.readbuf == RT_NULL))
    {
        if (app_mqtt_client.buf != RT_NULL)
        {
            rt_free(app_mqtt_client.buf);
        }
        if (app_mqtt_client.readbuf != RT_NULL)
        {
            rt_free(app_mqtt_client.readbuf);
        }
        rt_memset(&app_mqtt_client, 0, sizeof(app_mqtt_client));
        rt_kprintf("APP mqtt: no memory for mqtt buffer\r\n");
        return -RT_ENOMEM;
    }

    app_mqtt_client.connect_callback = app_mqtt_connect_callback;
    app_mqtt_client.online_callback = app_mqtt_online_callback;
    app_mqtt_client.offline_callback = app_mqtt_offline_callback;
    app_mqtt_client.defaultMessageHandler = app_mqtt_print_message;

    paho_mqtt_control(&app_mqtt_client, MQTT_CTRL_SET_CONN_TIMEO, &timeout_ms);
    paho_mqtt_control(&app_mqtt_client, MQTT_CTRL_SET_RECONN_INTERVAL, &reconnect_ms);
    paho_mqtt_control(&app_mqtt_client, MQTT_CTRL_SET_KEEPALIVE_INTERVAL, &keepalive_s);
    paho_mqtt_control(&app_mqtt_client, MQTT_CTRL_PUBLISH_BLOCK, &publish_block);

    rt_kprintf("APP mqtt: start uri=%s client=%s\r\n", app_mqtt_uri, app_mqtt_client_id);
    ret = paho_mqtt_start(&app_mqtt_client);
    if (ret != PAHO_SUCCESS)
    {
        rt_kprintf("APP mqtt: start failed ret=%d\r\n", ret);
        rt_free(app_mqtt_client.buf);
        rt_free(app_mqtt_client.readbuf);
        rt_memset(&app_mqtt_client, 0, sizeof(app_mqtt_client));
        return -RT_ERROR;
    }

    app_mqtt_started = RT_TRUE;
    ret = app_mqtt_wait_online(APP_MQTT_ONLINE_TIMEOUT_MS);
    if (ret != RT_EOK)
    {
        app_mqtt_stop_client(RT_FALSE);
    }
    return ret;
}

rt_err_t APP_Mqtt_Stop(void)
{
    return app_mqtt_stop_client(RT_TRUE);
}

static int app_mqtt_subscribe(int argc, char **argv)
{
    const char *topic = APP_MQTT_DEFAULT_TOPIC;
    int ret;

    if (!app_mqtt_started)
    {
        rt_kprintf("APP mqtt: start first\r\n");
        return -RT_ERROR;
    }

    if (argc >= 3)
    {
        topic = argv[2];
    }

    if (app_mqtt_copy_text(app_mqtt_topic, sizeof(app_mqtt_topic), topic) != RT_EOK)
    {
        rt_kprintf("APP mqtt: topic too long\r\n");
        return -RT_EINVAL;
    }

    ret = paho_mqtt_subscribe(&app_mqtt_client, QOS1, app_mqtt_topic, app_mqtt_print_message);
    rt_kprintf("APP mqtt: subscribe topic=%s ret=%d\r\n", app_mqtt_topic, ret);
    return ret;
}

static int app_mqtt_publish(int argc, char **argv)
{
    const char *topic = APP_MQTT_DEFAULT_TOPIC;
    const char *message = APP_MQTT_DEFAULT_MESSAGE;
    char message_buf[APP_MQTT_MESSAGE_SIZE];
    int ret;

    if (!app_mqtt_started)
    {
        rt_kprintf("APP mqtt: start first\r\n");
        return -RT_ERROR;
    }

    if (argc >= 3)
    {
        topic = argv[2];
    }
    if (argc >= 4)
    {
        ret = app_mqtt_build_message(argc, argv, 3, message_buf, sizeof(message_buf));
        if (ret != RT_EOK)
        {
            rt_kprintf("APP mqtt: message too long\r\n");
            return ret;
        }
        message = message_buf;
    }

    ret = paho_mqtt_publish(&app_mqtt_client, QOS1, topic, message);
    rt_kprintf("APP mqtt: publish topic=%s ret=%d\r\n", topic, ret);
    return ret;
}

rt_err_t APP_Mqtt_StartDefault(void)
{
    char *argv[] = {"mqtt", "start"};

    return app_mqtt_start(2, argv);
}

rt_err_t APP_Mqtt_PublishText(const char *topic, const char *message)
{
    int ret;

    if ((topic == RT_NULL) || (message == RT_NULL))
    {
        return -RT_EINVAL;
    }
    if (!app_mqtt_started || !app_mqtt_client.isconnected)
    {
        rt_kprintf("APP mqtt: publish blocked, client offline\r\n");
        return -RT_ERROR;
    }

    ret = paho_mqtt_publish(&app_mqtt_client, QOS1, topic, message);
    rt_kprintf("APP mqtt: publish topic=%s ret=%d\r\n", topic, ret);
    return ret;
}

rt_bool_t APP_Mqtt_IsConnected(void)
{
    return (app_mqtt_started && app_mqtt_client.isconnected) ? RT_TRUE : RT_FALSE;
}
#else
rt_err_t APP_Mqtt_StartDefault(void)
{
    return -RT_ENOSYS;
}

rt_err_t APP_Mqtt_PublishText(const char *topic, const char *message)
{
    (void)topic;
    (void)message;
    return -RT_ENOSYS;
}

rt_bool_t APP_Mqtt_IsConnected(void)
{
    return RT_FALSE;
}
#endif

static void app_mqtt_print_usage(void)
{
    rt_kprintf("Usage:\r\n");
    rt_kprintf("  APP mqtt status\r\n");
    rt_kprintf("  APP mqtt start [tcp://host:1883]\r\n");
    rt_kprintf("  APP mqtt sub [topic]\r\n");
    rt_kprintf("  APP mqtt pub [topic] [message]\r\n");
    rt_kprintf("  APP mqtt stop\r\n");
    rt_kprintf("Examples:\r\n");
    rt_kprintf("  APP mqtt start\r\n");
    rt_kprintf("  APP mqtt sub rtt_to_atk/explorer/test\r\n");
    rt_kprintf("  APP mqtt pub rtt_to_atk/explorer/test hello\r\n");
}

int APP_Mqtt_Test(int argc, char **argv)
{
#ifndef PKG_USING_PAHOMQTT
    (void)argc;
    (void)argv;
    rt_kprintf("APP mqtt: Paho MQTT package is not enabled\r\n");
    return -RT_ENOSYS;
#else
    if (argc <= 1)
    {
        app_mqtt_print_usage();
        return RT_EOK;
    }

    if (rt_strcmp(argv[1], "status") == 0)
    {
        app_mqtt_print_status();
        return RT_EOK;
    }
    if (rt_strcmp(argv[1], "start") == 0)
    {
        return app_mqtt_start(argc, argv);
    }
    if (rt_strcmp(argv[1], "sub") == 0)
    {
        return app_mqtt_subscribe(argc, argv);
    }
    if (rt_strcmp(argv[1], "pub") == 0)
    {
        return app_mqtt_publish(argc, argv);
    }
    if (rt_strcmp(argv[1], "stop") == 0)
    {
        return APP_Mqtt_Stop();
    }

    app_mqtt_print_usage();
    return -RT_EINVAL;
#endif
}

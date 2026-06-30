#include <rtthread.h>
#include <rthw.h>
#include <rtdevice.h>
#include <board.h>

#include "APP_Touch.h"

/* GT9xxx 触摸坐标范围，和 4.3 寸 LCD 的竖屏显示方向保持一致。 */
#define APP_GT9XXX_WIDTH              480U
#define APP_GT9XXX_HEIGHT             800U

/* GT9xxx 8 位 I2C 地址，写地址 0x28，读地址 0x29。 */
#define APP_GT9XXX_ADDR_WR            0x28U
#define APP_GT9XXX_ADDR_RD            0x29U

/* GT9xxx 常用寄存器地址。 */
#define APP_GT9XXX_CTRL_REG           0x8040U  /**< 控制寄存器。 */
#define APP_GT9XXX_PID_REG            0x8140U  /**< 产品 ID 起始寄存器。 */
#define APP_GT9XXX_GSTID_REG          0x814EU  /**< 触摸状态寄存器。 */
#define APP_GT9XXX_TP1_REG            0x8150U  /**< 第 1 个触点坐标起始寄存器。 */

/* 4.3 寸 LCD 排线上 GT9xxx 已固定连接到以下 GPIO。 */
#define APP_GT9XXX_SCL_PORT           GPIOB
#define APP_GT9XXX_SCL_PIN            GPIO_PIN_0
#define APP_GT9XXX_SDA_PORT           GPIOF
#define APP_GT9XXX_SDA_PIN            GPIO_PIN_11
#define APP_GT9XXX_RST_PORT           GPIOC
#define APP_GT9XXX_RST_PIN            GPIO_PIN_13
#define APP_GT9XXX_INT_PORT           GPIOB
#define APP_GT9XXX_INT_PIN            GPIO_PIN_1
#define APP_GT9XXX_INT_RT_PIN         GET_PIN(B, 1)  /**< RT-Thread PIN 设备使用的中断引脚编号。 */

static uint8_t app_touch_ready;
static uint8_t app_touch_max_points = 5;
static char app_touch_pid[5];
static uint8_t app_touch_was_pressed;
static uint16_t app_touch_last_x;
static uint16_t app_touch_last_y;
static struct rt_semaphore app_touch_irq_sem;
static struct rt_mutex app_touch_bus_lock;
static uint8_t app_touch_ipc_ready;

static void app_touch_map_lcd_point(uint16_t *x, uint16_t *y)
{
    if ((x == RT_NULL) || (y == RT_NULL))
    {
        return;
    }

    *x = (uint16_t)(APP_GT9XXX_WIDTH - 1U - *x);
    *y = (uint16_t)(APP_GT9XXX_HEIGHT - 1U - *y);
}

static void app_touch_i2c_delay(void)
{
    rt_hw_us_delay(2);
}

static void app_touch_scl(uint8_t level)
{
    HAL_GPIO_WritePin(APP_GT9XXX_SCL_PORT,
                      APP_GT9XXX_SCL_PIN,
                      level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void app_touch_sda(uint8_t level)
{
    HAL_GPIO_WritePin(APP_GT9XXX_SDA_PORT,
                      APP_GT9XXX_SDA_PIN,
                      level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static uint8_t app_touch_read_sda(void)
{
    return HAL_GPIO_ReadPin(APP_GT9XXX_SDA_PORT, APP_GT9XXX_SDA_PIN) == GPIO_PIN_SET;
}

static void app_touch_rst(uint8_t level)
{
    HAL_GPIO_WritePin(APP_GT9XXX_RST_PORT,
                      APP_GT9XXX_RST_PIN,
                      level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void app_touch_irq_callback(void *args)
{
    (void)args;

    if (app_touch_ipc_ready)
    {
        rt_sem_release(&app_touch_irq_sem);
    }
}

static int app_touch_ipc_init(void)
{
    rt_err_t ret;

    if (app_touch_ipc_ready)
    {
        return RT_EOK;
    }

    ret = rt_sem_init(&app_touch_irq_sem, "tp_irq", 0, RT_IPC_FLAG_FIFO);
    if (ret != RT_EOK)
    {
        return ret;
    }

    ret = rt_mutex_init(&app_touch_bus_lock, "tp_bus", RT_IPC_FLAG_PRIO);
    if (ret != RT_EOK)
    {
        rt_sem_detach(&app_touch_irq_sem);
        return ret;
    }

    app_touch_ipc_ready = 1U;
    return RT_EOK;
}

static void app_touch_i2c_start(void)
{
    app_touch_sda(1);
    app_touch_scl(1);
    app_touch_i2c_delay();
    app_touch_sda(0);
    app_touch_i2c_delay();
    app_touch_scl(0);
    app_touch_i2c_delay();
}

static void app_touch_i2c_stop(void)
{
    app_touch_sda(0);
    app_touch_i2c_delay();
    app_touch_scl(1);
    app_touch_i2c_delay();
    app_touch_sda(1);
    app_touch_i2c_delay();
}

static uint8_t app_touch_i2c_wait_ack(void)
{
    uint16_t wait = 0;

    app_touch_sda(1);
    app_touch_i2c_delay();
    app_touch_scl(1);
    app_touch_i2c_delay();

    while (app_touch_read_sda())
    {
        wait++;
        if (wait > 250U)
        {
            app_touch_i2c_stop();
            return 1;
        }
        app_touch_i2c_delay();
    }

    app_touch_scl(0);
    app_touch_i2c_delay();
    return 0;
}

static void app_touch_i2c_ack(void)
{
    app_touch_sda(0);
    app_touch_i2c_delay();
    app_touch_scl(1);
    app_touch_i2c_delay();
    app_touch_scl(0);
    app_touch_i2c_delay();
    app_touch_sda(1);
    app_touch_i2c_delay();
}

static void app_touch_i2c_nack(void)
{
    app_touch_sda(1);
    app_touch_i2c_delay();
    app_touch_scl(1);
    app_touch_i2c_delay();
    app_touch_scl(0);
    app_touch_i2c_delay();
}

static void app_touch_i2c_send_byte(uint8_t data)
{
    uint8_t i;

    for (i = 0; i < 8U; i++)
    {
        app_touch_sda((data & 0x80U) ? 1U : 0U);
        app_touch_i2c_delay();
        app_touch_scl(1);
        app_touch_i2c_delay();
        app_touch_scl(0);
        data <<= 1;
    }

    app_touch_sda(1);
}

static uint8_t app_touch_i2c_read_byte(uint8_t ack)
{
    uint8_t i;
    uint8_t data = 0;

    for (i = 0; i < 8U; i++)
    {
        data <<= 1;
        app_touch_scl(1);
        app_touch_i2c_delay();
        if (app_touch_read_sda())
        {
            data++;
        }
        app_touch_scl(0);
        app_touch_i2c_delay();
    }

    if (ack)
    {
        app_touch_i2c_ack();
    }
    else
    {
        app_touch_i2c_nack();
    }

    return data;
}

static void app_touch_gpio_init(void)
{
    GPIO_InitTypeDef gpio;

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();

    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

    gpio.Pin = APP_GT9XXX_SCL_PIN;
    HAL_GPIO_Init(APP_GT9XXX_SCL_PORT, &gpio);

    gpio.Pin = APP_GT9XXX_SDA_PIN;
    HAL_GPIO_Init(APP_GT9XXX_SDA_PORT, &gpio);

    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Pin = APP_GT9XXX_RST_PIN;
    HAL_GPIO_Init(APP_GT9XXX_RST_PORT, &gpio);

    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    gpio.Pin = APP_GT9XXX_INT_PIN;
    HAL_GPIO_Init(APP_GT9XXX_INT_PORT, &gpio);

    app_touch_scl(1);
    app_touch_sda(1);
}

static int app_touch_irq_init(void)
{
    rt_err_t ret;

    rt_pin_mode(APP_GT9XXX_INT_RT_PIN, PIN_MODE_INPUT_PULLUP);

    ret = rt_pin_attach_irq(APP_GT9XXX_INT_RT_PIN,
                            PIN_IRQ_MODE_FALLING,
                            app_touch_irq_callback,
                            RT_NULL);
    if (ret != RT_EOK && ret != -RT_EBUSY)
    {
        rt_kprintf("APP touch: attach INT irq failed, ret=%d\r\n", ret);
        return ret;
    }

    ret = rt_pin_irq_enable(APP_GT9XXX_INT_RT_PIN, PIN_IRQ_ENABLE);
    if (ret != RT_EOK)
    {
        rt_kprintf("APP touch: enable INT irq failed, ret=%d\r\n", ret);
        return ret;
    }

    return RT_EOK;
}

static uint8_t app_touch_write_reg(uint16_t reg, const uint8_t *buf, uint8_t len)
{
    uint8_t i;
    uint8_t ret = 0;

    app_touch_i2c_start();
    app_touch_i2c_send_byte(APP_GT9XXX_ADDR_WR);
    ret |= app_touch_i2c_wait_ack();
    app_touch_i2c_send_byte((uint8_t)(reg >> 8));
    ret |= app_touch_i2c_wait_ack();
    app_touch_i2c_send_byte((uint8_t)(reg & 0xFFU));
    ret |= app_touch_i2c_wait_ack();

    for (i = 0; i < len; i++)
    {
        app_touch_i2c_send_byte(buf[i]);
        ret |= app_touch_i2c_wait_ack();
        if (ret)
        {
            break;
        }
    }

    app_touch_i2c_stop();
    return ret ? 1U : 0U;
}

static uint8_t app_touch_read_reg(uint16_t reg, uint8_t *buf, uint8_t len)
{
    uint8_t i;
    uint8_t ret = 0;

    app_touch_i2c_start();
    app_touch_i2c_send_byte(APP_GT9XXX_ADDR_WR);
    ret |= app_touch_i2c_wait_ack();
    app_touch_i2c_send_byte((uint8_t)(reg >> 8));
    ret |= app_touch_i2c_wait_ack();
    app_touch_i2c_send_byte((uint8_t)(reg & 0xFFU));
    ret |= app_touch_i2c_wait_ack();
    app_touch_i2c_start();
    app_touch_i2c_send_byte(APP_GT9XXX_ADDR_RD);
    ret |= app_touch_i2c_wait_ack();

    if (ret)
    {
        app_touch_i2c_stop();
        return 1;
    }

    for (i = 0; i < len; i++)
    {
        buf[i] = app_touch_i2c_read_byte(i == (uint8_t)(len - 1U) ? 0U : 1U);
    }

    app_touch_i2c_stop();
    return 0;
}

static uint8_t app_touch_pid_supported(const char *pid)
{
    return (rt_strcmp(pid, "911") == 0) ||
           (rt_strcmp(pid, "9147") == 0) ||
           (rt_strcmp(pid, "917S") == 0) ||
           (rt_strcmp(pid, "968") == 0) ||
           (rt_strcmp(pid, "1151") == 0) ||
           (rt_strcmp(pid, "1158") == 0) ||
           (rt_strcmp(pid, "9271") == 0);
}

int APP_Touch_Init(void)
{
    uint8_t ctrl;
    int ret;

    if (app_touch_ready)
    {
        return RT_EOK;
    }

    ret = app_touch_ipc_init();
    if (ret != RT_EOK)
    {
        rt_kprintf("APP touch: ipc init failed, ret=%d\r\n", ret);
        return ret;
    }

    app_touch_gpio_init();
    app_touch_rst(0);
    rt_thread_mdelay(10);
    app_touch_rst(1);
    rt_thread_mdelay(100);

    if (app_touch_read_reg(APP_GT9XXX_PID_REG, (uint8_t *)app_touch_pid, 4) != 0)
    {
        rt_kprintf("APP touch: GT9xxx i2c read id failed\r\n");
        return -RT_ERROR;
    }

    app_touch_pid[4] = '\0';
    if (!app_touch_pid_supported(app_touch_pid))
    {
        rt_kprintf("APP touch: unsupported GT9xxx id '%s'\r\n", app_touch_pid);
        return -RT_ERROR;
    }

    app_touch_max_points = (rt_strcmp(app_touch_pid, "9271") == 0) ? 10U : 5U;

    ctrl = 0x02U;
    if (app_touch_write_reg(APP_GT9XXX_CTRL_REG, &ctrl, 1) != 0)
    {
        return -RT_ERROR;
    }
    rt_thread_mdelay(10);

    ctrl = 0x00U;
    if (app_touch_write_reg(APP_GT9XXX_CTRL_REG, &ctrl, 1) != 0)
    {
        return -RT_ERROR;
    }

    ret = app_touch_irq_init();
    if (ret != RT_EOK)
    {
        return ret;
    }

    app_touch_ready = 1;
    rt_kprintf("APP touch: GT9xxx id=%s, max_points=%d\r\n", app_touch_pid, app_touch_max_points);
    return RT_EOK;
}

void APP_Touch_ResetState(void)
{
    uint8_t clear = 0;

    if (APP_Touch_Init() != RT_EOK)
    {
        return;
    }

    rt_mutex_take(&app_touch_bus_lock, RT_WAITING_FOREVER);
    (void)app_touch_write_reg(APP_GT9XXX_GSTID_REG, &clear, 1);
    app_touch_was_pressed = 0U;
    app_touch_last_x = 0U;
    app_touch_last_y = 0U;
    while (rt_sem_take(&app_touch_irq_sem, 0) == RT_EOK)
    {
    }
    rt_mutex_release(&app_touch_bus_lock);
}

int APP_Touch_Read(APP_TouchState *state)
{
    uint8_t status;
    uint8_t point_count;
    uint8_t clear = 0;
    uint8_t i;
    uint8_t buf[4];
    int ret = RT_EOK;

    if (state == RT_NULL)
    {
        return -RT_EINVAL;
    }

    rt_memset(state, 0, sizeof(*state));

    if (APP_Touch_Init() != RT_EOK)
    {
        return -RT_ERROR;
    }

    rt_mutex_take(&app_touch_bus_lock, RT_WAITING_FOREVER);

    if (app_touch_read_reg(APP_GT9XXX_GSTID_REG, &status, 1) != 0)
    {
        ret = -RT_ERROR;
        goto out;
    }

    if ((status & 0x80U) == 0U)
    {
        state->pressed = app_touch_was_pressed;
        goto out;
    }

    point_count = status & 0x0FU;
    if (point_count == 0U)
    {
        app_touch_write_reg(APP_GT9XXX_GSTID_REG, &clear, 1);
        if (app_touch_was_pressed)
        {
            state->released = 1U;
            state->changed = 1U;
        }
        app_touch_was_pressed = 0U;
        goto out;
    }

    if (point_count > app_touch_max_points || point_count > APP_TOUCH_MAX_POINTS)
    {
        app_touch_write_reg(APP_GT9XXX_GSTID_REG, &clear, 1);
        ret = -RT_ERROR;
        goto out;
    }

    state->count = point_count;

    for (i = 0; i < point_count; i++)
    {
        if (app_touch_read_reg((uint16_t)(APP_GT9XXX_TP1_REG + i * 8U), buf, sizeof(buf)) != 0)
        {
            app_touch_write_reg(APP_GT9XXX_GSTID_REG, &clear, 1);
            ret = -RT_ERROR;
            goto out;
        }

        state->x[i] = ((uint16_t)buf[1] << 8) | buf[0];
        state->y[i] = ((uint16_t)buf[3] << 8) | buf[2];

        if (state->x[i] >= APP_GT9XXX_WIDTH || state->y[i] >= APP_GT9XXX_HEIGHT)
        {
            if ((i == 0U) && (point_count > 1U))
            {
                state->x[0] = state->x[1];
                state->y[0] = state->y[1];
            }
            else if ((i == 0U) && app_touch_was_pressed)
            {
                state->x[0] = app_touch_last_x;
                state->y[0] = app_touch_last_y;
            }
            else
            {
                state->count = 0U;
                app_touch_write_reg(APP_GT9XXX_GSTID_REG, &clear, 1);
                goto out;
            }
        }

        app_touch_map_lcd_point(&state->x[i], &state->y[i]);
    }

    app_touch_write_reg(APP_GT9XXX_GSTID_REG, &clear, 1);
    state->pressed = 1U;
    state->changed = (!app_touch_was_pressed ||
                      state->x[0] != app_touch_last_x ||
                      state->y[0] != app_touch_last_y) ? 1U : 0U;
    app_touch_was_pressed = 1U;
    app_touch_last_x = state->x[0];
    app_touch_last_y = state->y[0];

out:
    rt_mutex_release(&app_touch_bus_lock);
    return ret;
}

int APP_Touch_WaitEvent(int32_t timeout_ms)
{
    rt_int32_t timeout_tick;
    rt_err_t ret;

    if (APP_Touch_Init() != RT_EOK)
    {
        return -RT_ERROR;
    }

    if (timeout_ms < 0)
    {
        timeout_tick = RT_WAITING_FOREVER;
    }
    else
    {
        timeout_tick = (rt_int32_t)rt_tick_from_millisecond((rt_uint32_t)timeout_ms);
        if (timeout_tick <= 0)
        {
            timeout_tick = 1;
        }
    }

    ret = rt_sem_take(&app_touch_irq_sem, timeout_tick);
    return ret == RT_EOK ? RT_EOK : -RT_ETIMEOUT;
}

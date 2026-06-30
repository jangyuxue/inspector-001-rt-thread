/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-11-06     SummerGift   first version
 * 2018-11-19     flybreak     add stm32f407-atk-explorer bsp
 */

#include <rtthread.h>
#include <board.h>

/** 板载 LED0，探索者 F407 V3 固定连接到 PF9。 */
#define APP_LED0_PORT    GPIOF
#define APP_LED0_PIN     GPIO_PIN_9

static void app_led0_init(void)
{
    GPIO_InitTypeDef gpio;

    __HAL_RCC_GPIOF_CLK_ENABLE();

    gpio.Pin = APP_LED0_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(APP_LED0_PORT, &gpio);
}

int main(void)
{
    app_led0_init();

    while (1)
    {
        HAL_GPIO_WritePin(APP_LED0_PORT, APP_LED0_PIN, GPIO_PIN_SET);
        rt_thread_mdelay(500);
        HAL_GPIO_WritePin(APP_LED0_PORT, APP_LED0_PIN, GPIO_PIN_RESET);
        rt_thread_mdelay(500);
    }
}

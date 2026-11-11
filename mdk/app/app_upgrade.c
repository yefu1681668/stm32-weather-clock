/**
 ******************************************************************************
 * @file    app_upgrade.c
 * @brief   APP 侧的"请求升级"入口（OTA 第②步）
 *
 * 这个函数里唯一有点危险的动作是擦写内部 Flash 参数页。之所以敢在
 * 运行中的 FreeRTOS 里直接做，是因为写完马上 NVIC_SystemReset()：
 * 挂起调度器 → 擦页 → 写 32 B → 复位，全程几十毫秒，没有任务切换，
 * 也不会有别的任务同时去碰 FLASH 控制器。
 *
 * 注意：这里用 vTaskSuspendAll() 而不是 __disable_irq()。APP 的 HAL 时基
 * 是 TIM7 中断，HAL_FLASHEx_Erase() 内部靠 HAL_GetTick() 判断超时；
 * 关总中断会把这个时基冻住，反而可能误判成擦除超时。
 ******************************************************************************
 */
#include "app_upgrade.h"
#include "boot_cfg.h"
#include "boot_param.h"
#include "stm32f1xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

/* 只由中断写、只由 heartbeat_task 读并清零，所以用 volatile 就够了。
 * 单字节写入在 Cortex-M3 上本身就是原子的，不需要临界区。 */
static volatile uint8_t g_upgrade_flag = 0;

void app_upgrade_init(void)
{
    /* 上电到这里的几十毫秒里，串口线上可能落了噪声字节（尤其 CH340
     * 插拔瞬间）。先读 SR、再读 DR 把它们清干净，避免一开机就误触发。 */
    (void)USART1->SR;
    (void)USART1->DR;

    USART1->CR1 |= USART_CR1_RXNEIE;        /* 只开"收到一个字节"中断 */

    /* 优先级给 5：比外设 DMA/ESP32 串口高一点，保证命令不丢。
     * ISR 里不调 RTOS API，所以这个数值不违反 FreeRTOS 的中断约束。 */
    HAL_NVIC_SetPriority(USART1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
}

void app_upgrade_rx_isr(void)
{
    /* F1 的规定顺序：先读 SR 再读 DR 才能清掉 RXNE / ORE / NE / FE。
     * 就算只有错误标志没有数据，也必须读一次 DR，否则中断会一直重进。 */
    uint32_t sr = USART1->SR;
    uint8_t  ch = (uint8_t)(USART1->DR & 0xFF);

    if ((sr & USART_SR_RXNE) &&
        (ch == BOOT_CMD_CHAR || ch == (BOOT_CMD_CHAR + 0x20)))
    {
        g_upgrade_flag = 1;                 /* 只置标志，具体动作交给任务 */
    }
}

/** 写参数页并复位。调用者已经关中断或在任务上下文里，不再重复保护。 */
static void app_request_upgrade(void)
{
    boot_param_t p;

    printf("\r\n[OTA] upgrade requested -> reboot to BootLoader\r\n");
    while ((USART1->SR & USART_SR_TC) == 0)
        ;                                    /* 等最后一个字节真正发完 */

    /* 读当前账本再改一个字段：app_size / app_crc32 / boot_try 都原样保留，
     * 不能 memset 成 0 —— 那些是第③步升级流程要用的。 */
    boot_param_read(&p);
    p.upgrade_req = 1;

    /* 挂起调度器（而不是关中断）：其他任务不会来抢 FLASH 控制器，
     * 但 TIM7 的 HAL 时基中断仍在跑 —— HAL_FLASHEx_Erase / HAL_FLASH_Program
     * 内部要用 HAL_GetTick() 做超时判断，关中断会让这个时钟冻住。 */
    vTaskSuspendAll();

    if (!boot_param_write(&p))
    {
        xTaskResumeAll();
        printf("[OTA] ERROR: param page write failed, stay in APP\r\n");
        return;                              /* 写失败就不复位，还能正常用 */
    }

    xTaskResumeAll();

    /* 参数页擦写已完成，复位。此后 Boot 会读到 upgrade_req=1。 */
    NVIC_SystemReset();
    while (1)
        ;                                    /* 复位需要一点时间，喂狗式死等 */
}

void app_upgrade_poll(void)
{
    if (!g_upgrade_flag)
        return;

    g_upgrade_flag = 0;
    app_request_upgrade();
}

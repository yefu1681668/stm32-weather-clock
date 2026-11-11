#ifndef __APP_UPGRADE_H__
#define __APP_UPGRADE_H__

/* ==================================================================
 * OTA 第②步：让 APP 能"主动请求升级"
 *
 * 流程：串口助手发字符 'U'（大小写都认）
 *   → USART1 接收中断置标志
 *   → heartbeat_task 看到标志，往参数页写 upgrade_req=1
 *   → NVIC_SystemReset() 复位
 *   → Boot 上电读到标志，清掉它并停在升级等待窗口
 *
 * 为什么用参数页而不是 RAM 变量：复位会把 RAM 清空，只有 Flash 里的
 * 标志能跨过一次复位传话。这也是 Boot 和 APP 之间唯一的"信箱"。
 *
 * 参数页格式直接复用 boot/boot_param.h，两边共用同一份结构体和 CRC，
 * 不会出现"APP 写了 A 格式、Boot 按 B 格式读"的经典错位。
 * ================================================================== */

/** 在 freertos_start() 开头调用：打开 USART1 接收中断。
 *  PA9/PA10 与 115200 已由 MX_USART1_UART_Init() 配好，这里只补中断。 */
void app_upgrade_init(void);

/** 由 Core/Src/stm32f1xx_it.c 的 USART1_IRQHandler 调用。
 *  只做"读一个字符 + 置标志"，绝不调用任何 FreeRTOS API，
 *  所以不受 configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 约束。 */
void app_upgrade_rx_isr(void);

/** 由 heartbeat_task 每 50 ms 调用一次；检测到请求就写参数页并复位。
 *  正常运行时只是一次 volatile 判断，开销可以忽略。 */
void app_upgrade_poll(void);

#endif /* __APP_UPGRADE_H__ */

#ifndef __DELAY_H__
#define __DELAY_H__

#include <stdint.h>

/* ==================================================================
 * Boot 专用的 delay 适配层
 *
 * mdk/driver/w25q/w25q.c 里 include 了 "delay.h"，那原本是 APP 工程
 * mdk/app/delay.c 提供的 DWT 周期计数版延时。Boot 工程没有链那个文件，
 * 但为了「W25Q 驱动一行都不改、两个工程共用同一份源码」，这里按同名
 * 接口补一个实现：delay_ms 直接借用 HAL_Delay（Boot 已实现 SysTick_Handler）。
 *
 * 注意：Boot 工程的 IncludePath 里没有 ..\mdk\app，所以编译器找到的
 * 一定是本文件，不会和 APP 的 delay.h 撞车。
 * ================================================================== */

void delay_init(void);
void delay_us(uint32_t us);
void delay_ms(uint32_t ms);

#endif /* __DELAY_H__ */

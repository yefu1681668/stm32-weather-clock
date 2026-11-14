#include "delay.h"
#include "stm32f1xx_hal.h"

void delay_init(void)
{
    /* Boot 不用 DWT；SysTick 已由 HAL_Init() 配成 1 kHz，直接可用。 */
}

void delay_us(uint32_t us)
{
    /* W25Q 驱动只调用 delay_ms()，这里给一个粗略实现备用。
     * 72 MHz 下每次空循环约 6~8 个周期，取 12 次/微秒偏保守。 */
    volatile uint32_t n = us * 12u;
    while (n--)
    {
        __NOP();
    }
}

void delay_ms(uint32_t ms)
{
    HAL_Delay(ms);
}

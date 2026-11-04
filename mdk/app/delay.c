#include "delay.h"
#include "stm32f1xx_hal.h"


/* DWT(Data Watchpoint and Trace)周期计数器实现us级阻塞延时
 * 在任何上下文(任务/ISR/调度器启动前)均可使用
 * 系统时钟72MHz -> 1us = 72个周期 */
#define DWT_CYCCNT  (*(volatile uint32_t *)0xE0001004)
#define DWT_CTRL     (*(volatile uint32_t *)0xE0001000)
#define DEM_CR       (*(volatile uint32_t *)0xE000EDFC)
#define DEM_CR_TRCENA (1 << 24)
#define DWT_CTRL_CYCCNTENA (1 << 0)


static uint32_t cycles_per_us;


void delay_init(void)
{
    cycles_per_us = SystemCoreClock / 1000000;
    DEM_CR |= DEM_CR_TRCENA;
    DWT_CYCCNT = 0;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;
}


void delay_us(uint32_t us)
{
    uint32_t start = DWT_CYCCNT;
    uint32_t total = us * cycles_per_us;
    while ((DWT_CYCCNT - start) < total)
    {
    }
}


void delay_ms(uint32_t ms)
{
    while (ms--)
    {
        delay_us(1000);
    }
}

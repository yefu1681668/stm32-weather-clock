#include "boot_jump.h"
#include "boot_cfg.h"
#include "stm32f1xx_hal.h"

typedef void (*app_entry_t)(void);

/* Boot 已经用过、跳转前必须还回去的外设中断：这里不逐个关，
 * 统一把 NVIC 的使能位和挂起位全清 —— APP 会重新配置自己需要的中断。 */
static void boot_nvic_clear_all(void)
{
    for (uint32_t i = 0; i < 8; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFu;    /* 关掉所有外部中断 */
        NVIC->ICPR[i] = 0xFFFFFFFFu;    /* 清掉所有挂起标志 */
    }
}

int boot_app_check(uint32_t app_addr)
{
    uint32_t sp = *(volatile uint32_t *)app_addr;
    uint32_t pc = *(volatile uint32_t *)(app_addr + 4u);

    if (sp < RAM_START_ADDR || sp > RAM_END_ADDR)
        return 0;                        /* 栈顶不在 RAM：多半是空片 0xFFFFFFFF */

    if (pc < app_addr || pc >= APP_END_ADDR)
        return 0;                        /* 复位向量不在 APP 区 */

    if ((pc & 1u) == 0)
        return 0;                        /* Thumb 地址最低位必须是 1 */

    return 1;
}

int boot_jump_to_app(uint32_t app_addr)
{
    app_entry_t entry;

    if (!boot_app_check(app_addr))
        return 0;

    entry = (app_entry_t)(*(volatile uint32_t *)(app_addr + 4u));

    /* ① 关总中断：此时任何中断进来都会打到"半成品"状态 */
    __disable_irq();

    /* ② 停掉 Boot 用的 SysTick，否则跳过去后它还在按 Boot 的重装值计数 */
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    /* ③ 时钟复位回 HSI 8 MHz。这样 APP 的 SystemClock_Config() 面对的是
     *    和"冷启动"完全一样的状态，不会出现"PLL 已经在跑、再配一次报错"。 */
    HAL_RCC_DeInit();

    /* ④ 清空 NVIC，把 Boot 用过的串口/LED 中断痕迹抹掉 */
    boot_nvic_clear_all();

    /* ⑤ 向量表搬过去。这一步最容易漏：MSP 和 PC 都换了，但中断向量表
     *    还指着 Boot 的表，一进中断就会执行到 Boot 的处理函数上（通常直接跑飞）。
     *    SCB->VTOR 必须写 APP 的起始地址。 */
    SCB->VTOR = app_addr;

    /* ⑥ 先设 MSP，再跳。顺序不能反：MSP 一旦设成 APP 的栈顶，
     *    Boot 自己的局部变量就失效了，所以这之后不能再调用任何 Boot 的函数。 */
    __set_MSP(*(volatile uint32_t *)app_addr);

    __enable_irq();

    entry();                             /* 进入 APP 的 Reset_Handler */

    return 1;                            /* 正常情况走不到这里 */
}

uint32_t boot_app_measure_size(uint32_t app_addr, uint32_t max_size)
{
    const uint8_t *p = (const uint8_t *)app_addr;
    uint32_t last = 0;

    for (uint32_t i = 0; i < max_size; i++)
    {
        if (p[i] != 0xFF)
            last = i + 1;
    }
    return last;
}

/**
 ******************************************************************************
 * @file    boot_main.c
 * @brief   OTA BootLoader 主程序（第①步：能跳、能记录状态）
 *
 * 上电后的完整动作：
 *   初始化时钟/串口/LED → 读参数页 → 检查 APP 是否可跳
 *   → 留 3 秒窗口等升级请求 → 无请求且 APP 正常就跳过去
 *
 * 烧录设置（务必确认，否则会把自己锁死）：
 *   IROM1 起始 0x08000000，大小 0x4000（16 KB）
 *   Flash Download 勾 "Erase Sectors"，**绝不能勾 Erase Full Chip**
 ******************************************************************************
 */
#include "boot_cfg.h"
#include "boot_param.h"
#include "boot_jump.h"
#include "stm32f1xx_hal.h"
#include <string.h>

static boot_param_t g_param;

/* ---------------- 串口日志（直接操作 USART1 寄存器） ----------------
 * 为什么不用 HAL_UART：这个工程（和 APP 一样）没有打开 HAL_UART 模块，
 * Boot 只需要"发几个字符"，用寄存器 20 行就够了，还能省 1~2 KB ROM。 */
static void boot_uart_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    gpio.Pin   = GPIO_PIN_9;                /* TX */
    gpio.Mode  = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin  = GPIO_PIN_10;                /* RX（第③步收固件要用） */
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* 72 MHz / 115200 / 16 倍采样 = 39.0625 → BRR = 0x271 */
    USART1->BRR = 0x271;
    USART1->CR2 = 0;
    USART1->CR3 = 0;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;   /* 8N1 */
}

static void boot_uart_send(const char *s, uint16_t len)
{
    uint32_t guard;

    for (uint16_t i = 0; i < len; i++)
    {
        guard = 100000;
        while ((USART1->SR & USART_SR_TXE) == 0)
        {
            if (--guard == 0)
                return;                     /* 串口异常也不能把 Boot 卡死 */
        }
        USART1->DR = (uint8_t)s[i];
    }

    guard = 100000;
    while ((USART1->SR & USART_SR_TC) == 0)
    {
        if (--guard == 0)
            return;
    }
}

static void boot_log(const char *s)
{
    boot_uart_send(s, (uint16_t)strlen(s));
}

static void boot_log_u32(uint32_t v)
{
    char buf[11];
    int  i = 10;

    buf[i--] = '\0';
    if (v == 0)
        buf[i--] = '0';
    while (v > 0 && i >= 0)
    {
        buf[i--] = (char)('0' + (v % 10));
        v /= 10;
    }
    boot_log(&buf[i + 1]);
}

static void boot_log_hex(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[11];
    int  i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 8; i++)
        buf[2 + i] = hex[(v >> ((7 - i) * 4)) & 0x0F];
    buf[10] = '\0';
    boot_log(buf);
}

/* ---------------- 板级初始化 ---------------- */

/** SysTick 中断服务函数 —— Boot 必须自己提供这一个
 *
 *  启动文件（startup_stm32f103xb.s）里所有中断都是"弱定义"，默认展开成
 *  `B .` 无限循环。HAL_Delay() 靠的就是 SysTick 中断里的 HAL_IncTick()
 *  把 uwTick 加 1，如果不实现它，第一次 HAL_Delay 就会掉进默认死循环，
 *  表现为"上电后 LED 亮着不动、什么都不发生"。
 *
 *  APP 那个工程里有 CubeMX 生成的 stm32f1xx_it.c 负责这件事，
 *  Boot 没引那个文件，所以在自己实现。 */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/** HAL 的底层初始化：这里只管"总线开关"，具体引脚由下面各函数自己配 */
void HAL_MspInit(void)
{
    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();
}

/** 72 MHz：HSE 8 MHz × 9 = 72 MHz，和 APP 完全一致 */
static int boot_clock_init(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState       = RCC_HSE_ON;
    osc.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    osc.PLL.PLLState   = RCC_PLL_ON;
    osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLMUL     = RCC_PLL_MUL9;

    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
        return 0;

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;

    return (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) == HAL_OK);
}

static void boot_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();

    gpio.Pin   = GPIO_PIN_13;               /* 板载 LED */
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);   /* 灭 */
}

static void boot_led_toggle(void)
{
    static uint8_t on = 0;
    on = !on;
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

/* ---------------- 主流程 ---------------- */

int main(void)
{
    HAL_Init();

    if (!boot_clock_init())
    {
        /* 时钟起不来（晶振坏了/板子异常）：只能快闪报错，别的都做不了 */
        boot_gpio_init();
        while (1)
        {
            boot_led_toggle();
            HAL_Delay(100);
        }
    }

    boot_gpio_init();
    boot_uart_init();

    boot_log("\r\n===== OTA BootLoader v" BOOT_VERSION_STR " =====\r\n");

    /* 参数页只是"账本"，读坏了也不能拦着开机，所以返回值只用来打印 */
    boot_log("param: ");
    boot_log(boot_param_read(&g_param) ? "ok" : "fresh");
    boot_log(", upgrade_req=");
    boot_log_u32(g_param.upgrade_req);
    boot_log(", boot_try=");
    boot_log_u32(g_param.boot_try);
    boot_log("\r\n");

    boot_log("app @");
    boot_log_hex(APP_START_ADDR);
    if (boot_app_check(APP_START_ADDR))
    {
        boot_log(" valid, size=");
        boot_log_u32(boot_app_measure_size(APP_START_ADDR, APP_MAX_SIZE));
    }
    else
    {
        boot_log(" INVALID");
    }
    boot_log("\r\n");

    /* 3 秒窗口：LED 快闪 + 串口打点，让人一眼看出"现在还在 Boot 里"。
     * 日后加了升级请求通道（串口/CAN/W25Q 标志），就在这里判断要不要留。 */
    boot_log("waiting ");
    boot_log_u32(BOOT_WAIT_MS);
    boot_log(" ms...\r\n");

    for (uint32_t t = 0; t < BOOT_WAIT_MS; t += 100)
    {
        boot_led_toggle();
        boot_log(".");
        HAL_Delay(100);
    }
    boot_log("\r\n");

    if (g_param.upgrade_req)
    {
        /* 第③步在这里接固件接收流程；现在先老实待着，不乱跳 */
        boot_log("upgrade requested -> stay in boot\r\n");
    }
    else if (boot_app_check(APP_START_ADDR))
    {
        boot_log("jumping to app...\r\n");
        HAL_Delay(10);                       /* 等最后一个字节发完 */
        boot_jump_to_app(APP_START_ADDR);
        boot_log("jump refused\r\n");        /* 只有校验不过才会走到这 */
    }
    else
    {
        boot_log("no valid app -> stay in boot\r\n");
    }

    while (1)
    {
        boot_led_toggle();
        HAL_Delay(200);                      /* 停在 Boot：5 Hz 慢闪等升级 */
    }
}

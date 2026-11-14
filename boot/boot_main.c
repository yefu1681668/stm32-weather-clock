/**
 ******************************************************************************
 * @file    boot_main.c
 * @brief   OTA BootLoader 主程序（第②步：按需等待 + 升级请求）
 *
 * 上电后的完整动作：
 *   初始化时钟/串口/LED → 读参数页 → 检查 APP 是否可跳
 *   → 500 ms 常规窗口（LED 5 Hz 快闪，期间串口发 'U' 可手动请求升级）
 *   → 若参数页有升级请求 / 收到命令：立刻清标志，进入 30 s 升级等待
 *   → 等不到就回退跳 APP，绝不因为一次误触发把板子卡死
 *
 * LED 语言（不用接串口也能判断状态）：
 *   5 Hz 快闪 0.5 s 后跳走  = 正常启动
 *   2 Hz 慢闪最长 30 s      = 已进入升级等待
 *   10 Hz 快闪不停          = HSE 时钟起不来
 *   5 Hz 慢闪不停           = APP 无效 / 校验不通过，停在 Boot
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

    /* 清空复位前可能残留在接收寄存器里的字节。
     * 从 APP 复位过来时，串口线上偶尔会挂着一个噪声字节；
     * 不清的话 500 ms 窗口里可能被误读成升级命令。 */
    (void)USART1->SR;
    (void)USART1->DR;
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

/** 非阻塞收一个字符：没有数据时立刻返回 -1，绝不等待。
 *
 *  为什么不用中断：Boot 里没有 RTOS，也没有中断向量重定位的负担，
 *  但第③步收固件时是"一次一包"的阻塞流程，用中断反而要做环形缓冲。
 *  这里只需要在等待窗口里轮询，10 ms 一次足够 —— RXNE 是一直锁存的，
 *  读 DR 之前不会丢，单字符命令不存在溢出问题。 */
static int boot_uart_getchar(void)
{
    if ((USART1->SR & USART_SR_RXNE) == 0)
        return -1;

    /* 顺手把溢出/噪声错误标志清掉：先读 SR 再读 DR 是 F1 的规定顺序 */
    (void)USART1->SR;
    return (int)(USART1->DR & 0xFF);
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
        /* scan_end 只是「最后一个非 0xFF 字节」的位置，含 APP 区残留旧数据，
         * 不是固件真实长度。详见 boot_jump.c 里 boot_app_measure_size 的说明。 */
        boot_log(" valid, scan_end=");
        boot_log_u32(boot_app_measure_size(APP_START_ADDR, APP_MAX_SIZE));
    }
    else
    {
        boot_log(" INVALID");
    }
    boot_log("\r\n");

    /* 参数页里的请求是"一次性"的：读到就立刻清 0 再写回。
     * 不清的话，这次升级结束、下一次复位时 Boot 又会以为有请求，
     * 板子就永远停在 Boot —— 这是单分区 OTA 最容易踩的坑。
     * 注意：只在真的有请求时才擦写参数页，正常开机绝不写，避免磨损 Flash。 */
    int stay_for_upgrade = 0;
    if (g_param.upgrade_req)
    {
        g_param.upgrade_req = 0;
        if (boot_param_write(&g_param))
        {
            stay_for_upgrade = 1;
            boot_log("upgrade_req found -> stay in boot\r\n");
        }
        else
        {
            /* 清不掉就别停：否则每次开机都白等 30 秒，比丢一次请求更糟。 */
            boot_log("WARN: clear upgrade_req failed, ignore request\r\n");
        }
    }

    /* 常规窗口：500 ms、LED 5 Hz 快闪。
     * 期间串口发来 'U'（大小写都认）也能手动进升级 —— APP 跑不起来时，
     * 这是唯一还能进 Boot 升级流程的入口。 */
    boot_log("waiting ");
    boot_log_u32(BOOT_WAIT_MS);
    boot_log(" ms ('U' to upgrade)...\r\n");

    for (uint32_t t = 0; t < BOOT_WAIT_MS; t += 10)
    {
        if ((t % 100) == 0)
            boot_led_toggle();

        int ch = boot_uart_getchar();
        if (ch == BOOT_CMD_CHAR || ch == (BOOT_CMD_CHAR + 0x20))
        {
            stay_for_upgrade = 1;
            boot_log("\r\nmanual upgrade request\r\n");
            break;
        }
        HAL_Delay(10);
    }

    /* 升级等待窗口。第③步会把这个循环换成"收固件 → 写 W25Q → CRC 校验"，
     * 现在先做超时回退：请求了但没接上位机，也能自己回到 APP，不会变砖。 */
    if (stay_for_upgrade)
    {
        boot_log("waiting for firmware ");
        boot_log_u32(BOOT_UPGRADE_WAIT_MS);
        boot_log(" ms...\r\n");

        for (uint32_t elapsed = 0; elapsed < BOOT_UPGRADE_WAIT_MS; elapsed += 10)
        {
            if ((elapsed % 250) == 0)
                boot_led_toggle();          /* 2 Hz 慢闪，和启动窗口区分开 */
            HAL_Delay(10);
        }
        boot_log("timeout -> fallback to app\r\n");
    }

    if (boot_app_check(APP_START_ADDR))
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
        HAL_Delay(100);                      /* 停在 Boot：5 Hz 慢闪等升级 */
    }
}

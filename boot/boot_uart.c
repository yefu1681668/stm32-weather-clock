#include "boot_uart.h"
#include "stm32f1xx_hal.h"
#include <string.h>

void boot_uart_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    gpio.Pin   = GPIO_PIN_9;                /* TX */
    gpio.Mode  = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin  = GPIO_PIN_10;                /* RX：升级时收固件全靠它 */
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;                /* 上拉：没接 CH340 时不会被噪声误触发 */
    HAL_GPIO_Init(GPIOA, &gpio);

    /* 72 MHz / 115200 / 16 倍采样 = 39.0625 → BRR = 0x271 */
    USART1->BRR = 0x271;
    USART1->CR2 = 0;
    USART1->CR3 = 0;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;   /* 8N1 */

    /* 从 APP 复位过来时，串口线上偶尔挂着一个噪声字节。
     * 不清的话可能被 500 ms 窗口误读成升级命令，或被升级状态机当成半个帧。 */
    (void)USART1->SR;
    (void)USART1->DR;
}

void boot_uart_putc(uint8_t c)
{
    uint32_t guard = 100000;

    /* 只等 TXE（发送寄存器空），不等 TC。
     * ACK/NAK 是逐帧应答，上位机在等这个字节，越早发出越不易触发它超时。 */
    while ((USART1->SR & USART_SR_TXE) == 0)
    {
        if (--guard == 0)
            return;                         /* 串口异常也不能把 Boot 卡死 */
    }
    USART1->DR = c;
}

void boot_uart_send(const char *s, uint16_t len)
{
    uint32_t guard;

    for (uint16_t i = 0; i < len; i++)
    {
        guard = 100000;
        while ((USART1->SR & USART_SR_TXE) == 0)
        {
            if (--guard == 0)
                return;
        }
        USART1->DR = (uint8_t)s[i];
    }

    /* 等最后一个字节真正移出移位寄存器。
     * 复位/跳转前必须做，否则日志会缺尾巴（第②步就踩过这个坑）。 */
    guard = 100000;
    while ((USART1->SR & USART_SR_TC) == 0)
    {
        if (--guard == 0)
            return;
    }
}

void boot_log(const char *s)
{
    boot_uart_send(s, (uint16_t)strlen(s));
}

void boot_log_u32(uint32_t v)
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

void boot_log_hex(uint32_t v)
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

int boot_uart_getchar(void)
{
    if ((USART1->SR & USART_SR_RXNE) == 0)
        return -1;

    /* 顺手清溢出/噪声/帧错误标志：F1 规定先读 SR 再读 DR */
    (void)USART1->SR;
    return (int)(USART1->DR & 0xFF);
}

int boot_uart_getchar_timeout(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < timeout_ms)
    {
        int ch = boot_uart_getchar();
        if (ch >= 0)
            return ch;
    }
    return -1;
}

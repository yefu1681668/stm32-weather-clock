#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "stm32f1xx_hal.h"
#include "lcd_spi.h"
#include "delay.h"


static lcd_spi_send_finish_callback_t lcd_spi_send_finish_callback;


static void lcd_spi_gpio_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    /* SPI1_SCK = PA5, 复用推挽 */
    GPIO_InitStructure.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStructure.Pin = GPIO_PIN_5;
    GPIO_InitStructure.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStructure);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);

    /* SPI1_MOSI = PA7, 复用推挽 */
    GPIO_InitStructure.Pin = GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStructure);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_SET);
}

static void lcd_spi_nvic_init(void)
{
    HAL_NVIC_SetPriority(DMA1_Channel3_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel3_IRQn);
}

static void lcd_spi_lowlevel_init(void)
{
    /* SPI1: 单线发送主模式, 8位数据, CPOL=High, CPHA=2Edge, 软件NSS, 分频4=18MHz, MSB优先 */
    __HAL_RCC_SPI1_CLK_ENABLE();

    /* 关闭SPI再配置 */
    SPI1->CR1 = 0;

    SPI1->CR1 = SPI_CR1_CPOL       /* CPOL=High */
              | SPI_CR1_CPHA       /* CPHA=2Edge */
              | SPI_CR1_MSTR       /* 主模式 */
              | SPI_CR1_SSM | SPI_CR1_SSI   /* 软件NSS管理 */
              | SPI_CR1_BR_1       /* 分频4: 72/4=18MHz */
              | SPI_CR1_LSBFIRST;  /* 保持原样: 不置位则为MSB, 这里清0 */

    /* 清除LSBFIRST以MSB优先发送 */
    SPI1->CR1 &= ~SPI_CR1_LSBFIRST;

    /* 单向发送: BIDIMODE=1, BIDIOE=1 (仅发送) */
    SPI1->CR1 |= SPI_CR1_BIDIMODE | SPI_CR1_BIDIOE;

    /* 启用SPI */
    SPI1->CR1 |= SPI_CR1_SPE;

    /* 启用SPI1的DMA发送请求 */
    SPI1->CR2 = SPI_CR2_TXDMAEN;
}

void lcd_spi_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    lcd_spi_gpio_init();
    lcd_spi_nvic_init();
    lcd_spi_lowlevel_init();
}

/* 阻塞写: 逐字节轮询 TXE 写入。
 * 超时计数要"小": 正常情况第一轮就能置位, 根本不需要等;
 * 一旦 SPI 异常, 超时值过大会让每字节白等几十毫秒,
 * 32KB 一屏就算成 10 分钟, 表现就像死机。这里压到约 0.1ms。*/
void lcd_spi_write(uint8_t *data, uint16_t length)
{
    uint32_t guard;

    for (uint16_t i = 0; i < length; i++)
    {
        guard = 2000UL;
        while (((SPI1->SR & SPI_SR_TXE) == 0) && (--guard != 0))
            ;
        if (guard == 0)
            return;                     /* SPI 异常, 宁可丢数据也不死等 */
        *(volatile uint8_t *)&SPI1->DR = data[i];
    }

    guard = 2000UL;
    while (((SPI1->SR & SPI_SR_BSY) != 0) && (--guard != 0))
        ;
}

void lcd_spi_write_async(uint8_t *data, uint16_t length)
{
    /* 关闭DMA通道再配置 */
    DMA1_Channel3->CCR &= ~DMA_CCR_EN;

    DMA1_Channel3->CPAR = (uint32_t)&SPI1->DR;
    DMA1_Channel3->CMAR = (uint32_t)data;
    DMA1_Channel3->CNDTR = length;
    /* 从内存到外设, 内存递增, 外设不递增, 8位/8位, 普通模式, 中等优先级, 不内存到内存.
     * 关闭传输完成中断(TCIE), 调用方轮询DMA_ISR_TCIF3标志位. */
    DMA1_Channel3->CCR = DMA_CCR_MINC        /* 内存递增 */
                       | DMA_CCR_DIR;        /* 读自内存 */
    /* 中等优先级 (PL=0) */

    /* 清除DMA1通道3传输完成标志 */
    DMA1->IFCR = DMA_IFCR_CTCIF3;

    /* 启用DMA通道 */
    DMA1_Channel3->CCR |= DMA_CCR_EN;
}

void lcd_spi_send_finish_register(lcd_spi_send_finish_callback_t callback)
{
    lcd_spi_send_finish_callback = callback;
}

/* DMA1通道3传输完成中断 (启动文件中为weak, 这里强定义) */
void DMA1_Channel3_IRQHandler(void)
{
    if (DMA1->ISR & DMA_ISR_TCIF3)
    {
        while (SPI1->SR & SPI_SR_BSY)
            ;

        if (lcd_spi_send_finish_callback)
            lcd_spi_send_finish_callback();

        /* 清除传输完成标志 */
        DMA1->IFCR = DMA_IFCR_CTCIF3;
    }
}

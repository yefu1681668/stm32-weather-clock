#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "stm32f1xx_hal.h"
#include "esp_usart.h"


static esp_usart_receive_callback_t esp_usart_receive_callback;


void esp_usart_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();

    /* ESP_USART TX = PA2 复用推挽 */
    GPIO_InitStructure.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStructure.Pin = GPIO_PIN_2;
    GPIO_InitStructure.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStructure);

    /* ESP_USART RX = PA3 上拉输入 */
    GPIO_InitStructure.Mode = GPIO_MODE_INPUT;
    GPIO_InitStructure.Pin = GPIO_PIN_3;
    GPIO_InitStructure.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStructure);

    /* NVIC配置 USART2中断, 抢占优先级5 */
    HAL_NVIC_SetPriority(USART2_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);

    /* 关闭USART2再配置 */
    USART2->CR1 = 0;

    /* 115200波特率: APB1=36MHz, OVER16, BRR = 36000000/115200 = 312.5 -> 313 = 0x139 */
    USART2->BRR = SystemCoreClock / 2 / 115200;

    /* 8位数据, 无校验, 1停止位 (CR1/CR2默认) */
    USART2->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;
}

void esp_usart_write_data(uint8_t *data, uint16_t length)
{
    for (uint16_t i = 0; i < length; i++)
    {
        while ((USART2->SR & USART_SR_TXE) == 0)
            ;
        *(volatile uint8_t *)&USART2->DR = data[i];
    }
}

void esp_usart_write_string(const char *str)
{
    uint16_t len = strlen(str);
    esp_usart_write_data((uint8_t *)str, len);
}

void esp_usart_receive_register(esp_usart_receive_callback_t callback)
{
    esp_usart_receive_callback = callback;
}

/* USART2中断处理 (启动文件中为weak, 这里强定义) */
void USART2_IRQHandler(void)
{
    if (USART2->SR & USART_SR_RXNE)
    {
        uint8_t data = (uint8_t)(USART2->DR & 0xFF);

        if (esp_usart_receive_callback)
            esp_usart_receive_callback(data);
    }
}

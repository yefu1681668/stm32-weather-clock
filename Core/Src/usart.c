/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usart.c
  * @brief   USART1 debug port - register level (no HAL UART dependency)
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "usart.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>

/* USER CODE BEGIN 0 */
int fputc(int ch, FILE *f)
{
    while ((USART1->SR & USART_SR_TXE) == 0)
        ;
    *(volatile uint8_t *)&USART1->DR = (uint8_t)ch;
    return (ch);
}
/* USER CODE END 0 */

/* USART1 init function */
void MX_USART1_UART_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA9 TX - AF PP */
    GPIO_InitStruct.Pin = GPIO_PIN_9;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* PA10 RX - Input
     * 必须上拉：CH340 没接时 RX 若浮空，线路噪声会被当成起始位，
     * 触发假的 RXNE 中断甚至误判成升级命令。Boot 侧同样配了上拉。 */
    GPIO_InitStruct.Pin = GPIO_PIN_10;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* 115200 @ 72MHz: BRR = 72000000 / 115200 = 625 = 0x271 */
    USART1->BRR = SystemCoreClock / 115200;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

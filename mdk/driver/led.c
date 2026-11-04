#include <stdbool.h>
#include "stm32f1xx_hal.h"
#include "led.h"


#define LED_PORT    GPIOC
#define LED_PIN     GPIO_PIN_13


static bool led_state = false;


void led_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitStructure.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStructure.Pin = LED_PIN;
    GPIO_InitStructure.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(LED_PORT, &GPIO_InitStructure);
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);
}

void led_set(bool on)
{
    led_state = on;
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

void led_on(void)
{
    led_set(true);
}

void led_off(void)
{
    led_set(false);
}

void led_toggle(void)
{
    led_set(!led_state);
}

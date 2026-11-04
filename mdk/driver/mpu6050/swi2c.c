#include <stdbool.h>
#include <stdint.h>
#include "stm32f1xx_hal.h"
#include "main.h"
#include "delay.h"
#include "swi2c.h"


#define SCL_PORT    GPIOB
#define SCL_PIN     GPIO_PIN_6
#define SDA_PORT    GPIOB
#define SDA_PIN     GPIO_PIN_7

#define SCL_HIGH()  HAL_GPIO_WritePin(SCL_PORT, SCL_PIN, GPIO_PIN_SET)
#define SCL_LOW()   HAL_GPIO_WritePin(SCL_PORT, SCL_PIN, GPIO_PIN_RESET)
#define SDA_HIGH()  HAL_GPIO_WritePin(SDA_PORT, SDA_PIN, GPIO_PIN_SET)
#define SDA_LOW()   HAL_GPIO_WritePin(SDA_PORT, SDA_PIN, GPIO_PIN_RESET)
#define SDA_READ()  HAL_GPIO_ReadPin(SDA_PORT, SDA_PIN)
#define DELAY()     i2c_delay()

static void i2c_delay(void)
{
    delay_us(5);
}

static void i2c_start(void)
{
    SDA_HIGH();
    SCL_HIGH();
    DELAY();
    SDA_LOW();
    DELAY();
    SCL_LOW();
}

static void i2c_stop(void)
{
    SDA_LOW();
    DELAY();
    SCL_HIGH();
    DELAY();
    SDA_HIGH();
    DELAY();
}

static bool i2c_write_byte(uint8_t data)
{
    for (uint8_t i = 0; i < 8; i++)
    {
        if (data & 0x80)
            SDA_HIGH();
        else
            SDA_LOW();
        DELAY();
        SCL_HIGH();
        DELAY();
        SCL_LOW();
        data <<= 1;
    }
    SDA_HIGH();
    DELAY();
    SCL_HIGH();
    DELAY();
    if (SDA_READ())
    {
        SCL_LOW();
        return false;
    }
    SCL_LOW();
    DELAY();
    return true;
}

static uint8_t i2c_read_byte(bool ack)
{
    uint8_t data = 0;
    SDA_HIGH();
    for (uint8_t i = 0; i < 8; i++)
    {
        data <<= 1;
        SCL_HIGH();
        DELAY();
        if (SDA_READ())
            data |= 0x01;
        SCL_LOW();
        DELAY();
    }
    if (ack)
        SDA_LOW();
    else
        SDA_HIGH();
    DELAY();
    SCL_HIGH();
    DELAY();
    SCL_LOW();
    SDA_HIGH();
    DELAY();
    return data;
}

static void i2c_io_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* SCL 开漏输出 */
    GPIO_InitStructure.Mode = GPIO_MODE_OUTPUT_OD;
    GPIO_InitStructure.Pin = SCL_PIN;
    GPIO_InitStructure.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(SCL_PORT, &GPIO_InitStructure);

    /* SDA 开漏输出 */
    GPIO_InitStructure.Pin = SDA_PIN;
    HAL_GPIO_Init(SDA_PORT, &GPIO_InitStructure);
}

void swi2c_init(void)
{
    i2c_io_init();
}

bool swi2c_write(uint8_t addr, uint8_t reg, uint8_t *data, uint16_t length)
{
    i2c_start();
    if (!i2c_write_byte(addr << 1))
    {
        i2c_stop();
        return false;
    }
    i2c_write_byte(reg);
    for (uint16_t i = 0; i < length; i++)
    {
        i2c_write_byte(data[i]);
    }
    i2c_stop();
    return true;
}

bool swi2c_read(uint8_t addr, uint8_t reg, uint8_t *data, uint16_t length)
{
    i2c_start();
    if (!i2c_write_byte(addr << 1))
    {
        i2c_stop();
        return false;
    }
    i2c_write_byte(reg);
    i2c_start();
    i2c_write_byte((addr << 1) | 0x01);
    for (uint16_t i = 0; i < length; i++)
    {
        data[i] = i2c_read_byte(i == length - 1 ? false : true);
    }
    i2c_stop();
    return true;
}

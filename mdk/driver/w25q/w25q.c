#include "w25q.h"
#include "stm32f1xx_hal.h"
#include "delay.h"

/* ---------- 指令集（W25Q 系列通用） ---------- */
#define CMD_WRITE_ENABLE   0x06
#define CMD_READ_STATUS    0x05
#define CMD_READ_DATA      0x03
#define CMD_PAGE_PROGRAM   0x02
#define CMD_SECTOR_ERASE   0x20    /* 4 KB */
#define CMD_JEDEC_ID       0x9F

/* ---------- 引脚操作 ---------- */
#define W25Q_CS_LOW()      HAL_GPIO_WritePin(W25Q_CS_PORT,   W25Q_CS_PIN,   GPIO_PIN_RESET)
#define W25Q_CS_HIGH()     HAL_GPIO_WritePin(W25Q_CS_PORT,   W25Q_CS_PIN,   GPIO_PIN_SET)
#define W25Q_CLK_LOW()     HAL_GPIO_WritePin(W25Q_CLK_PORT,  W25Q_CLK_PIN,  GPIO_PIN_RESET)
#define W25Q_CLK_HIGH()    HAL_GPIO_WritePin(W25Q_CLK_PORT,  W25Q_CLK_PIN,  GPIO_PIN_SET)
#define W25Q_MOSI_LOW()    HAL_GPIO_WritePin(W25Q_MOSI_PORT, W25Q_MOSI_PIN, GPIO_PIN_RESET)
#define W25Q_MOSI_HIGH()   HAL_GPIO_WritePin(W25Q_MOSI_PORT, W25Q_MOSI_PIN, GPIO_PIN_SET)
#define W25Q_MISO_READ()   HAL_GPIO_ReadPin(W25Q_MISO_PORT,  W25Q_MISO_PIN)

/* 半周期延时：72 MHz 下几个 NOP 就有 100 ns 以上，
 * 远低于 W25Q 的时序上限（时钟可到 100 MHz+），够用且不占定时器。 */
static inline void spi_delay(void)
{
    __NOP(); __NOP(); __NOP(); __NOP();
}

/** 收发一个字节（Mode0：时钟空闲低电平，上升沿采样，MSB 先出）。
 *  注意：函数参数 out 会被移位消耗，所以这里用局部副本。 */
static uint8_t spi_xfer(uint8_t out)
{
    uint8_t in = 0;

    for (uint8_t i = 0; i < 8; i++)
    {
        if (out & 0x80)
            W25Q_MOSI_HIGH();
        else
            W25Q_MOSI_LOW();
        out <<= 1;

        spi_delay();
        W25Q_CLK_HIGH();            /* 上升沿：从机采样 MOSI */
        spi_delay();
        in = (uint8_t)((in << 1) | (W25Q_MISO_READ() == GPIO_PIN_SET ? 1 : 0));
        W25Q_CLK_LOW();             /* 下降沿：从机移出下一位 */
        spi_delay();
    }
    return in;
}

/** 轮询状态寄存器的 BUSY 位（bit0），超时返回 false。
 *  页写典型 0.7 ms，4 KB 扇区擦除典型 45 ms、最坏 400 ms。 */
static bool w25q_wait_ready(uint32_t timeout_ms)
{
    while (timeout_ms--)
    {
        W25Q_CS_LOW();
        spi_xfer(CMD_READ_STATUS);
        uint8_t sr = spi_xfer(0xFF);
        W25Q_CS_HIGH();

        if ((sr & 0x01) == 0)
            return true;

        delay_ms(1);
    }
    return false;
}

static void w25q_write_enable(void)
{
    W25Q_CS_LOW();
    spi_xfer(CMD_WRITE_ENABLE);
    W25Q_CS_HIGH();
}

bool w25q_init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* CS / CLK / MOSI：推挽输出。先把 CS 拉高，避免上电瞬间被误当成指令。 */
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Pull  = GPIO_NOPULL;

    g.Pin = W25Q_CS_PIN;   HAL_GPIO_Init(W25Q_CS_PORT,   &g); W25Q_CS_HIGH();
    g.Pin = W25Q_CLK_PIN;  HAL_GPIO_Init(W25Q_CLK_PORT,  &g); W25Q_CLK_LOW();
    g.Pin = W25Q_MOSI_PIN; HAL_GPIO_Init(W25Q_MOSI_PORT, &g); W25Q_MOSI_HIGH();

    /* MISO：上拉输入。上拉很关键：模块没插时读回全 1，能立刻区分“没接”。 */
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    g.Pin  = W25Q_MISO_PIN;
    HAL_GPIO_Init(W25Q_MISO_PORT, &g);

    delay_ms(5);                    /* tPUW：上电到可访问最坏 5 ms */

    uint32_t id = w25q_jedec_id();

    /* 0x000000 = MISO 被拉死（短路/接错脚）；0xFFFFFF = 芯片完全没应答 */
    return (id != 0x00000000UL) && (id != 0x00FFFFFFUL);
}

uint32_t w25q_jedec_id(void)
{
    uint32_t id;

    W25Q_CS_LOW();
    spi_xfer(CMD_JEDEC_ID);
    id  = (uint32_t)spi_xfer(0xFF) << 16;   /* 厂商，Winbond = 0xEF */
    id |= (uint32_t)spi_xfer(0xFF) << 8;    /* 类型，通常 0x40 */
    id |= (uint32_t)spi_xfer(0xFF);         /* 容量代码 */
    W25Q_CS_HIGH();

    return id;
}

uint32_t w25q_capacity(void)
{
    uint32_t id   = w25q_jedec_id();
    uint8_t  code = (uint8_t)(id & 0xFF);

    /* Winbond 常见容量代码 0x14~0x1F，对应 1 MB ~ 32 MB */
    if (code < 0x10 || code > 0x1F)
        return 0;

    return 1UL << code;             /* 0x15 -> 2 MB，0x17 -> 8 MB */
}

void w25q_read(uint32_t addr, void *buf, uint32_t len)
{
    uint8_t *p = (uint8_t *)buf;

    W25Q_CS_LOW();
    spi_xfer(CMD_READ_DATA);
    spi_xfer((uint8_t)(addr >> 16));    /* W25Q 用 24 位地址，最高 16 MB */
    spi_xfer((uint8_t)(addr >> 8));
    spi_xfer((uint8_t)addr);

    while (len--)
        *p++ = spi_xfer(0xFF);          /* 发 0xFF 只为产生时钟，把数据移出来 */

    W25Q_CS_HIGH();
}

/** 写一页（不超过 256 B，且不能跨页边界）。 */
static bool w25q_page_program(uint32_t addr, const uint8_t *buf, uint16_t len)
{
    if (!w25q_wait_ready(500))
        return false;

    w25q_write_enable();

    W25Q_CS_LOW();
    spi_xfer(CMD_PAGE_PROGRAM);
    spi_xfer((uint8_t)(addr >> 16));
    spi_xfer((uint8_t)(addr >> 8));
    spi_xfer((uint8_t)addr);
    while (len--)
        spi_xfer(*buf++);
    W25Q_CS_HIGH();

    return w25q_wait_ready(500);
}

bool w25q_write(uint32_t addr, const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;

    while (len)
    {
        /* 一页 256 B，跨页必须拆开，否则会回卷覆盖页首 */
        uint16_t space = (uint16_t)(256 - (addr & 0xFF));
        uint16_t n     = (len < space) ? (uint16_t)len : space;

        if (!w25q_page_program(addr, p, n))
            return false;

        addr += n;
        p    += n;
        len  -= n;
    }
    return true;
}

bool w25q_erase_sector(uint32_t addr)
{
    if (!w25q_wait_ready(1000))
        return false;

    w25q_write_enable();

    W25Q_CS_LOW();
    spi_xfer(CMD_SECTOR_ERASE);
    spi_xfer((uint8_t)(addr >> 16));
    spi_xfer((uint8_t)(addr >> 8));
    spi_xfer((uint8_t)addr);
    W25Q_CS_HIGH();

    return w25q_wait_ready(1000);
}

bool w25q_erase_range(uint32_t addr, uint32_t len)
{
    uint32_t end = addr + len;

    addr &= ~0xFFFUL;               /* 向下对齐到 4 KB 扇区边界 */

    while (addr < end)
    {
        if (!w25q_erase_sector(addr))
            return false;
        addr += 4096;
    }
    return true;
}

bool w25q_is_blank(uint32_t addr, uint32_t len)
{
    uint8_t tmp[64];

    while (len)
    {
        uint16_t n = (len < sizeof(tmp)) ? (uint16_t)len : (uint16_t)sizeof(tmp);

        w25q_read(addr, tmp, n);
        for (uint16_t i = 0; i < n; i++)
        {
            if (tmp[i] != 0xFF)
                return false;
        }

        addr += n;
        len  -= n;
    }
    return true;
}

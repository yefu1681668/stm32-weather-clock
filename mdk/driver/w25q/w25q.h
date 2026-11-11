#ifndef __W25Q_H__
#define __W25Q_H__

#include <stdint.h>
#include <stdbool.h>

/* ==================================================================
 * W25Q 外部 SPI Flash 驱动（软件 SPI，Mode0）
 *
 * 为什么不用硬件 SPI1：
 *   TFT 已经占了 SPI1，而且 lcd_spi.c 把 SPI1 配成 BIDIMODE 单向只发 + Mode3；
 *   W25Q 需要读回 MISO（双向）且用 Mode0。共用同一条硬件总线，每次切换都要
 *   重配 SPI1，任何一次配错就白屏。软件 SPI 只多占 4 个空闲脚，对刷屏零影响。
 *
 * 接线（模块丝印 -> C8T6）：
 *   VCC      -> 3.3V        （不要接 5V）
 *   GND      -> GND
 *   CS       -> PB12        （片选，低有效）
 *   CLK      -> PB13        （软件 SPI 时钟）
 *   MOSI/DI  -> PB15        （STM32 发给 Flash）
 *   MISO/DO  -> PA6         （Flash 回给 STM32）
 *   WP/HOLD  -> 3.3V        （模块上没有就悬空，正片内部已上拉）
 *
 * 引脚冲突检查（2026-11-11 全工程 grep 确认全部空闲）：
 *   PA4=TFT_CS  PA5=TFT_SCL  PA7=TFT_SDA  PB0=TFT_RES  PB1=TFT_DC  PB10=BLK
 *   PB6/PB7=MPU6050 I2C      PA2/PA3=ESP32 UART2        PC13=LED
 *   PC14/PC15=LSE 晶振       PA13/PA14=SWD
 * ================================================================== */

#define W25Q_CS_PORT      GPIOB
#define W25Q_CS_PIN       GPIO_PIN_12
#define W25Q_CLK_PORT     GPIOB
#define W25Q_CLK_PIN      GPIO_PIN_13
#define W25Q_MOSI_PORT    GPIOB
#define W25Q_MOSI_PIN     GPIO_PIN_15
#define W25Q_MISO_PORT    GPIOA
#define W25Q_MISO_PIN     GPIO_PIN_6

/* 常用容量对应的 JEDEC 低字节（Winbond：EF 40 xx）：
 *   0x14 = 1 MB   0x15 = 2 MB (W25Q16)
 *   0x16 = 4 MB   0x17 = 8 MB (W25Q64)
 *   0x18 = 16 MB (W25Q128) */
#define W25Q_ID_W25Q16    0x15
#define W25Q_ID_W25Q64    0x17

/** 初始化引脚并读 JEDEC ID。
 *  返回 true = 芯片应答正常；false = 没接好（MISO 全 0 或全 1）。 */
bool w25q_init(void);

/** 读 JEDEC ID，形如 0xEF4017（厂商 | 类型 | 容量）。 */
uint32_t w25q_jedec_id(void);

/** 按 ID 推算容量（字节）；ID 不认识返回 0。 */
uint32_t w25q_capacity(void);

/** 顺序读任意长度。addr 为 24 位地址。 */
void w25q_read(uint32_t addr, void *buf, uint32_t len);

/** 顺序写任意长度，内部自动按 256 B 页边界拆分。写入前地址所在扇区必须是擦除态。 */
bool w25q_write(uint32_t addr, const void *buf, uint32_t len);

/** 擦除 addr 所在的 4 KB 扇区。 */
bool w25q_erase_sector(uint32_t addr);

/** 擦除一段区域（起点自动向下对齐到 4 KB）。 */
bool w25q_erase_range(uint32_t addr, uint32_t len);

/** 判断一段区域是否全为 0xFF（擦除态）。 */
bool w25q_is_blank(uint32_t addr, uint32_t len);

#endif /* __W25Q_H__ */

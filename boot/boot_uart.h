#ifndef __BOOT_UART_H__
#define __BOOT_UART_H__

#include <stdint.h>

/* ==================================================================
 * Boot 的 USART1（PA9=TX / PA10=RX，115200 8N1）
 *
 * 第③步把串口从 boot_main.c 里抽出来，是因为升级状态机 boot_ota.c
 * 既要收帧、又要回 ACK/NAK、还要打日志，两边共用同一套底层才不会
 * 出现"两处各自配 USART1"的隐患。
 *
 * 依然不用 HAL_UART：这个工程没打开 HAL_UART 模块，寄存器直写约 40 行，
 * 省 1~2 KB ROM —— Boot 总共只有 16 KB，能省一点是一点。
 * ================================================================== */

/** 初始化引脚、波特率，并清掉复位前残留在 RX 里的噪声字节 */
void boot_uart_init(void);

/** 发一个字节（只等 TXE，用于 ACK/NAK 这种要求快应答的单字节） */
void boot_uart_putc(uint8_t c);

/** 发一段数据，末尾等 TC 确保最后一字节已经真正发完 */
void boot_uart_send(const char *s, uint16_t len);

/** 打日志（字符串 / 十进制 / 十六进制），不引入 printf 省 ROM */
void boot_log(const char *s);
void boot_log_u32(uint32_t v);
void boot_log_hex(uint32_t v);

/** 非阻塞收一个字符，没数据立刻返回 -1 */
int  boot_uart_getchar(void);

/** 阻塞收一个字符，timeout_ms 内没等到返回 -1。
 *  升级状态机用它做"帧内字节间超时"：上位机发一半卡住时不会把 Boot 卡死。 */
int  boot_uart_getchar_timeout(uint32_t timeout_ms);

#endif /* __BOOT_UART_H__ */

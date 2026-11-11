#ifndef __BOOT_CRC_H__
#define __BOOT_CRC_H__

#include <stdint.h>

/* CRC32（多项式 0xEDB88320，与 zlib / PKZIP 一致，初值 0xFFFFFFFF，末尾取反）
 *
 * 为什么 Boot 和上位机要用同一个 CRC：Boot 收到固件后算出来的值必须能和
 * 上位机发的值逐位对上，差一位就整包重传。用标准 CRC32 的好处是电脑端
 * 直接用 Python 的 zlib.crc32() 就能算出同一个数，不用自己写算法。 */

/** 一次性算完一整块 */
uint32_t boot_crc32(const void *data, uint32_t len);

/** 分块累加：第一次传 boot_crc32_init() 的返回值，之后把上次结果传回来 */
uint32_t boot_crc32_init(void);
uint32_t boot_crc32_update(uint32_t crc, const void *data, uint32_t len);
uint32_t boot_crc32_final(uint32_t crc);

#endif /* __BOOT_CRC_H__ */

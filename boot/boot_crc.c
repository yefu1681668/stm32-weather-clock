#include "boot_crc.h"

/* 位运算版 CRC32：不用查表，省 1 KB ROM。
 * Boot 只有 16 KB，而且 CRC 只在升级时算几次（21 KB 固件约 20 ms），
 * 速度完全不是瓶颈 —— 用空间换时间在这里不划算。 */
uint32_t boot_crc32_init(void)
{
    return 0xFFFFFFFFu;
}

uint32_t boot_crc32_update(uint32_t crc, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    while (len--)
    {
        crc ^= *p++;

        for (uint8_t i = 0; i < 8; i++)
        {
            if (crc & 1u)
                crc = (crc >> 1) ^ 0xEDB88320u;
            else
                crc >>= 1;
        }
    }
    return crc;
}

uint32_t boot_crc32_final(uint32_t crc)
{
    return ~crc;
}

uint32_t boot_crc32(const void *data, uint32_t len)
{
    return boot_crc32_final(boot_crc32_update(boot_crc32_init(), data, len));
}

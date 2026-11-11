#ifndef __BOOT_PARAM_H__
#define __BOOT_PARAM_H__

#include <stdint.h>

/* 参数页（0x0800FC00）里那 32 个字节的"账本"
 *
 * 为什么单独占一页：内部 Flash 最小擦除单位就是 1 KB 页，参数只能整页擦、
 * 整页写。这一页只放一个结构体，和 APP、Boot 都不重叠，谁升级都不会碰到它。 */

#define BOOT_PARAM_MAGIC      0x4F544131UL      /* "OTA1" */

typedef struct
{
    uint32_t magic;          /* 必须等于 BOOT_PARAM_MAGIC，否则视为首次上电 */
    uint32_t upgrade_req;    /* 1 = APP 请求升级，Boot 停在升级流程不跳转 */
    uint32_t app_size;       /* 待升级 / 已升级固件的字节数 */
    uint32_t app_crc32;      /* 该固件的 CRC32 */
    uint32_t boot_try;       /* 连续上电尝试次数，超限就停 Boot（失败保护） */
    uint32_t reserved[2];
    uint32_t crc;            /* 上面所有字段的 CRC32，防止参数页被写坏 */
} boot_param_t;

/** 从参数页读一份到 RAM；magic 或 CRC 不对时自动填一份默认值并返回 0 */
int  boot_param_read(boot_param_t *p);

/** 把结构体写回参数页（内部会先擦这一页） */
int  boot_param_write(const boot_param_t *p);

/** 恢复到出厂默认值（upgrade_req=0，boot_try=0） */
void boot_param_default(boot_param_t *p);

#endif /* __BOOT_PARAM_H__ */

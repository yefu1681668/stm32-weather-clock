#include "boot_param.h"
#include "boot_cfg.h"
#include "boot_flash.h"
#include "boot_crc.h"
#include <string.h>

/** 结构体里 crc 字段之前的字节数（CRC 只覆盖前面的字段） */
#define PARAM_CRC_LEN   (sizeof(boot_param_t) - sizeof(uint32_t))

void boot_param_default(boot_param_t *p)
{
    memset(p, 0, sizeof(boot_param_t));
    p->magic       = BOOT_PARAM_MAGIC;
    p->upgrade_req = 0;
    p->app_size    = 0;
    p->app_crc32   = 0;
    p->boot_try    = 0;
}

int boot_param_read(boot_param_t *p)
{
    /* 直接按地址读：内部 Flash 是内存映射的，读它和读数组一样快，
     * 不需要任何 HAL 函数。 */
    memcpy(p, (const void *)PARAM_PAGE_ADDR, sizeof(boot_param_t));

    if (p->magic != BOOT_PARAM_MAGIC)
    {
        boot_param_default(p);       /* 首次上电：整页是 0xFF */
        return 0;
    }

    if (boot_crc32(p, PARAM_CRC_LEN) != p->crc)
    {
        boot_param_default(p);       /* 参数页被写坏：宁可当没升级过 */
        return 0;
    }

    return 1;
}

int boot_param_write(const boot_param_t *p)
{
    boot_param_t tmp = *p;

    tmp.magic = BOOT_PARAM_MAGIC;
    tmp.crc   = boot_crc32(&tmp, PARAM_CRC_LEN);

    if (!boot_flash_erase_page(PARAM_PAGE_ADDR))
        return 0;

    return boot_flash_write(PARAM_PAGE_ADDR, &tmp, sizeof(tmp));
}

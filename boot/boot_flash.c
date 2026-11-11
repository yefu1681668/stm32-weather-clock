#include "boot_flash.h"
#include "boot_cfg.h"
#include "stm32f1xx_hal.h"

int boot_flash_erase_page(uint32_t addr)
{
    FLASH_EraseInitTypeDef cfg;
    uint32_t err = 0;
    int ok;

    if (addr < FLASH_BASE_ADDR || addr >= (FLASH_BASE_ADDR + 0x10000UL))
        return 0;                       /* 出了 Flash 范围，直接拒绝 */

    addr &= ~(BOOT_FLASH_PAGE_SIZE - 1UL);   /* 对齐到页首 */

    HAL_FLASH_Unlock();

    cfg.TypeErase   = FLASH_TYPEERASE_PAGES;
    cfg.PageAddress = addr;
    cfg.NbPages     = 1;

    ok = (HAL_FLASHEx_Erase(&cfg, &err) == HAL_OK) && (err == 0xFFFFFFFFUL);

    HAL_FLASH_Lock();
    return ok;
}

int boot_flash_erase_range(uint32_t addr, uint32_t len)
{
    uint32_t page = addr & ~(BOOT_FLASH_PAGE_SIZE - 1UL);
    uint32_t end  = addr + len;

    while (page < end)
    {
        if (!boot_flash_erase_page(page))
            return 0;                   /* 中途失败立刻停手，不再继续擦 */
        page += BOOT_FLASH_PAGE_SIZE;
    }
    return 1;
}

int boot_flash_write(uint32_t addr, const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    int ok = 1;

    HAL_FLASH_Unlock();

    while (len)
    {
        uint16_t half;

        if (len >= 2)
        {
            half = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
            p   += 2;
            len -= 2;
        }
        else
        {
            half = (uint16_t)(0xFF00u | p[0]);   /* 末尾单字节：高字节补 0xFF */
            p   += 1;
            len -= 1;
        }

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, half) != HAL_OK)
        {
            ok = 0;
            break;
        }
        addr += 2;
    }

    HAL_FLASH_Lock();
    return ok;
}

int boot_flash_is_blank(uint32_t addr, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)addr;   /* 直接按地址读，无需解锁 */

    while (len--)
    {
        if (*p++ != 0xFF)
            return 0;
    }
    return 1;
}

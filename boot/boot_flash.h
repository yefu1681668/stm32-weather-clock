#ifndef __BOOT_FLASH_H__
#define __BOOT_FLASH_H__

#include <stdint.h>

/* 内部 Flash 的擦 / 写封装
 *
 * 为什么 Boot 要自己写这层：升级时要把 APP 区 47 KB 整片擦掉再逐块写回，
 * 这是整个 OTA 里最危险的动作 —— 擦到一半断电，APP 就没了。所有函数都返回
 * 成功/失败，调用方必须检查，绝不能"擦完就假定成功"。 */

#define BOOT_FLASH_PAGE_SIZE   1024UL           /* C8T6 中等容量：1 KB/页 */

/** 擦除 addr 所在的 1 KB 页（addr 会被对齐到页首） */
int boot_flash_erase_page(uint32_t addr);

/** 擦除 [addr, addr+len) 覆盖到的所有页 */
int boot_flash_erase_range(uint32_t addr, uint32_t len);

/** 写入任意长度；按半字编程，奇数长度用 0xFF 补齐 */
int boot_flash_write(uint32_t addr, const void *buf, uint32_t len);

/** 判断一段区域是否全为 0xFF（擦除后的状态），用于跳过无谓的擦除 */
int boot_flash_is_blank(uint32_t addr, uint32_t len);

#endif /* __BOOT_FLASH_H__ */

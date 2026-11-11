#ifndef __BOOT_CFG_H__
#define __BOOT_CFG_H__

/* ==================================================================
 * OTA 分区表（C8T6，64 KB Flash）
 *
 *   0x08000000 ┌──────────────────┐
 *              │ BootLoader 16 KB │  ← 本工程，永不升级
 *   0x08004000 ├──────────────────┤
 *              │ APP 47 KB        │  ← 天气时钟，OTA 目标
 *   0x0800FC00 ├──────────────────┤
 *              │ 参数页 1 KB      │  ← 升级标志 / 大小 / CRC / 尝试次数
 *   0x08010000 └──────────────────┘
 *
 * 外部 W25Q（1 MB）:
 *   0x010000  6 张天气图标 27 KB
 *   0x020000  升级固件暂存区（Boot 先写这里，校验通过才搬进 APP 区）
 * ================================================================== */

#define BOOT_VERSION_STR      "0.1"

#define FLASH_BASE_ADDR       0x08000000UL
#define BOOT_MAX_SIZE         0x4000UL          /* 16 KB，Boot 自己绝不能超 */
#define APP_START_ADDR        0x08004000UL
#define APP_MAX_SIZE          0xBC00UL          /* 47 KB = 0xFC00 - 0x4000 */
#define APP_END_ADDR          (APP_START_ADDR + APP_MAX_SIZE)
#define PARAM_PAGE_ADDR       0x0800FC00UL      /* 参数页，1 KB 一页 */
#define PARAM_PAGE_SIZE       0x400UL

/* RAM 范围：用来粗判 APP 的初始栈顶是否合理 */
#define RAM_START_ADDR        0x20000000UL
#define RAM_END_ADDR          0x20005000UL      /* C8T6 = 20 KB */

/* 上电等在 Boot 里的时间窗口（毫秒）：这段时间内可以发起升级 */
#define BOOT_WAIT_MS          3000UL

#endif /* __BOOT_CFG_H__ */

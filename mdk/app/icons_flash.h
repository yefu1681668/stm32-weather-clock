#ifndef __ICONS_FLASH_H__
#define __ICONS_FLASH_H__

#include <stdint.h>

/* ==================================================================
 * 图标在 W25Q 里的固定布局（与 icons_flash/icons_index.h 一致）
 *
 *   W25Q 地址        内容                 大小
 *   0x000000         保留（元数据/参数）   64 KB
 *   0x010000         6 张天气图标          27 648 B
 *   0x020000         OTA 固件暂存区        ≤ 47 KB（今天不用，先占位）
 *
 * 每张图标 48x48 RGB565 = 4608 B，按 0x1200 间隔排布，
 * 正好对应 tools/extract_icons.py 生成的 icons.bin（紧凑排列，无文件头）。
 * ================================================================== */

#define W25Q_ICON_BASE     0x010000UL
#define W25Q_FW_BASE       0x020000UL

#define ICON_W             48
#define ICON_H             48
#define ICON_BYTES         (ICON_W * ICON_H * 2)      /* 4608 B */

#define ICON_OFF_QING      0x010000UL                 /* 晴 */
#define ICON_OFF_DUOYUN    0x011200UL                 /* 多云 */
#define ICON_OFF_YIN       0x012400UL                 /* 阴 */
#define ICON_OFF_YU        0x013600UL                 /* 雨 */
#define ICON_OFF_XUE       0x014800UL                 /* 雪 */
#define ICON_OFF_FENG      0x015A00UL                 /* 风 */

#define ICON_TOTAL_BYTES   27648UL                    /* 6 x 4608 */

/* 认不出的天气字符串, 用作"没有图标"的哨兵值 (不可能等于任何真实偏移) */
#define ICON_OFF_NONE      0xFFFFFFFFUL

#endif /* __ICONS_FLASH_H__ */

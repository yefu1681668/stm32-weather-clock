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

#define BOOT_VERSION_STR      "0.2"

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

/* 上电后在 Boot 里的常规窗口（毫秒）。
 * 第①步是固定 3 秒，每次开机都白等；第②步改成 500 ms —— 只有真的收到
 * 升级请求（参数页标志，或这个窗口里串口发来的命令字符）才会长等。 */
#define BOOT_WAIT_MS          500UL

/* 确认要升级后停留的等待时间（毫秒）。第③步的固件接收流程会占用这个窗口，
 * 现在先做超时回退：等不到就跳回 APP，保证板子不会因为一次误触发而卡住。 */
#define BOOT_UPGRADE_WAIT_MS  30000UL

/* 手动进入升级的单字符命令。Boot 窗口和 APP 运行时都认它，
 * 这样即使 APP 坏了、跑不起来，也能在复位瞬间从 Boot 手动进升级。 */
#define BOOT_CMD_CHAR         'U'

/* ==================================================================
 * 第③步：串口升级协议（OTA v1）
 *
 * 为什么必须走 W25Q 暂存：C8T6 只有 20 KB SRAM，而 APP 固件 22 564 B，
 * 根本不可能整包收进 RAM 再校验。所以每收一帧就顺序写进外部 Flash，
 * 收完再从 W25Q 流式读回来算 CRC32，通过了才敢擦 APP 区。
 *
 * W25Q 暂存区里的「镜像」布局（起点 0x020000）：
 *   +0x00  32 B  镜像头：magic / length / crc32 / version / 保留
 *   +0x20  N B   固件原始数据（从 0x08004000 开始的连续映像）
 *
 * 串口分帧（115200 8N1）：
 *   SOF 1B(0xA5) | TYPE 1B | SEQ 2B(小端) | LEN 2B(小端)
 *   | PAYLOAD LEN B | CRC16 2B(小端, 覆盖 TYPE..PAYLOAD)
 *   TYPE: 0x01=HEAD(payload 32B) 0x02=DATA(<=128B) 0x03=END(0B)
 *   应答: 0x06=ACK  0x15=NAK(重发本帧)  0x07=DONE(成功待复位)  0x1B=ABORT
 *
 * 【硬约束】length 只能来自镜像头，绝不能靠扫 0xFF 推断 ——
 * APP 区后半段残留着迁址前的旧固件，扫出来的长度是假的（见 11-14 日志）。
 * ================================================================== */

/* 与 mdk/app/icons_flash.h 的 W25Q_FW_BASE 必须是同一个地址 */
#define OTA_STAGING_ADDR      0x020000UL
#define OTA_IMAGE_HDR_SIZE    32UL
#define OTA_IMAGE_MAGIC       0x3241544FUL      /* "OTA2" 按小端读成 uint32 */

#define OTA_FRAME_SOF         0xA5u
#define OTA_TYPE_HEAD         0x01u
#define OTA_TYPE_DATA         0x02u
#define OTA_TYPE_END          0x03u
#define OTA_PAYLOAD_MAX       128u

#define OTA_ACK               0x06u
#define OTA_NAK               0x15u
#define OTA_DONE              0x07u
#define OTA_ABORT             0x1Bu

#define OTA_BYTE_TIMEOUT_MS   500UL             /* 一帧内部字节间最大间隔 */
#define OTA_FIRST_FRAME_MS    BOOT_UPGRADE_WAIT_MS  /* 等第一个 SOF 的时间 */

#endif /* __BOOT_CFG_H__ */

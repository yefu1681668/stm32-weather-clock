#ifndef __BOOT_OTA_H__
#define __BOOT_OTA_H__

#include "boot_param.h"

/* ==================================================================
 * 第③步：串口固件升级状态机
 *
 *   HEAD(32B 镜像头) -> DATA(128B x N) -> END
 *        ↓ 每帧立即写 W25Q 暂存区（RAM 装不下 22 KB 固件）
 *   END 后：W25Q 读回算 CRC32 -> 擦 APP 区 -> 复制回内部 Flash
 *           -> 读回再校验 -> 记账参数页 -> 复位
 *
 * 关键安全性质：CRC32 通过之前，**APP 区一个字节都不会被擦**。
 * 传坏包、断线、手动取消，都只是白忙一场，板子照常能跳回原来的 APP。
 * ================================================================== */

/** 运行一次升级会话。
 *
 *  @param param  参数页在 RAM 里的副本；升级成功时会写入 app_size / app_crc32
 *  @return 1 = 升级成功（函数内部已 NVIC_SystemReset，正常不会返回）
 *          0 = 超时 / 校验失败 / 中止，主流程应继续尝试跳 APP
 */
int boot_ota_run(boot_param_t *param);

#endif /* __BOOT_OTA_H__ */

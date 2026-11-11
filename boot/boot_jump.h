#ifndef __BOOT_JUMP_H__
#define __BOOT_JUMP_H__

#include <stdint.h>

/* APP 合法性检查 —— 返回 0 = 不可跳转
 *
 * 只查两件事，但这两件足以挡住 99% 的"跳过去就死"：
 *   ① 第 1 个字（初始栈顶 SP）必须落在 RAM 范围内 —— APP 被擦掉时这里是 0xFFFFFFFF
 *   ② 第 2 个字（复位向量）必须在 APP 区内且最低位为 1 —— Cortex-M 的 Thumb 地址
 *      最低位恒为 1，若读出偶数说明要么空片、要么数据错位。 */
int  boot_app_check(uint32_t app_addr);

/* 真正的跳转：关中断 → 停 SysTick → 复位时钟 → 设 VTOR → 设 MSP → 跳
 * 成功跳过去就不会返回；参数不合法时返回 0 */
int  boot_jump_to_app(uint32_t app_addr);

/** 取 APP 区固件的"有效长度"：从 APP 起始往下找最后一段非 0xFF 数据
 *  Boot 自己算不出精确长度（没有 hex 头），升级时用上位机发来的长度即可，
 *  这个函数只在"没记录过长度"的兜底场景用到。 */
uint32_t boot_app_measure_size(uint32_t app_addr, uint32_t max_size);

#endif /* __BOOT_JUMP_H__ */

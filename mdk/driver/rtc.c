#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "stm32f1xx_hal.h"
#include "rtc.h"


/* 寄存器级RTC驱动 (F1的RTC是一个32位计数器, 非BCD日历)
 * 用作Unix时间戳计数器, 与 ts_to_date / date_to_ts 配合 */
#ifndef PWR_CR_DBP
#define PWR_CR_DBP  PWR_CR_DBP_Msk
#endif


static bool date_validate(const rtc_date_t *date)
{
    if (date->year < 1970 || date->year > 2099)
        return false;
    if (date->month < 1 || date->month > 12)
        return false;
    if (date->day < 1 || date->day > 31)
        return false;
    if (date->hour > 23)
        return false;
    if (date->minute > 59)
        return false;
    if (date->second > 59)
        return false;

    return true;
}

uint32_t date_to_ts(const rtc_date_t *date)
{
    uint16_t year = date->year;
    uint8_t month = date->month;
    uint8_t day = date->day;
    uint8_t hour = date->hour;
    uint8_t minute = date->minute;
    uint8_t second = date->second;

    uint64_t ts = 0;

    /* 二月闰月，计算当作最后一个月份 */
    month -= 2;
    if ((int8_t)month <= 0)
    {
        month += 12;
        year -= 1;
    }

    /* 计算时间戳 */
    ts = (((year / 4 - year / 100 + year / 400 + 367 * month / 12 + day + year * 365 - 719499) * 24 +
          hour) * 60 + minute) * 60 + second;

    return ts;
}

void ts_to_date(uint32_t seconds, rtc_date_t *date)
{
    uint32_t leapyears = 0, yearhours = 0;
    const uint32_t mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const uint16_t ONE_YEAR_HOURS = 8760;

    memset(date, 0, sizeof(rtc_date_t));

    /* 秒 */
    date->second = seconds % 60;
    seconds /= 60;

    /* 分 */
    date->minute = seconds % 60;
    seconds /= 60;

    /* 年 */
    leapyears = seconds / (1461 * 24);
    date->year = (leapyears << 2) + 1970;
    seconds %= 1461 * 24;

    for (;;)
    {
        yearhours = ONE_YEAR_HOURS;

        /* 闰年加一天 */
        if ((date->year & 3) == 0)
            yearhours += 24;

        /* 剩余时间已经不足一年了跳出循环 */
        if (seconds < yearhours)
            break;

        date->year++;
        /* 减去一年的时间 */
        seconds -= yearhours;
    }

    /* 时 */
    date->hour = seconds % 24;
    seconds /= 24;
    seconds++;

    /* 闰年 */
    if ((date->year & 3) == 0)
    {
        if (seconds > 60)
        {
            seconds--;
        }
        else
        {
            if (seconds == 60)
            {
                date->month = 2;
                date->day = 29;
                return;
            }
        }
    }

    /* 月 */
    for (date->month = 0; mdays[date->month] < seconds; date->month++)
        seconds -= mdays[date->month];
    date->month++;

    /* 日 */
    date->day = seconds;
}

/* RTOFF 是 RTC 寄存器写完成标志。若 RTC 没有任何时钟源(既无LSE也无LSI),
 * 它永远不会置位, 原代码的裸 while 会永久卡死。加超时兜底。*/
static void rtc_wait_for_rtoff(void)
{
    uint32_t timeout = 0x400000UL;   /* 72MHz 下约 0.5 秒 */

    while (((RTC->CRL & RTC_CRL_RTOFF) == 0) && (--timeout != 0))
        ;
}

static void rtc_enter_config(void)
{
    rtc_wait_for_rtoff();
    RTC->CRL |= RTC_CRL_CNF;
}

static void rtc_exit_config(void)
{
    RTC->CRL &= ~RTC_CRL_CNF;
    rtc_wait_for_rtoff();
}

/* LSE 起振等待超时计数 (72MHz 下约 2.3 秒)
 * 原代码为无超时死等: 若板上未焊 32.768kHz 晶振或晶振不起振,
 * 程序会永久停在这里, 表现为屏幕停在 st7735_fill_screen 刷出的白屏。
 * LSE 正常起振只需几十~几百 ms, 只有异常时才会等满这个时限。*/
#define RTC_LSE_TIMEOUT   0x1000000UL

void rtc_init(void)
{
    uint32_t timeout;
    uint8_t  use_lsi = 0;

    /* 使能PWR和BKP时钟, 开启后备区写访问 */
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_RCC_BKP_CLK_ENABLE();
    PWR->CR |= PWR_CR_DBP;

    /* RTCSEL 一旦写入即被锁定, 只有复位备份域才能改;
     * 先把可能残留的旧配置清掉, 否则下面写 RTCSEL 无效。*/
    RCC->BDCR |= RCC_BDCR_BDRST;
    RCC->BDCR &= ~RCC_BDCR_BDRST;

    /* 使能LSE外部低速晶振, 带超时等待就绪 */
    RCC->BDCR |= RCC_BDCR_LSEON;
    timeout = RTC_LSE_TIMEOUT;
    while (((RCC->BDCR & RCC_BDCR_LSERDY) == 0) && (--timeout != 0))
        ;

    if (timeout == 0)
    {
        /* LSE 起振失败: 降级用内部 LSI(约40kHz), 保证程序继续运行。
         * 代价是走时精度差(LSI 偏差可达 ±50%), 仅作界面可用的兜底。
         *
         * 注意: 先把 LSI 振荡器打开并等稳定, 再切 RTCSEL。
         * 顺序反了(直接选LSI但LSION没开)会让 RTC 完全没有时钟源,
         * RTOFF 永不置位 -> 卡在 rtc_enter_config, 依旧白屏。*/
        RCC->CSR |= RCC_CSR_LSION;
        timeout = RTC_LSE_TIMEOUT;
        while (((RCC->CSR & RCC_CSR_LSIRDY) == 0) && (--timeout != 0))
            ;

        RCC->BDCR &= ~(RCC_BDCR_RTCSEL);
        RCC->BDCR |= RCC_BDCR_RTCSEL_1;   /* RTCSEL = 10 = LSI */
        use_lsi = 1;
    }
    else
    {
        RCC->BDCR &= ~(RCC_BDCR_RTCSEL);
        RCC->BDCR |= RCC_BDCR_RTCSEL_0;   /* RTCSEL = 01 = LSE */
    }

    /* 使能RTC */
    RCC->BDCR |= RCC_BDCR_RTCEN;

    /* 等待寄存器同步 (同样加超时, 避免二次死等) */
    RTC->CRL &= ~RTC_CRL_RSF;
    timeout = RTC_LSE_TIMEOUT;
    while (((RTC->CRL & RTC_CRL_RSF) == 0) && (--timeout != 0))
        ;

    /* 设置预分频器: LSE=32768Hz -> 32767; LSI≈40kHz -> 39999 */
    rtc_enter_config();
    RTC->PRLL = use_lsi ? 39999U : 32767U;
    rtc_exit_config();
}

void rtc_set_date(rtc_date_t *date)
{
    if (!date_validate(date))
        return;

    uint32_t ts = date_to_ts(date);

    rtc_enter_config();
    RTC->CNTH = (uint16_t)(ts >> 16);
    RTC->CNTL = (uint16_t)(ts & 0xFFFF);
    rtc_exit_config();
}

void rtc_get_date(rtc_date_t *date)
{
    /* 必须先读CNTH再读CNTL以保证一致性 */
    uint16_t hi = RTC->CNTH;
    uint16_t lo = RTC->CNTL;
    uint32_t ts = ((uint32_t)hi << 16) | lo;

    if (date)
    {
        ts_to_date(ts, date);
    }
}

void rtc_set_timestamp(uint32_t timestamp)
{
    rtc_enter_config();
    RTC->CNTH = (uint16_t)(timestamp >> 16);
    RTC->CNTL = (uint16_t)(timestamp & 0xFFFF);
    rtc_exit_config();
}

void rtc_get_timestamp(uint32_t *timestamp)
{
    if (timestamp)
    {
        uint16_t hi = RTC->CNTH;
        uint16_t lo = RTC->CNTL;
        *timestamp = ((uint32_t)hi << 16) | lo;
    }
}

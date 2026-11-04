#include "freertos_demo.h"

/* FreeRTOS头文件 */
#include "FreeRTOS.h"
#include "task.h"

/* 驱动与应用头文件 */
#include <string.h>
#include "st7735.h"
#include "stfonts.h"
#include "stimage.h"
#include "esp_at.h"
#include "rtc.h"
#include "mpu6050.h"
#include "weather.h"
#include "led.h"
#include "wifi_config.h"     /* WiFi 账号 / 天气 API key（本地私有，勿提交真实值） */

/* 调试: ESP32最后响应长度 */
extern uint32_t esp_at_last_rxlen;

/* ---------- 配置参数（真实值放在 wifi_config.h，勿提交） ---------- */
#define WIFI_SSID     WIFI_CFG_SSID
#define WIFI_PWD      WIFI_CFG_PWD
#define WEATHER_URI   "https://api.seniverse.com/v3/weather/now.json?key=" WEATHER_CFG_API_KEY \
                      "&location=" WEATHER_CFG_CITY "&language=en&unit=c"
#define CITY_NAME     "Guilin"

/* ---------- 配色方案1: 清爽白昼 (RGB565) ---------- */
#define COL_WHITE       0xFFFF
#define COL_BLACK       0x0000
#define COL_GRAY_LT     0xDEFB    /* 状态栏: 浅灰 */
#define COL_GRAY_DK     0x8410    /* 深灰文字 */
#define COL_INDOOR_BG   0x5C9F    /* 室内面板: 天蓝 */
#define COL_OUTDOOR_BG  0xFD20    /* 室外面板: 暖橙 */
#define COL_TEXT_W      0xFFFF    /* 面板内白字 */
#define COL_TIME_BLK    0x0000    /* 时间: 黑 */
#define COL_TEMP_HI     0xF800    /* 温度强调: 红 */

/* ---------- 布局坐标 (128x128) ---------- */
#define Y_STAT      0       /* 状态栏 y */
#define H_STAT      16
#define Y_TIME      16      /* 时间 y */
#define H_TIME      48
#define Y_DATE      64      /* 日期 y */
#define H_DATE      16
#define Y_PANEL     80      /* 双面板 y */
#define H_PANEL     48
#define W_PANEL     64

/* ---------- 任务参数 ---------- */
#define START_TASK_STACK    128
#define START_TASK_PRIORITY 3
#define UI_TASK_STACK       640
#define UI_TASK_PRIORITY    1

static TaskHandle_t start_task_handle;
static TaskHandle_t ui_task_handle;

static void start_task(void *pvParameters);
static void ui_task(void *pvParameters);

/* 全局状态 */
static uint8_t  g_wifi_ok = 0;          /* WiFi是否连接成功 */
static int16_t  g_out_temp_x10 = 999;   /* 室外温度x10, 999=未知 */
static char     g_out_weather[16] = "--";  /* 天气文字 */

/* ---------- 中文字体 16x16 (微软雅黑) ---------- */
static const uint16_t cn_font[][16] = {
    /* 室 */ {0x0000,0x0000,0x0180,0x3FFC,0x2004,0x1FF8,0x0620,0x0818,0x1FFC,0x0100,0x1FFC,0x0100,0x0100,0x3FFC,0x0000,0x0000},
    /* 内 */ {0x0000,0x0100,0x0100,0x3FF8,0x2108,0x2108,0x2308,0x22C8,0x2C68,0x3818,0x2008,0x2008,0x2078,0x0000,0x0000,0x0000},
    /* 湿 */ {0x0000,0x0000,0x33FC,0x1A04,0x03FC,0x2204,0x33FC,0x1204,0x0092,0x1494,0x1294,0x1198,0x2090,0x27FE,0x0000,0x0000},
    /* 度 */ {0x0000,0x0000,0x00C0,0x1FFE,0x1210,0x1FFC,0x1210,0x13F0,0x1000,0x17F8,0x1210,0x21A0,0x21E0,0x4E1E,0x0000,0x0000},
    /* 桂 */ {0x0000,0x0000,0x1020,0x1020,0x11FC,0x7C20,0x1020,0x3BFE,0x3C00,0x5420,0x53FC,0x1020,0x1020,0x13FE,0x0000,0x0000},
    /* 林 */ {0x0000,0x0000,0x0820,0x0820,0x3FFC,0x0820,0x0C70,0x1AB0,0x19A8,0x2928,0x2924,0x2A24,0x0822,0x0820,0x0000,0x0000},
};
enum { CN_SHI=0, CN_NEI=1, CN_SHI2=2, CN_DU=3, CN_GUI=4, CN_LIN=5 };

/** 绘制单个16x16中文字符 */
static void draw_cn_char(uint16_t x, uint16_t y, uint8_t idx, uint16_t color, uint16_t bg)
{
    const uint16_t *data = cn_font[idx];
    for (uint8_t row = 0; row < 16; row++)
    {
        uint16_t bits = data[row];
        for (uint8_t col = 0; col < 16; col++)
        {
            uint16_t c = (bits & (1 << (15 - col))) ? color : bg;
            st7735_draw_pixel(x + col, y + row, c);
        }
    }
}

/** 绘制中文词: 室内 / 湿度 */
static void draw_cn_word(uint16_t x, uint16_t y, uint8_t first, uint8_t second,
                         uint16_t color, uint16_t bg)
{
    draw_cn_char(x, y, first, color, bg);
    draw_cn_char(x + 16, y, second, color, bg);
}

/* ---------- 工具函数 ---------- */

/** 轻量非负整数转定宽字符串 */
static void u8_to_str(uint8_t val, char *buf, uint8_t width)
{
    uint8_t i = width;
    while (i > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
    buf[width] = '\0';
}

static void u16_to_str(uint16_t val, char *buf, uint8_t width)
{
    uint8_t i = width;
    while (i > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
    buf[width] = '\0';
}

/** 计算星期几 (0=Sun..6=Sat), 基于Zeller公式简化 */
static uint8_t calc_weekday(uint16_t year, uint8_t month, uint8_t day)
{
    if (month < 3) { month += 12; year--; }
    uint16_t k = year % 100;
    uint16_t j = year / 100;
    int32_t h = (day + (13 * (month + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    /* Zeller返回 0=Sat,1=Sun,...,6=Fri, 转为 0=Sun..6=Sat */
    return (uint8_t)((h + 6) % 7);
}

/** 绘制WiFi图标 (简单的信号弧) */
static void draw_wifi_icon(uint16_t x, uint16_t y, uint8_t connected)
{
    uint16_t col = connected ? 0x07E0 : 0xF800;  /* 绿=已连, 红=未连 */
    /* 三弧信号 */
    st7735_draw_pixel(x + 4, y + 8, col);
    st7735_draw_pixel(x + 3, y + 7, col);
    st7735_draw_pixel(x + 5, y + 7, col);
    st7735_draw_pixel(x + 2, y + 5, col);
    st7735_draw_pixel(x + 6, y + 5, col);
    st7735_draw_pixel(x + 1, y + 3, col);
    st7735_draw_pixel(x + 7, y + 3, col);
    if (connected) {
        st7735_draw_pixel(x + 4, y + 9, col);
    }
}

/** 2:1缩放绘制RGB565图片 (用于把48x48图标显示为24x24) */
static void draw_image_scaled(uint16_t x, uint16_t y,
                              const uint8_t *data, uint16_t w, uint16_t h)
{
    for (uint16_t py = 0; py < h; py += 2)
    {
        for (uint16_t px = 0; px < w; px += 2)
        {
            uint32_t idx = ((uint32_t)py * w + px) * 2;
            uint16_t color = ((uint16_t)data[idx] << 8) | data[idx + 1];
            st7735_draw_pixel(x + px / 2, y + py / 2, color);
        }
    }
}

/* ---------- FreeRTOS启动 ---------- */

void freertos_start(void)
{
    xTaskCreate((TaskFunction_t)start_task,
                (char *)"start_task",
                (configSTACK_DEPTH_TYPE)START_TASK_STACK,
                (void *)NULL,
                (UBaseType_t)START_TASK_PRIORITY,
                (TaskHandle_t *)&start_task_handle);

    vTaskStartScheduler();
}

static void start_task(void *pvParameters)
{
    (void)pvParameters;

    xTaskCreate((TaskFunction_t)ui_task,
                (char *)"ui_task",
                (configSTACK_DEPTH_TYPE)UI_TASK_STACK,
                (void *)NULL,
                (UBaseType_t)UI_TASK_PRIORITY,
                (TaskHandle_t *)&ui_task_handle);

    vTaskDelete(NULL);
}

/* ---------- UI绘制 ---------- */

/** 绘制静态框架 */
static void ui_draw_frame(void)
{
    /* 全屏白底 */
    st7735_fill_screen(COL_WHITE);
    /* 状态栏: 浅灰 */
    st7735_fill_rect(0, Y_STAT, 128, H_STAT, COL_GRAY_LT);
    /* 室内面板: 天蓝 */
    st7735_fill_rect(0, Y_PANEL, W_PANEL, H_PANEL, COL_INDOOR_BG);
    /* 室外面板: 暖橙 */
    st7735_fill_rect(W_PANEL, Y_PANEL, W_PANEL, H_PANEL, COL_OUTDOOR_BG);
    /* 面板分割线 */
    st7735_fill_rect(W_PANEL - 1, Y_PANEL, 2, H_PANEL, COL_WHITE);
}

/** 更新状态栏: WiFi图标 + SSID + 桂林(末尾) */
static void ui_update_statusbar(void)
{
    st7735_fill_rect(0, Y_STAT, 128, H_STAT, COL_GRAY_LT);
    /* WiFi图标 (8x8) */
    draw_wifi_icon(2, Y_STAT + 4, g_wifi_ok);
    /* SSID */
    st7735_write_string(12, Y_STAT, g_wifi_ok ? WIFI_SSID : "NoWiFi",
                        &font_ascii_8x16, COL_GRAY_DK, COL_GRAY_LT);
    /* 桂林 放在行尾 (2字x16=32px, x=128-32=96) */
    draw_cn_word(96, Y_STAT, CN_GUI, CN_LIN, COL_GRAY_DK, COL_GRAY_LT);
}

/** 联网初始化 (显示详细进度) */
static uint8_t ui_init_network(void)
{
    st7735_fill_rect(0, 64, 128, 16, COL_WHITE);
    st7735_write_string(2, 64, "1/4 Init ESP...",
                        &font_ascii_8x16, COL_GRAY_DK, COL_WHITE);

    if (!esp_at_init())
    {
        char dbg[24];
        uint8_t i = 0;
        const char *s = "ESP NO RESP! rx=";
        while (*s) dbg[i++] = *s++;
        u16_to_str((uint16_t)esp_at_last_rxlen, dbg + i, 4);
        dbg[i + 4] = '\0';
        st7735_write_string(2, 64, dbg,
                            &font_ascii_8x16, COL_TEMP_HI, COL_WHITE);
        return 0;
    }

    st7735_fill_rect(0, 64, 128, 16, COL_WHITE);
    st7735_write_string(2, 64, "2/4 STA Mode...",
                        &font_ascii_8x16, COL_GRAY_DK, COL_WHITE);

    if (!esp_at_wifi_init())
    {
        st7735_write_string(2, 64, "STA Mode Fail!    ",
                            &font_ascii_8x16, COL_TEMP_HI, COL_WHITE);
        return 0;
    }

    st7735_fill_rect(0, 64, 128, 16, COL_WHITE);
    char connmsg[40];
    uint8_t i = 0;
    connmsg[i++] = '3'; connmsg[i++] = '/'; connmsg[i++] = '4'; connmsg[i++] = ' ';
    const char *s = WIFI_SSID;
    while (*s && i < 30) connmsg[i++] = *s++;
    connmsg[i++] = '.'; connmsg[i++] = '.'; connmsg[i++] = '.';
    connmsg[i] = '\0';
    st7735_write_string(2, 64, connmsg,
                        &font_ascii_8x16, COL_GRAY_DK, COL_WHITE);

    if (!esp_at_wifi_connect(WIFI_SSID, WIFI_PWD))
    {
        char dbg[24];
        uint8_t i = 0;
        const char *s = "WiFi FAIL rx=";
        while (*s) dbg[i++] = *s++;
        u16_to_str((uint16_t)esp_at_last_rxlen, dbg + i, 4);
        dbg[i + 4] = '\0';
        st7735_write_string(2, 64, dbg,
                            &font_ascii_8x16, COL_TEMP_HI, COL_WHITE);
        return 0;
    }

    st7735_fill_rect(0, 64, 128, 16, COL_WHITE);
    st7735_write_string(2, 64, "4/4 SNTP Sync...",
                        &font_ascii_8x16, COL_GRAY_DK, COL_WHITE);

    if (!esp_at_sntp_init())
    {
        st7735_write_string(2, 64, "SNTP Fail(cont)   ",
                            &font_ascii_8x16, 0xFD20, COL_WHITE);
    }
    else
    {
        uint32_t ts = 0;
        if (esp_at_get_time(&ts))
        {
            rtc_set_timestamp(ts + 8 * 60 * 60);
        }
    }

    g_wifi_ok = 1;
    st7735_fill_rect(0, 64, 128, 16, COL_WHITE);
    return 1;
}

/** 更新日期 + 星期 */
static void ui_update_date(void)
{
    rtc_date_t date;
    rtc_get_date(&date);

    static const char *wday[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    uint8_t wd = calc_weekday(date.year, date.month, date.day);

    char str[24];
    u16_to_str((uint16_t)date.year, str, 4);
    str[4] = '-';
    u8_to_str(date.month, str + 5, 2);
    str[7] = '-';
    u8_to_str(date.day, str + 8, 2);
    str[10] = ' ';
    str[11] = wday[wd][0];
    str[12] = wday[wd][1];
    str[13] = wday[wd][2];
    str[14] = '\0';

    st7735_fill_rect(0, Y_DATE, 128, H_DATE, COL_WHITE);
    st7735_write_string(8, Y_DATE, str, &font_ascii_8x16, COL_GRAY_DK, COL_WHITE);
}

/** 更新中央大时间 (白底黑字, 冒号闪烁) */
static void ui_update_time(uint8_t blink)
{
    rtc_date_t date;
    rtc_get_date(&date);

    char str[8];
    u8_to_str(date.hour, str, 2);
    str[2] = blink ? ':' : ' ';
    u8_to_str(date.minute, str + 3, 2);
    str[5] = '\0';

    st7735_fill_rect(0, Y_TIME, 128, H_TIME, COL_WHITE);
    /* 5字符x24=120px, 居中x=4 */
    st7735_write_string(4, Y_TIME, str, &font_time_24x48, COL_TIME_BLK, COL_WHITE);
}

/** 更新室外天气 (右侧橙色面板) */
static void ui_update_weather(void)
{
    const char *rsp = NULL;
    if (esp_at_get_http(WEATHER_URI, &rsp, NULL, 10000))
    {
        weather_t weather;
        if (weather_parse(rsp, &weather))
        {
            /* 解析温度 */
            int16_t t = 0;
            uint8_t i = 0;
            while (weather.temperature[i] >= '0' && weather.temperature[i] <= '9' && i < 3)
            {
                t = t * 10 + (weather.temperature[i] - '0');
                i++;
            }
            g_out_temp_x10 = t * 10;
            strncpy(g_out_weather, weather.weather, 15);
            g_out_weather[15] = '\0';
        }
    }

    /* 擦除右面板 */
    st7735_fill_rect(W_PANEL, Y_PANEL, W_PANEL, H_PANEL, COL_OUTDOOR_BG);

    /* 天气图标 (48x48缩放到24x24, 居中) */
    const st_image_t *img = NULL;
    if (strcmp(g_out_weather, "Cloudy") == 0)        img = &icon_weather_duoyun;
    else if (strcmp(g_out_weather, "Wind") == 0)     img = &icon_weather_feng;
    else if (strcmp(g_out_weather, "Clear") == 0)    img = &icon_weather_qing;
    else if (strcmp(g_out_weather, "Sunny") == 0)    img = &icon_weather_qing;
    else if (strcmp(g_out_weather, "Snow") == 0)     img = &icon_weather_xue;
    else if (strcmp(g_out_weather, "Overcast") == 0) img = &icon_weather_yin;
    else if (strcmp(g_out_weather, "Rain") == 0)     img = &icon_weather_yu;

    if (img)
        draw_image_scaled(W_PANEL + 20, Y_PANEL, img->data, img->width, img->height);

    /* 室外温度 (ASCII字体, 带°C) */
    char tstr[10];
    if (g_out_temp_x10 != 999)
    {
        uint16_t val = (uint16_t)(g_out_temp_x10 / 10);
        u8_to_str((uint8_t)val, tstr, 2);
        tstr[2] = (char)0xDF;  /* ° */
        tstr[3] = 'C';
        tstr[4] = '\0';
    }
    else
    {
        tstr[0] = '-'; tstr[1] = '-'; tstr[2] = (char)0xDF; tstr[3] = 'C'; tstr[4] = '\0';
    }
    /* "28°C" 5字符x8=40px, 居中 x=64+(64-40)/2=76 */
    st7735_write_string(W_PANEL + 12, Y_PANEL + 28, tstr,
                        &font_ascii_8x16, COL_TEXT_W, COL_OUTDOOR_BG);
}

/** 更新室内信息 (左侧蓝色面板) */
static void ui_update_indoor(void)
{
    float temper = mpu6050_read_temper();

    /* 擦除左面板 */
    st7735_fill_rect(0, Y_PANEL, W_PANEL, H_PANEL, COL_INDOOR_BG);

    /* 标签: 室内 (中文) */
    draw_cn_word(4, Y_PANEL, CN_SHI, CN_NEI, COL_TEXT_W, COL_INDOOR_BG);

    /* 室内温度 (带小数) */
    int16_t t_int = (int16_t)(temper * 10);
    uint8_t neg = 0;
    if (t_int < 0) { neg = 1; t_int = -t_int; }

    char str[12];
    uint8_t i = 0;
    if (neg) str[i++] = '-';
    uint16_t val = (uint16_t)(t_int / 10);
    if (val >= 100) str[i++] = '0' + (val / 100) % 10;
    if (val >= 10)  str[i++] = '0' + (val / 10) % 10;
    str[i++] = '0' + val % 10;
    str[i++] = '.';
    str[i++] = '0' + (t_int % 10);
    str[i++] = (char)0xDF;  /* ° */
    str[i++] = 'C';
    str[i] = '\0';
    st7735_write_string(4, Y_PANEL + 16, str,
                        &font_ascii_8x16, COL_TEXT_W, COL_INDOOR_BG);

    /* 湿度 (中文标签: 湿度:--%) */
    draw_cn_word(4, Y_PANEL + 32, CN_SHI2, CN_DU, COL_TEXT_W, COL_INDOOR_BG);
    st7735_write_string(36, Y_PANEL + 32, ":--%",
                        &font_ascii_8x16, COL_TEXT_W, COL_INDOOR_BG);
}

/* ---------- UI主任务 ---------- */

static void ui_task(void *pvParameters)
{
    (void)pvParameters;

    led_init();
    led_on();

    st7735_init();
    st7735_fill_screen(COL_WHITE);

    rtc_init();
    mpu6050_init();

    /* 先画框架和状态栏(WiFi未连接) */
    ui_draw_frame();
    ui_update_statusbar();
    ui_update_date();
    ui_update_time(1);
    ui_update_indoor();
    ui_update_weather();

    /* 联网 (失败也继续) */
    ui_init_network();

    /* 联网后刷新状态栏和天气 */
    ui_update_statusbar();
    if (g_wifi_ok) ui_update_weather();

    uint32_t tick = 0;
    uint8_t blink = 1;
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
        tick++;
        blink = !blink;

        ui_update_time(blink);

        if (tick % 4 == 0)              /* 每2s: 室内 */
            ui_update_indoor();

        if (tick % 120 == 0)            /* 每60s: 日期 */
            ui_update_date();

        if (tick % 1200 == 0)           /* 每10min: 天气+对时 */
        {
            ui_update_weather();
            uint32_t ts = 0;
            if (esp_at_get_time(&ts))
                rtc_set_timestamp(ts + 8 * 60 * 60);
        }

        if (blink) led_on();
        else       led_off();
    }
}

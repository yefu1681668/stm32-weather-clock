#include "freertos_demo.h"

/* FreeRTOS头文件 */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

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
#include "delay.h"
#include "wifi_config.h"     /* WiFi 账号 / 天气 API key（本地私有，勿提交真实值） */

/* 调试: ESP32最后响应长度 */
extern uint32_t esp_at_last_rxlen;

/* ---------- 配置参数（真实值放在 wifi_config.h，勿提交） ---------- */
#define WIFI_SSID     WIFI_CFG_SSID
#define WIFI_PWD      WIFI_CFG_PWD
#define WEATHER_URI   "https://api.seniverse.com/v3/weather/now.json?key=" WEATHER_CFG_API_KEY \
                      "&location=" WEATHER_CFG_CITY "&language=en&unit=c"
/* 湿度只能从 3 天预报接口拿: 实况(now)里只有 text/code/temperature, 没有 humidity。
 * daily.json 的每一天里都有 "humidity":"50" 字段, 取第一条(今天)即可。
 *
 * 这里故意把 language/unit/start/days 全省掉, 只用默认值(中文/摄氏/今天起3天),
 * 因为 esp_at_get_http() 里拼 AT 指令的缓冲区是 char cmd[128]:
 *   "AT+HTTPCGET=\"...\"" = 15 + URL 长度
 * 完整写法要 130 字节, 会溢出 2 字节; 省掉可选参数后只有 100 字节, 安全。 */
#define WEATHER_HUMI_URI "https://api.seniverse.com/v3/weather/daily.json?key=" WEATHER_CFG_API_KEY \
                      "&location=" WEATHER_CFG_CITY
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

/* ---------- 任务参数 ----------
 * 优先级: start(4) > ui(3) > net(2) > heartbeat(1)
 *   ui 最高 —— 时间刷新必须准时, 不能被 HTTP 的 10s 忙等拖住;
 *   net 次之 —— 网络慢只拖自己, 队列解耦后不会冻屏;
 *   heartbeat 最低 —— 纯指示, 偶尔抢不到 CPU 也无妨。
 * 三个任务栈都从已有 ucHeap(8KB) 分配, 不新增静态 RAM。 */
#define START_TASK_STACK        128
#define START_TASK_PRIORITY     4
#define UI_TASK_STACK           640
#define UI_TASK_PRIORITY        3
#define NET_TASK_STACK          384
#define NET_TASK_PRIORITY       2
#define HEARTBEAT_TASK_STACK    128
#define HEARTBEAT_TASK_PRIORITY 1

/* ---------- 任务间通信 ---------- */
typedef enum {
    MSG_WIFI_STATUS = 0,   /* WiFi 状态变化 -> 重画状态栏 */
    MSG_WEATHER,           /* 实况天气 + 气温 -> 重画右面板 */
    MSG_HUMIDITY,          /* 当天湿度 -> 重画左面板 */
    MSG_NET_DONE           /* 联网流程结束 -> 补画被进度行擦掉的日期 */
} msg_type_t;

typedef struct {
    uint8_t  type;
    uint8_t  wifi_ok;
    uint8_t  humidity;
    int16_t  temp_x10;
    char     weather[16];
} weather_msg_t;

static TaskHandle_t start_task_handle;
static TaskHandle_t ui_task_handle;
static TaskHandle_t net_task_handle;
static TaskHandle_t heartbeat_task_handle;

static QueueHandle_t     g_weather_q;  /* net -> ui, 深度 4 */
static SemaphoreHandle_t g_lcd_mutex;  /* 保护 ST7735: net 画进度 / ui 画界面 */
static SemaphoreHandle_t g_lcd_ready;  /* 二值信号量: 屏初始化完成才放行 net */

static void start_task(void *pvParameters);
static void ui_task(void *pvParameters);
static void net_task(void *pvParameters);
static void heartbeat_task(void *pvParameters);

/* 全局状态: 只由 ui_task 读写; net_task 一律通过队列传快照, 无需加锁 */
static uint8_t  g_wifi_ok = 0;          /* WiFi是否连接成功 */
static int16_t  g_out_temp_x10 = 999;   /* 室外温度x10, 999=未知 */
static char     g_out_weather[16] = "--";  /* 天气文字 */
static uint8_t  g_humidity = 0xFF;      /* 湿度%, 0xFF=未知(接口没给) */

/* 栈水位历史最小值, 供调试器观察 (heartbeat_task 每 20s 刷新) */
static volatile UBaseType_t g_stack_min_ui  = 0xFFFF;
static volatile UBaseType_t g_stack_min_net = 0xFFFF;

/* ---------- LCD 互斥访问 ---------- */
static void lcd_lock(void)
{
    xSemaphoreTake(g_lcd_mutex, portMAX_DELAY);
}

static void lcd_unlock(void)
{
    xSemaphoreGive(g_lcd_mutex);
}

/** 开机/联网进度行: 由 net_task 调用, 每次绘制只短暂持有 LCD 互斥量。
 *  绝不把整段联网流程锁在里面, 否则 ui_task 会被饿死, 又变回冻屏。 */
static void net_progress(const char *line, uint16_t color)
{
    lcd_lock();
    st7735_fill_rect(0, Y_DATE, 128, H_DATE, COL_WHITE);
    if (line != NULL)
        st7735_write_string(2, Y_DATE, line, &font_ascii_8x16, color, COL_WHITE);
    lcd_unlock();
}

/* ---------- 中文字体 16x16 (微软雅黑) ---------- */
static const uint16_t cn_font[][16] = {
    /* 室 */ {0x0000,0x0000,0x0180,0x3FFC,0x2004,0x1FF8,0x0620,0x0818,0x1FFC,0x0100,0x1FFC,0x0100,0x0100,0x3FFC,0x0000,0x0000},
    /* 内 */ {0x0000,0x0100,0x0100,0x3FF8,0x2108,0x2108,0x2308,0x22C8,0x2C68,0x3818,0x2008,0x2008,0x2078,0x0000,0x0000,0x0000},
    /* 湿 */ {0x0000,0x0000,0x33FC,0x1A04,0x03FC,0x2204,0x33FC,0x1204,0x0092,0x1494,0x1294,0x1198,0x2090,0x27FE,0x0000,0x0000},
    /* 度 */ {0x0000,0x0000,0x00C0,0x1FFE,0x1210,0x1FFC,0x1210,0x13F0,0x1000,0x17F8,0x1210,0x21A0,0x21E0,0x4E1E,0x0000,0x0000},
    /* 桂 */ {0x0000,0x0000,0x1020,0x1020,0x11FC,0x7C20,0x1020,0x3BFE,0x3C00,0x5420,0x53FC,0x1020,0x1020,0x13FE,0x0000,0x0000},
    /* 林 */ {0x0000,0x0000,0x0820,0x0820,0x3FFC,0x0820,0x0C70,0x1AB0,0x19A8,0x2928,0x2924,0x2A24,0x0822,0x0820,0x0000,0x0000},
    /* 气 */ {0x0000,0x0400,0x0400,0x0FFE,0x1800,0x17FC,0x1000,0x0000,0x1FF8,0x0008,0x0008,0x000C,0x000D,0x0007,0x0007,0x0000},
    /* 温 */ {0x0000,0x0000,0x19FC,0x0904,0x01FC,0x2104,0x1904,0x01FC,0x0000,0x0BFE,0x1ADA,0x1252,0x1252,0x3252,0x2FFF,0x0000},
};
enum { CN_SHI=0, CN_NEI=1, CN_SHI2=2, CN_DU=3, CN_GUI=4, CN_LIN=5,
       CN_QI=6, CN_WEN=7 };

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

/** 解析 ESP 返回的 "+CIPSNTPTIME:Mon Nov  9 18:10:35 2026"
 *
 *  重要: ESP-AT 的 CIPSNTPTIME 返回的【已经是加过时区的本地时间】,
 *        不能再手动 +8 小时。
 *
 *  原代码用 AT+SYSTIMESTAMP? 取 Unix 时间戳, 但精简版 AT 固件不支持
 *  这条命令(回 ERROR), 导致解析出 ts=0, RTC 被设成 1970-01-01 08:00,
 *  屏幕上就会看到"08:10"这种明显不对的时间。改用本函数后时间才正确。*/
static uint8_t parse_sntp_time(const char *rsp, rtc_date_t *d)
{
    const char *p = strstr(rsp, "+CIPSNTPTIME:");
    if (!p)
        return 0;
    p += 13;                            /* 跳过 "+CIPSNTPTIME:" */

    while (*p == ' ') p++;              /* 跳过前导空格 */
    while (*p && *p != ' ') p++;        /* 跳过星期 "Mon" */
    while (*p == ' ') p++;              /* 定位到月份 */

    static const char mons[12][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    uint8_t mon = 0;
    for (uint8_t i = 0; i < 12; i++)
    {
        if (p[0] == mons[i][0] && p[1] == mons[i][1] && p[2] == mons[i][2])
        {
            mon = i + 1;
            break;
        }
    }
    if (!mon)
        return 0;
    p += 3;

    while (*p == ' ') p++;              /* 日 */
    uint8_t day = 0;
    while (*p >= '0' && *p <= '9') { day = day * 10 + (*p - '0'); p++; }

    while (*p == ' ') p++;              /* 时 */
    uint8_t hh = 0;
    while (*p >= '0' && *p <= '9') { hh = hh * 10 + (*p - '0'); p++; }
    if (*p != ':') return 0;
    p++;
    uint8_t mi = 0;                     /* 分 */
    while (*p >= '0' && *p <= '9') { mi = mi * 10 + (*p - '0'); p++; }
    if (*p != ':') return 0;
    p++;
    uint8_t ss = 0;                     /* 秒 */
    while (*p >= '0' && *p <= '9') { ss = ss * 10 + (*p - '0'); p++; }

    while (*p == ' ') p++;              /* 年 */
    uint16_t yy = 0;
    while (*p >= '0' && *p <= '9') { yy = yy * 10 + (*p - '0'); p++; }

    /* 合理性检查: 解析失败时宁可不改 RTC, 也不要把时间写成乱值 */
    if (yy < 2020 || yy > 2099 || mon > 12 || day == 0 || day > 31 ||
        hh > 23 || mi > 59 || ss > 59)
        return 0;

    d->year   = yy;
    d->month  = mon;
    d->day    = day;
    d->hour   = hh;
    d->minute = mi;
    d->second = ss;
    return 1;
}

/** 向 ESP 要一次网络时间并写入 RTC, 成功返回 1 */
static uint8_t sync_time_from_esp(void)
{
    const char *rsp = NULL;
    if (!esp_at_send_command("AT+CIPSNTPTIME?", &rsp, NULL, 1500))
        return 0;

    rtc_date_t d;
    if (!parse_sntp_time(rsp, &d))
        return 0;

    rtc_set_date(&d);
    return 1;
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

/** 2:1缩放绘制RGB565图片 (用于把48x48图标显示为24x24)
 *
 *  两个关键点(原来都写错了):
 *  ① 图标数据是【小端】存放的: 先低字节、后高字节。
 *     原来按大端拼接, 晴天的太阳被画成青色、云被画成荧光绿, 颜色全乱。
 *  ② 图标背景是纯黑 0x0000, 那不是颜色而是"透明"。
 *     原来照抄着画, 屏上就出现一个 24x24 的黑方块贴在橙色面板上。
 *     这里遇到纯黑直接跳过, 让下面已经刷好的面板底色透出来。
 *     (图标自身的描边是深藏青/深灰, 不是纯黑, 不会被误删。) */
static void draw_image_scaled(uint16_t x, uint16_t y,
                              const uint8_t *data, uint16_t w, uint16_t h)
{
    for (uint16_t py = 0; py < h; py += 2)
    {
        for (uint16_t px = 0; px < w; px += 2)
        {
            uint32_t idx = ((uint32_t)py * w + px) * 2;
            uint16_t color = ((uint16_t)data[idx + 1] << 8) | data[idx];
            if (color == 0x0000)        /* 纯黑 = 透明, 保留面板底色 */
                continue;
            st7735_draw_pixel(x + px / 2, y + py / 2, color);
        }
    }
}

/* ---------- FreeRTOS启动 ---------- */

void freertos_start(void)
{
    /* 三个同步对象必须在调度器启动前创建(此时还不能用任务间 API)。
     * 队列深度 6: 开机时 net_task 会连发 WiFi状态/联网完成/天气/湿度 4 条,
     * 留 2 格余量, ui_task 来得及逐条消化, 不会溢出。 */
    g_weather_q = xQueueCreate(6, sizeof(weather_msg_t));
    g_lcd_mutex = xSemaphoreCreateMutex();
    g_lcd_ready = xSemaphoreCreateBinary();   /* 初值 0: UI 初始化完屏才 give */

    if (g_weather_q == NULL || g_lcd_mutex == NULL || g_lcd_ready == NULL)
    {
        /* ucHeap 不够: 快闪报警且不启动调度器, 避免带着空句柄跑飞 */
        led_init();
        while (1)
        {
            led_on();  delay_ms(100);
            led_off(); delay_ms(100);
        }
    }

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

    /* ===== 心跳1: 进入调度器后会慢闪 3 次(300ms 亮/灭) =====
     * 完全不闪 => 程序根本没跑起来(没烧进去 / 时钟或HardFault),
     *             与屏幕、接线无关, 应先查烧录。
     * 慢闪3次后停住 => 调度器在跑, 但下面创建任务失败(堆不足)。*/
    led_init();
    for (uint8_t i = 0; i < 3; i++)
    {
        led_on();
        vTaskDelay(pdMS_TO_TICKS(300));
        led_off();
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    xTaskCreate((TaskFunction_t)ui_task,
                (char *)"ui_task",
                (configSTACK_DEPTH_TYPE)UI_TASK_STACK,
                (void *)NULL,
                (UBaseType_t)UI_TASK_PRIORITY,
                (TaskHandle_t *)&ui_task_handle);

    xTaskCreate((TaskFunction_t)net_task,
                (char *)"net_task",
                (configSTACK_DEPTH_TYPE)NET_TASK_STACK,
                (void *)NULL,
                (UBaseType_t)NET_TASK_PRIORITY,
                (TaskHandle_t *)&net_task_handle);

    xTaskCreate((TaskFunction_t)heartbeat_task,
                (char *)"heartbeat_task",
                (configSTACK_DEPTH_TYPE)HEARTBEAT_TASK_STACK,
                (void *)NULL,
                (UBaseType_t)HEARTBEAT_TASK_PRIORITY,
                (TaskHandle_t *)&heartbeat_task_handle);

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

/** 联网初始化 (显示详细进度) —— 由 net_task 调用
 *  进度绘制统一走 net_progress(): 每次只短暂持有 LCD 互斥量,
 *  联网的几十秒里 ui_task 依然能抢占画时间。 */
static uint8_t net_init_network(void)
{
    net_progress("1/4 Init ESP...", COL_GRAY_DK);

    if (!esp_at_init())
    {
        /* 提示必须够短: 8x16 字体下 128px 只能放 16 个字符,
         * "ESP rx=" 占 7 个, 后面 4 位数字是最关键的诊断信息。*/
        char dbg[16];
        uint8_t i = 0;
        const char *s = "ESP rx=";
        while (*s) dbg[i++] = *s++;
        u16_to_str((uint16_t)esp_at_last_rxlen, dbg + i, 4);
        dbg[i + 4] = '\0';
        net_progress(dbg, COL_TEMP_HI);
        return 0;
    }

    net_progress("2/4 STA Mode...", COL_GRAY_DK);
    if (!esp_at_wifi_init())
    {
        net_progress("STA Mode Fail!    ", COL_TEMP_HI);
        return 0;
    }

    char connmsg[40];
    uint8_t k = 0;
    connmsg[k++] = '3'; connmsg[k++] = '/'; connmsg[k++] = '4'; connmsg[k++] = ' ';
    const char *ss = WIFI_SSID;
    while (*ss && k < 30) connmsg[k++] = *ss++;
    connmsg[k++] = '.'; connmsg[k++] = '.'; connmsg[k++] = '.';
    connmsg[k] = '\0';
    net_progress(connmsg, COL_GRAY_DK);

    if (!esp_at_wifi_connect(WIFI_SSID, WIFI_PWD))
    {
        char dbg[16];
        uint8_t i = 0;
        const char *s = "WIFI rx=";
        while (*s) dbg[i++] = *s++;
        u16_to_str((uint16_t)esp_at_last_rxlen, dbg + i, 4);
        dbg[i + 4] = '\0';
        net_progress(dbg, COL_TEMP_HI);
        return 0;
    }

    net_progress("4/4 SNTP Sync...", COL_GRAY_DK);
    if (!esp_at_sntp_init())
    {
        net_progress("SNTP Fail(cont)   ", 0xFD20);
    }
    else
    {
        /* CIPSNTPTIME 返回的已经是本地时间, 绝对不能再 +8 小时,
         * 否则解析失败会写成 1970-01-01 的假时间。*/
        sync_time_from_esp();
    }

    net_progress(NULL, COL_WHITE);   /* 擦掉进度行 */
    return 1;                       /* WiFi 状态由 net_task 发队列消息给 UI */
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

/** 取实况天气: 只做 HTTP + 解析, 不碰屏幕 (由 net_task 调用)。
 *  esp_at_get_http() 最长阻塞 10s, 但它跑在低优先级任务里,
 *  ui_task 依然能抢占刷新时间 —— 这正是拆任务要解决的冻屏问题。 */
static uint8_t net_fetch_weather(weather_msg_t *msg)
{
    const char *rsp = NULL;
    msg->type = MSG_WEATHER;
    msg->temp_x10 = 999;
    strncpy(msg->weather, "--", 15);
    msg->weather[15] = '\0';

    if (!esp_at_get_http(WEATHER_URI, &rsp, NULL, 10000))
        return 0;

    weather_t weather;
    if (!weather_parse(rsp, &weather))
        return 0;

    int16_t t = 0;
    uint8_t i = 0;
    while (weather.temperature[i] >= '0' && weather.temperature[i] <= '9' && i < 3)
    {
        t = t * 10 + (weather.temperature[i] - '0');
        i++;
    }
    msg->temp_x10 = t * 10;
    strncpy(msg->weather, weather.weather, 15);
    msg->weather[15] = '\0';
    return 1;
}

/** 刷新右侧橙色面板: 只画屏, 数据来自 ui_task 的缓存 (只由 ui_task 调用) */
static void ui_update_weather(void)
{
    /* 擦除右面板 */
    st7735_fill_rect(W_PANEL, Y_PANEL, W_PANEL, H_PANEL, COL_OUTDOOR_BG);

    /* 中文标签"气温" (32px, 居中: x = 64 + (64-32)/2 = 80)
     * 垂直账: 标签16 + 图标24 = 40px, 面板48px, 所以上下各留4px */
    draw_cn_word(W_PANEL + 16, Y_PANEL + 4, CN_QI, CN_WEN,
                 COL_TEXT_W, COL_OUTDOOR_BG);

    /* 天气图标 (48x48 缩放到 24x24, 放在标签下方左侧) */
    const st_image_t *img = NULL;
    if (strcmp(g_out_weather, "Cloudy") == 0)        img = &icon_weather_duoyun;
    else if (strcmp(g_out_weather, "Wind") == 0)     img = &icon_weather_feng;
    else if (strcmp(g_out_weather, "Clear") == 0)    img = &icon_weather_qing;
    else if (strcmp(g_out_weather, "Sunny") == 0)    img = &icon_weather_qing;
    else if (strcmp(g_out_weather, "Snow") == 0)     img = &icon_weather_xue;
    else if (strcmp(g_out_weather, "Overcast") == 0) img = &icon_weather_yin;
    else if (strcmp(g_out_weather, "Rain") == 0)     img = &icon_weather_yu;

    if (img)
        draw_image_scaled(W_PANEL + 2, Y_PANEL + 20, img->data, img->width, img->height);

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
    /* 4字符x8=32px。
     * 有图标时: 图标占 x=66~89, 文字放 93~124;
     * 无图标时: 文字直接居中 x = 64 + (64-32)/2 = 80。
     * 垂直方向与 24px 高的图标居中对齐: y = 80+20 + (24-16)/2 = 104 */
    st7735_write_string(img ? (W_PANEL + 29) : (W_PANEL + 16),
                        Y_PANEL + 24, tstr,
                        &font_ascii_8x16, COL_TEXT_W, COL_OUTDOOR_BG);
}

/** 更新左下角面板 —— 显示"湿度"
 *
 *  原来这里还有"室内 xx.x°C"，但板子上并没有环境温度传感器，
 *  那个值是 MPU6050 的【芯片结温】(通常比室温高 10℃ 以上)，显示出来是错的，
 *  所以整块去掉；面板改成只放中文标签"湿度"。
 *
 *  ★ 湿度值从哪来: 板子上没有湿度传感器(DHT11 之类都没有)，
 *    这个数是天气接口返回的【城市湿度】(心知天气 now.humidity)，
 *    也就是桂林市的室外湿度，不是室内实测。
 *    取不到时 g_humidity 保持 0xFF，显示 "--%"。
 *
 *  布局 (面板 64x48, y=80~127):
 *    y=88   湿度   32px 居中, x=16
 *    y=104  41%    24px 居中, x=20  ("100%" 时32px, x=16)
 */
static void ui_update_indoor(void)
{
    char hstr[6];

    if (g_humidity == 0xFF)
    {
        hstr[0] = '-'; hstr[1] = '-'; hstr[2] = '%'; hstr[3] = '\0';
    }
    else
    {
        uint8_t i = 0;
        if (g_humidity >= 100) hstr[i++] = '0' + (g_humidity / 100) % 10;
        if (g_humidity >= 10)  hstr[i++] = '0' + (g_humidity / 10) % 10;
        hstr[i++] = '0' + (g_humidity % 10);
        hstr[i++] = '%';
        hstr[i] = '\0';
    }

    /* 擦除左面板 */
    st7735_fill_rect(0, Y_PANEL, W_PANEL, H_PANEL, COL_INDOOR_BG);

    /* 中文标签: 湿度 */
    draw_cn_word(16, Y_PANEL + 8, CN_SHI2, CN_DU, COL_TEXT_W, COL_INDOOR_BG);

    /* 湿度数值: 按字符数居中 (3字符=24px -> x=20; 4字符=32px -> x=16) */
    uint8_t n = 0;
    while (hstr[n]) n++;
    st7735_write_string((uint16_t)((W_PANEL - (uint16_t)n * 8) / 2),
                        Y_PANEL + 24, hstr,
                        &font_ascii_8x16, COL_TEXT_W, COL_INDOOR_BG);
}

/** 单独取一次湿度 (心知 daily.json), 结果经队列交给 ui_task 刷新左面板
 *
 *  为什么要单独请求: 实况接口 now.json 只返回 text/code/temperature,
 *  没有湿度字段; 湿度在 3 天预报接口里。
 *
 *  调用频率: 每小时一次(不是每 10 分钟), 因为免费 key 每天只有 400 次调用额度,
 *  now.json 每 10 分钟一次 = 144 次/天, daily.json 每小时一次 = 24 次/天,
 *  合计 168 次, 留足余量。
 *
 *  取不到就保持上一次的值(或 --%), 不会把界面刷成空白。 */
static uint8_t net_fetch_humidity(uint8_t *humi)
{
    const char *rsp = NULL;
    uint8_t h = 0;
    if (!esp_at_get_http(WEATHER_HUMI_URI, &rsp, NULL, 10000))
        return 0;
    if (!weather_parse_humidity(rsp, &h))
        return 0;
    *humi = h;
    return 1;
}

/* ---------- 三个任务的实现 ---------- */

/** 网络任务 (优先级 2): 联网、对时、周期取天气/湿度。
 *  所有耗时操作都关在这里, 结果通过队列发给 ui_task。
 *  即使某次 HTTP 卡满 10s, ui_task 与 heartbeat_task 照常运行。 */
static void net_task(void *pvParameters)
{
    (void)pvParameters;
    weather_msg_t msg;
    uint8_t humi = 0;

    /* 等 ui_task 完成 st7735_init()/rtc_init() 后再碰屏幕,
     * 否则 net_progress() 会往一个还没初始化的 LCD 写数据。 */
    xSemaphoreTake(g_lcd_ready, portMAX_DELAY);

    uint8_t wifi_ok = net_init_network();

    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_WIFI_STATUS;
    msg.wifi_ok = wifi_ok;
    xQueueSend(g_weather_q, &msg, 0);

    /* 进度行画在日期区, 结束时擦掉了日期, 通知 UI 补画一次 */
    msg.type = MSG_NET_DONE;
    xQueueSend(g_weather_q, &msg, 0);

    if (wifi_ok)
    {
        if (net_fetch_weather(&msg))    /* 失败就不发, UI 保持上一次的值 */
            xQueueSend(g_weather_q, &msg, 0);

        if (net_fetch_humidity(&humi))
        {
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_HUMIDITY;
            msg.humidity = humi;
            xQueueSend(g_weather_q, &msg, 0);
        }
    }

    /* 1s 节拍: 10min 刷天气+对时, 60min 刷湿度(按免费额度控制频率) */
    uint32_t sec = 0;
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!wifi_ok) continue;
        sec++;

        if (sec % 600 == 0)             /* 每 10 min */
        {
            if (net_fetch_weather(&msg))
                xQueueSend(g_weather_q, &msg, 0);
            sync_time_from_esp();       /* SNTP 重新对时 */
            msg.type = MSG_NET_DONE;    /* 跨日时日期要跟着更新 */
            xQueueSend(g_weather_q, &msg, 0);
        }

        if (sec % 3600 == 0)            /* 每 60 min */
        {
            if (net_fetch_humidity(&humi))
            {
                memset(&msg, 0, sizeof(msg));
                msg.type = MSG_HUMIDITY;
                msg.humidity = humi;
                xQueueSend(g_weather_q, &msg, 0);
            }
        }
    }
}

/** 显示任务 (优先级 3): 只画屏, 不发起任何网络请求。
 *  队列用非阻塞接收(超时 0), 消化完积压消息才进 500ms 延时,
 *  所以时间/冒号刷新完全不受网络快慢影响。 */
static void ui_task(void *pvParameters)
{
    (void)pvParameters;
    weather_msg_t msg;
    uint32_t tick = 0;
    uint8_t blink = 1;

    /* 屏、RTC、传感器只在 UI 任务里初始化一次 */
    lcd_lock();
    st7735_init();
    rtc_init();          /* LSE 起振失败会自动降级 LSI, 不会卡死 */
    mpu6050_init();
    ui_draw_frame();
    ui_update_statusbar();
    ui_update_date();
    ui_update_time(1);
    ui_update_indoor();
    lcd_unlock();

    /* 放行网络任务: 从这一刻起 net_task 才能调用 net_progress() */
    xSemaphoreGive(g_lcd_ready);

    while (1)
    {
        /* 先把队列里积压的消息全部消化掉 */
        while (xQueueReceive(g_weather_q, &msg, 0) == pdTRUE)
        {
            lcd_lock();
            switch (msg.type)
            {
            case MSG_WIFI_STATUS:
                g_wifi_ok = msg.wifi_ok;
                ui_update_statusbar();
                break;

            case MSG_WEATHER:
                g_out_temp_x10 = msg.temp_x10;
                strncpy(g_out_weather, msg.weather, 15);
                g_out_weather[15] = '\0';
                ui_update_weather();
                break;

            case MSG_HUMIDITY:
                g_humidity = msg.humidity;
                ui_update_indoor();
                break;

            case MSG_NET_DONE:
                ui_update_date();       /* 进度行擦掉日期后补画 */
                break;

            default:
                break;
            }
            lcd_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(500));
        tick++;
        blink = !blink;

        lcd_lock();
        ui_update_time(blink);
        if (tick % 120 == 0) ui_update_date();   /* 每 60 s 校准日期显示 */
        lcd_unlock();
    }
}

/** 心跳任务 (优先级 1): LED 1Hz 慢闪 + 每 20s 采样各任务栈水位。
 *  用 uxTaskGetStackHighWaterMark() 记录历史最小剩余栈, 供调试器观察;
 *  低于 64 words(256B) 时快闪 3 次告警, 把栈隐患暴露在烧录阶段。 */
static void heartbeat_task(void *pvParameters)
{
    (void)pvParameters;
    led_init();
    uint32_t beat = 0;

    while (1)
    {
        led_on();
        vTaskDelay(pdMS_TO_TICKS(500));
        led_off();
        vTaskDelay(pdMS_TO_TICKS(500));
        beat++;

        if (beat % 20 == 0)             /* 每 20 s */
        {
            UBaseType_t hw_ui  = uxTaskGetStackHighWaterMark(ui_task_handle);
            UBaseType_t hw_net = uxTaskGetStackHighWaterMark(net_task_handle);
            if (hw_ui  < g_stack_min_ui)  g_stack_min_ui  = hw_ui;
            if (hw_net < g_stack_min_net) g_stack_min_net = hw_net;

            if (hw_ui < 64 || hw_net < 64)
            {
                for (uint8_t i = 0; i < 3; i++)
                {
                    led_on();  vTaskDelay(pdMS_TO_TICKS(80));
                    led_off(); vTaskDelay(pdMS_TO_TICKS(80));
                }
            }
        }
    }
}

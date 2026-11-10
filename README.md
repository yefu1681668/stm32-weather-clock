# STM32 智能天气时钟（FreeRTOS 版）

基于 **STM32F103C8T6** 的桌面天气时钟：ESP32-C3（AT 固件）联网获取天气与时间，1.8" ST7735 彩屏显示时钟、气温与湿度，RTC 独立走时，FreeRTOS 调度。

本项目为「裸机版天气时钟」的 FreeRTOS 重构版本，同时也是后续 **OTA 远程升级** 改造的起点。

> **状态：全部功能已在真机验证通过**（2026-11-10），详见文末「真机验证记录」。

![实物接线：STM32F103C8T6 + ST7735 彩屏 + ESP32-C3 面包板系统](docs/hardware.jpg)

> 实物照片：面包板上的 C8T6、ST7735 彩屏、ESP32-C3 与杜邦线连接。照片中的 WiFi 名称已脱敏。

---

## 功能特性

- **联网天气**：ESP32-C3 通过 AT 指令连接 2.4 GHz WiFi，向心知天气请求实况（天气现象、温度）与当天湿度
- **SNTP 对时**：启动时用 `AT+CIPSNTPTIME?` 取本地时间写入 RTC；RTC 由 LSE 32.768 kHz 晶振走时，掉电保持
- **界面**：128×128 分区布局 —— 状态栏（WiFi 名 + 城市）、48 px 大字号时间、日期行、湿度面板、气温面板
- **湿度**：来自天气 API 的**城市相对湿度**（板上没有湿度传感器），显示如 `49%`
- **气温**：天气 API 实况温度 + 对应的 24×24 天气图标（晴 / 多云 / 阴 / 雨 / 雪 / 风）
- **RTOS 调度**：FreeRTOS 动态创建任务（`start_task` → `ui_task`），`heap_4` 内存管理

> 说明：MPU6050 驱动仍在工程中并完成了初始化，但当前界面不读取它 —— 它的温度是**芯片结温**（比室温高 10 ℃ 以上），不是环境温度，显示出来会误导，因此已停用。姿态自动翻转功能尚未实现。

## 硬件清单

| 器件 | 型号 / 规格 | 说明 |
|---|---|---|
| 主控 | STM32F103C8T6 | 64 KB Flash / 20 KB RAM，72 MHz |
| 显示 | ST7735 1.8" TFT | 128×128，SPI 接口，RGB565 |
| 联网 | ESP32-C3 开发板 | 预烧精简版 AT 固件，UART 115200，自带 Type-C 供电 |
| 姿态 | MPU6050 | 六轴，软件 I2C（当前仅初始化，未使用读数） |
| 走时 | 32.768 kHz 晶振 | 接 PC14 / PC15，供 RTC 使用 |
| 下载 | ST-Link V2 | SWD 四线 |

## 接线表

引脚定义直接取自驱动代码（`driver/` 下各文件的 `#define`），与实际接线一致。

### ST7735 显示屏（SPI1）

| 信号 | 引脚 | 说明 |
|---|---|---|
| SCK | PA5 | SPI1 时钟，18 MHz（72 MHz ÷ 4） |
| MOSI | PA7 | SPI1 数据输出 |
| CS | PA4 | 片选，低有效 |
| DC | PB1 | 命令/数据选择 |
| **RES** | **PB0** | **硬复位，必须接！漏接 = 背光亮但纯白屏** |
| BLK | PB10 | 背光，高电平点亮 |

> SPI1 配置为 Mode 3（CPOL=1, CPHA=2Edge）、单线发送主模式、软件 NSS。屏幕只写不读，因此未占用 MISO（PA6）。
> 像素数据当前使用**阻塞轮询发送**（`ST7735_USE_DMA = 0`），不经过 DMA，避免 DMA 未启动时永久死等。

### ESP32-C3（USART2）

| 信号 | 引脚 | 说明 |
|---|---|---|
| TX | PA2 | STM32 → ESP32 |
| RX | PA3 | ESP32 → STM32 |

> 波特率 115200，中断接收，接收缓冲 4096 B。注意不是 PA9/PA10（那是 USART1）。
> **TX / RX 必须交叉**，且 ESP32 与 STM32 **必须共地**。

### MPU6050（软件 I2C）

| 信号 | 引脚 |
|---|---|
| SCL | PB6 |
| SDA | PB7 |

### 其他

| 器件 | 引脚 |
|---|---|
| 板载 LED | PC13 |
| RTC 晶振 | PC14 / PC15（LSE） |

## 软件架构

```
main()
 ├─ HAL_Init()                     HAL 与系统时基
 ├─ SystemClock_Config()           72 MHz（HSE 8 MHz × 9）
 ├─ MX_GPIO_Init() / MX_USART1_UART_Init()
 └─ freertos_start()               Core/Src/main.c → user/freertos_demo.c
      ├─ xTaskCreate(start_task)
      ├─ vTaskStartScheduler()     TICK = 1000 Hz
      └─ start_task 动态创建 ui_task 后自删除
           └─ ui_task: 初始化外设 → 绘制界面框架 → 联网 → 循环刷新（500 ms 周期）
```

`ui_task` 启动顺序（**顺序很重要**）：

```
led_init → st7735_init → rtc_init → mpu6050_init
→ ui_draw_frame / 状态栏 / 日期 / 时间 / 湿度面板
→ ui_init_network()           ← ESP32 串口到这一步才初始化
→ 补画日期（进度行会擦掉它）
→ 取实况天气 → 取湿度
→ 主循环：每 2 s 刷湿度面板、60 s 刷日期、10 min 刷天气+对时、60 min 刷湿度
```

> 曾经的顺序 bug：`ui_update_weather()` 排在 `ui_init_network()` 之前，等于把 AT 命令发给一个还没打开的串口，会白等 10 秒，表现为「橙色面板空白」。现在已修正。

任务参数：优先级 5 级（`configMAX_PRIORITIES = 5`）、最小栈 128 word、堆 8 KB（`heap_4`）。当前为**单任务轮询**，尚未拆分网络 / UI / 传感器任务。

## 目录结构

```
weather_clock/
├── Core/                    CubeMX 生成的应用与 HAL 配置
│   ├── Inc/                 main.h / FreeRTOSConfig.h / HAL 配置
│   └── Src/                 main.c / gpio.c / usart.c / stm32f1xx_it.c
├── Drivers/                 STM32F1 HAL 库 + CMSIS（ST 官方，未改动）
├── FreeRTOS/                内核源码（tasks / queue / list）+ heap_4 + ARM_CM3 port
├── docs/                    真机实物照片（hardware.jpg / screen.jpg）
└── mdk/                     Keil MDK 工程
    ├── Freertos的嵌入式智能天气时钟.uvprojx
    ├── app/                 应用层：天气解析、字库、开机动画、图标
    ├── driver/              外设驱动：st7735 / esp-at / mpu6050 / led / rtc
    └── user/                任务实现 freertos_demo.c、本地配置 wifi_config.h
```

工程文件使用 `..\Core`、`..\Drivers`、`..\FreeRTOS` 相对路径引用上层目录，因此**必须保持上述目录结构**，不可只复制 `mdk/`。

## 编译与烧录

1. **配置本地参数**：编辑 `mdk/user/wifi_config.h`，填入自己的 WiFi 账号与心知天气 API key：

   ```c
   #define WIFI_CFG_SSID        "你的WiFi名"          /* 必须 2.4 GHz */
   #define WIFI_CFG_PWD         "你的WiFi密码"
   #define WEATHER_CFG_API_KEY  "你的心知天气key"      /* seniverse.com 免费注册 */
   #define WEATHER_CFG_CITY     "guilin"
   ```

   > 仓库中该文件是占位符版本（`YOUR_WIFI_SSID` 等），不填无法联网，但能正常编译。

2. **打开工程**：Keil MDK 打开 `mdk/Freertos的嵌入式智能天气时钟.uvprojx`
3. **编译烧录**：`F7` 编译 → `F8` 下载（ST-Link，起始地址 `0x08000000`，BOOT0 接 GND）
4. **上电观察**：屏幕先出完整界面，中间提示行依次走 `1/4 Init ESP...` → `2/4 STA Mode...` → `3/4 <SSID>...` → `4/4 SNTP Sync...`，随后进入时钟界面

### 排障提示（都是真机踩过的坑）

| 现象 | 原因与排查方向 |
|---|---|
| **背光亮、屏幕纯白** | **头号原因：RES 没接 PB0**，或 RES/DC 接反（PB0 与 PB1 是相邻两孔）。屏没被硬件复位唤醒，发什么命令都不响应 |
| 背光亮、有画面但卡住 | 检查 `ui_init_network()` 顺序，AT 命令不能发在串口初始化之前 |
| 屏幕全黑 | 量 PB10 背光是否为高、VCC 是否 3.3 V |
| 顶部彩条都画不出来 | 用 `st7735_probe_bars()` 做最小路径测试（绕过上层绘图）；能出彩条 = 屏与 SPI 正常，问题在绘图层 |
| `ESP rx=0000` | ESP32 一个字节都没回：查 TX/RX 交叉、共地、EN/BOOT 脚、供电（不要用 ST-Link 的 3.3 V 带 ESP） |
| 时间显示 `08:xx` | `AT+SYSTIMESTAMP?` 不被固件支持导致时间戳为 0（1970+8h）。本工程已改用 `AT+CIPSNTPTIME?` |
| 图标是黑底 + 颜色错乱 | RGB565 图标数组是**小端**存放，且 `0x0000` 表示透明。本工程已按小端解析并跳过纯黑 |
| 湿度显示 `--%` | 湿度来自 `daily.json`，联网失败或接口未返回时会保持占位符 |

> **AT 固件差异**：实测这颗 ESP32-C3 是裁剪版乐鑫 AT 固件，支持 `AT` / `CWMODE` / `CWJAP` / `CIPSNTPCFG` / `CIPSNTPTIME?` / `HTTPCGET`，但不支持 `AT+GMR` / `AT+VER` / `AT+CIPSTA?` / `AT+SYSTIMESTAMP?`。因此初始化采用「先用 `AT` 探活，其余尽力而为」的容错写法，不因某条可选命令报错就判定模组失联。

## 资源占用（实测，Keil V5.06 编译结果）

```
Program Size: Code=13044  RO-data=32052  RW-data=220  ZI-data=17684
```

| 项目 | 占用 | 总容量 | 使用率 |
|---|---|---|---|
| Flash（Code + RO + RW） | 45 316 B ≈ **44.3 KB** | 64 KB | 69.2% |
| RAM（RW + ZI） | 17 904 B ≈ **17.5 KB** | 20 KB | 87.4% |

> 数值含本地 WiFi 账号字符串，换用不同长度的 SSID 会有几十字节偏差。

**空间分析（决定了后续改造路线）**

- `RO-data` 中 **27 648 B（27 KB）是 6 张天气图标**（48×48 RGB565），占 Flash 的 61%，占 RO-data 的 86%。若迁移到外部 SPI Flash，Flash 占用可降到约 17 KB。
- RAM 仅剩 **2.5 KB**。若直接移植 OTA BootLoader 并与 APP 共享缓冲区，极易栈溢出，需重新核算任务栈与堆。
- 若套用 16 KB Boot + 47 KB APP 的 OTA 分区，APP 可用 48 128 B，当前 45 316 B 仅余 **2.8 KB**，加任何功能都会溢出 —— 所以**图标外置 W25Q 是移植 OTA 的前置条件**。

## 真机验证记录（2026-11-10）

在 STM32F103C8T6 + ST7735 + ESP32-C3 实物上完成，屏幕实测显示（状态栏 WiFi 名称已脱敏为 `<SSID>`）：

![屏幕特写：状态栏、时间、日期、湿度与气温面板](docs/screen.jpg)

```
状态栏   <SSID> | 桂林       ← WiFi 已连接
时间     19:51                 ← 与北京时间一致，SNTP 对时正确
日期     2026-11-10 TUE
湿度     49%                   ← 心知 daily.json 返回
气温     15°C + 多云图标        ← 心知 now.json 返回，图标颜色/透明正常
```

已验证：SPI 显示链路、RGB565 字序、16×16 中文字模、窗口坐标、RTC 走时、USART2 中断接收、AT 指令建链、WiFi 入网、SNTP 对时、HTTP 取天气与湿度、FreeRTOS 任务调度、LED 心跳。

排查过程中定位并修复的问题（均已在代码中）：

| # | 问题 | 修复 |
|---|---|---|
| 1 | RES 未接导致白屏 | 接线修正；驱动本来就有 `st7735_reset()` 硬件复位时序 |
| 2 | `rtc_init()` 裸等 LSE 无超时，晶振不起振即死机 | 加 2.3 s 超时并降级 LSI；`rtc_wait_for_rtoff()` 同样加超时 |
| 3 | `st7735_write_data()` 等 DMA TCIF 无超时 | 加超时兜底 + 默认切到阻塞写 |
| 4 | `ui_update_weather()` 早于串口初始化 | 调整顺序，先联网再取天气/湿度 |
| 5 | `AT+SYSTIMESTAMP?` 不被支持 → 时间变成 1970 | 改用 `AT+CIPSNTPTIME?` 解析，且不再重复 +8 h |
| 6 | 联网进度行擦掉日期行 | 联网结束后补画一次日期 |
| 7 | 湿度行超出 64 px 面板 4 px，蓝色侵入橙色面板 | 重排文字起点，收进面板边界 |
| 8 | 图标字节序错（大端→小端）+ 黑底 | 小端解析 + `0x0000` 视为透明 |
| 9 | 湿度无数据来源 | 新增 `daily.json` 请求与 `weather_parse_humidity()` |

## 当前进度与后续计划

- [x] 裸机版 → FreeRTOS 重构，Keil 编译 0 Error / 0 Warning
- [x] **全部功能真机验证通过**（显示 / 对时 / 联网 / 天气 / 湿度）
- [ ] 图标外置到 W25Q（释放 27 KB Flash，为 OTA 腾出空间）
- [ ] 移植 OTA BootLoader（`0x08000000` 16 KB Boot + `0x08004000` APP）
- [ ] 多任务重构：网络任务 / UI 任务 / 传感器任务拆分，改为队列通信

## 说明

- `mdk/user/wifi_config.h` 在仓库中为占位符版本。本地填入真实值后，建议执行
  `git update-index --skip-worktree mdk/user/wifi_config.h` 避免敏感信息被提交。
- `Drivers/` 与 `FreeRTOS/` 为 ST 官方与 FreeRTOS 官方源码，未做修改。

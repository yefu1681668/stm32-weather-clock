# STM32 智能天气时钟（FreeRTOS 版）

基于 **STM32F103C8T6** 的桌面天气时钟：ESP32-C3（AT 固件）联网获取实时天气，1.8" ST7735 彩屏显示时间与天气，MPU6050 检测姿态自动翻转显示方向，FreeRTOS 调度，RTC 独立走时。

本项目为「裸机版天气时钟」的 FreeRTOS 重构版本，同时也是后续 **OTA 远程升级** 改造的起点。

---

## 功能特性

- **联网天气**：ESP32-C3 通过 AT 指令连接 WiFi，向心知天气 API 请求实时天气（天气现象、温度）
- **时间显示**：ESP32 提供 SNTP 对时，STM32 RTC（LSE 外部 32.768 kHz 晶振）掉电保持走时
- **姿态感应**：MPU6050（软件 I2C）读取加速度，判断屏幕正放/倒放，自动旋转显示方向
- **界面**：128×128 分区布局 —— 顶部状态栏、室内面板、室外面板、大字号时间
- **RTOS 调度**：FreeRTOS 动态创建任务（`start_task` → `ui_task`），`heap_4` 内存管理

## 硬件清单

| 器件 | 型号 / 规格 | 说明 |
|---|---|---|
| 主控 | STM32F103C8T6 | 64 KB Flash / 20 KB RAM，72 MHz |
| 显示 | ST7735 1.8" TFT | 128×128，SPI 接口，RGB565 |
| 联网 | ESP32-C3 模组 | 预烧 AT 固件，UART 115200 |
| 姿态 | MPU6050 | 六轴，软件 I2C |
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
| RES | PB0 | 硬复位 |
| BLK | PB10 | 背光，高电平点亮 |

> SPI1 配置为 Mode 3（CPOL=1, CPHA=2Edge）、单线发送主模式、软件 NSS。屏幕只写不读，因此未占用 MISO（PA6）。

### ESP32-C3（USART2）

| 信号 | 引脚 | 说明 |
|---|---|---|
| TX | PA2 | STM32 → ESP32 |
| RX | PA3 | ESP32 → STM32 |

> 波特率 115200，中断接收，接收缓冲 4096 B。注意不是 PA9/PA10（那是 USART1）。

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
           └─ ui_task: 初始化外设 → 绘制界面框架 → 循环刷新（500 ms 周期）
```

任务参数：优先级 5 级（`configMAX_PRIORITIES = 5`）、最小栈 128 word、堆 8 KB（`heap_4`）。

## 目录结构

```
weather_clock/
├── Core/                    CubeMX 生成的应用与 HAL 配置
│   ├── Inc/                 main.h / FreeRTOSConfig.h / HAL 配置
│   └── Src/                 main.c / gpio.c / usart.c / stm32f1xx_it.c
├── Drivers/                 STM32F1 HAL 库 + CMSIS（ST 官方，未改动）
├── FreeRTOS/                内核源码（tasks / queue / list）+ heap_4 + ARM_CM3 port
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
3. **编译烧录**：`F7` 编译 → `F8` 下载（ST-Link，起始地址 `0x08000000`）
4. **上电观察**：屏幕依次显示 `Init ESP32...` → `Connect WIFI...` → `Sync Time...`，随后进入时钟界面

### 排障提示

| 现象 | 排查方向 |
|---|---|
| 屏幕全黑 | ① 量 PB10 是否为高（背光）② 量 PB0 / PA4 是否 3.3 V ③ 检查 ESP32 的 PA2/PA3 是否接反 |
| 卡在 `Failed!!!` | ESP32 内未烧 AT 固件，或波特率不是 115200 |
| 界面不刷新 | MPU6050 未应答（软件 I2C 无应答检测），检查 PB6/PB7 与上拉 |

## 资源占用（实测，Keil V5.06 编译结果）

```
Program Size: Code=12924  RO-data=31840  RW-data=220  ZI-data=17684
```

| 项目 | 占用 | 总容量 | 使用率 |
|---|---|---|---|
| Flash（Code + RO + RW） | 44 984 B ≈ **43.9 KB** | 64 KB | 68.6% |
| RAM（RW + ZI） | 17 904 B ≈ **17.5 KB** | 20 KB | 87.4% |

**空间分析（决定了后续改造路线）**

- `RO-data` 中 **27 648 B（27 KB）是 6 张天气图标**（48×48 RGB565），占 Flash 的 62%，占 RO-data 的 87%。若迁移到外部 SPI Flash，Flash 占用可降到约 17 KB。
- RAM 仅剩 **2.5 KB**。若直接移植 OTA BootLoader 并与 APP 共享缓冲区，极易栈溢出，需重新核算任务栈与堆。
- 若套用 16 KB Boot + 47 KB APP 的 OTA 分区，APP 可用 48 128 B，当前 44 984 B 仅余 **3.1 KB**，加任何功能都会溢出。

## 当前进度与后续计划

- [x] 裸机版 → FreeRTOS 重构，编译 0 Error / 0 Warning，任务调度正常
- [x] 联网天气 / SNTP 对时 / 姿态翻转 / RTC 走时功能保留
- [ ] 图标外置到 W25Q（释放 27 KB Flash）
- [ ] 移植 OTA BootLoader（`0x08000000` 16 KB Boot + `0x08004000` APP）
- [ ] 多任务重构：网络任务 / UI 任务 / 传感器任务拆分，改为队列通信（当前为单任务轮询）

## 说明

- `mdk/user/wifi_config.h` 在仓库中为占位符版本。本地填入真实值后，建议执行
  `git update-index --skip-worktree mdk/user/wifi_config.h` 避免敏感信息被提交。
- `Drivers/` 与 `FreeRTOS/` 为 ST 官方与 FreeRTOS 官方源码，未做修改。

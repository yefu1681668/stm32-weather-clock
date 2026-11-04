/**
  ******************************************************************************
  * @file    wifi_config.h
  * @brief   本地私有配置：WiFi 账号 + 心知天气 API key
  *
  * 说明：
  *   仓库中提交的是【占位符版本】，保证 clone 后可直接编译。
  *   本地开发时填入真实值，并用下面命令避免被提交：
  *
  *       git update-index --skip-worktree mdk/user/wifi_config.h
  *
  *   恢复跟踪：
  *       git update-index --no-skip-worktree mdk/user/wifi_config.h
  ******************************************************************************
  */
#ifndef __WIFI_CONFIG_H
#define __WIFI_CONFIG_H

/* ---------- WiFi（必须 2.4GHz，ESP32 不支持 5GHz） ---------- */
#define WIFI_CFG_SSID           "YOUR_WIFI_SSID"
#define WIFI_CFG_PWD            "YOUR_WIFI_PASSWORD"

/* ---------- 心知天气（seniverse.com 注册免费版获取 key） ---------- */
#define WEATHER_CFG_API_KEY     "YOUR_API_KEY"
#define WEATHER_CFG_CITY        "guilin"        /* 城市拼音: guilin / nanning / liuzhou */

#endif /* __WIFI_CONFIG_H */

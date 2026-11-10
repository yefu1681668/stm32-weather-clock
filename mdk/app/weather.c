#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "weather.h"


// 示例输入：
// +HTTPCGET:261,{"results":[{"location":{"id":"WTEMH46Z5N09","name":"Hefei","country":"CN","path":"Hefei,Hefei,Anhui,China","timezone":"Asia/Shanghai","timezone_offset":"+08:00"},"now":{"text":"Cloudy","code":"4","temperature":"36"},"last_update":"2024-08-26T16:20:13+08:00"}]}
//
// OK


bool weather_parse(const char *data, weather_t *weather)
{
    char *p = strstr(data, "\"text\":\"");
    if (p == NULL)
    {
        return false;
    }
    p += strlen("\"text\":\"");
    char *q = strchr(p, '\"');
    if (q == NULL)
    {
        return false;
    }
    int len = q - p;
    if (len >= sizeof(weather->weather))
    {
        len = sizeof(weather->weather) - 1;
    }
    strncpy(weather->weather, p, len);
    weather->weather[len] = '\0';

    p = strstr(data, "\"temperature\":\"");
    if (p == NULL)
    {
        return false;
    }
    p += strlen("\"temperature\":\"");
    q = strchr(p, '\"');
    if (q == NULL)
    {
        return false;
    }
    len = q - p;
    if (len >= sizeof(weather->temperature))
    {
        len = sizeof(weather->temperature) - 1;
    }
    strncpy(weather->temperature, p, len);
    weather->temperature[len] = '\0';

    return true;
}


// 从 daily.json 里取今天(第一条)的湿度
// 示例: ...,"high":"18","low":"9","rainfall":"0.00","precip":"0.00",
//       "wind_direction":"NE",...,"humidity":"50"}],...
bool weather_parse_humidity(const char *data, uint8_t *out)
{
    const char *p = strstr(data, "\"humidity\":\"");
    if (p == NULL)
    {
        return false;
    }
    p += strlen("\"humidity\":\"");

    uint16_t v = 0;
    uint8_t n = 0;
    while (*p >= '0' && *p <= '9' && n < 3)
    {
        v = v * 10 + (uint8_t)(*p - '0');
        p++;
        n++;
    }
    if (n == 0 || v > 100)
    {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

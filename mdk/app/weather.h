#ifndef __WEATHER_H__
#define __WEATHER_H__


typedef struct
{
    char weather[32];
    char temperature[8];
} weather_t;


bool weather_parse(const char *data, weather_t *weather);

/* 从 3 天预报接口(daily.json)里取今天的相对湿度。
 * 实况接口(now.json)没有湿度字段, 只有 text/code/temperature,
 * 所以湿度必须单独请求一次 daily.json。
 * 成功返回 true 并把 0~100 的值写入 *out。 */
bool weather_parse_humidity(const char *data, uint8_t *out);


#endif /* __WEATHER_H__ */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "main.h"
#include "delay.h"
#include "esp_usart.h"
#include "esp_at.h"

/* 轻量字符串拼接, 替代snprintf避免拉入printf库 */
static void str_append(char *dst, const char *src)
{
    while (*dst) dst++;
    while ((*dst++ = *src++) != '\0') { }
}


#define RX_BUFFER_SIZE  4096

#define RX_RESULT_OK    0
#define RX_RESULT_ERROR 1
#define RX_RESULT_FAIL  2


static uint8_t rxdata[RX_BUFFER_SIZE];
static uint32_t rxlen;
static bool rxready;
static uint8_t rxresult;

/* 调试: 最后一次响应长度 */
uint32_t esp_at_last_rxlen = 0;


static void on_usart_received(uint8_t data)
{
    // 没有数据请求，不接受数据
    if (!rxready)
    {
        return;
    }

    // 接收数据，防止缓冲区溢出
    if (rxlen < RX_BUFFER_SIZE)
    {
        rxdata[rxlen++] = data;
    }
    else
    {
        rxresult = RX_RESULT_FAIL;
        rxready = false;
        return;
    }

    // 数据接收完毕判断
    if (data == '\n')
    {
        // 判断接收换行符是否为\r\n
        if (rxlen >= 2 && rxdata[rxlen - 2] == '\r')
        {
            // 收到OK
            if (rxlen >= 4 &&
                rxdata[rxlen - 4] == 'O' && rxdata[rxlen - 3] == 'K')
            {
                rxresult = RX_RESULT_OK;
                rxready = false;
            }
            else if (rxlen >= 7 &&
                     rxdata[rxlen - 5] == 'E' && rxdata[rxlen - 4] == 'R' &&
                     rxdata[rxlen - 3] == 'R' && rxdata[rxlen - 2] == 'O' &&
                     rxdata[rxlen - 1] == 'R')
            {
                rxresult = RX_RESULT_ERROR;
                rxready = false;
            }
        }
    }
}

bool esp_at_init(void)
{
    rxready = false;

    esp_usart_init();
    esp_usart_receive_register(on_usart_received);

    return esp_at_reset();
}

bool esp_at_send_command(const char *cmd, const char **rsp, uint32_t *length, uint32_t timeout)
{
    rxlen = 0;
    rxready = true;
    rxresult = RX_RESULT_FAIL;

    esp_usart_write_string(cmd);
    esp_usart_write_string("\r\n");

    while (rxready && timeout--)
    {
        delay_ms(1);
    }
    rxready = false;

    esp_at_last_rxlen = rxlen;

    if (rsp)
    {
        *rsp = (const char *)rxdata;
    }
    if (length)
    {
        *length = rxlen;
    }

    return rxresult == RX_RESULT_OK;
}

bool esp_at_send_data(const uint8_t *data, uint32_t length)
{
    esp_usart_write_data((uint8_t *)data, length);

    return true;
}

bool esp_at_reset(void)
{
    // 复位esp32 (RESTORE会重启模块, 需等待)
    if (!esp_at_send_command("AT+RESTORE", NULL, NULL, 3000))
    {
        return false;
    }
    delay_ms(3000);
    // 关闭回显
    if (!esp_at_send_command("ATE0", NULL, NULL, 1000))
    {
        return false;
    }
    // 关闭存储 (ESP8266不支持此命令, 失败也继续)
    esp_at_send_command("AT+SYSSTORE=0", NULL, NULL, 1000);

    return true;
}

bool esp_at_wifi_init(void)
{
    // 设置为station模式
    if (!esp_at_send_command("AT+CWMODE=1", NULL, NULL, 1000))
    {
        return false;
    }

    return true;
}
bool esp_at_wifi_connect(const char *ssid, const char *pwd)
{
    char cmd[64];

    // 连接wifi
    cmd[0] = '\0';
    str_append(cmd, "AT+CWJAP=\"");
    str_append(cmd, ssid);
    str_append(cmd, "\",\"");
    str_append(cmd, pwd);
    str_append(cmd, "\"");
    // WiFi连接可能需要较长时间, 超时20秒, 重试2次
    for (uint8_t retry = 0; retry < 2; retry++)
    {
        if (esp_at_send_command(cmd, NULL, NULL, 20000))
        {
            return true;
        }
        delay_ms(1000);
    }

    return false;
}

bool esp_at_get_http(const char *url, const char **rsp, uint32_t *length, uint32_t timeout)
{
    char cmd[128];

    cmd[0] = '\0';
    str_append(cmd, "AT+HTTPCGET=\"");
    str_append(cmd, url);
    str_append(cmd, "\"");
    if (!esp_at_send_command(cmd, rsp, length, 10000))
    {
        return false;
    }

    return true;
}

bool esp_at_sntp_init(void)
{
    // 设置为SNTP模式
    if (!esp_at_send_command("AT+CIPSNTPCFG=1,8,\"cn.ntp.org.cn\",\"ntp.sjtu.edu.cn\"", NULL, NULL, 1000))
    {
        return false;
    }

    // 查询sntp时间
    if (!esp_at_send_command("AT+CIPSNTPTIME?", NULL, NULL, 1000))
    {
        return false;
    }

    return true;
}

bool esp_at_get_time(uint32_t *timestamp)
{
    const char *rsp;
    uint32_t length;

    if (!esp_at_send_command("AT+SYSTIMESTAMP?", &rsp, &length, 1000))
    {
        return false;
    }

    char *sts = strstr(rsp, "+SYSTIMESTAMP:") + strlen("+SYSTIMESTAMP:");

    /* 轻量字符串转整数, 替代atoi避免拉入stdlib */
    uint32_t ts = 0;
    while (*sts >= '0' && *sts <= '9')
    {
        ts = ts * 10 + (*sts - '0');
        sts++;
    }
    *timestamp = ts;

    return true;
}

bool esp_at_wifi_get_ip(char ip[16])
{
    const char *rsp;
    if (!esp_at_send_command("AT+CIPSTA?", &rsp, NULL, 1000))
    {
        return false;
    }

    // 解析ip地址
    const char *pip = strstr(rsp, "+CIPSTA:ip:") + strlen("+CIPSTA:ip:");
    if (pip)
    {
        for (int i = 0; i < 16; i++)
        {
            if (pip[i] == '\r')
            {
                ip[i] = '\0';
                break;
            }
            ip[i] = pip[i];
        }
        return true;
    }

    return true;
}

bool esp_at_wifi_get_mac(char mac[18])
{
    const char *rsp;
    if (!esp_at_send_command("AT+CIPSTAMAC?", &rsp, NULL, 1000))
    {
        return false;
    }

    // 解析mac地址
    const char *pmac = strstr(rsp, "+CIPSTAMAC:mac:") + strlen("+CIPSTAMAC:mac:");
    if (pmac)
    {
        strncpy(mac, pmac, 18);
        return true;
    }

    return true;
}

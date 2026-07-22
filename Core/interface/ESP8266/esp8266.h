#ifndef INC_ESP8266_H_
#define INC_ESP8266_H_
#include <stdio.h>
#include "usart.h"
#include <string.h>

#define wifi_huart          (&huart2) /* WiFi模块使用的串口 */
#define WIFI_RX_BUF_SIZE    1024U
#define WIFI_REPLY_BUF_SIZE 512U

extern char         g_wifi_rxbuf[WIFI_REPLY_BUF_SIZE];
extern volatile int g_wifi_rxbytes;

typedef struct
{
    uint8_t  valid;
    uint32_t size;
    char     title[32];
    char     version[32];
    char     checksum_algorithm[16];
    char     checksum[80];
} esp8266_tb_firmware_info_t;

/* 清除WiFi模块接收buffer里的数据内容宏，用宏不用函数是因为函数调用需要额外时间开销 */
#define clear_atcmd_buf()                                                                          \
    do                                                                                             \
    {                                                                                              \
        memset(g_wifi_rxbuf, 0, sizeof(g_wifi_rxbuf));                                             \
        g_wifi_rxbytes = 0;                                                                        \
    } while (0)

void ESP8266_UartStartReceive(void);
void ESP8266_UartRxCpltCallback(UART_HandleTypeDef *huart);
void ESP8266_UartErrorCallback(UART_HandleTypeDef *huart);

/* ESP8266 WiFi模块发送AT命令函数。返回值为0表示成功，!0 表示失败 */
#define EXPECT_OK "OK\r\n"
extern int send_atcmd(char *atcmd, char *expect_reply, unsigned int timeout);

/* ESP8266 WiFi模块初始化函数。返回值为0表示成功，!0 表示失败 */
extern int esp8266_module_init(void);

/* ESP8266 WiFi模块复位重启函数。返回值为0表示成功，!0 表示失败 */
extern int esp8266_module_reset(void);

/* ESP8266 WiFi模块连接路由器函数。返回值为0表示成功，!0 表示失败 */
extern int esp8266_join_network(char *ssid, char *pwd);

/* ESP8266 WiFi模块断开当前AP连接。返回值为0表示成功，!0 表示失败 */
extern int esp8266_wifi_disconnect(void);

/* ESP8266 WiFi模块扫描AP。ssid为NULL时只打印扫描结果，非NULL时检查目标AP是否存在 */
extern int esp8266_scan_ap(char *ssid);

/* ESP8266 获取自己的IP地址和网关IP地址。返回值为0表示成功，!0 表示失败 */
int esp8266_get_ipaddr(char *ipaddr, char *gateway, int ipaddr_size);

/* ESP8266 WiFi模块做ping命令测试网络连通性。返回值为0表示成功，!0 表示失败 */
int esp8266_ping_test(char *host);

/* ESP8266 WiFi模块建立TCP socket 连接函数。返回值为0表示成功，!0 表示失败 */
extern int esp8266_sock_connect(char *servip, int port);

/* ESP8266 WiFi模块断开TCP socket 连接函数。返回值为0表示成功，!0 表示失败 */
extern int esp8266_sock_disconnect(void);

/* ESP8266 WiFi通过TCP Socket发送数据函数。返回值为0表示失败，>0 表示成功发送字节数 */
extern int esp8266_sock_send(unsigned char *data, int bytes);

/* ESP8266 WiFi通过TCP Socket接收数据函数。返回值为0无数据，>0 表示接收到数据字节数 */
extern int esp8266_sock_recv(unsigned char *buf, int size);

/* ESP-AT MQTT连接函数。access_token 用作 ThingsBoard MQTT username */
extern int esp8266_mqtt_connect(char *host, int port, char *access_token);

/* ESP-AT MQTT断开并清理连接 */
extern int esp8266_mqtt_disconnect(void);

/* ESP-AT MQTT发布原始payload，适合发布JSON */
extern int esp8266_mqtt_publish_raw(char *topic, unsigned char *data, int bytes);

/* ESP-AT MQTT订阅topic */
extern int esp8266_mqtt_subscribe(char *topic, int qos);

/* ThingsBoard telemetry发布，topic固定为 v1/devices/me/telemetry */
extern int esp8266_thingsboard_publish_telemetry(unsigned char *json, int bytes);

/* ThingsBoard OTA：只请求固件元信息，不下载固件内容 */
extern int esp8266_thingsboard_request_firmware_info(void);
extern int esp8266_thingsboard_get_firmware_info(esp8266_tb_firmware_info_t *info);
extern int esp8266_thingsboard_subscribe_firmware_chunks(void);

/* ThingsBoard OTA：请求单个固件chunk到调用方buffer */
extern int esp8266_thingsboard_request_firmware_chunk(
    uint32_t chunk_index, uint32_t chunk_size, uint8_t *buf, uint16_t buf_size, uint16_t *out_len);

#endif /* INC_ESP8266_H_ */

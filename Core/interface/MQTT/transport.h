#ifndef __TRANSPORT_H_
#define __TRANSPORT_H_

/* 使用 ESP8266 创建 socket 连接到 MQTT 服务器函数 */
int transport_open(char *host, int port);

/* ESP8266 关闭 socket 连接函数 */
int transport_close(void);

/* 使用 ESP8266 发送一个 MQTT 数据报文的函数 */
int transport_sendPacketBuffer(unsigned char *buf, int buflen);

/* 使用 ESP8266 接收 MQTT 数据报文函数 */
int transport_getdata(unsigned char *buf, int count);

/* 清除 ESP8266 接收 MQTT 数据的 socket buffer 函数 */
void transport_clearBuf(void);

#endif /* __TRANSPORT_H_ */

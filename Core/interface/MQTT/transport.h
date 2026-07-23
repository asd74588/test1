#ifndef __TRANSPORT_H_
#define __TRANSPORT_H_

/* 使用 ESP8266 创建 socket 连接到 MQTT 服务器函数 */
int transport_open(char *host, int port);

/* ESP8266 关闭 socket 连接函数 */
int transport_close(void);

/* 使用 ESP8266 发送一个 MQTT 数据报文的函数 */
int transport_sendPacketBuffer(unsigned char *buf, int buflen);

/* 非阻塞读取 TCP 字节流：>0 为本次读取字节数，0 为暂无数据，<0 为错误 */
int transport_getdata(unsigned char *buf, int count);

/* 清除 ESP8266 TCP 接收环形缓冲中的残留数据 */
void transport_clearBuf(void);

#endif /* __TRANSPORT_H_ */

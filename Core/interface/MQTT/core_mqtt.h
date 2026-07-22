#ifndef CORE_MQTT_H_
#define CORE_MQTT_H_

#include "MQTTPacket.h"
#include "transport.h"

#define MQTT_KEEP_ALIVE_TIMEOUT_SECONDS (60U)

enum
{
    Qos0 = 0,
    Qos1,
    Qos2,
};

/* MQTT 连接 Broker 函数 */
int mqtt_connect(char *host, int port, char *clientid, char *username, char *passwd);

/* MQTT 断开 Broker 连接函数 */
int mqtt_disconnect(void);

/* MQTT 订阅主题函数 */
int mqtt_subscribe_topic(char *topic, int qos, int msgid);

/* MQTT 取消主题订阅函数 */
int mqtt_unsubscribe_topic(char *topic, int msgid);

/* MQTT 发布消息函数 */
int mqtt_publish(char *topic, int qos, char *payload);

/* MQTT 保持连接心跳包函数 */
int mqtt_pingreq(void);

#endif /* CORE_MQTT_H_ */

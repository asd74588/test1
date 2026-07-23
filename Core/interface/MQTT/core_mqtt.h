#ifndef CORE_MQTT_H_
#define CORE_MQTT_H_

#include <stdint.h>

#include "MQTTPacket.h"

#define MQTT_KEEP_ALIVE_TIMEOUT_SECONDS 60U
#define MQTT_RX_TOPIC_MAX 128U
#define MQTT_RX_PAYLOAD_MAX 512U
#define MQTT_PACKET_BUF_SIZE 672U

typedef enum
{
    MQTT_WAIT_RESULT_IDLE = 0,
    MQTT_WAIT_RESULT_SUCCESS,
    MQTT_WAIT_RESULT_REJECTED,
    MQTT_WAIT_RESULT_TIMEOUT,
    MQTT_WAIT_RESULT_TRANSPORT_ERROR,
    MQTT_WAIT_RESULT_PROTOCOL_ERROR,
} mqtt_wait_result_t;

typedef struct
{
    uint16_t packet_id;
    uint16_t topic_len;
    uint16_t payload_len;
    uint8_t qos;
    uint8_t dup;
    uint8_t retained;
    char topic[MQTT_RX_TOPIC_MAX + 1U];
    uint8_t payload[MQTT_RX_PAYLOAD_MAX];
} mqtt_rx_msg_t;

enum
{
    Qos0 = 0,
    Qos1,
    Qos2,
};

int mqtt_connect(char *host, int port, char *clientid, char *username, char *passwd);
int mqtt_disconnect(void);
int mqtt_subscribe_topic(char *topic, int qos, int msgid);
int mqtt_unsubscribe_topic(char *topic, int msgid);
int mqtt_publish(char *topic, int qos, char *payload);
int mqtt_pingreq(void);

int mqtt_poll_once(void);
int mqtt_poll(uint32_t timeout_ms);
int mqtt_message_available(void);
int mqtt_message_pop(mqtt_rx_msg_t *message);

#endif /* CORE_MQTT_H_ */

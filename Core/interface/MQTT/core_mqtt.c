#include <stdio.h>
#include <string.h>

#include "stm32l4xx_hal.h"

#include "core_mqtt.h"
#include "log_config.h"
#include "transport.h"

#define CORE_MQTT_TX_BUF_SIZE 256U
#define CORE_MQTT_ACK_TIMEOUT_MS 3000U
#define CORE_MQTT_POLL_DELAY_MS 1U
#define CORE_MQTT_MESSAGE_QUEUE_LEN 2U

#if LOG_WIFI_ENABLE
#define mqtt_log(...) printf(__VA_ARGS__)
#else
#define mqtt_log(...) ((void)0)
#endif

#if LOG_WIFI_TRACE_ENABLE
#define mqtt_trace(...) printf(__VA_ARGS__)
#else
#define mqtt_trace(...) ((void)0)
#endif

typedef enum
{
    MQTT_WAIT_NONE = 0,
    MQTT_WAIT_CONNACK,
    MQTT_WAIT_SUBACK,
    MQTT_WAIT_UNSUBACK,
    MQTT_WAIT_PUBACK,
} mqtt_wait_type_t;

typedef struct
{
    uint8_t active;
    uint8_t done;
    mqtt_wait_type_t expect_type;
    uint16_t expect_msgid;
    mqtt_wait_result_t result;
    uint16_t ack_msgid;
    int granted_qos;
    uint8_t connack_rc;
    uint8_t session_present;
} mqtt_wait_ctx_t;

enum
{
    MQTT_POLL_TRANSPORT_ERROR = -1,
    MQTT_POLL_PROTOCOL_ERROR = -2,
    MQTT_POLL_NO_PACKET = 0,
    MQTT_POLL_PACKET_HANDLED = 1,
};

static unsigned char s_mqtt_rx_buf[MQTT_PACKET_BUF_SIZE];
static unsigned char s_mqtt_tx_buf[CORE_MQTT_TX_BUF_SIZE];
static MQTTTransport s_mqtt_transport;
static mqtt_wait_ctx_t s_wait_ctx;
static mqtt_rx_msg_t s_message_queue[CORE_MQTT_MESSAGE_QUEUE_LEN];
static uint8_t s_message_read_index;
static uint8_t s_message_write_index;
static uint8_t s_message_count;
static uint8_t s_socket_open;
static uint8_t s_mqtt_connected;
static uint16_t s_next_packet_id;

static int mqtt_transport_getdata(void *context, unsigned char *buf, int count)
{
    (void)context;
    return transport_getdata(buf, count);
}

static void mqtt_transport_state_reset(void)
{
    memset(&s_mqtt_transport, 0, sizeof(s_mqtt_transport));
    s_mqtt_transport.getfn = mqtt_transport_getdata;
}

static void mqtt_wait_ctx_reset(void)
{
    memset(&s_wait_ctx, 0, sizeof(s_wait_ctx));
    s_wait_ctx.result = MQTT_WAIT_RESULT_IDLE;
    s_wait_ctx.granted_qos = -1;
}

static int mqtt_wait_begin(mqtt_wait_type_t expect_type, uint16_t expect_msgid)
{
    if (s_wait_ctx.active != 0U)
    {
        mqtt_log("MQTT: another ACK wait is already active\r\n");
        return -1;
    }

    mqtt_wait_ctx_reset();
    s_wait_ctx.active = 1U;
    s_wait_ctx.expect_type = expect_type;
    s_wait_ctx.expect_msgid = expect_msgid;
    return 0;
}

static void mqtt_wait_fail(mqtt_wait_result_t result)
{
    if (s_wait_ctx.active == 0U || s_wait_ctx.done != 0U)
    {
        return;
    }

    s_wait_ctx.result = result;
    s_wait_ctx.done = 1U;
}

static void mqtt_message_queue_reset(void)
{
    memset(s_message_queue, 0, sizeof(s_message_queue));
    s_message_read_index = 0U;
    s_message_write_index = 0U;
    s_message_count = 0U;
}

static void mqtt_session_state_reset(void)
{
    mqtt_transport_state_reset();
    mqtt_wait_ctx_reset();
    mqtt_message_queue_reset();
    memset(s_mqtt_rx_buf, 0, sizeof(s_mqtt_rx_buf));
    memset(s_mqtt_tx_buf, 0, sizeof(s_mqtt_tx_buf));
    s_socket_open = 0U;
    s_mqtt_connected = 0U;
    s_next_packet_id = 0U;
}

static uint16_t mqtt_allocate_packet_id(void)
{
    s_next_packet_id++;
    if (s_next_packet_id == 0U)
    {
        s_next_packet_id = 1U;
    }

    return s_next_packet_id;
}

static int mqtt_wait_matches(mqtt_wait_type_t type, uint16_t msgid)
{
    if (s_wait_ctx.active == 0U || s_wait_ctx.expect_type != type)
    {
        mqtt_trace("MQTT: unexpected ACK type=%d msgid=%u\r\n", (int)type, (unsigned int)msgid);
        return 0;
    }

    if (s_wait_ctx.expect_msgid != msgid)
    {
        mqtt_trace("MQTT: unmatched ACK msgid=%u, expected=%u\r\n", (unsigned int)msgid,
                   (unsigned int)s_wait_ctx.expect_msgid);
        return 0;
    }

    return 1;
}

static int mqtt_handle_connack(int packet_len)
{
    uint8_t connack_rc;
    uint8_t session_present;
    int rv;

    connack_rc = 0U;
    session_present = 0U;
    rv = MQTTDeserialize_connack(&session_present, &connack_rc, s_mqtt_rx_buf, packet_len);
    if (rv != 1)
    {
        mqtt_log("MQTT: malformed CONNACK\r\n");
        return MQTT_POLL_PROTOCOL_ERROR;
    }

    if (s_wait_ctx.active == 0U || s_wait_ctx.expect_type != MQTT_WAIT_CONNACK)
    {
        mqtt_trace("MQTT: unexpected CONNACK rc=%u\r\n", (unsigned int)connack_rc);
        return MQTT_POLL_PACKET_HANDLED;
    }

    s_wait_ctx.connack_rc = connack_rc;
    s_wait_ctx.session_present = session_present;
    s_wait_ctx.result = (connack_rc == MQTT_CONNECTION_ACCEPTED) ? MQTT_WAIT_RESULT_SUCCESS
                                                                 : MQTT_WAIT_RESULT_REJECTED;
    s_wait_ctx.done = 1U;
    return MQTT_POLL_PACKET_HANDLED;
}

static int mqtt_handle_suback(int packet_len)
{
    uint16_t submsgid;
    int subcount;
    int granted_qos;
    int rv;

    submsgid = 0U;
    subcount = 0;
    granted_qos = -1;
    rv = MQTTDeserialize_suback(&submsgid, 1, &subcount, &granted_qos, s_mqtt_rx_buf, packet_len);
    if (rv != 1 || subcount != 1)
    {
        mqtt_log("MQTT: malformed SUBACK\r\n");
        return MQTT_POLL_PROTOCOL_ERROR;
    }

    if (!mqtt_wait_matches(MQTT_WAIT_SUBACK, submsgid))
    {
        return MQTT_POLL_PACKET_HANDLED;
    }

    s_wait_ctx.ack_msgid = submsgid;
    s_wait_ctx.granted_qos = granted_qos;
    s_wait_ctx.result =
        (granted_qos == 0x80) ? MQTT_WAIT_RESULT_REJECTED : MQTT_WAIT_RESULT_SUCCESS;
    s_wait_ctx.done = 1U;
    return MQTT_POLL_PACKET_HANDLED;
}

static int mqtt_handle_unsuback(int packet_len)
{
    uint16_t unsubmsgid;
    int rv;

    unsubmsgid = 0U;
    rv = MQTTDeserialize_unsuback(&unsubmsgid, s_mqtt_rx_buf, packet_len);
    if (rv != 1)
    {
        mqtt_log("MQTT: malformed UNSUBACK\r\n");
        return MQTT_POLL_PROTOCOL_ERROR;
    }

    if (!mqtt_wait_matches(MQTT_WAIT_UNSUBACK, unsubmsgid))
    {
        return MQTT_POLL_PACKET_HANDLED;
    }

    s_wait_ctx.ack_msgid = unsubmsgid;
    s_wait_ctx.result = MQTT_WAIT_RESULT_SUCCESS;
    s_wait_ctx.done = 1U;
    return MQTT_POLL_PACKET_HANDLED;
}

static int mqtt_handle_puback(int packet_len)
{
    uint8_t packet_type;
    uint8_t dup;
    uint16_t packet_id;
    int rv;

    packet_type = 0U;
    dup = 0U;
    packet_id = 0U;
    rv = MQTTDeserialize_ack(&packet_type, &dup, &packet_id, s_mqtt_rx_buf, packet_len);
    if (rv != 1 || packet_type != PUBACK)
    {
        mqtt_log("MQTT: malformed PUBACK\r\n");
        return MQTT_POLL_PROTOCOL_ERROR;
    }

    if (!mqtt_wait_matches(MQTT_WAIT_PUBACK, packet_id))
    {
        return MQTT_POLL_PACKET_HANDLED;
    }

    s_wait_ctx.ack_msgid = packet_id;
    s_wait_ctx.result = MQTT_WAIT_RESULT_SUCCESS;
    s_wait_ctx.done = 1U;
    return MQTT_POLL_PACKET_HANDLED;
}

static int mqtt_send_puback(uint16_t packet_id)
{
    unsigned char ack_buf[4];
    int rv;

    rv = MQTTSerialize_puback(ack_buf, (int)sizeof(ack_buf), packet_id);
    if (rv <= 0 || transport_sendPacketBuffer(ack_buf, rv) != rv)
    {
        mqtt_log("MQTT: PUBACK send failed, packet_id=%u\r\n", (unsigned int)packet_id);
        return -1;
    }

    mqtt_trace("MQTT: PUBACK sent, packet_id=%u\r\n", (unsigned int)packet_id);
    return 0;
}

static int mqtt_handle_publish(int packet_len)
{
    mqtt_rx_msg_t *node;
    MQTTString topic_name = MQTTString_initializer;
    unsigned char *payload;
    unsigned char dup;
    unsigned char retained;
    uint16_t packet_id;
    int payload_len;
    int qos;
    int topic_len;
    int rv;

    payload = NULL;
    dup = 0U;
    retained = 0U;
    packet_id = 0U;
    payload_len = 0;
    qos = 0;

    rv = MQTTDeserialize_publish(&dup, &qos, &retained, &packet_id, &topic_name, &payload,
                                 &payload_len, s_mqtt_rx_buf, packet_len);
    if (rv != 1)
    {
        mqtt_log("MQTT: malformed PUBLISH\r\n");
        return MQTT_POLL_PROTOCOL_ERROR;
    }

    if (topic_name.cstring != NULL)
    {
        topic_len = (int)strlen(topic_name.cstring);
    }
    else
    {
        topic_len = topic_name.lenstring.len;
    }

    if (topic_len <= 0 || topic_len > (int)MQTT_RX_TOPIC_MAX)
    {
        mqtt_log("MQTT: PUBLISH topic length invalid, bytes=%d\r\n", topic_len);
        return MQTT_POLL_PROTOCOL_ERROR;
    }

    if (payload_len < 0 || payload_len > (int)MQTT_RX_PAYLOAD_MAX)
    {
        mqtt_log("MQTT: PUBLISH payload too large, bytes=%d\r\n", payload_len);
        return MQTT_POLL_PROTOCOL_ERROR;
    }

    if (qos < Qos0 || qos > Qos1)
    {
        mqtt_log("MQTT: unsupported downlink QoS=%d\r\n", qos);
        return MQTT_POLL_PROTOCOL_ERROR;
    }

    if (s_message_count >= CORE_MQTT_MESSAGE_QUEUE_LEN)
    {
        mqtt_log("MQTT: receive queue full, topic=[%.*s]\r\n", topic_len,
                 (topic_name.cstring != NULL) ? topic_name.cstring : topic_name.lenstring.data);
        return MQTT_POLL_PROTOCOL_ERROR;
    }

    node = &s_message_queue[s_message_write_index];
    memset(node, 0, sizeof(*node));
    node->packet_id = packet_id;
    node->topic_len = (uint16_t)topic_len;
    node->payload_len = (uint16_t)payload_len;
    node->qos = (uint8_t)qos;
    node->dup = dup;
    node->retained = retained;

    if (topic_name.cstring != NULL)
    {
        memcpy(node->topic, topic_name.cstring, (uint32_t)topic_len);
    }
    else
    {
        memcpy(node->topic, topic_name.lenstring.data, (uint32_t)topic_len);
    }
    node->topic[topic_len] = '\0';

    if (payload_len > 0)
    {
        memcpy(node->payload, payload, (uint32_t)payload_len);
    }

    s_message_write_index = (uint8_t)((s_message_write_index + 1U) % CORE_MQTT_MESSAGE_QUEUE_LEN);
    s_message_count++;
    mqtt_trace("MQTT: PUBLISH queued topic=[%s], bytes=%u, count=%u\r\n", node->topic,
               (unsigned int)node->payload_len, (unsigned int)s_message_count);

    if (qos == Qos1 && mqtt_send_puback(packet_id) != 0)
    {
        return MQTT_POLL_TRANSPORT_ERROR;
    }

    return MQTT_POLL_PACKET_HANDLED;
}

static mqtt_wait_result_t mqtt_wait_for_completion(uint32_t timeout_ms)
{
    uint32_t start_tick;
    int rv;

    start_tick = HAL_GetTick();
    while (s_wait_ctx.done == 0U)
    {
        rv = mqtt_poll_once();
        if (rv < 0)
        {
            if (s_wait_ctx.done == 0U)
            {
                mqtt_wait_fail((rv == MQTT_POLL_TRANSPORT_ERROR) ? MQTT_WAIT_RESULT_TRANSPORT_ERROR
                                                                 : MQTT_WAIT_RESULT_PROTOCOL_ERROR);
            }
            break;
        }

        if ((HAL_GetTick() - start_tick) >= timeout_ms)
        {
            mqtt_wait_fail(MQTT_WAIT_RESULT_TIMEOUT);
            mqtt_transport_state_reset();
            s_mqtt_connected = 0U;
            break;
        }

        if (rv == MQTT_POLL_NO_PACKET)
        {
            HAL_Delay(CORE_MQTT_POLL_DELAY_MS);
        }
    }

    return s_wait_ctx.result;
}

int mqtt_poll_once(void)
{
    int packet_len;
    int packet_type;
    int rv;

    if (s_socket_open == 0U)
    {
        return MQTT_POLL_TRANSPORT_ERROR;
    }

    packet_type = MQTTPacket_readnb(s_mqtt_rx_buf, (int)sizeof(s_mqtt_rx_buf), &s_mqtt_transport);
    if (packet_type == 0)
    {
        return MQTT_POLL_NO_PACKET;
    }

    if (packet_type < 0)
    {
        mqtt_log("MQTT: packet receive failed, rv=%d\r\n", packet_type);
        mqtt_transport_state_reset();
        mqtt_wait_fail(MQTT_WAIT_RESULT_TRANSPORT_ERROR);
        s_mqtt_connected = 0U;
        return MQTT_POLL_TRANSPORT_ERROR;
    }

    packet_len = s_mqtt_transport.len;
    if (packet_len <= 0 || packet_len > (int)sizeof(s_mqtt_rx_buf))
    {
        mqtt_log("MQTT: invalid packet length=%d\r\n", packet_len);
        mqtt_transport_state_reset();
        mqtt_wait_fail(MQTT_WAIT_RESULT_PROTOCOL_ERROR);
        s_mqtt_connected = 0U;
        return MQTT_POLL_PROTOCOL_ERROR;
    }

    mqtt_trace("MQTT: packet type=%d, bytes=%d\r\n", packet_type, packet_len);
    switch (packet_type)
    {
    case CONNACK:
        rv = mqtt_handle_connack(packet_len);
        break;

    case SUBACK:
        rv = mqtt_handle_suback(packet_len);
        break;

    case UNSUBACK:
        rv = mqtt_handle_unsuback(packet_len);
        break;

    case PUBACK:
        rv = mqtt_handle_puback(packet_len);
        break;

    case PUBLISH:
        rv = mqtt_handle_publish(packet_len);
        break;

    case PINGRESP:
        mqtt_trace("MQTT: PINGRESP received\r\n");
        rv = MQTT_POLL_PACKET_HANDLED;
        break;

    default:
        mqtt_trace("MQTT: packet type=%d ignored\r\n", packet_type);
        rv = MQTT_POLL_PACKET_HANDLED;
        break;
    }

    if (rv < 0)
    {
        mqtt_transport_state_reset();
        mqtt_wait_fail((rv == MQTT_POLL_TRANSPORT_ERROR) ? MQTT_WAIT_RESULT_TRANSPORT_ERROR
                                                         : MQTT_WAIT_RESULT_PROTOCOL_ERROR);
        s_mqtt_connected = 0U;
    }

    return rv;
}

int mqtt_poll(uint32_t timeout_ms)
{
    uint32_t start_tick;
    int packet_count;
    int rv;

    start_tick = HAL_GetTick();
    packet_count = 0;

    do
    {
        rv = mqtt_poll_once();
        if (rv < 0)
        {
            return rv;
        }

        if (rv == MQTT_POLL_PACKET_HANDLED)
        {
            packet_count++;
        }
        else if (timeout_ms > 0U)
        {
            HAL_Delay(CORE_MQTT_POLL_DELAY_MS);
        }
    } while (timeout_ms > 0U && (HAL_GetTick() - start_tick) < timeout_ms);

    return packet_count;
}

int mqtt_message_available(void)
{
    return (int)s_message_count;
}

int mqtt_message_pop(mqtt_rx_msg_t *message)
{
    mqtt_rx_msg_t *node;

    if (message == NULL)
    {
        return -1;
    }

    if (s_message_count == 0U)
    {
        return 0;
    }

    node = &s_message_queue[s_message_read_index];
    *message = *node;
    memset(node, 0, sizeof(*node));

    s_message_read_index = (uint8_t)((s_message_read_index + 1U) % CORE_MQTT_MESSAGE_QUEUE_LEN);
    s_message_count--;
    mqtt_trace("MQTT: PUBLISH popped topic=[%s], bytes=%u, count=%u\r\n", message->topic,
               (unsigned int)message->payload_len, (unsigned int)s_message_count);
    return 1;
}

int mqtt_connect(char *host, int port, char *clientid, char *username, char *passwd)
{
    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    mqtt_wait_result_t wait_result;
    uint8_t connack_rc;
    int rv;

    if (host == NULL || port <= 0 || clientid == NULL)
    {
        mqtt_log("MQTT: invalid connect arguments\r\n");
        return -1;
    }

    mqtt_session_state_reset();
    if (transport_open(host, port) != 0)
    {
        mqtt_log("MQTT: socket connect [%s:%d] failed\r\n", host, port);
        return -2;
    }
    s_socket_open = 1U;

    data.MQTTVersion = 4U;
    data.clientID.cstring = clientid;
    data.keepAliveInterval = MQTT_KEEP_ALIVE_TIMEOUT_SECONDS;
    data.cleansession = 1U;
    if (username != NULL)
    {
        data.username.cstring = username;
    }
    if (passwd != NULL)
    {
        data.password.cstring = passwd;
    }

    rv = MQTTSerialize_connect(s_mqtt_tx_buf, (int)sizeof(s_mqtt_tx_buf), &data);
    if (rv <= 0)
    {
        mqtt_log("MQTT: CONNECT serialize failed, rv=%d\r\n", rv);
        transport_close();
        mqtt_session_state_reset();
        return -3;
    }

    if (mqtt_wait_begin(MQTT_WAIT_CONNACK, 0U) != 0)
    {
        transport_close();
        mqtt_session_state_reset();
        return -3;
    }

    if (transport_sendPacketBuffer(s_mqtt_tx_buf, rv) != rv)
    {
        mqtt_log("MQTT: CONNECT send failed\r\n");
        mqtt_wait_ctx_reset();
        transport_close();
        mqtt_session_state_reset();
        return -3;
    }

    wait_result = mqtt_wait_for_completion(CORE_MQTT_ACK_TIMEOUT_MS);
    connack_rc = s_wait_ctx.connack_rc;
    mqtt_wait_ctx_reset();
    if (wait_result != MQTT_WAIT_RESULT_SUCCESS)
    {
        mqtt_log("MQTT: CONNACK wait failed, result=%d rc=%u\r\n", (int)wait_result,
                 (unsigned int)connack_rc);
        transport_close();
        mqtt_session_state_reset();
        return (wait_result == MQTT_WAIT_RESULT_REJECTED) ? -4 : -3;
    }

    s_mqtt_connected = 1U;
    mqtt_log("MQTT: connected [%s:%d]\r\n", host, port);
    return 0;
}

int mqtt_disconnect(void)
{
    int close_rv;
    int rv;
    int send_rv;

    if (s_socket_open == 0U)
    {
        mqtt_session_state_reset();
        return 0;
    }

    rv = MQTTSerialize_disconnect(s_mqtt_tx_buf, (int)sizeof(s_mqtt_tx_buf));
    send_rv = -1;
    if (rv > 0)
    {
        send_rv = transport_sendPacketBuffer(s_mqtt_tx_buf, rv);
    }

    close_rv = transport_close();
    mqtt_session_state_reset();

    if (rv <= 0)
    {
        mqtt_log("MQTT: DISCONNECT serialize failed, rv=%d\r\n", rv);
        return -1;
    }
    if (send_rv != rv)
    {
        mqtt_log("MQTT: DISCONNECT send failed\r\n");
        return -2;
    }

    return close_rv;
}

int mqtt_subscribe_topic(char *topic, int qos, int msgid)
{
    MQTTString topic_string = MQTTString_initializer;
    mqtt_wait_result_t wait_result;
    int granted_qos;
    int rv;

    if (s_mqtt_connected == 0U || topic == NULL || qos < Qos0 || qos > Qos1 || msgid <= 0 ||
        msgid > 0xFFFF)
    {
        mqtt_log("MQTT: invalid subscribe arguments\r\n");
        return -1;
    }

    topic_string.cstring = topic;
    rv = MQTTSerialize_subscribe(s_mqtt_tx_buf, (int)sizeof(s_mqtt_tx_buf), 0U, (uint16_t)msgid, 1,
                                 &topic_string, &qos);
    if (rv <= 0)
    {
        mqtt_log("MQTT: SUBSCRIBE serialize failed, rv=%d\r\n", rv);
        return -2;
    }

    if (mqtt_wait_begin(MQTT_WAIT_SUBACK, (uint16_t)msgid) != 0)
    {
        return -3;
    }
    if (transport_sendPacketBuffer(s_mqtt_tx_buf, rv) != rv)
    {
        mqtt_log("MQTT: SUBSCRIBE send failed, msgid=%d\r\n", msgid);
        mqtt_wait_ctx_reset();
        return -3;
    }

    wait_result = mqtt_wait_for_completion(CORE_MQTT_ACK_TIMEOUT_MS);
    granted_qos = s_wait_ctx.granted_qos;
    mqtt_wait_ctx_reset();
    if (wait_result != MQTT_WAIT_RESULT_SUCCESS)
    {
        mqtt_log("MQTT: SUBACK wait failed, msgid=%d result=%d qos=%d\r\n", msgid, (int)wait_result,
                 granted_qos);
        return (wait_result == MQTT_WAIT_RESULT_REJECTED) ? -5 : -4;
    }

    mqtt_trace("MQTT: subscribed topic=[%s], msgid=%d qos=%d\r\n", topic, msgid, granted_qos);
    return 0;
}

int mqtt_unsubscribe_topic(char *topic, int msgid)
{
    MQTTString topic_string = MQTTString_initializer;
    mqtt_wait_result_t wait_result;
    int rv;

    if (s_mqtt_connected == 0U || topic == NULL || msgid <= 0 || msgid > 0xFFFF)
    {
        mqtt_log("MQTT: invalid unsubscribe arguments\r\n");
        return -1;
    }

    topic_string.cstring = topic;
    rv = MQTTSerialize_unsubscribe(s_mqtt_tx_buf, (int)sizeof(s_mqtt_tx_buf), 0U, (uint16_t)msgid,
                                   1, &topic_string);
    if (rv <= 0)
    {
        mqtt_log("MQTT: UNSUBSCRIBE serialize failed, rv=%d\r\n", rv);
        return -2;
    }

    if (mqtt_wait_begin(MQTT_WAIT_UNSUBACK, (uint16_t)msgid) != 0)
    {
        return -3;
    }
    if (transport_sendPacketBuffer(s_mqtt_tx_buf, rv) != rv)
    {
        mqtt_log("MQTT: UNSUBSCRIBE send failed, msgid=%d\r\n", msgid);
        mqtt_wait_ctx_reset();
        return -3;
    }

    wait_result = mqtt_wait_for_completion(CORE_MQTT_ACK_TIMEOUT_MS);
    mqtt_wait_ctx_reset();
    if (wait_result != MQTT_WAIT_RESULT_SUCCESS)
    {
        mqtt_log("MQTT: UNSUBACK wait failed, msgid=%d result=%d\r\n", msgid, (int)wait_result);
        return -4;
    }

    return 0;
}

int mqtt_publish(char *topic, int qos, char *payload)
{
    MQTTString topic_string = MQTTString_initializer;
    mqtt_wait_result_t wait_result;
    uint16_t packet_id;
    int rv;

    if (s_mqtt_connected == 0U || topic == NULL || payload == NULL || qos < Qos0 || qos > Qos1)
    {
        mqtt_log("MQTT: invalid publish arguments\r\n");
        return -1;
    }

    packet_id = (qos == Qos1) ? mqtt_allocate_packet_id() : 0U;
    topic_string.cstring = topic;
    rv = MQTTSerialize_publish(s_mqtt_tx_buf, (int)sizeof(s_mqtt_tx_buf), 0U, qos, 0U, packet_id,
                               topic_string, (unsigned char *)payload, (int)strlen(payload));
    if (rv <= 0)
    {
        mqtt_log("MQTT: PUBLISH serialize failed, rv=%d\r\n", rv);
        return -2;
    }

    if (qos == Qos1 && mqtt_wait_begin(MQTT_WAIT_PUBACK, packet_id) != 0)
    {
        return -3;
    }
    if (transport_sendPacketBuffer(s_mqtt_tx_buf, rv) != rv)
    {
        mqtt_log("MQTT: PUBLISH send failed, packet_id=%u\r\n", (unsigned int)packet_id);
        if (qos == Qos1)
        {
            mqtt_wait_ctx_reset();
        }
        return -3;
    }

    if (qos == Qos0)
    {
        return 0;
    }

    wait_result = mqtt_wait_for_completion(CORE_MQTT_ACK_TIMEOUT_MS);
    mqtt_wait_ctx_reset();
    if (wait_result != MQTT_WAIT_RESULT_SUCCESS)
    {
        mqtt_log("MQTT: PUBACK wait failed, packet_id=%u result=%d\r\n", (unsigned int)packet_id,
                 (int)wait_result);
        return -4;
    }

    return 0;
}

int mqtt_pingreq(void)
{
    int rv;

    if (s_mqtt_connected == 0U)
    {
        return -1;
    }

    rv = MQTTSerialize_pingreq(s_mqtt_tx_buf, (int)sizeof(s_mqtt_tx_buf));
    if (rv <= 0)
    {
        mqtt_log("MQTT: PINGREQ serialize failed, rv=%d\r\n", rv);
        return -1;
    }

    if (transport_sendPacketBuffer(s_mqtt_tx_buf, rv) != rv)
    {
        mqtt_log("MQTT: PINGREQ send failed\r\n");
        return -2;
    }

    return 0;
}

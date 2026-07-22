#include <stdio.h>
#include <string.h>

#include "stm32l4xx_hal.h"

#include "core_mqtt.h"

#define CORE_MQTT_BUF_SIZE      256U
#define CORE_MQTT_READ_RETRY    50U
#define CORE_MQTT_READ_DELAY_MS 20U

static int mqtt_wait_packet_type(unsigned char *buf, int buflen, int expect_type)
{
    uint32_t retry;
    int      rv;

    for (retry = 0U; retry < CORE_MQTT_READ_RETRY; retry++)
    {
        memset(buf, 0, (uint32_t)buflen);
        rv = MQTTPacket_read(buf, buflen, transport_getdata);
        if (rv == expect_type)
        {
            return rv;
        }

        if (rv < 0)
        {
            return rv;
        }

        HAL_Delay(CORE_MQTT_READ_DELAY_MS);
    }

    return -1;
}

int mqtt_connect(char *host, int port, char *clientid, char *username, char *passwd)
{
    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    int                    rv;
    unsigned char          buf[CORE_MQTT_BUF_SIZE];
    unsigned char          session_present = 0U;
    unsigned char          connack_rc      = 0U;

    if (host == NULL || port <= 0 || clientid == NULL)
    {
        printf("ERROR: Invalid input arguments\r\n");
        return -1;
    }

    if ((rv = transport_open(host, port)) < 0)
    {
        printf("socket connect [%s:%d] failure, rv=%d\r\n", host, port, rv);
        return rv;
    }

    printf("socket connect [%s:%d] ok\r\n", host, port);

    data.MQTTVersion       = 4U;
    data.clientID.cstring  = clientid;
    data.keepAliveInterval = MQTT_KEEP_ALIVE_TIMEOUT_SECONDS;
    data.cleansession      = 1U;

    if (username != NULL)
    {
        data.username.cstring = username;
    }

    if (passwd != NULL)
    {
        data.password.cstring = passwd;
    }

    rv = MQTTSerialize_connect(buf, (int)sizeof(buf), &data);
    if (rv < 0)
    {
        printf("MQTTSerialize_connect failure, rv=%d\r\n", rv);
        transport_close();
        return -1;
    }

    if (rv != transport_sendPacketBuffer(buf, rv))
    {
        printf("transport_sendPacketBuffer for mqtt_connect failure, rv=%d\r\n", rv);
        transport_close();
        return -2;
    }

    HAL_Delay(800U);
    rv = mqtt_wait_packet_type(buf, (int)sizeof(buf), CONNACK);
    if (rv != CONNACK)
    {
        printf("MQTTPacket_read for MQTT CONNACK failure, rv=%d\r\n", rv);
        transport_close();
        return -3;
    }

    rv = MQTTDeserialize_connack(&session_present, &connack_rc, buf, (int)sizeof(buf));
    if (rv != 1 || connack_rc != MQTT_CONNECTION_ACCEPTED)
    {
        printf("MQTTDeserialize_connack failure, rv=%d connack_rc=%u\r\n", rv, connack_rc);
        transport_close();
        return -4;
    }

    return 0;
}

int mqtt_disconnect(void)
{
    int           rv;
    unsigned char buf[CORE_MQTT_BUF_SIZE];

    rv = MQTTSerialize_disconnect(buf, (int)sizeof(buf));
    if (rv < 0)
    {
        printf("MQTTSerialize_disconnect failure, rv=%d\r\n", rv);
        return -1;
    }

    if (rv != transport_sendPacketBuffer(buf, rv))
    {
        printf("transport_sendPacketBuffer for mqtt_disconnect failure, rv=%d\r\n", rv);
        return -2;
    }

    return transport_close();
}

int mqtt_subscribe_topic(char *topic, int qos, int msgid)
{
    MQTTString     topic_string = MQTTString_initializer;
    unsigned short submsgid;
    int            subcount;
    int            granted_qos;
    int            rv;
    unsigned char  buf[CORE_MQTT_BUF_SIZE];

    if (topic == NULL || qos < Qos0 || qos > Qos1 || msgid <= 0)
    {
        printf("ERROR: Invalid subscribe arguments\r\n");
        return -1;
    }

    topic_string.cstring = topic;

    rv = MQTTSerialize_subscribe(
        buf, (int)sizeof(buf), 0U, (unsigned short)msgid, 1, &topic_string, &qos);
    if (rv < 0)
    {
        printf("MQTTSerialize_subscribe failure, rv=%d\r\n", rv);
        return -2;
    }

    if (rv != transport_sendPacketBuffer(buf, rv))
    {
        printf("transport_sendPacketBuffer for mqtt_subscribe failure, rv=%d\r\n", rv);
        return -3;
    }

    rv = mqtt_wait_packet_type(buf, (int)sizeof(buf), SUBACK);
    if (rv != SUBACK)
    {
        printf("MQTTPacket_read for MQTT SUBACK failure, rv=%d\r\n", rv);
        return -4;
    }

    rv = MQTTDeserialize_suback(&submsgid, 1, &subcount, &granted_qos, buf, (int)sizeof(buf));
    if (rv != 1 || submsgid != (unsigned short)msgid || subcount != 1 || granted_qos == 0x80)
    {
        printf("MQTTDeserialize_suback failure, rv=%d submsgid=%u granted_qos=%d\r\n",
               rv,
               (unsigned int)submsgid,
               granted_qos);
        return -5;
    }

    return 0;
}

int mqtt_unsubscribe_topic(char *topic, int msgid)
{
    MQTTString     topic_string = MQTTString_initializer;
    unsigned short unsubmsgid;
    int            rv;
    unsigned char  buf[CORE_MQTT_BUF_SIZE];

    if (topic == NULL || msgid <= 0)
    {
        printf("ERROR: Invalid unsubscribe arguments\r\n");
        return -1;
    }

    topic_string.cstring = topic;

    rv = MQTTSerialize_unsubscribe(
        buf, (int)sizeof(buf), 0U, (unsigned short)msgid, 1, &topic_string);
    if (rv < 0)
    {
        printf("MQTTSerialize_unsubscribe failure, rv=%d\r\n", rv);
        return -2;
    }

    if (rv != transport_sendPacketBuffer(buf, rv))
    {
        printf("transport_sendPacketBuffer for mqtt_unsubscribe failure, rv=%d\r\n", rv);
        return -3;
    }

    rv = mqtt_wait_packet_type(buf, (int)sizeof(buf), UNSUBACK);
    if (rv != UNSUBACK)
    {
        printf("MQTTPacket_read for MQTT UNSUBACK failure, rv=%d\r\n", rv);
        return -4;
    }

    rv = MQTTDeserialize_unsuback(&unsubmsgid, buf, (int)sizeof(buf));
    if (rv != 1 || unsubmsgid != (unsigned short)msgid)
    {
        printf("MQTTDeserialize_unsuback failure, rv=%d unsubmsgid=%u\r\n",
               rv,
               (unsigned int)unsubmsgid);
        return -5;
    }

    return 0;
}

int mqtt_publish(char *topic, int qos, char *payload)
{
    MQTTString     topic_string = MQTTString_initializer;
    int            rv;
    unsigned char  buf[CORE_MQTT_BUF_SIZE];
    unsigned short packet_id = 0U;

    if (topic == NULL || payload == NULL || qos < Qos0 || qos > Qos1)
    {
        printf("ERROR: Invalid publish arguments\r\n");
        return -1;
    }

    topic_string.cstring = topic;
    if (qos > 0)
    {
        packet_id = 1U;
    }

    rv = MQTTSerialize_publish(buf,
                               (int)sizeof(buf),
                               0U,
                               qos,
                               0U,
                               packet_id,
                               topic_string,
                               (unsigned char *)payload,
                               (int)strlen(payload));
    if (rv < 0)
    {
        printf("MQTTSerialize_publish failure, rv=%d\r\n", rv);
        return -2;
    }

    if (rv != transport_sendPacketBuffer(buf, rv))
    {
        printf("transport_sendPacketBuffer for mqtt_publish failure, rv=%d\r\n", rv);
        return -3;
    }

    return 0;
}

int mqtt_pingreq(void)
{
    int           rv;
    unsigned char buf[CORE_MQTT_BUF_SIZE];

    rv = MQTTSerialize_pingreq(buf, (int)sizeof(buf));
    if (rv < 0)
    {
        printf("MQTTSerialize_pingreq failure, rv=%d\r\n", rv);
        return -1;
    }

    if (rv != transport_sendPacketBuffer(buf, rv))
    {
        printf("transport_sendPacketBuffer for mqtt_pingreq failure, rv=%d\r\n", rv);
        return -2;
    }

    return 0;
}

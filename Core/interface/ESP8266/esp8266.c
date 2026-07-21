#include <stdlib.h>
#include <string.h>
#include "usart.h"
#include "esp8266.h"
#include "log_config.h"

#define ESP_RX_RING_SIZE          WIFI_RX_BUF_SIZE
#define ESP_LINE_BUF_SIZE         256U
#define ESP_MQTT_TOPIC_SIZE       128U
#define ESP_MQTT_PAYLOAD_SIZE     512U
#define ESP_MQTT_EVENT_QUEUE_SIZE 2U

typedef enum {
    AT_CMD_IDLE = 0,
    AT_CMD_WAITING,
    AT_CMD_SUCCESS,
    AT_CMD_ERROR,
    AT_CMD_TIMEOUT,
} at_cmd_state_t;

typedef struct {
    uint8_t active;
    char expect[32];
    uint8_t got_expect;
    uint8_t need_final_ok;
    at_cmd_state_t state;
} at_cmd_ctx_t;

typedef struct {
    char topic[ESP_MQTT_TOPIC_SIZE];
    uint16_t payload_len;
    uint8_t payload[ESP_MQTT_PAYLOAD_SIZE + 1U];
} mqtt_sub_event_t;

typedef enum {
    ESP_PARSE_LINE = 0,
    ESP_PARSE_MQTT_PAYLOAD,
} esp_parse_state_t;

static uint8_t s_wifi_rxch;
char g_wifi_rxbuf[WIFI_REPLY_BUF_SIZE] = {0};
volatile int g_wifi_rxbytes = 0;
static char s_atcmd_buf[256];

static uint8_t s_rx_ring[ESP_RX_RING_SIZE];
static volatile uint16_t s_rx_head = 0U;
static volatile uint16_t s_rx_tail = 0U;

static char s_line_buf[ESP_LINE_BUF_SIZE];
static uint16_t s_line_len = 0U;

static mqtt_sub_event_t s_mqtt_events[ESP_MQTT_EVENT_QUEUE_SIZE];
static uint8_t s_mqtt_evt_head = 0U;
static uint8_t s_mqtt_evt_tail = 0U;
static uint8_t s_mqtt_evt_count = 0U;
static mqtt_sub_event_t s_last_mqtt_event;
static uint8_t s_last_mqtt_valid = 0U;

static at_cmd_ctx_t s_cmd_ctx = {0};
static esp_parse_state_t s_parse_state = ESP_PARSE_LINE;
static mqtt_sub_event_t s_parse_mqtt_event;
static uint16_t s_parse_payload_pos = 0U;
static uint32_t s_tb_request_id = 1U;
static esp8266_tb_firmware_info_t s_tb_fw_info;

static void esp_at_process(void);
static int send_atcmd_ex(char *atcmd, char *expect_reply, uint8_t need_final_ok, unsigned int timeout);

void ESP8266_UartStartReceive(void)
{
    HAL_UART_Receive_IT(wifi_huart, &s_wifi_rxch, 1U);
}

void ESP8266_UartRxCpltCallback(UART_HandleTypeDef *huart)
{
    uint16_t next;

    if (huart->Instance != USART2) {
        return;
    }

    next = (uint16_t)((s_rx_head + 1U) % ESP_RX_RING_SIZE);
    if (next != s_rx_tail) {
        s_rx_ring[s_rx_head] = s_wifi_rxch;
        s_rx_head = next;
    }

    ESP8266_UartStartReceive();
}

void ESP8266_UartErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART2) {
        return;
    }

    __HAL_UART_CLEAR_OREFLAG(huart);
    huart->RxState = HAL_UART_STATE_READY;
    ESP8266_UartStartReceive();
}

#define ESP_AT_RESET_TIMEOUT_MS    5000U
#define ESP_AT_JOIN_TIMEOUT_MS     30000U
#define ESP_AT_QUERY_TIMEOUT_MS    1500U
#define ESP_TB_FW_INFO_RETRY       3U
#define ESP_TB_FW_INFO_TIMEOUT_MS  8000U

#if LOG_WIFI_ENABLE
#define wifi_dbg(format,args...)  printf(format, ##args)
#else
#define wifi_dbg(format,args...)  do{} while(0)
#endif

#if LOG_WIFI_ENABLE
#define wifi_print(format,args...) printf(format, ##args)
#else
#define wifi_print(format,args...) do{} while(0)
#endif

static void esp_debug_append(uint8_t ch)
{
    if( g_wifi_rxbytes < ((int)WIFI_REPLY_BUF_SIZE - 1) )
    {
        g_wifi_rxbuf[g_wifi_rxbytes++] = (char)ch;
        g_wifi_rxbuf[g_wifi_rxbytes] = '\0';
    }
}

static int esp_ring_pop(uint8_t *ch)
{
    if( s_rx_tail == s_rx_head )
    {
        return 0;
    }

    *ch = s_rx_ring[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1U) % ESP_RX_RING_SIZE);
    return 1;
}

static void mqtt_event_push(const mqtt_sub_event_t *evt)
{
    if( !evt || evt->payload_len == 0U )
    {
        return;
    }

    if( s_mqtt_evt_count >= ESP_MQTT_EVENT_QUEUE_SIZE )
    {
        s_mqtt_evt_tail = (uint8_t)((s_mqtt_evt_tail + 1U) % ESP_MQTT_EVENT_QUEUE_SIZE);
        s_mqtt_evt_count--;
    }

    s_mqtt_events[s_mqtt_evt_head] = *evt;
    s_mqtt_evt_head = (uint8_t)((s_mqtt_evt_head + 1U) % ESP_MQTT_EVENT_QUEUE_SIZE);
    s_mqtt_evt_count++;
    wifi_dbg("MQTT event queued topic [%s], %u bytes, count %u\r\n",
             evt->topic, evt->payload_len, s_mqtt_evt_count);
}

static int mqtt_event_pop(mqtt_sub_event_t *evt)
{
    if( s_mqtt_evt_count == 0U )
    {
        return 0;
    }

    *evt = s_mqtt_events[s_mqtt_evt_tail];
    s_mqtt_evt_tail = (uint8_t)((s_mqtt_evt_tail + 1U) % ESP_MQTT_EVENT_QUEUE_SIZE);
    s_mqtt_evt_count--;
    wifi_dbg("MQTT event popped topic [%s], %u bytes, count %u\r\n",
             evt->topic, evt->payload_len, s_mqtt_evt_count);
    return 1;
}

static void mqtt_event_clear(void)
{
    memset(s_mqtt_events, 0, sizeof(s_mqtt_events));
    memset(&s_last_mqtt_event, 0, sizeof(s_last_mqtt_event));
    s_mqtt_evt_head = 0U;
    s_mqtt_evt_tail = 0U;
    s_mqtt_evt_count = 0U;
    s_last_mqtt_valid = 0U;
}

static void esp8266_driver_reset_state(void)
{
    clear_atcmd_buf();
    s_rx_head = 0U;
    s_rx_tail = 0U;
    s_line_len = 0U;
    s_parse_state = ESP_PARSE_LINE;
    s_parse_payload_pos = 0U;
    memset(&s_parse_mqtt_event, 0, sizeof(s_parse_mqtt_event));
    memset(&s_cmd_ctx, 0, sizeof(s_cmd_ctx));
    mqtt_event_clear();
}

static int payload_contains(const uint8_t *payload, uint16_t payload_len, const char *text)
{
    uint16_t i;
    uint16_t j;
    uint16_t text_len;

    if( !payload || !text )
    {
        return 0;
    }

    text_len = (uint16_t)strlen(text);
    if( text_len == 0U || payload_len < text_len )
    {
        return 0;
    }

    for(i=0U; i <= (uint16_t)(payload_len - text_len); i++)
    {
        for(j=0U; j<text_len; j++)
        {
            if( payload[i + j] != (uint8_t)text[j] )
            {
                break;
            }
        }
        if( j == text_len )
        {
            return 1;
        }
    }

    return 0;
}

static int json_get_u32(const char *json, const char *key, uint32_t *value)
{
    char pattern[32];
    char *pos;

    if( !json || !key || !value )
    {
        return -1;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    pos = strstr(json, pattern);
    if( !pos )
    {
        return -2;
    }

    pos += strlen(pattern);
    *value = (uint32_t)strtoul(pos, NULL, 10);
    return 0;
}

static int json_get_string(const char *json, const char *key, char *out, uint16_t out_size)
{
    char pattern[40];
    char *pos;
    char *end;
    uint16_t len;

    if( !json || !key || !out || out_size == 0U )
    {
        return -1;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    pos = strstr(json, pattern);
    if( !pos )
    {
        return -2;
    }

    pos += strlen(pattern);
    end = strchr(pos, '"');
    if( !end )
    {
        return -3;
    }

    len = (uint16_t)(end - pos);
    if( len >= out_size )
    {
        len = out_size - 1U;
    }

    memcpy(out, pos, len);
    out[len] = '\0';
    return 0;
}

static int thingsboard_parse_firmware_info(void)
{
    char *json = (char *)s_last_mqtt_event.payload;

    memset(&s_tb_fw_info, 0, sizeof(s_tb_fw_info));

    if( !s_last_mqtt_valid || s_last_mqtt_event.payload_len == 0U )
    {
        return -1;
    }

    if( json_get_u32(json, "fw_size", &s_tb_fw_info.size) != 0 )
    {
        return -2;
    }

    json_get_string(json, "fw_title", s_tb_fw_info.title, sizeof(s_tb_fw_info.title));
    json_get_string(json, "fw_version", s_tb_fw_info.version, sizeof(s_tb_fw_info.version));
    json_get_string(json, "fw_checksum_algorithm",
                    s_tb_fw_info.checksum_algorithm,
                    sizeof(s_tb_fw_info.checksum_algorithm));
    json_get_string(json, "fw_checksum", s_tb_fw_info.checksum, sizeof(s_tb_fw_info.checksum));

    s_tb_fw_info.valid = 1U;
    return 0;
}

static void at_cmd_handle_text(const char *text)
{
    if( !s_cmd_ctx.active || !text )
    {
        return;
    }

    if( s_cmd_ctx.state == AT_CMD_SUCCESS ||
        s_cmd_ctx.state == AT_CMD_ERROR ||
        s_cmd_ctx.state == AT_CMD_TIMEOUT )
    {
        return;
    }

    if( strstr(text, "ERROR") || strstr(text, "FAIL") )
    {
        s_cmd_ctx.state = AT_CMD_ERROR;
        return;
    }

    if( s_cmd_ctx.expect[0] != '\0' && strstr(text, s_cmd_ctx.expect) )
    {
        s_cmd_ctx.got_expect = 1U;
        wifi_dbg("AT command matched expect '%s'\r\n", s_cmd_ctx.expect);
        if( !s_cmd_ctx.need_final_ok )
        {
            s_cmd_ctx.state = AT_CMD_SUCCESS;
        }
    }

    if( strstr(text, "OK") )
    {
        if( strcmp(s_cmd_ctx.expect, "OK\r\n") == 0 ||
            strcmp(s_cmd_ctx.expect, "OK") == 0 ||
            (s_cmd_ctx.need_final_ok && s_cmd_ctx.got_expect) )
        {
            s_cmd_ctx.state = AT_CMD_SUCCESS;
        }
    }
}

static int mqtt_payload_append_byte(uint8_t ch)
{
    if( s_parse_payload_pos < ESP_MQTT_PAYLOAD_SIZE )
    {
        s_parse_mqtt_event.payload[s_parse_payload_pos] = ch;
    }
    s_parse_payload_pos++;

    if( s_parse_payload_pos >= s_parse_mqtt_event.payload_len )
    {
        if( s_parse_mqtt_event.payload_len < ESP_MQTT_PAYLOAD_SIZE )
        {
            s_parse_mqtt_event.payload[s_parse_mqtt_event.payload_len] = '\0';
        }
        wifi_dbg("MQTT subrecv payload done topic [%s], %u bytes\r\n",
                 s_parse_mqtt_event.topic, s_parse_mqtt_event.payload_len);
        mqtt_event_push(&s_parse_mqtt_event);
        s_parse_state = ESP_PARSE_LINE;
        s_line_len = 0U;
        return 1;
    }

    return 0;
}

static int try_parse_mqtt_subrecv_header(void)
{
    char *topic_start;
    char *topic_end;
    char *len_start;
    char *payload_start;
    uint16_t topic_len;
    int payload_len;
    int buffered_payload_len;
    int i;

    s_line_buf[s_line_len] = '\0';
    if( strncmp(s_line_buf, "+MQTTSUBRECV:", strlen("+MQTTSUBRECV:")) != 0 )
    {
        return 0;
    }

    topic_start = strchr(s_line_buf, '"');
    if( !topic_start )
    {
        return 0;
    }

    topic_end = strchr(topic_start + 1, '"');
    if( !topic_end )
    {
        return 0;
    }

    len_start = strchr(topic_end + 1, ',');
    if( !len_start )
    {
        return 0;
    }
    len_start++;

    payload_start = strchr(len_start, ',');
    if( !payload_start )
    {
        return 0;
    }

    payload_len = atoi(len_start);
    if( payload_len < 0 || payload_len > (int)ESP_MQTT_PAYLOAD_SIZE )
    {
        wifi_print("ERROR: MQTT payload too large (%d)\r\n", payload_len);
        s_line_len = 0U;
        return 0;
    }

    memset(&s_parse_mqtt_event, 0, sizeof(s_parse_mqtt_event));
    topic_len = (uint16_t)(topic_end - topic_start - 1);
    if( topic_len >= ESP_MQTT_TOPIC_SIZE )
    {
        topic_len = ESP_MQTT_TOPIC_SIZE - 1U;
    }

    memcpy(s_parse_mqtt_event.topic, topic_start + 1, topic_len);
    s_parse_mqtt_event.topic[topic_len] = '\0';
    s_parse_mqtt_event.payload_len = (uint16_t)payload_len;
    s_parse_payload_pos = 0U;

    payload_start++;
    buffered_payload_len = (int)(s_line_len - (uint16_t)(payload_start - s_line_buf));
    for(i=0; i<buffered_payload_len && s_parse_payload_pos < (uint16_t)payload_len; i++)
    {
        if( mqtt_payload_append_byte((uint8_t)payload_start[i]) )
        {
            return 1;
        }
    }

    s_line_len = 0U;
    s_parse_state = ESP_PARSE_MQTT_PAYLOAD;
    return 1;
}

static void esp_parse_line(void)
{
    s_line_buf[s_line_len] = '\0';

    if( s_line_len == 0U )
    {
        return;
    }

    at_cmd_handle_text(s_line_buf);
    s_line_len = 0U;
}

static void esp_at_process(void)
{
    uint8_t ch;

    while( esp_ring_pop(&ch) )
    {
        if( s_parse_state == ESP_PARSE_MQTT_PAYLOAD )
        {
            mqtt_payload_append_byte(ch);
            continue;
        }

        esp_debug_append(ch);

        if( ch == '>' )
        {
            at_cmd_handle_text(">");
            s_line_len = 0U;
            continue;
        }

        if( s_line_len < (ESP_LINE_BUF_SIZE - 1U) )
        {
            s_line_buf[s_line_len++] = (char)ch;
        }
        else
        {
            s_line_len = 0U;
        }

        if( try_parse_mqtt_subrecv_header() )
        {
            continue;
        }

        if( ch == '\n' )
        {
            esp_parse_line();
        }
    }
}

static int esp8266_query_ip_status(unsigned int timeout)
{
    if( !send_atcmd_ex("AT+CIPSTA?\r\n", "255.", 1U, timeout) )
    {
        return 0;
    }

    return send_atcmd_ex("AT+CIPSTA_CUR?\r\n", "255.", 1U, timeout);
}

int send_atcmd(char *atcmd, char *expect_reply, unsigned int timeout)
{
    return send_atcmd_ex(atcmd, expect_reply, 0U, timeout);
}

static int send_atcmd_ex(char *atcmd, char *expect_reply, uint8_t need_final_ok, unsigned int timeout)
{
    int          rv = 1;
    unsigned int i;

    /* check function input arguments validation */
    if( !atcmd || strlen(atcmd)<=0 )
    {
        wifi_print("ERROR: Invalid input arguments\r\n");
        return -1;
    }

    wifi_dbg("\r\nStart send AT command: %s", atcmd);
    clear_atcmd_buf();
    memset(&s_cmd_ctx, 0, sizeof(s_cmd_ctx));
    s_cmd_ctx.active = 1U;
    s_cmd_ctx.state = AT_CMD_WAITING;
    s_cmd_ctx.need_final_ok = need_final_ok;
    strncpy(s_cmd_ctx.expect, expect_reply ? expect_reply : EXPECT_OK, sizeof(s_cmd_ctx.expect) - 1U);

    HAL_UART_Transmit(wifi_huart, (uint8_t *)atcmd, strlen(atcmd), 1000);

    /* Receive AT reply string by UART interrupt handler, stop by event parser or timeout */
    for(i=0; i<timeout; i++)
    {
        esp_at_process();

        if( s_cmd_ctx.state == AT_CMD_SUCCESS )
        {
            wifi_dbg("AT command Got expect reply '%s'\r\n", s_cmd_ctx.expect);
            rv = 0;
            goto CleanUp;
        }

        if( s_cmd_ctx.state == AT_CMD_ERROR )
        {
            rv = 2;
            goto CleanUp;
        }

        HAL_Delay(1);
    }

    s_cmd_ctx.state = AT_CMD_TIMEOUT;

CleanUp:
    s_cmd_ctx.active = 0U;
    wifi_dbg("<<<< AT command reply:\r\n%s", g_wifi_rxbuf);
    return rv;
}

int atcmd_send_data(unsigned char *data, int bytes, unsigned int timeout)
{
    int          rv = -1;
    unsigned int i;

    /* check function input arguments validation */
    if( !data || bytes <= 0 )
    {
        wifi_print("ERROR: Invalid input arguments\r\n");
        return -1;
    }

    wifi_dbg("\r\nStart AT command send [%d] bytes data\n", bytes);
    clear_atcmd_buf();
    memset(&s_cmd_ctx, 0, sizeof(s_cmd_ctx));
    s_cmd_ctx.active = 1U;
    s_cmd_ctx.state = AT_CMD_WAITING;
    strncpy(s_cmd_ctx.expect, "SEND OK", sizeof(s_cmd_ctx.expect) - 1U);

    HAL_UART_Transmit(wifi_huart, data, bytes, 1000);

    /* Receive AT reply string by UART interrupt handler, stop by "OK/ERROR" or timeout */
    for(i=0; i<timeout; i++)
    {
        esp_at_process();

        if( s_cmd_ctx.state == AT_CMD_SUCCESS )
        {
            rv = 0;
            goto CleanUp;
        }

        if( s_cmd_ctx.state == AT_CMD_ERROR )
        {
            rv = 1;
            goto CleanUp;
        }

        HAL_Delay(1);
    }

    s_cmd_ctx.state = AT_CMD_TIMEOUT;
    wifi_print("ERROR: AT command send data timeout\r\n");

CleanUp:
    s_cmd_ctx.active = 0U;
    wifi_dbg("<<<< AT command reply:\r\n%s", g_wifi_rxbuf);
    return rv;
}

static int atcmd_send_data_expect(unsigned char *data, int bytes, char *expect_reply, unsigned int timeout)
{
    int          rv = -1;
    unsigned int i;
    char        *expect = expect_reply ? expect_reply : EXPECT_OK;

    if( !data || bytes <= 0 )
    {
        wifi_print("ERROR: Invalid input arguments\r\n");
        return -1;
    }

    wifi_dbg("\r\nStart AT command send [%d] bytes raw data\n", bytes);
    clear_atcmd_buf();
    memset(&s_cmd_ctx, 0, sizeof(s_cmd_ctx));
    s_cmd_ctx.active = 1U;
    s_cmd_ctx.state = AT_CMD_WAITING;
    strncpy(s_cmd_ctx.expect, expect, sizeof(s_cmd_ctx.expect) - 1U);

    HAL_UART_Transmit(wifi_huart, data, bytes, 1000);

    for(i=0; i<timeout; i++)
    {
        esp_at_process();

        if( s_cmd_ctx.state == AT_CMD_SUCCESS )
        {
            rv = 0;
            goto CleanUp;
        }

        if( s_cmd_ctx.state == AT_CMD_ERROR )
        {
            rv = 1;
            goto CleanUp;
        }

        HAL_Delay(1);
    }

    s_cmd_ctx.state = AT_CMD_TIMEOUT;
    wifi_print("ERROR: AT command raw data expect '%s' timeout\r\n", expect);

CleanUp:
    s_cmd_ctx.active = 0U;
    wifi_dbg("<<<< AT command reply:\r\n%s", g_wifi_rxbuf);
    return rv;
}

int esp8266_module_init(void)
{
    int i;

    HAL_UART_AbortReceive(wifi_huart);
    esp8266_driver_reset_state();
    ESP8266_UartStartReceive();

    wifi_print("INFO: Reset ESP8266 module now...\r\n");
    if( send_atcmd("AT+RST\r\n", "ready", ESP_AT_RESET_TIMEOUT_MS) )
    {
        wifi_print("ERROR: ESP8266 reset ready timeout\r\n");
        return -1;
    }

    for(i=0; i<6; i++)
    {
        if( !send_atcmd("AT\r\n", EXPECT_OK, 500) )
        {
            wifi_print("INFO: Send AT to ESP8266 and got reply ok\r\n");
            break;
        }
        HAL_Delay(100);
    }

    if( i>= 6 )
    {
        wifi_print("ERROR: Can't receive AT replay after reset\r\n");
        return -2;
    }

    if( send_atcmd("AT+CWMODE=1\r\n", EXPECT_OK, 500) )
    {
        wifi_print("ERROR: Set ESP8266 work as Station mode failure\r\n");
        return -3;
    }

    if( send_atcmd("AT+CWDHCP=1,1\r\n", EXPECT_OK, 500) )
    {
        wifi_print("ERROR: Enable ESP8266 Station mode DHCP failure\r\n");
        return -4;
    }

    send_atcmd("AT+CWAUTOCONN=0\r\n", EXPECT_OK, 500);
    send_atcmd("AT+CWQAP\r\n", EXPECT_OK, 1000);

#if 0
    if( send_atcmd("AT+GMR\r\n", EXPECT_OK, 500) )
    {
        wifi_print("ERROR: AT+GMR check ESP8266 reversion failure\r\n");
        return -5;
    }
#endif

    HAL_Delay(500);
    return 0;
}

int esp8266_join_network(char *ssid, char *pwd)
{
    char *atcmd = s_atcmd_buf;
    int  i;

    if( !ssid || !pwd )
    {
        wifi_print("ERROR: Invalid input arguments\r\n");
        return -1;
    }

    memset(atcmd, 0, sizeof(s_atcmd_buf));
    snprintf(atcmd, sizeof(s_atcmd_buf), "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, pwd);
    if( send_atcmd(atcmd, EXPECT_OK, ESP_AT_JOIN_TIMEOUT_MS) )
    {
        wifi_print("ERROR: ESP8266 connect to '%s' failure\r\n", ssid);
        return -2;
    }

    wifi_print("INFO: ESP8266 connect to '%s' ok\r\n", ssid);

    /* check got IP address or not by netmask (255.)*/
    for(i=0; i<10; i++)
    {
        if( !esp8266_query_ip_status(ESP_AT_QUERY_TIMEOUT_MS) )
        {
            wifi_print("INFO: ESP8266 got IP address ok\r\n");
            return 0;
        }
        HAL_Delay(300);
    }

    wifi_print("ERROR: ESP8266 assigned IP address failure\r\n");
    return -3;
}

int esp8266_wifi_disconnect(void)
{
    if( send_atcmd("AT+CWQAP\r\n", EXPECT_OK, 1000) )
    {
        wifi_print("ERROR: ESP8266 WiFi disconnect failure\r\n");
        return -1;
    }

    return 0;
}

int esp8266_scan_ap(char *ssid)
{
    if( send_atcmd("AT+CWLAP\r\n", EXPECT_OK, 10000) )
    {
        wifi_print("ERROR: ESP8266 scan AP failure\r\n");
        return -1;
    }

    if( ssid && strlen(ssid)>0 && !strstr(g_wifi_rxbuf, ssid) )
    {
        wifi_print("ERROR: ESP8266 target AP '%s' not found\r\n", ssid);
        return -2;
    }

    if( ssid && strlen(ssid)>0 )
    {
        wifi_print("INFO: ESP8266 target AP '%s' found\r\n", ssid);
    }

    return 0;
}

/*
 * +CIPSTA_CUR:ip:"192.168.2.100"
 * +CIPSTA_CUR:gateway:"192.168.2.1"
 */
static int util_parser_ipaddr(char *buf, char *key, char *ipaddr, int size)
{
    char *start;
    char *end;
    int   len;

    if( !buf || !key || !ipaddr )
    {
        return -1;
    }

    /* find the key string */
    start = strstr(buf, key);
    if( !start )
    {
        return -2;
    }

    start += strlen(key) + 1;   /* Skip " */
    end = strchr(start, '"');   /* find last " */
    if( !end )
    {
        return -3;
    }

    len = end - start;
    len = len>size ? size : len;
    memset(ipaddr, 0, size);
    strncpy(ipaddr, start, len);
    return 0;
}

int esp8266_get_ipaddr(char *ipaddr, char *gateway, int ipaddr_size)
{
    if( !ipaddr || !gateway || ipaddr_size<7 )
    {
        wifi_print("ERROR: Invalid input arguments\r\n");
        return -1;
    }

    if( esp8266_query_ip_status(ESP_AT_QUERY_TIMEOUT_MS) )
    {
        wifi_print("ERROR: ESP8266 AT+CIPSTA command failure\r\n");
        return -2;
    }

    if( util_parser_ipaddr(g_wifi_rxbuf, "ip:", ipaddr, ipaddr_size) )
    {
        wifi_print("ERROR: ESP8266 AT+CIPSTA parser IP address failure\r\n");
        return -3;
    }

    if( util_parser_ipaddr(g_wifi_rxbuf, "gateway:", gateway, ipaddr_size) )
    {
        wifi_print("ERROR: ESP8266 AT+CIPSTA parser gateway failure\r\n");
        return -4;
    }

    wifi_print("INFO: ESP8266 got IP address[%s] gateway[%s] ok\r\n", ipaddr, gateway);
    return 0;
}

int esp8266_ping_test(char *host)
{
    char *atcmd = s_atcmd_buf;

    if( !host )
    {
        wifi_print("ERROR: Invalid input arguments\r\n");
        return -1;
    }

    memset(atcmd, 0, sizeof(s_atcmd_buf));
    snprintf(atcmd, sizeof(s_atcmd_buf), "AT+PING=\"%s\"\r\n", host);
    if( send_atcmd(atcmd, EXPECT_OK, 3000) )
    {
        wifi_print("ERROR: ESP8266 ping test [%s] failure\r\n", host);
        return -2;
    }

    wifi_print("INFO: ESP8266 ping test [%s] ok\r\n", host);
    return 0;
}

int esp8266_sock_connect(char *servip, int port)
{
    char *atcmd = s_atcmd_buf;

    if( !servip || port<=0 )
    {
        wifi_print("ERROR: Invalid input arguments\r\n");
        return -1;
    }

    send_atcmd("AT+CIPMUX=0\r\n", EXPECT_OK, 1500);

    memset(atcmd, 0, sizeof(s_atcmd_buf));
    snprintf(atcmd, sizeof(s_atcmd_buf), "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", servip, port);
    if( send_atcmd_ex(atcmd, "CONNECT", 1U, 3000) )
    {
        wifi_print("ERROR: ESP8266 socket connect to [%s:%d] failure\r\n", servip, port);
        return -2;
    }

    wifi_print("INFO: ESP8266 socket connect to [%s:%d] ok\r\n", servip, port);
    return 0;
}

int esp8266_sock_disconnect(void)
{
    send_atcmd("AT+CIPCLOSE\r\n", EXPECT_OK, 1500);
    return 0;
}

int esp8266_sock_send(unsigned char *data, int bytes)
{
    char *atcmd = s_atcmd_buf;

    if( !data || bytes<= 0)
    {
        wifi_print("ERROR: Invalid input arguments\r\n");
        return -1;
    }

    memset(atcmd, 0, sizeof(s_atcmd_buf));
    snprintf(atcmd, sizeof(s_atcmd_buf), "AT+CIPSEND=%d\r\n", bytes);
    if( send_atcmd(atcmd, ">", 500) )
    {
        wifi_print("ERROR: AT+CIPSEND command failure\r\n");
        return 0;
    }

    if( atcmd_send_data((unsigned char *)data, bytes, 1000) )
    {
        wifi_print("ERROR: AT+CIPSEND send data failure\r\n");
        return 0;
    }

    return bytes;
}

int esp8266_sock_recv(unsigned char *buf, int size)
{
    char *data = NULL;
    char *ptr  = NULL;
    int   len;
    int   rv;
    int   bytes;

    if( !buf || size<= 0)
    {
        wifi_print("ERROR: Invalid input arguments\r\n");
        return -1;
    }

    esp_at_process();

    if( g_wifi_rxbytes<= 0 )
    {
        return 0;
    }

    /* No data arrive or not integrated */
    ptr = strstr(g_wifi_rxbuf, "+IPD,");
    if( !ptr )
    {
        return 0;
    }

    data = strchr(ptr, ':' );
    if( !data )
    {
        return 0;
    }

    data ++;
    bytes = atoi(ptr+strlen("+IPD,"));
    len = g_wifi_rxbytes - (data-g_wifi_rxbuf);
    if( len < bytes )
    {
        wifi_dbg("+IPD data not receive over, receive again later ...\r\n");
        return 0;
    }

    memset(buf, 0, size);
    rv = bytes>size ? size : bytes;
    memcpy(buf, data, rv);
    clear_atcmd_buf();
    return rv;
}

int esp8266_mqtt_connect(char *host, int port, char *access_token)
{
    char *atcmd = s_atcmd_buf;
    static char client_id[64];

    if( !host || port<=0 || !access_token )
    {
        wifi_print("ERROR: Invalid input arguments\r\n");
        return -1;
    }

    send_atcmd("AT+MQTTCLEAN=0\r\n", EXPECT_OK, 1000);

    memset(atcmd, 0, sizeof(s_atcmd_buf));
    memset(client_id, 0, sizeof(client_id));
    snprintf(client_id, sizeof(client_id),
             "stm32_ota_%08lX%08lX%08lX_%lu",
             (unsigned long)HAL_GetUIDw0(),
             (unsigned long)HAL_GetUIDw1(),
             (unsigned long)HAL_GetUIDw2(),
             (unsigned long)HAL_GetTick());

    snprintf(atcmd, sizeof(s_atcmd_buf),
             "AT+MQTTUSERCFG=0,1,\"%s\",\"%s\",\"\",0,0,\"\"\r\n",
             client_id, access_token);
    if( send_atcmd(atcmd, EXPECT_OK, 2000) )
    {
        wifi_print("ERROR: ESP8266 MQTT user config failure\r\n");
        return -2;
    }

    wifi_print("INFO: ESP8266 MQTT client id [%s]\r\n", client_id);

    snprintf(atcmd, sizeof(s_atcmd_buf), "AT+MQTTCONN=0,\"%s\",%d,0\r\n", host, port);
    if( send_atcmd_ex(atcmd, "+MQTTCONNECTED", 1U, 10000) )
    {
        wifi_print("ERROR: ESP8266 MQTT connect to [%s:%d] failure\r\n", host, port);
        return -3;
    }

    wifi_print("INFO: ESP8266 MQTT connect to [%s:%d] ok\r\n", host, port);
    HAL_Delay(200);
    return 0;
}

int esp8266_mqtt_disconnect(void)
{
    send_atcmd("AT+MQTTCLEAN=0\r\n", EXPECT_OK, 1000);
    return 0;
}

int esp8266_mqtt_publish_raw(char *topic, unsigned char *data, int bytes)
{
    char *atcmd = s_atcmd_buf;

    if( !topic || !data || bytes<=0 )
    {
        wifi_print("ERROR: Invalid input arguments\r\n");
        return -1;
    }

    memset(atcmd, 0, sizeof(s_atcmd_buf));
    snprintf(atcmd, sizeof(s_atcmd_buf), "AT+MQTTPUBRAW=0,\"%s\",%d,1,0\r\n", topic, bytes);
    if( send_atcmd(atcmd, ">", 2000) )
    {
        wifi_print("ERROR: ESP8266 MQTT publish raw command failure\r\n");
        return -2;
    }

    if( atcmd_send_data_expect(data, bytes, "+MQTTPUB:OK", 5000) )
    {
        wifi_print("ERROR: ESP8266 MQTT publish raw data failure\r\n");
        return -3;
    }

    wifi_print("INFO: ESP8266 MQTT publish [%d] bytes to topic [%s] ok\r\n", bytes, topic);
    return bytes;
}

int esp8266_mqtt_subscribe(char *topic, int qos)
{
    char *atcmd = s_atcmd_buf;

    if( !topic || qos<0 || qos>1 )
    {
        wifi_print("ERROR: Invalid input arguments\r\n");
        return -1;
    }

    memset(atcmd, 0, sizeof(s_atcmd_buf));
    snprintf(atcmd, sizeof(s_atcmd_buf), "AT+MQTTSUB=0,\"%s\",%d\r\n", topic, qos);
    if( send_atcmd(atcmd, EXPECT_OK, 2000) )
    {
        wifi_print("ERROR: ESP8266 MQTT subscribe [%s] failure\r\n", topic);
        return -2;
    }

    wifi_print("INFO: ESP8266 MQTT subscribe [%s] ok\r\n", topic);
    return 0;
}

int esp8266_thingsboard_publish_telemetry(unsigned char *json, int bytes)
{
    return esp8266_mqtt_publish_raw("v1/devices/me/telemetry", json, bytes);
}

static int esp8266_mqtt_wait_subrecv_event(char *expect,
                                           mqtt_sub_event_t *out_event,
                                           unsigned int timeout,
                                           uint8_t print_payload)
{
    unsigned int i;
    static mqtt_sub_event_t evt;

    for(i=0; i<timeout; i++)
    {
        esp_at_process();

        if( mqtt_event_pop(&evt) )
        {
            s_last_mqtt_event = evt;
            s_last_mqtt_valid = 1U;
            if( out_event )
            {
                *out_event = evt;
            }

            wifi_print("INFO: ESP8266 MQTT received topic [%s], %u bytes\r\n",
                       evt.topic, evt.payload_len);
            if( print_payload )
            {
                wifi_print("%.*s\r\n", evt.payload_len, (char *)evt.payload);
            }

            if( !expect ||
                strstr(evt.topic, expect) ||
                payload_contains(evt.payload, evt.payload_len, expect) )
            {
                return 0;
            }

            return -2;
        }

        HAL_Delay(1);
    }

    if( s_parse_state == ESP_PARSE_MQTT_PAYLOAD )
    {
        wifi_print("ERROR: MQTT subrecv payload incomplete topic [%s], %u/%u bytes\r\n",
                   s_parse_mqtt_event.topic,
                   s_parse_payload_pos,
                   s_parse_mqtt_event.payload_len);
    }

    return -1;
}

static int esp8266_mqtt_wait_subrecv(char *expect, unsigned int timeout)
{
    return esp8266_mqtt_wait_subrecv_event(expect, NULL, timeout, 1U);
}

int esp8266_thingsboard_request_firmware_info(void)
{
    unsigned char request[] =
        "{\"sharedKeys\":\"fw_title,fw_version,fw_size,fw_checksum,fw_checksum_algorithm\"}";
    static char request_topic[64];
    uint32_t request_id;
    uint8_t attempt;
    int wait_ret;

    if( esp8266_mqtt_subscribe("v1/devices/me/attributes/response/+", 1) )
    {
        return -1;
    }

    if( esp8266_mqtt_subscribe("v1/devices/me/attributes", 1) )
    {
        return -2;
    }

    HAL_Delay(200);

    for(attempt=1U; attempt<=ESP_TB_FW_INFO_RETRY; attempt++)
    {
        mqtt_event_clear();

        request_id = s_tb_request_id++;
        if( s_tb_request_id == 0U )
        {
            s_tb_request_id = 1U;
        }
        memset(request_topic, 0, sizeof(request_topic));
        snprintf(request_topic, sizeof(request_topic),
                 "v1/devices/me/attributes/request/%lu",
                 (unsigned long)request_id);

        if( esp8266_mqtt_publish_raw(request_topic,
                                     request,
                                     (int)(sizeof(request) - 1U)) <= 0 )
        {
            wifi_print("ERROR: ThingsBoard firmware info request publish failure\r\n");
            return -3;
        }

        wifi_print("INFO: Waiting ThingsBoard firmware info response, request id %lu, attempt %u/%u\r\n",
                   (unsigned long)request_id,
                   attempt,
                   ESP_TB_FW_INFO_RETRY);

        wait_ret = esp8266_mqtt_wait_subrecv("fw_title", ESP_TB_FW_INFO_TIMEOUT_MS);
        if( wait_ret == 0 )
        {
            if( thingsboard_parse_firmware_info() != 0 )
            {
                wifi_print("ERROR: ThingsBoard firmware info parse failure\r\n");
                return -4;
            }
            wifi_print("INFO: ThingsBoard firmware info found\r\n");
            wifi_print("INFO: ThingsBoard firmware size %lu bytes, title [%s], version [%s]\r\n",
                       (unsigned long)s_tb_fw_info.size,
                       s_tb_fw_info.title,
                       s_tb_fw_info.version);
            return 0;
        }

        if( s_last_mqtt_valid )
        {
            wifi_print("INFO: ThingsBoard firmware response without fw_title, topic [%s]\r\n",
                       s_last_mqtt_event.topic);
            return -4;
        }

        wifi_print("INFO: ThingsBoard firmware info request timeout, attempt %u/%u\r\n",
                   attempt,
                   ESP_TB_FW_INFO_RETRY);
        HAL_Delay(300);
    }

    return -4;
}

int esp8266_thingsboard_get_firmware_info(esp8266_tb_firmware_info_t *info)
{
    if( !info || !s_tb_fw_info.valid )
    {
        return -1;
    }

    *info = s_tb_fw_info;
    return 0;
}

int esp8266_thingsboard_subscribe_firmware_chunks(void)
{
    if( esp8266_mqtt_subscribe("v2/fw/response/+/chunk/+", 1) )
    {
        return -1;
    }

    HAL_Delay(200);
    return 0;
}

int esp8266_thingsboard_request_firmware_chunk(uint32_t chunk_index,
                                               uint32_t chunk_size,
                                               uint8_t *buf,
                                               uint16_t buf_size,
                                               uint16_t *out_len)
{
    static char topic[96];
    static char expect_topic[96];
    static char payload[16];
    uint32_t request_id;
    static mqtt_sub_event_t evt;
    int payload_len;

    if( !buf || !out_len || chunk_size == 0U || chunk_size > buf_size )
    {
        wifi_print("ERROR: Invalid firmware chunk request arguments\r\n");
        return -1;
    }

    if( chunk_size > ESP_MQTT_PAYLOAD_SIZE )
    {
        wifi_print("ERROR: Firmware chunk size %lu exceeds MQTT payload buffer\r\n",
                   (unsigned long)chunk_size);
        return -2;
    }

    mqtt_event_clear();

    request_id = s_tb_request_id++;
    if( s_tb_request_id == 0U )
    {
        s_tb_request_id = 1U;
    }

    memset(topic, 0, sizeof(topic));
    memset(expect_topic, 0, sizeof(expect_topic));
    memset(payload, 0, sizeof(payload));
    snprintf(topic, sizeof(topic),
             "v2/fw/request/%lu/chunk/%lu",
             (unsigned long)request_id,
             (unsigned long)chunk_index);
    payload_len = snprintf(payload, sizeof(payload), "%lu", (unsigned long)chunk_size);

    if( payload_len <= 0 || payload_len >= (int)sizeof(payload) )
    {
        wifi_print("ERROR: Firmware chunk request payload build failure\r\n");
        return -4;
    }

    if( esp8266_mqtt_publish_raw(topic,
                                 (unsigned char *)payload,
                                 payload_len) <= 0 )
    {
        wifi_print("ERROR: ThingsBoard firmware chunk request publish failure\r\n");
        return -5;
    }

    wifi_print("INFO: Waiting ThingsBoard firmware chunk %lu, request id %lu, size %lu\r\n",
               (unsigned long)chunk_index,
               (unsigned long)request_id,
               (unsigned long)chunk_size);

    snprintf(expect_topic, sizeof(expect_topic),
             "v2/fw/response/%lu/chunk/%lu",
             (unsigned long)request_id,
             (unsigned long)chunk_index);

    if( esp8266_mqtt_wait_subrecv_event(expect_topic, &evt, ESP_TB_FW_INFO_TIMEOUT_MS, 0U) )
    {
        wifi_print("ERROR: ThingsBoard firmware chunk %lu receive timeout\r\n",
                   (unsigned long)chunk_index);
        return -6;
    }

    if( evt.payload_len == 0U || evt.payload_len > buf_size )
    {
        wifi_print("ERROR: Invalid ThingsBoard firmware chunk length %u\r\n",
                   evt.payload_len);
        return -7;
    }

    memcpy(buf, evt.payload, evt.payload_len);
    *out_len = evt.payload_len;
    wifi_print("INFO: ThingsBoard firmware chunk %lu received, %u bytes\r\n",
               (unsigned long)chunk_index,
               evt.payload_len);
    return 0;
}

/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include <string.h>
#include "stdio.h"
#include "global.h"
#include "w25qxx.h"
#include "eeprom_emul.h"
#include "ota_state_machine.h"
#include "cmox_crypto.h"
#include "log_config.h"
#include "thingsboard_ota.h"
#include "esp8266.h"
#include "net_cfg.h"
#include "core_mqtt.h"
#include "app_verify.h"
#include "flash_bootloader.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

#if LOG_MAIN_ENABLE
#define main_log(...) printf(__VA_ARGS__)
#else
#define main_log(...) ((void)0)
#endif

#if LOG_MAIN_TRACE_ENABLE
#define main_trace(...) printf(__VA_ARGS__)
#else
#define main_trace(...) ((void)0)
#endif

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static uint8_t spinor_lfs_mount(lfs_ctx_t *fs);
#if 0
static void    wifi_mqtt_smoke_test(void);
#endif

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

lfs_ctx_t lfs_ctx;

extern const struct lfs_config      my_lfs_config;
extern const struct lfs_file_config lfs_file_cfg;
extern spinor_info_t                spinor;

static uint8_t spinor_lfs_mount(lfs_ctx_t *fs)
{
    if (fs == NULL)
    {
        return 1;
    }

    if (spinor_init(&spinor) < 0)
    {
        return 1;
    }

    int mount_err = lfs_mount(&fs->lfs, &my_lfs_config);
    main_trace("lfs_mount: %d\r\n", mount_err);

    if (mount_err)
    {
        lfs_format(&fs->lfs, &my_lfs_config);
        mount_err = lfs_mount(&fs->lfs, &my_lfs_config);
        main_log("LittleFS formatted and mounted, err: %d\r\n", mount_err);
    }

    if (mount_err != 0)
    {
        return 1;
    }

    fs->mounted   = 1;
    fs->file_open = 0;

    return 0;
}

#if 0
static const char s_mqtt_firmware_path[] = "mqtt_fw.tmp";

static int mqtt_json_get_u32(const uint8_t *json,
                             uint16_t       json_len,
                             const char    *key,
                             uint32_t      *value)
{
    char     pattern[32];
    uint32_t parsed_value;
    uint16_t cursor;
    uint16_t digit_start;
    uint16_t pattern_len;
    uint16_t start;
    int      pattern_bytes;

    if (json == NULL || key == NULL || value == NULL || json_len == 0U)
    {
        return -1;
    }

    pattern_bytes = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    if (pattern_bytes <= 0 || pattern_bytes >= (int)sizeof(pattern))
    {
        return -2;
    }

    pattern_len = (uint16_t)pattern_bytes;
    for (start = 0U; start + pattern_len <= json_len; start++)
    {
        if (memcmp(&json[start], pattern, pattern_len) != 0)
        {
            continue;
        }

        cursor = (uint16_t)(start + pattern_len);
        while (cursor < json_len && (json[cursor] == ' ' || json[cursor] == '\t'))
        {
            cursor++;
        }

        parsed_value = 0U;
        digit_start  = cursor;
        while (cursor < json_len && json[cursor] >= '0' && json[cursor] <= '9')
        {
            uint32_t digit = (uint32_t)(json[cursor] - '0');

            if (parsed_value > (UINT32_MAX - digit) / 10U)
            {
                return -3;
            }

            parsed_value = parsed_value * 10U + digit;
            cursor++;
        }

        if (cursor == digit_start)
        {
            return -4;
        }

        *value = parsed_value;
        return 0;
    }

    return -5;
}

static int mqtt_json_get_string(const uint8_t *json,
                                uint16_t       json_len,
                                const char    *key,
                                char          *value,
                                uint16_t       value_size)
{
    char     pattern[32];
    uint16_t cursor;
    uint16_t pattern_len;
    uint16_t start;
    uint16_t value_len;
    int      pattern_bytes;

    if (json == NULL || key == NULL || value == NULL || value_size == 0U || json_len == 0U)
    {
        return -1;
    }

    pattern_bytes = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    if (pattern_bytes <= 0 || pattern_bytes >= (int)sizeof(pattern))
    {
        return -2;
    }

    pattern_len = (uint16_t)pattern_bytes;
    for (start = 0U; start + pattern_len <= json_len; start++)
    {
        if (memcmp(&json[start], pattern, pattern_len) != 0)
        {
            continue;
        }

        cursor = (uint16_t)(start + pattern_len);
        while (cursor < json_len && (json[cursor] == ' ' || json[cursor] == '\t'))
        {
            cursor++;
        }

        if (cursor >= json_len || json[cursor] != '"')
        {
            return -3;
        }

        cursor++;
        value_len = 0U;
        while (cursor < json_len && json[cursor] != '"')
        {
            if (value_len + 1U >= value_size)
            {
                value[0] = '\0';
                return -4;
            }

            value[value_len++] = (char)json[cursor++];
        }

        if (cursor >= json_len)
        {
            value[0] = '\0';
            return -5;
        }

        value[value_len] = '\0';
        return 0;
    }

    value[0] = '\0';
    return -6;
}

static int mqtt_wait_topic_message(const char   *expected_topic,
                                   mqtt_rx_msg_t *message,
                                   uint32_t       timeout_ms)
{
    uint32_t start_tick;
    int      rv;

    if (expected_topic == NULL || message == NULL || timeout_ms == 0U)
    {
        return -1;
    }

    start_tick = HAL_GetTick();
    while (1)
    {
        while (mqtt_message_pop(message) > 0)
        {
            if (strcmp(message->topic, expected_topic) == 0)
            {
                return 1;
            }

            main_log("WiFi MQTT test: ignore unrelated topic=[%s]\r\n", message->topic);
        }

        if ((HAL_GetTick() - start_tick) >= timeout_ms)
        {
            return 0;
        }

        rv = mqtt_poll_once();
        if (rv < 0)
        {
            return -2;
        }

        if (rv == 0)
        {
            HAL_Delay(1U);
        }
    }
}

static int mqtt_download_firmware_to_lfs(lfs_ctx_t    *fs,
                                          mqtt_rx_msg_t *message,
                                          uint32_t       firmware_size)
{
    static const char firmware_chunk_filter[] = "v2/fw/response/+/chunk/+";
    static const char firmware_chunk_size[]   = "512";
    static char       request_topic[64];
    static char       response_topic[64];
    lfs_soff_t        file_size;
    lfs_ssize_t       written;
    uint32_t          chunk_index;
    uint32_t          expected_chunk_len;
    uint32_t          request_id;
    uint32_t          total_written;
    uint16_t          preview_index;
    uint16_t          preview_len;
    int               err;
    int               request_topic_len;
    int               response_topic_len;
    int               rv;

    if (fs == NULL || message == NULL || fs->mounted == 0U || firmware_size == 0U)
    {
        return -1;
    }

    if (fs->file_open != 0U)
    {
        err = lfs_file_close(&fs->lfs, &fs->file);
        fs->file_open = 0U;
        if (err != 0)
        {
            main_log("WiFi MQTT test: close stale LFS file failed, err=%d\r\n", err);
            return -2;
        }
    }

    err = lfs_remove(&fs->lfs, s_mqtt_firmware_path);
    if (err != 0 && err != LFS_ERR_NOENT)
    {
        main_log("WiFi MQTT test: remove stale %s failed, err=%d\r\n",
                 s_mqtt_firmware_path,
                 err);
        return -2;
    }

    err = lfs_file_opencfg(&fs->lfs,
                           &fs->file,
                           s_mqtt_firmware_path,
                           LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC,
                           &lfs_file_cfg);
    if (err != 0)
    {
        main_log("WiFi MQTT test: open %s failed, err=%d\r\n", s_mqtt_firmware_path, err);
        return -3;
    }
    fs->file_open = 1U;

    rv = mqtt_subscribe_topic((char *)firmware_chunk_filter, Qos1, 2);
    if (rv != 0)
    {
        main_log("WiFi MQTT test: subscribe firmware chunk failed, rv=%d\r\n", rv);
        rv = -4;
        goto DownloadFailed;
    }

    main_log("WiFi MQTT test: subscribe firmware chunk ok\r\n");
    total_written = 0U;
    chunk_index   = 0U;
    request_id    = 2U;

    while (total_written < firmware_size)
    {
        expected_chunk_len = firmware_size - total_written;
        if (expected_chunk_len > MQTT_RX_PAYLOAD_MAX)
        {
            expected_chunk_len = MQTT_RX_PAYLOAD_MAX;
        }

        request_topic_len = snprintf(request_topic,
                                     sizeof(request_topic),
                                     "v2/fw/request/%lu/chunk/%lu",
                                     (unsigned long)request_id,
                                     (unsigned long)chunk_index);
        response_topic_len = snprintf(response_topic,
                                      sizeof(response_topic),
                                      "v2/fw/response/%lu/chunk/%lu",
                                      (unsigned long)request_id,
                                      (unsigned long)chunk_index);
        if (request_topic_len <= 0 || request_topic_len >= (int)sizeof(request_topic) ||
            response_topic_len <= 0 || response_topic_len >= (int)sizeof(response_topic))
        {
            rv = -5;
            goto DownloadFailed;
        }

        rv = mqtt_publish(request_topic, Qos1, (char *)firmware_chunk_size);
        if (rv != 0)
        {
            main_log("WiFi MQTT test: chunk %lu request failed, rv=%d\r\n",
                     (unsigned long)chunk_index,
                     rv);
            rv = -6;
            goto DownloadFailed;
        }

        rv = mqtt_wait_topic_message(response_topic, message, 5000U);
        if (rv <= 0)
        {
            main_log("WiFi MQTT test: chunk %lu response failed, rv=%d\r\n",
                     (unsigned long)chunk_index,
                     rv);
            rv = -7;
            goto DownloadFailed;
        }

        if (message->payload_len != (uint16_t)expected_chunk_len)
        {
            main_log("WiFi MQTT test: chunk %lu length mismatch, expected=%lu actual=%u\r\n",
                     (unsigned long)chunk_index,
                     (unsigned long)expected_chunk_len,
                     (unsigned int)message->payload_len);
            rv = -8;
            goto DownloadFailed;
        }

        if (chunk_index == 0U)
        {
            main_log("WiFi MQTT test: chunk 0 first 16 bytes:");
            preview_len = (message->payload_len < 16U) ? message->payload_len : 16U;
            for (preview_index = 0U; preview_index < preview_len; preview_index++)
            {
                main_log(" %02X", (unsigned int)message->payload[preview_index]);
            }
            main_log("\r\n");
        }

        written = lfs_file_write(&fs->lfs, &fs->file, message->payload, message->payload_len);
        if (written < 0 || (uint16_t)written != message->payload_len)
        {
            main_log("WiFi MQTT test: LFS write chunk %lu failed, ret=%d\r\n",
                     (unsigned long)chunk_index,
                     (int)written);
            rv = -9;
            goto DownloadFailed;
        }

        total_written += message->payload_len;
        main_log("WiFi MQTT test: download progress %lu/%lu, chunk=%lu bytes=%u\r\n",
                 (unsigned long)total_written,
                 (unsigned long)firmware_size,
                 (unsigned long)chunk_index,
                 (unsigned int)message->payload_len);
        chunk_index++;
        request_id++;
    }

    err = lfs_file_sync(&fs->lfs, &fs->file);
    if (err != 0)
    {
        main_log("WiFi MQTT test: LFS sync %s failed, err=%d\r\n",
                 s_mqtt_firmware_path,
                 err);
        rv = -10;
        goto DownloadFailed;
    }

    file_size = lfs_file_size(&fs->lfs, &fs->file);
    if (file_size < 0 || (uint32_t)file_size != firmware_size)
    {
        main_log("WiFi MQTT test: LFS file size mismatch, expected=%lu actual=%ld\r\n",
                 (unsigned long)firmware_size,
                 (long)file_size);
        rv = -11;
        goto DownloadFailed;
    }

    err           = lfs_file_close(&fs->lfs, &fs->file);
    fs->file_open = 0U;
    if (err != 0)
    {
        int remove_err;

        main_log("WiFi MQTT test: LFS close %s failed, err=%d\r\n",
                 s_mqtt_firmware_path,
                 err);
        remove_err = lfs_remove(&fs->lfs, s_mqtt_firmware_path);
        if (remove_err != 0 && remove_err != LFS_ERR_NOENT)
        {
            main_log("WiFi MQTT test: cleanup remove %s failed, err=%d\r\n",
                      s_mqtt_firmware_path,
                     remove_err);
        }
        return -12;
    }

    main_log("WiFi MQTT test: firmware saved to %s, bytes=%lu, chunks=%lu\r\n",
              s_mqtt_firmware_path,
             (unsigned long)total_written,
             (unsigned long)chunk_index);
    return 0;

DownloadFailed:
    if (fs->file_open != 0U)
    {
        err = lfs_file_close(&fs->lfs, &fs->file);
        fs->file_open = 0U;
        if (err != 0)
        {
            main_log("WiFi MQTT test: cleanup close %s failed, err=%d\r\n",
                     s_mqtt_firmware_path,
                     err);
        }
    }
    err = lfs_remove(&fs->lfs, s_mqtt_firmware_path);
    if (err != 0 && err != LFS_ERR_NOENT)
    {
        main_log("WiFi MQTT test: cleanup remove %s failed, err=%d\r\n",
                 s_mqtt_firmware_path,
                 err);
    }
    return rv;
}

static void wifi_mqtt_smoke_test(void)
{
    const net_cfg_t *cfg;
    static const char firmware_response_filter[] = "v1/devices/me/attributes/response/+";
    static const char firmware_response_topic[]  = "v1/devices/me/attributes/response/1";
    static const char firmware_request_topic[]   = "v1/devices/me/attributes/request/1";
    static const char firmware_request_payload[] =
        "{\"sharedKeys\":\"fw_title,fw_version,fw_size,fw_checksum,fw_checksum_algorithm\"}";
    static char          ip[32];
    static char          gateway[32];
    static char          client_id[64];
    static char          firmware_checksum[65];
    static char          checksum_algorithm[16];
    static char          calculated_checksum[65];
    static mqtt_rx_msg_t rx_message;
    uint32_t             firmware_size;
    uint32_t             active_slot;
    uint32_t             target_addr;
    uint32_t             target_slot;
    bootloader_load_status_t load_status;
    firmware_verify_status_t verify_status;
    int                  rv;

    cfg = net_cfg_get();
    if (net_cfg_is_valid(cfg) == 0)
    {
        main_log("WiFi MQTT test: network config invalid\r\n");
        return;
    }

    if (esp8266_module_init() != 0)
    {
        main_log("WiFi MQTT test: ESP8266 init failed\r\n");
        return;
    }

    if (esp8266_scan_ap((char *)cfg->wifi_ssid) != 0)
    {
        main_log("WiFi MQTT test: AP [%s] not found\r\n", cfg->wifi_ssid);
        return;
    }

    if (esp8266_join_network((char *)cfg->wifi_ssid, (char *)cfg->wifi_password) != 0)
    {
        main_log("WiFi MQTT test: join AP failed\r\n");
        return;
    }

    if (esp8266_get_ipaddr(ip, gateway, (int)sizeof(ip)) != 0)
    {
        main_log("WiFi MQTT test: get IP failed\r\n");
        goto ExitWiFi;
    }

    main_log("WiFi MQTT test: ip=%s gateway=%s\r\n", ip, gateway);

    if (esp8266_ping_test((char *)cfg->mqtt_host) != 0)
    {
        main_log("WiFi MQTT test: ping host [%s] failed\r\n", cfg->mqtt_host);
        goto ExitWiFi;
    }

    memset(client_id, 0, sizeof(client_id));
    snprintf(client_id,
             sizeof(client_id),
             "stm32_ota_%08lX%08lX%08lX_%lu",
             (unsigned long)HAL_GetUIDw0(),
             (unsigned long)HAL_GetUIDw1(),
             (unsigned long)HAL_GetUIDw2(),
             (unsigned long)HAL_GetTick());

    rv = mqtt_connect(
        (char *)cfg->mqtt_host, (int)cfg->mqtt_port, client_id, (char *)cfg->access_token, "");
    if (rv != 0)
    {
        main_log("WiFi MQTT test: mqtt_connect [%s:%u] failed, rv=%d\r\n",
                 cfg->mqtt_host,
                 (unsigned int)cfg->mqtt_port,
                 rv);
        goto ExitWiFi;
    }

    main_log("WiFi MQTT test: mqtt_connect [%s:%u] ok\r\n",
             cfg->mqtt_host,
             (unsigned int)cfg->mqtt_port);

    rv = mqtt_subscribe_topic((char *)firmware_response_filter, Qos1, 1);
    if (rv != 0)
    {
        main_log("WiFi MQTT test: subscribe firmware response failed, rv=%d\r\n", rv);
        goto ExitMqtt;
    }

    main_log("WiFi MQTT test: subscribe firmware response ok\r\n");

    rv = mqtt_publish(
        (char *)firmware_request_topic, Qos1, (char *)firmware_request_payload);
    if (rv != 0)
    {
        main_log("WiFi MQTT test: firmware attributes request failed, rv=%d\r\n", rv);
        goto ExitMqtt;
    }

    main_log("WiFi MQTT test: firmware attributes request published\r\n");
    rv = mqtt_wait_topic_message(firmware_response_topic, &rx_message, 5000U);
    if (rv <= 0)
    {
        main_log("WiFi MQTT test: firmware attributes response failed, rv=%d\r\n", rv);
        goto ExitMqtt;
    }

    main_log("WiFi MQTT test: firmware attributes received, qos=%u, bytes=%u\r\n",
             (unsigned int)rx_message.qos,
             (unsigned int)rx_message.payload_len);
    main_log("WiFi MQTT test: firmware attributes=%.*s\r\n",
             (int)rx_message.payload_len,
             (char *)rx_message.payload);

    rv = mqtt_json_get_u32(
        rx_message.payload, rx_message.payload_len, "fw_size", &firmware_size);
    if (rv != 0 || firmware_size == 0U)
    {
        main_log("WiFi MQTT test: parse fw_size failed, rv=%d\r\n", rv);
        goto ExitMqtt;
    }

    memset(firmware_checksum, 0, sizeof(firmware_checksum));
    memset(checksum_algorithm, 0, sizeof(checksum_algorithm));

    rv = mqtt_json_get_string(rx_message.payload,
                              rx_message.payload_len,
                              "fw_checksum",
                              firmware_checksum,
                              sizeof(firmware_checksum));
    if (rv != 0)
    {
        main_log("WiFi MQTT test: parse fw_checksum failed, rv=%d\r\n", rv);
        goto ExitMqtt;
    }

    rv = mqtt_json_get_string(rx_message.payload,
                              rx_message.payload_len,
                              "fw_checksum_algorithm",
                              checksum_algorithm,
                              sizeof(checksum_algorithm));
    if (rv != 0 || strcmp(checksum_algorithm, "SHA256") != 0)
    {
        main_log("WiFi MQTT test: unsupported checksum algorithm [%s], rv=%d\r\n",
                 checksum_algorithm,
                 rv);
        goto ExitMqtt;
    }

    main_log("WiFi MQTT test: firmware download begin, size=%lu\r\n",
             (unsigned long)firmware_size);
    rv = mqtt_download_firmware_to_lfs(&lfs_ctx, &rx_message, firmware_size);
    if (rv != 0)
    {
        main_log("WiFi MQTT test: firmware download failed, rv=%d\r\n", rv);
        goto ExitMqtt;
    }

    memset(calculated_checksum, 0, sizeof(calculated_checksum));
    main_log("WiFi MQTT test: SHA256 verify begin, file=%s bytes=%lu\r\n",
             s_mqtt_firmware_path,
             (unsigned long)firmware_size);
    verify_status = verify_lfs_file_sha256(&lfs_ctx,
                                           s_mqtt_firmware_path,
                                           firmware_checksum,
                                           firmware_size,
                                           calculated_checksum,
                                           sizeof(calculated_checksum));
    if (verify_status != FIRMWARE_VERIFY_OK)
    {
        int remove_err;

        main_log("WiFi MQTT test: SHA256 verify failed, ret=%d\r\n", verify_status);
        main_log("WiFi MQTT test: SHA256 expected=%s\r\n", firmware_checksum);
        if (calculated_checksum[0] != '\0')
        {
            main_log("WiFi MQTT test: SHA256 actual  =%s\r\n", calculated_checksum);
        }

        remove_err = lfs_remove(&lfs_ctx.lfs, s_mqtt_firmware_path);
        if (remove_err != 0 && remove_err != LFS_ERR_NOENT)
        {
            main_log("WiFi MQTT test: remove invalid %s failed, err=%d\r\n",
                     s_mqtt_firmware_path,
                     remove_err);
        }
        goto ExitMqtt;
    }

    main_log("WiFi MQTT test: SHA256 verify ok, checksum=%s\r\n", calculated_checksum);

    main_log("WiFi MQTT test: firmware package verify begin, file=%s\r\n",
             s_mqtt_firmware_path);
    verify_status = verify_firmware(s_mqtt_firmware_path);
    if (verify_status != FIRMWARE_VERIFY_OK)
    {
        int remove_err;

        main_log("WiFi MQTT test: firmware package verify failed, ret=%d\r\n", verify_status);
        remove_err = lfs_remove(&lfs_ctx.lfs, s_mqtt_firmware_path);
        if (remove_err != 0 && remove_err != LFS_ERR_NOENT)
        {
            main_log("WiFi MQTT test: remove invalid %s failed, err=%d\r\n",
                     s_mqtt_firmware_path,
                     remove_err);
        }
        goto ExitMqtt;
    }

    main_log("WiFi MQTT test: firmware package verify ok\r\n");

    active_slot = Read_Flag(EE_VAR_ACTIVE_SLOT);
    if (active_slot != SLOT_A && active_slot != SLOT_B)
    {
        main_log("WiFi MQTT test: active slot flag invalid [%lu], use slot A for test\r\n",
                 (unsigned long)active_slot);
        active_slot = SLOT_A;
    }

    target_slot = OPPOSITE_SLOT(active_slot);
    target_addr = (target_slot == SLOT_B) ? APP_B_START_ADDR : APP_A_START_ADDR;
    main_log("WiFi MQTT test: target load begin, active=%s target=%s addr=0x%08lX\r\n",
             SLOT_NAME(active_slot),
             SLOT_NAME(target_slot),
             (unsigned long)target_addr);

    load_status = bootloader_load_target((uint8_t)target_slot, s_mqtt_firmware_path);
    if (load_status != BOOTLOADER_LOAD_OK)
    {
        main_log("WiFi MQTT test: target load failed, ret=%d\r\n", load_status);
        goto ExitMqtt;
    }

    if (Verify_APP_Integrity_Flash(target_addr) == 0)
    {
        main_log("WiFi MQTT test: target slot %s integrity verify failed\r\n",
                 SLOT_NAME(target_slot));
        goto ExitMqtt;
    }

    main_log("WiFi MQTT test: target slot %s load and integrity verify ok\r\n",
             SLOT_NAME(target_slot));
    main_log("WiFi MQTT test: target slot not committed, disconnect network before temporary jump\r\n");

    mqtt_disconnect();
    esp8266_wifi_disconnect();

    main_log("WiFi MQTT test: temporary jump to slot %s @ 0x%08lX\r\n",
             SLOT_NAME(target_slot),
             (unsigned long)target_addr);
    if (Jump_To_App_Flash(&lfs_ctx, target_addr) == 0)
    {
        main_log("WiFi MQTT test: temporary jump failed\r\n");
    }
    return;

ExitMqtt:
    mqtt_disconnect();

ExitWiFi:
    esp8266_wifi_disconnect();
}
#endif
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
    /* USER CODE BEGIN 1 */

    /* USER CODE END 1 */

    /* MCU Configuration--------------------------------------------------------*/

    /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
    HAL_Init();

    /* USER CODE BEGIN Init */

    /* USER CODE END Init */

    /* Configure the system clock */
    SystemClock_Config();

    /* USER CODE BEGIN SysInit */

    /* USER CODE END SysInit */

    /* Initialize all configured peripherals */
    MX_GPIO_Init();
    MX_DMA_Init();
    MX_USART1_UART_Init();
    MX_SPI1_Init();
    MX_USART3_UART_Init();
    MX_USART2_UART_Init();
    /* USER CODE BEGIN 2 */

    EE_Init();
    cmox_initialize(NULL);

    memset(&lfs_ctx, 0, sizeof(lfs_ctx_t));
    if (spinor_lfs_mount(&lfs_ctx) != 0U)
    {
        main_log("LittleFS mount failed\r\n");
        Error_Handler();
    }

    {
        ota_ctx_t      ctx;
        transfer_cfg_t transfer_cfg;

        thingsboard_ota_bind_transport(&ctx, &transfer_cfg, &lfs_ctx);

        main_log("Starting ThingsBoard OTA state machine...\r\n");
        ota_run(&ctx);
    }
    /* USER CODE END 2 */

    /* Infinite loop */
    /* USER CODE BEGIN WHILE */
    while (1)
    {
        /* USER CODE END WHILE */

        /* USER CODE BEGIN 3 */
        HAL_Delay(10U);
    }
    /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    /** Configure the main internal regulator output voltage
  */
    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
    {
        Error_Handler();
    }

    /** Configure LSE Drive Capability
  */
    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);

    /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
    RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_LSE | RCC_OSCILLATORTYPE_MSI;
    RCC_OscInitStruct.LSEState            = RCC_LSE_ON;
    RCC_OscInitStruct.MSIState            = RCC_MSI_ON;
    RCC_OscInitStruct.MSICalibrationValue = 0;
    RCC_OscInitStruct.MSIClockRange       = RCC_MSIRANGE_6;
    RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_MSI;
    RCC_OscInitStruct.PLL.PLLM            = 1;
    RCC_OscInitStruct.PLL.PLLN            = 40;
    RCC_OscInitStruct.PLL.PLLP            = RCC_PLLP_DIV7;
    RCC_OscInitStruct.PLL.PLLQ            = RCC_PLLQ_DIV2;
    RCC_OscInitStruct.PLL.PLLR            = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    /** Initializes the CPU, AHB and APB buses clocks
  */
    RCC_ClkInitStruct.ClockType =
        RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
    {
        Error_Handler();
    }

    /** Enable MSI Auto calibration
  */
    HAL_RCCEx_EnableMSIPLLMode();
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
    /* USER CODE BEGIN Error_Handler_Debug */
    /* User can add his own implementation to report the HAL error return state */
    __disable_irq();
    while (1)
    {
    }
    /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
    /* USER CODE BEGIN 6 */
    /* User can add his own implementation to report the file name and line number,
     ex: dbg_printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
    /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "thingsboard_ota.h"

#include "app_verify.h"
#include "cmds.h"
#include "eeprom_emul.h"
#include "esp8266.h"
#include "log_config.h"
#include "net_cfg.h"
#if !ESP8266_MQTT_BACKEND_AT_ENABLE
#include "core_mqtt.h"
#endif

extern const struct lfs_file_config lfs_file_cfg;

#if LOG_WIFI_ENABLE
#define tb_ota_log(...) printf(__VA_ARGS__)
#else
#define tb_ota_log(...) ((void)0)
#endif

#if LOG_WIFI_TRACE_ENABLE
#define tb_ota_trace(...) printf(__VA_ARGS__)
#else
#define tb_ota_trace(...) ((void)0)
#endif

typedef struct
{
    uint8_t  valid;
    uint32_t size;
    char     title[32];
    char     version[32];
    char     checksum_algorithm[16];
    char     checksum[80];
} tb_firmware_info_t;

static void thingsboard_build_download_path(const tb_firmware_info_t *fw_info,
                                            char                     *path,
                                            uint16_t                  path_size)
{
    uint16_t    out_pos = 0U;
    uint16_t    i;
    const char *title;
    uint8_t     has_ext = 0U;

    if (path == NULL || path_size < 8U)
    {
        return;
    }

    memset(path, 0, path_size);

    if (fw_info == NULL || fw_info->title[0] == '\0')
    {
        strncpy(path, THINGSBOARD_OTA_DEFAULT_PATH, path_size - 1U);
        return;
    }

    title = fw_info->title;
    for (i = 0U; title[i] != '\0' && out_pos < (uint16_t)(path_size - 1U); i++)
    {
        char ch = title[i];

        if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            ch == '.' || ch == '_' || ch == '-')
        {
            path[out_pos++] = ch;
        }
        else
        {
            path[out_pos++] = '_';
        }
    }

    if (out_pos >= 4U)
    {
        const char *ext = &path[out_pos - 4U];
        if (strcmp(ext, ".elf") == 0)
        {
            has_ext = 1U;
        }
    }

    if (has_ext == 0U && out_pos < (uint16_t)(path_size - 5U))
    {
        path[out_pos++] = '.';
        path[out_pos++] = 'e';
        path[out_pos++] = 'l';
        path[out_pos++] = 'f';
    }

    path[out_pos] = '\0';
}
static uint16_t tb_cfg_chunk_size(void)
{
    return THINGSBOARD_OTA_DEFAULT_CHUNK_SIZE;
}

static const char *version_skip_to_digit(const char *text)
{
    while (text != NULL && *text != '\0' && isdigit((unsigned char)*text) == 0)
    {
        text++;
    }

    return text;
}

static uint8_t version_tail_has_nonzero(const char *text)
{
    const char *cursor = text;

    while (cursor != NULL)
    {
        unsigned long value;
        char         *end;

        cursor = version_skip_to_digit(cursor);
        if (*cursor == '\0')
        {
            return 0U;
        }

        value = strtoul(cursor, &end, 10);
        if (value != 0U)
        {
            return 1U;
        }

        cursor = end;
    }

    return 0U;
}

static int firmware_version_compare(const char *remote_version, const char *local_version)
{
    const char *remote = remote_version;
    const char *local  = local_version;

    if (remote == NULL || local == NULL)
    {
        return 0;
    }

    while (1)
    {
        unsigned long remote_value;
        unsigned long local_value;
        char         *remote_end;
        char         *local_end;

        remote = version_skip_to_digit(remote);
        local  = version_skip_to_digit(local);

        if (*remote == '\0' && *local == '\0')
        {
            return 0;
        }

        if (*remote == '\0')
        {
            return version_tail_has_nonzero(local) ? -1 : 0;
        }

        if (*local == '\0')
        {
            return version_tail_has_nonzero(remote) ? 1 : 0;
        }

        remote_value = strtoul(remote, &remote_end, 10);
        local_value  = strtoul(local, &local_end, 10);

        if (remote_value > local_value)
        {
            return 1;
        }

        if (remote_value < local_value)
        {
            return -1;
        }

        remote = remote_end;
        local  = local_end;
    }
}

#if ESP8266_MQTT_BACKEND_AT_ENABLE
static int thingsboard_firmware_download_to_lfs(lfs_ctx_t                *fs,
                                                const tb_firmware_info_t *fw_info,
                                                const char               *path)
{
    static uint8_t chunk_buf[THINGSBOARD_OTA_DEFAULT_CHUNK_SIZE];
    uint16_t       chunk_len = 0U;
    lfs_ssize_t    written;
    uint32_t       total_written = 0U;
    uint32_t       chunk_index   = 0U;
    uint32_t       request_size;
    uint32_t       expected_chunk_len;
    uint16_t       chunk_size = tb_cfg_chunk_size();
    int            err;

    if (fs == NULL || fw_info == NULL || path == NULL || path[0] == '\0' || fs->mounted == 0U ||
        fw_info->size == 0U)
    {
        tb_ota_log("Invalid firmware download arguments\r\n");
        return -1;
    }

    if (chunk_size > sizeof(chunk_buf))
    {
        tb_ota_log("ThingsBoard chunk size %u exceeds local buffer\r\n", chunk_size);
        return -2;
    }

    if (fs->file_open != 0U)
    {
        lfs_file_close(&fs->lfs, &fs->file);
        fs->file_open = 0U;
    }

    lfs_remove(&fs->lfs, path);
    err = lfs_file_opencfg(
        &fs->lfs, &fs->file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &lfs_file_cfg);
    if (err != 0)
    {
        tb_ota_log("LittleFS open %s failed: %d\r\n", path, err);
        return -3;
    }
    fs->file_open = 1U;

    tb_ota_trace("ThingsBoard firmware download start: %lu bytes -> %s\r\n",
                 (unsigned long)fw_info->size,
                 path);

    if (esp8266_thingsboard_subscribe_firmware_chunks() != 0)
    {
        tb_ota_log("ThingsBoard firmware chunk topic subscribe failed\r\n");
        lfs_file_close(&fs->lfs, &fs->file);
        fs->file_open = 0U;
        return -4;
    }

    while (total_written < fw_info->size)
    {
        expected_chunk_len = fw_info->size - total_written;
        if (expected_chunk_len > chunk_size)
        {
            expected_chunk_len = chunk_size;
        }
        request_size = chunk_size;

        if (esp8266_thingsboard_request_firmware_chunk(
                chunk_index, request_size, chunk_buf, chunk_size, &chunk_len) != 0)
        {
            tb_ota_log("ThingsBoard firmware chunk %lu receive failed\r\n",
                       (unsigned long)chunk_index);
            lfs_file_close(&fs->lfs, &fs->file);
            fs->file_open = 0U;
            return -5;
        }

        if (chunk_len != (uint16_t)expected_chunk_len)
        {
            tb_ota_log("ThingsBoard firmware chunk %lu length mismatch, expect %lu got %u\r\n",
                       (unsigned long)chunk_index,
                       (unsigned long)expected_chunk_len,
                       chunk_len);
            lfs_file_close(&fs->lfs, &fs->file);
            fs->file_open = 0U;
            return -6;
        }

        written = lfs_file_write(&fs->lfs, &fs->file, chunk_buf, chunk_len);
        if (written < 0 || (uint16_t)written != chunk_len)
        {
            tb_ota_log("LittleFS write %s failed at chunk %lu: %d\r\n",
                       path,
                       (unsigned long)chunk_index,
                       (int)written);
            lfs_file_close(&fs->lfs, &fs->file);
            fs->file_open = 0U;
            return -7;
        }

        total_written += chunk_len;
        tb_ota_trace("ThingsBoard firmware download progress: %lu/%lu bytes\r\n",
                     (unsigned long)total_written,
                     (unsigned long)fw_info->size);
        chunk_index++;
    }

    err = lfs_file_sync(&fs->lfs, &fs->file);
    if (err != 0)
    {
        tb_ota_log("LittleFS sync %s failed: %d\r\n", path, err);
        lfs_file_close(&fs->lfs, &fs->file);
        fs->file_open = 0U;
        return -8;
    }

    err           = lfs_file_close(&fs->lfs, &fs->file);
    fs->file_open = 0U;
    if (err != 0)
    {
        tb_ota_log("LittleFS close %s failed: %d\r\n", path, err);
        return -9;
    }

    tb_ota_log("ThingsBoard firmware saved to %s, %lu bytes, chunks %lu\r\n",
               path,
               (unsigned long)total_written,
               (unsigned long)chunk_index);
    return 0;
}
#else
static int thingsboard_mqtt_json_get_u32(const uint8_t *json,
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

static int thingsboard_mqtt_json_get_string(const uint8_t *json,
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

static int thingsboard_mqtt_wait_topic_message(const char *expected_topic,
                                               mqtt_rx_msg_t *message,
                                               uint32_t timeout_ms)
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

            tb_ota_trace("ThingsBoard MQTT ignore topic [%s]\r\n", message->topic);
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

static int thingsboard_firmware_download_to_lfs(lfs_ctx_t                *fs,
                                                const tb_firmware_info_t *fw_info,
                                                const char               *path)
{
    static const char firmware_chunk_filter[] = "v2/fw/response/+/chunk/+";
    static const char firmware_chunk_size[]   = "512";
    static char       request_topic[64];
    static char       response_topic[64];
    static mqtt_rx_msg_t rx_message;
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

    if (fs == NULL || fw_info == NULL || path == NULL || path[0] == '\0' || fs->mounted == 0U ||
        fw_info->size == 0U)
    {
        return -1;
    }

    if (fs->file_open != 0U)
    {
        err           = lfs_file_close(&fs->lfs, &fs->file);
        fs->file_open = 0U;
        if (err != 0)
        {
            tb_ota_log("LittleFS close stale file failed: %d\r\n", err);
            return -2;
        }
    }

    err = lfs_remove(&fs->lfs, path);
    if (err != 0 && err != LFS_ERR_NOENT)
    {
        tb_ota_log("LittleFS remove stale %s failed: %d\r\n", path, err);
        return -3;
    }

    err = lfs_file_opencfg(
        &fs->lfs, &fs->file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &lfs_file_cfg);
    if (err != 0)
    {
        tb_ota_log("LittleFS open %s failed: %d\r\n", path, err);
        return -4;
    }
    fs->file_open = 1U;

    rv = mqtt_subscribe_topic((char *)firmware_chunk_filter, Qos1, 2);
    if (rv != 0)
    {
        tb_ota_log("ThingsBoard firmware chunk topic subscribe failed: %d\r\n", rv);
        rv = -5;
        goto download_failed;
    }

    tb_ota_log("ThingsBoard firmware download start: %lu bytes -> %s\r\n",
               (unsigned long)fw_info->size,
               path);

    total_written = 0U;
    chunk_index   = 0U;
    request_id    = 2U;

    while (total_written < fw_info->size)
    {
        expected_chunk_len = fw_info->size - total_written;
        if (expected_chunk_len > THINGSBOARD_OTA_DEFAULT_CHUNK_SIZE)
        {
            expected_chunk_len = THINGSBOARD_OTA_DEFAULT_CHUNK_SIZE;
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
            rv = -6;
            goto download_failed;
        }

        rv = mqtt_publish(request_topic, Qos1, (char *)firmware_chunk_size);
        if (rv != 0)
        {
            tb_ota_log("ThingsBoard firmware chunk %lu request failed: %d\r\n",
                       (unsigned long)chunk_index,
                       rv);
            rv = -7;
            goto download_failed;
        }

        rv = thingsboard_mqtt_wait_topic_message(response_topic, &rx_message, 5000U);
        if (rv <= 0)
        {
            tb_ota_log("ThingsBoard firmware chunk %lu response failed: %d\r\n",
                       (unsigned long)chunk_index,
                       rv);
            rv = -8;
            goto download_failed;
        }

        if (rx_message.payload_len != (uint16_t)expected_chunk_len)
        {
            tb_ota_log("ThingsBoard firmware chunk %lu length mismatch, expect %lu got %u\r\n",
                       (unsigned long)chunk_index,
                       (unsigned long)expected_chunk_len,
                       (unsigned int)rx_message.payload_len);
            rv = -9;
            goto download_failed;
        }

        if (chunk_index == 0U)
        {
            tb_ota_trace("ThingsBoard firmware chunk 0 first 16 bytes:");
            preview_len = (rx_message.payload_len < 16U) ? rx_message.payload_len : 16U;
            for (preview_index = 0U; preview_index < preview_len; preview_index++)
            {
                tb_ota_trace(" %02X", (unsigned int)rx_message.payload[preview_index]);
            }
            tb_ota_trace("\r\n");
        }

        written = lfs_file_write(&fs->lfs, &fs->file, rx_message.payload, rx_message.payload_len);
        if (written < 0 || (uint16_t)written != rx_message.payload_len)
        {
            tb_ota_log("LittleFS write %s failed at chunk %lu: %d\r\n",
                       path,
                       (unsigned long)chunk_index,
                       (int)written);
            rv = -10;
            goto download_failed;
        }

        total_written += rx_message.payload_len;
        tb_ota_trace("ThingsBoard firmware download progress: %lu/%lu bytes\r\n",
                     (unsigned long)total_written,
                     (unsigned long)fw_info->size);
        chunk_index++;
        request_id++;
    }

    err = lfs_file_sync(&fs->lfs, &fs->file);
    if (err != 0)
    {
        tb_ota_log("LittleFS sync %s failed: %d\r\n", path, err);
        rv = -11;
        goto download_failed;
    }

    file_size = lfs_file_size(&fs->lfs, &fs->file);
    if (file_size < 0 || (uint32_t)file_size != fw_info->size)
    {
        tb_ota_log("LittleFS file size mismatch, expect %lu got %ld\r\n",
                   (unsigned long)fw_info->size,
                   (long)file_size);
        rv = -12;
        goto download_failed;
    }

    err           = lfs_file_close(&fs->lfs, &fs->file);
    fs->file_open = 0U;
    if (err != 0)
    {
        tb_ota_log("LittleFS close %s failed: %d\r\n", path, err);
        return -13;
    }

    tb_ota_log("ThingsBoard firmware saved to %s, %lu bytes, chunks %lu\r\n",
               path,
               (unsigned long)total_written,
               (unsigned long)chunk_index);
    return 0;

download_failed:
    if (fs->file_open != 0U)
    {
        err           = lfs_file_close(&fs->lfs, &fs->file);
        fs->file_open = 0U;
        if (err != 0)
        {
            tb_ota_log("LittleFS cleanup close %s failed: %d\r\n", path, err);
        }
    }

    err = lfs_remove(&fs->lfs, path);
    if (err != 0 && err != LFS_ERR_NOENT)
    {
        tb_ota_log("LittleFS cleanup remove %s failed: %d\r\n", path, err);
    }

    return rv;
}
#endif

static int thingsboard_firmware_should_download(const tb_firmware_info_t *fw_info)
{
    const char *local_version = sys_get_version();
    int         version_cmp;

    if (fw_info == NULL || fw_info->valid == 0U || fw_info->size == 0U)
    {
        tb_ota_log("ThingsBoard firmware info invalid, skip download\r\n");
        return -1;
    }

    if (fw_info->version[0] == '\0')
    {
        tb_ota_log("ThingsBoard firmware version empty, skip download\r\n");
        return -2;
    }

    if (fw_info->checksum_algorithm[0] != '\0' &&
        strcmp(fw_info->checksum_algorithm, "SHA256") != 0)
    {
        tb_ota_log("ThingsBoard checksum algorithm [%s] unsupported, skip download\r\n",
                   fw_info->checksum_algorithm);
        return -3;
    }

    version_cmp = firmware_version_compare(fw_info->version, local_version);
    tb_ota_log(
        "ThingsBoard firmware version remote[%s], local[%s]\r\n", fw_info->version, local_version);

    if (version_cmp <= 0)
    {
        tb_ota_log("ThingsBoard firmware is not newer, skip download\r\n");
        return 0;
    }

    tb_ota_log("ThingsBoard firmware update available, size %lu bytes\r\n",
               (unsigned long)fw_info->size);
    return 1;
}

static int thingsboard_firmware_verify_download(lfs_ctx_t                *fs,
                                                const tb_firmware_info_t *fw_info,
                                                const char               *path)
{
    static char              calc_checksum[65];
    firmware_verify_status_t verify_ret;

    if (fs == NULL || fw_info == NULL || path == NULL || path[0] == '\0' ||
        fw_info->checksum[0] == '\0')
    {
        tb_ota_log("ThingsBoard firmware checksum missing\r\n");
        return -1;
    }

    if (strcmp(fw_info->checksum_algorithm, "SHA256") != 0)
    {
        tb_ota_log("ThingsBoard checksum algorithm [%s] unsupported\r\n",
                   fw_info->checksum_algorithm);
        return -2;
    }

    tb_ota_trace("ThingsBoard firmware checksum verify begin\r\n");
    memset(calc_checksum, 0, sizeof(calc_checksum));
    verify_ret = verify_lfs_file_sha256(
        fs, path, fw_info->checksum, fw_info->size, calc_checksum, sizeof(calc_checksum));
    if (verify_ret != FIRMWARE_VERIFY_OK)
    {
        tb_ota_log("ThingsBoard firmware checksum verify failed: %d\r\n", verify_ret);
        tb_ota_trace("expected=%s\r\n", fw_info->checksum);
        if (calc_checksum[0] != '\0')
        {
            tb_ota_trace("actual  =%s\r\n", calc_checksum);
        }
        return -3;
    }

    tb_ota_log("ThingsBoard firmware checksum verify ok: SHA256 %s\r\n", calc_checksum);
    return 0;
}

static int thingsboard_wifi_prepare(const net_cfg_t *cfg, char *ip, char *gateway, int ip_size)
{
    uint8_t joined = 0U;
    int     ret    = -1;

    if (esp8266_module_init() != 0)
    {
        tb_ota_log("ESP8266 init failed\r\n");
        return -1;
    }

    tb_ota_log("ESP8266 init ok\r\n");

    if (esp8266_scan_ap((char *)cfg->wifi_ssid) != 0)
    {
        tb_ota_log("WiFi AP '%s' not found, stop OTA check\r\n", cfg->wifi_ssid);
        return -2;
    }

    if (esp8266_join_network((char *)cfg->wifi_ssid, (char *)cfg->wifi_password) != 0)
    {
        tb_ota_log("WiFi join failed, stop OTA check\r\n");
        return -3;
    }
    joined = 1U;

    tb_ota_log("WiFi joined\r\n");

    if (esp8266_get_ipaddr(ip, gateway, ip_size) != 0)
    {
        tb_ota_log("WiFi get IP failed, stop OTA check\r\n");
        ret = -4;
        goto fail_after_join;
    }

    tb_ota_trace("ip=%s gateway=%s\r\n", ip, gateway);

    if (esp8266_ping_test(gateway) != 0)
    {
        tb_ota_log("Gateway ping failed, stop OTA check\r\n");
        ret = -5;
        goto fail_after_join;
    }

    if (esp8266_ping_test("www.baidu.com") != 0)
    {
        tb_ota_log("Public network ping failed, stop OTA check\r\n");
        ret = -6;
        goto fail_after_join;
    }

    if (esp8266_ping_test((char *)cfg->mqtt_host) != 0)
    {
        tb_ota_log("ThingsBoard server ping failed, stop OTA check\r\n");
        ret = -7;
        goto fail_after_join;
    }

    return 0;

fail_after_join:
    if (joined != 0U)
    {
        esp8266_wifi_disconnect();
    }
    return ret;
}

#if ESP8266_MQTT_BACKEND_AT_ENABLE
static int thingsboard_mqtt_prepare(const net_cfg_t *cfg, tb_firmware_info_t *fw_info)
{
    static unsigned char telemetry[] = "{\"fw_test\":1,\"wifi_rssi\":-34}";
    esp8266_tb_firmware_info_t at_fw_info;
    int                  sent;

    memset(&at_fw_info, 0, sizeof(at_fw_info));

    if (esp8266_mqtt_connect(
            (char *)cfg->mqtt_host, (int)cfg->mqtt_port, (char *)cfg->access_token) != 0)
    {
        tb_ota_log(
            "ThingsBoard MQTT connect %s:%d failed\r\n", cfg->mqtt_host, (int)cfg->mqtt_port);
        return -1;
    }

    sent = esp8266_thingsboard_publish_telemetry(telemetry, (int)(sizeof(telemetry) - 1U));
    tb_ota_trace("ThingsBoard telemetry sent %d bytes\r\n", sent);
    if (sent <= 0)
    {
        esp8266_mqtt_disconnect();
        return -2;
    }

    if (esp8266_thingsboard_request_firmware_info() != 0)
    {
        tb_ota_log("ThingsBoard firmware info unavailable\r\n");
        esp8266_mqtt_disconnect();
        return -3;
    }

    tb_ota_log("ThingsBoard firmware info received\r\n");

    if (esp8266_thingsboard_get_firmware_info(&at_fw_info) != 0)
    {
        tb_ota_log("ThingsBoard firmware info read failed\r\n");
        esp8266_mqtt_disconnect();
        return -4;
    }

    memset(fw_info, 0, sizeof(*fw_info));
    fw_info->valid = at_fw_info.valid;
    fw_info->size  = at_fw_info.size;
    strncpy(fw_info->title, at_fw_info.title, sizeof(fw_info->title) - 1U);
    strncpy(fw_info->version, at_fw_info.version, sizeof(fw_info->version) - 1U);
    strncpy(fw_info->checksum_algorithm,
            at_fw_info.checksum_algorithm,
            sizeof(fw_info->checksum_algorithm) - 1U);
    strncpy(fw_info->checksum, at_fw_info.checksum, sizeof(fw_info->checksum) - 1U);

    return 0;
}
#else
static int thingsboard_mqtt_prepare(const net_cfg_t *cfg, tb_firmware_info_t *fw_info)
{
    static const char firmware_response_filter[] = "v1/devices/me/attributes/response/+";
    static const char firmware_response_topic[]  = "v1/devices/me/attributes/response/1";
    static const char firmware_request_topic[]   = "v1/devices/me/attributes/request/1";
    static const char firmware_request_payload[] =
        "{\"sharedKeys\":\"fw_title,fw_version,fw_size,fw_checksum,fw_checksum_algorithm\"}";
    static char        client_id[64];
    static mqtt_rx_msg_t rx_message;
    int                rv;

    if (cfg == NULL || fw_info == NULL)
    {
        return -1;
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
        tb_ota_log("ThingsBoard MQTT connect %s:%u failed: %d\r\n",
                   cfg->mqtt_host,
                   (unsigned int)cfg->mqtt_port,
                   rv);
        return -2;
    }

    rv = mqtt_subscribe_topic((char *)firmware_response_filter, Qos1, 1);
    if (rv != 0)
    {
        tb_ota_log("ThingsBoard firmware response subscribe failed: %d\r\n", rv);
        mqtt_disconnect();
        return -3;
    }

    rv = mqtt_publish((char *)firmware_request_topic, Qos1, (char *)firmware_request_payload);
    if (rv != 0)
    {
        tb_ota_log("ThingsBoard firmware attributes request failed: %d\r\n", rv);
        mqtt_disconnect();
        return -4;
    }

    rv = thingsboard_mqtt_wait_topic_message(firmware_response_topic, &rx_message, 5000U);
    if (rv <= 0)
    {
        tb_ota_log("ThingsBoard firmware attributes response failed: %d\r\n", rv);
        mqtt_disconnect();
        return -5;
    }

    memset(fw_info, 0, sizeof(*fw_info));
    rv = thingsboard_mqtt_json_get_u32(rx_message.payload,
                                       rx_message.payload_len,
                                       "fw_size",
                                       &fw_info->size);
    if (rv != 0 || fw_info->size == 0U)
    {
        tb_ota_log("ThingsBoard parse fw_size failed: %d\r\n", rv);
        mqtt_disconnect();
        return -6;
    }

    rv = thingsboard_mqtt_json_get_string(rx_message.payload,
                                          rx_message.payload_len,
                                          "fw_title",
                                          fw_info->title,
                                          sizeof(fw_info->title));
    if (rv != 0)
    {
        fw_info->title[0] = '\0';
    }

    rv = thingsboard_mqtt_json_get_string(rx_message.payload,
                                          rx_message.payload_len,
                                          "fw_version",
                                          fw_info->version,
                                          sizeof(fw_info->version));
    if (rv != 0)
    {
        tb_ota_log("ThingsBoard parse fw_version failed: %d\r\n", rv);
        mqtt_disconnect();
        return -7;
    }

    rv = thingsboard_mqtt_json_get_string(rx_message.payload,
                                          rx_message.payload_len,
                                          "fw_checksum_algorithm",
                                          fw_info->checksum_algorithm,
                                          sizeof(fw_info->checksum_algorithm));
    if (rv != 0)
    {
        tb_ota_log("ThingsBoard parse fw_checksum_algorithm failed: %d\r\n", rv);
        mqtt_disconnect();
        return -8;
    }

    rv = thingsboard_mqtt_json_get_string(rx_message.payload,
                                          rx_message.payload_len,
                                          "fw_checksum",
                                          fw_info->checksum,
                                          sizeof(fw_info->checksum));
    if (rv != 0)
    {
        tb_ota_log("ThingsBoard parse fw_checksum failed: %d\r\n", rv);
        mqtt_disconnect();
        return -9;
    }

    fw_info->valid = 1U;
    tb_ota_log("ThingsBoard firmware info received\r\n");
    tb_ota_log("ThingsBoard firmware size %lu bytes, title [%s], version [%s]\r\n",
               (unsigned long)fw_info->size,
               fw_info->title,
               fw_info->version);
    return 0;
}
#endif

static int thingsboard_ota_receive_cb(void *user_ctx, void *file_info_out)
{
    ota_ctx_t      *ota_ctx   = (ota_ctx_t *)user_ctx;
    lfs_ctx_t      *fs        = (ota_ctx != NULL) ? (lfs_ctx_t *)ota_ctx->resource_ctx : NULL;
    YmodemFileInfo *file_info = (YmodemFileInfo *)file_info_out;
    static char     ip[32];
    static char     gateway[32];
    static char     download_path[64];
    static tb_firmware_info_t fw_info;
    const net_cfg_t          *cfg = net_cfg_get();
    int                       decision;
    int                       stage_ret;
    int                       ret        = -1;
    uint8_t                   wifi_ready = 0U;
    uint8_t                   mqtt_ready = 0U;

    if (fs == NULL)
    {
        tb_ota_log("ThingsBoard OTA receive invalid fs context\r\n");
        return -100;
    }

    if (net_cfg_is_valid(cfg) == 0)
    {
        tb_ota_log("ThingsBoard OTA network config invalid\r\n");
        return -101;
    }

    if (ota_ctx != NULL)
    {
        memset(ota_ctx->ota_target_version, 0, sizeof(ota_ctx->ota_target_version));
    }

    memset(&fw_info, 0, sizeof(fw_info));

    stage_ret = thingsboard_wifi_prepare(cfg, ip, gateway, (int)sizeof(ip));
    if (stage_ret != 0)
    {
        tb_ota_log("ThingsBoard OTA WiFi prepare failed: %d\r\n", stage_ret);
        ret = -110;
        goto cleanup;
    }
    wifi_ready = 1U;

    stage_ret = thingsboard_mqtt_prepare(cfg, &fw_info);
    if (stage_ret != 0)
    {
        tb_ota_log("ThingsBoard OTA MQTT prepare failed: %d\r\n", stage_ret);
        ret = -120;
        goto cleanup;
    }
    mqtt_ready = 1U;

    decision = thingsboard_firmware_should_download(&fw_info);
    if (decision < 0)
    {
        tb_ota_log("ThingsBoard OTA firmware decision failed: %d\r\n", decision);
        ret = -130;
        goto cleanup;
    }

    if (decision == 0 && ota_ctx != NULL && ota_ctx->active_valid == 0U)
    {
        tb_ota_log("Active firmware invalid, force download remote firmware [%s]\r\n",
                   fw_info.version);
        decision = 1;
    }

    if (decision == 0)
    {
        tb_ota_log("ThingsBoard firmware no update, skip OTA transfer\r\n");
        ret = 0;
        goto cleanup;
    }

    thingsboard_build_download_path(&fw_info, download_path, sizeof(download_path));

    if (thingsboard_firmware_download_to_lfs(fs, &fw_info, download_path) != 0)
    {
        tb_ota_log("ThingsBoard firmware LFS download failed\r\n");
        ret = -140;
        goto cleanup;
    }

    if (thingsboard_firmware_verify_download(fs, &fw_info, download_path) != 0)
    {
        lfs_remove(&fs->lfs, download_path);
        tb_ota_log("ThingsBoard firmware checksum invalid, removed %s\r\n", download_path);
        ret = -150;
        goto cleanup;
    }

    tb_ota_log("ThingsBoard firmware LFS download ok\r\n");
    if (ota_ctx != NULL)
    {
        strncpy(
            ota_ctx->ota_target_version, fw_info.version, sizeof(ota_ctx->ota_target_version) - 1U);
    }
    if (file_info != NULL)
    {
        memset(file_info, 0, sizeof(*file_info));
        strncpy(file_info->filename, download_path, sizeof(file_info->filename) - 1U);
        file_info->filesize = fw_info.size;
    }
    ret = (int)fw_info.size;

cleanup:
    if (mqtt_ready != 0U)
    {
#if ESP8266_MQTT_BACKEND_AT_ENABLE
        esp8266_mqtt_disconnect();
#else
        mqtt_disconnect();
#endif
    }

    if (wifi_ready != 0U)
    {
        esp8266_wifi_disconnect();
    }

    tb_ota_trace("ThingsBoard OTA receive exit ret=%d\r\n", ret);
    return ret;
}

void thingsboard_ota_bind_transport(ota_ctx_t *ctx, transfer_cfg_t *transfer_cfg, lfs_ctx_t *fs)
{
    memset(transfer_cfg, 0, sizeof(*transfer_cfg));
    memset(ctx, 0, sizeof(*ctx));

    transfer_cfg->write_cb       = NULL;
    transfer_cfg->write_user_ctx = NULL;
    transfer_cfg->receive_cb     = thingsboard_ota_receive_cb;
    transfer_cfg->recv_user_ctx  = ctx;

    ctx->transfer_cfg = transfer_cfg;
    ctx->resource_ctx = fs;
}

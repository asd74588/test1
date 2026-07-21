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

extern const struct lfs_file_config lfs_file_cfg;

#if LOG_WIFI_ENABLE
#define tb_ota_log(format,args...) printf(format, ##args)
#else
#define tb_ota_log(format,args...) do{}while(0)
#endif

static void thingsboard_build_download_path(const esp8266_tb_firmware_info_t *fw_info,
                                            char *path,
                                            uint16_t path_size)
{
    uint16_t out_pos = 0U;
    uint16_t i;
    const char *title;
    uint8_t has_ext = 0U;

    if (path == NULL || path_size < 8U) {
        return;
    }

    memset(path, 0, path_size);

    if (fw_info == NULL || fw_info->title[0] == '\0') {
        strncpy(path, THINGSBOARD_OTA_DEFAULT_PATH, path_size - 1U);
        return;
    }

    title = fw_info->title;
    for (i = 0U; title[i] != '\0' && out_pos < (uint16_t)(path_size - 1U); i++) {
        char ch = title[i];

        if ((ch >= '0' && ch <= '9') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            ch == '.' || ch == '_' || ch == '-') {
            path[out_pos++] = ch;
        } else {
            path[out_pos++] = '_';
        }
    }

    if (out_pos >= 4U) {
        const char *ext = &path[out_pos - 4U];
        if (strcmp(ext, ".elf") == 0) {
            has_ext = 1U;
        }
    }

    if (has_ext == 0U && out_pos < (uint16_t)(path_size - 5U)) {
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
    while (text != NULL && *text != '\0' && isdigit((unsigned char)*text) == 0) {
        text++;
    }

    return text;
}

static uint8_t version_tail_has_nonzero(const char *text)
{
    const char *cursor = text;

    while (cursor != NULL) {
        unsigned long value;
        char *end;

        cursor = version_skip_to_digit(cursor);
        if (*cursor == '\0') {
            return 0U;
        }

        value = strtoul(cursor, &end, 10);
        if (value != 0U) {
            return 1U;
        }

        cursor = end;
    }

    return 0U;
}

static int firmware_version_compare(const char *remote_version, const char *local_version)
{
    const char *remote = remote_version;
    const char *local = local_version;

    if (remote == NULL || local == NULL) {
        return 0;
    }

    while (1) {
        unsigned long remote_value;
        unsigned long local_value;
        char *remote_end;
        char *local_end;

        remote = version_skip_to_digit(remote);
        local = version_skip_to_digit(local);

        if (*remote == '\0' && *local == '\0') {
            return 0;
        }

        if (*remote == '\0') {
            return version_tail_has_nonzero(local) ? -1 : 0;
        }

        if (*local == '\0') {
            return version_tail_has_nonzero(remote) ? 1 : 0;
        }

        remote_value = strtoul(remote, &remote_end, 10);
        local_value = strtoul(local, &local_end, 10);

        if (remote_value > local_value) {
            return 1;
        }

        if (remote_value < local_value) {
            return -1;
        }

        remote = remote_end;
        local = local_end;
    }
}

static int thingsboard_firmware_download_to_lfs(lfs_ctx_t *fs,
                                                const esp8266_tb_firmware_info_t *fw_info,
                                                const char *path)
{
    static uint8_t chunk_buf[THINGSBOARD_OTA_DEFAULT_CHUNK_SIZE];
    uint16_t chunk_len = 0U;
    lfs_ssize_t written;
    uint32_t total_written = 0U;
    uint32_t chunk_index = 0U;
    uint32_t request_size;
    uint32_t expected_chunk_len;
    uint16_t chunk_size = tb_cfg_chunk_size();
    int err;

    if (fs == NULL || fw_info == NULL || path == NULL || path[0] == '\0' ||
        fs->mounted == 0U || fw_info->size == 0U) {
        tb_ota_log("Invalid firmware download arguments\r\n");
        return -1;
    }

    if (chunk_size > sizeof(chunk_buf)) {
        tb_ota_log("ThingsBoard chunk size %u exceeds local buffer\r\n", chunk_size);
        return -2;
    }

    if (fs->file_open != 0U) {
        lfs_file_close(&fs->lfs, &fs->file);
        fs->file_open = 0U;
    }

    lfs_remove(&fs->lfs, path);
    err = lfs_file_opencfg(&fs->lfs,
                           &fs->file,
                           path,
                           LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC,
                           &lfs_file_cfg);
    if (err != 0) {
        tb_ota_log("LittleFS open %s failed: %d\r\n", path, err);
        return -3;
    }
    fs->file_open = 1U;

    tb_ota_log("ThingsBoard firmware download start: %lu bytes -> %s\r\n",
               (unsigned long)fw_info->size,
               path);

    if (esp8266_thingsboard_subscribe_firmware_chunks() != 0) {
        tb_ota_log("ThingsBoard firmware chunk topic subscribe failed\r\n");
        lfs_file_close(&fs->lfs, &fs->file);
        fs->file_open = 0U;
        return -4;
    }

    while (total_written < fw_info->size) {
        expected_chunk_len = fw_info->size - total_written;
        if (expected_chunk_len > chunk_size) {
            expected_chunk_len = chunk_size;
        }
        request_size = chunk_size;

        if (esp8266_thingsboard_request_firmware_chunk(chunk_index,
                                                       request_size,
                                                       chunk_buf,
                                                       chunk_size,
                                                       &chunk_len) != 0) {
            tb_ota_log("ThingsBoard firmware chunk %lu receive failed\r\n",
                       (unsigned long)chunk_index);
            lfs_file_close(&fs->lfs, &fs->file);
            fs->file_open = 0U;
            return -5;
        }

        if (chunk_len != (uint16_t)expected_chunk_len) {
            tb_ota_log("ThingsBoard firmware chunk %lu length mismatch, expect %lu got %u\r\n",
                       (unsigned long)chunk_index,
                       (unsigned long)expected_chunk_len,
                       chunk_len);
            lfs_file_close(&fs->lfs, &fs->file);
            fs->file_open = 0U;
            return -6;
        }

        written = lfs_file_write(&fs->lfs, &fs->file, chunk_buf, chunk_len);
        if (written < 0 || (uint16_t)written != chunk_len) {
            tb_ota_log("LittleFS write %s failed at chunk %lu: %d\r\n",
                       path,
                       (unsigned long)chunk_index,
                       (int)written);
            lfs_file_close(&fs->lfs, &fs->file);
            fs->file_open = 0U;
            return -7;
        }

        total_written += chunk_len;
        tb_ota_log("ThingsBoard firmware download progress: %lu/%lu bytes\r\n",
                   (unsigned long)total_written,
                   (unsigned long)fw_info->size);
        chunk_index++;
    }

    err = lfs_file_sync(&fs->lfs, &fs->file);
    if (err != 0) {
        tb_ota_log("LittleFS sync %s failed: %d\r\n", path, err);
        lfs_file_close(&fs->lfs, &fs->file);
        fs->file_open = 0U;
        return -8;
    }

    err = lfs_file_close(&fs->lfs, &fs->file);
    fs->file_open = 0U;
    if (err != 0) {
        tb_ota_log("LittleFS close %s failed: %d\r\n", path, err);
        return -9;
    }

    tb_ota_log("ThingsBoard firmware saved to %s, %lu bytes, chunks %lu\r\n",
               path,
               (unsigned long)total_written,
               (unsigned long)chunk_index);
    return 0;
}

static int thingsboard_firmware_should_download(const esp8266_tb_firmware_info_t *fw_info)
{
    const char *local_version = sys_get_version();
    int version_cmp;

    if (fw_info == NULL || fw_info->valid == 0U || fw_info->size == 0U) {
        tb_ota_log("ThingsBoard firmware info invalid, skip download\r\n");
        return -1;
    }

    if (fw_info->version[0] == '\0') {
        tb_ota_log("ThingsBoard firmware version empty, skip download\r\n");
        return -2;
    }

    if (fw_info->checksum_algorithm[0] != '\0' &&
        strcmp(fw_info->checksum_algorithm, "SHA256") != 0) {
        tb_ota_log("ThingsBoard checksum algorithm [%s] unsupported, skip download\r\n",
                   fw_info->checksum_algorithm);
        return -3;
    }

    version_cmp = firmware_version_compare(fw_info->version, local_version);
    tb_ota_log("ThingsBoard firmware version remote[%s], local[%s]\r\n",
               fw_info->version,
               local_version);

    if (version_cmp <= 0) {
        tb_ota_log("ThingsBoard firmware is not newer, skip download\r\n");
        return 0;
    }

    tb_ota_log("ThingsBoard firmware update available, size %lu bytes\r\n",
               (unsigned long)fw_info->size);
    return 1;
}

static int thingsboard_firmware_verify_download(lfs_ctx_t *fs,
                                                const esp8266_tb_firmware_info_t *fw_info,
                                                const char *path)
{
    static char calc_checksum[65];
    firmware_verify_status_t verify_ret;

    if (fs == NULL || fw_info == NULL || path == NULL || path[0] == '\0' ||
        fw_info->checksum[0] == '\0') {
        tb_ota_log("ThingsBoard firmware checksum missing\r\n");
        return -1;
    }

    if (strcmp(fw_info->checksum_algorithm, "SHA256") != 0) {
        tb_ota_log("ThingsBoard checksum algorithm [%s] unsupported\r\n",
                   fw_info->checksum_algorithm);
        return -2;
    }

    tb_ota_log("ThingsBoard firmware checksum verify begin\r\n");
    memset(calc_checksum, 0, sizeof(calc_checksum));
    verify_ret = verify_lfs_file_sha256(fs,
                                        path,
                                        fw_info->checksum,
                                        fw_info->size,
                                        calc_checksum,
                                        sizeof(calc_checksum));
    if (verify_ret != FIRMWARE_VERIFY_OK) {
        tb_ota_log("ThingsBoard firmware checksum verify failed: %d\r\n", verify_ret);
        tb_ota_log("expected=%s\r\n", fw_info->checksum);
        if (calc_checksum[0] != '\0') {
            tb_ota_log("actual  =%s\r\n", calc_checksum);
        }
        return -3;
    }

    tb_ota_log("ThingsBoard firmware checksum verify ok: SHA256 %s\r\n",
               calc_checksum);
    return 0;
}

static int thingsboard_wifi_prepare(const net_cfg_t *cfg,
                                    char *ip,
                                    char *gateway,
                                    int ip_size)
{
    uint8_t joined = 0U;
    int ret = -1;

    if (esp8266_module_init() != 0) {
        tb_ota_log("ESP8266 init failed\r\n");
        return -1;
    }

    tb_ota_log("ESP8266 init ok\r\n");

    if (esp8266_scan_ap((char *)cfg->wifi_ssid) != 0) {
        tb_ota_log("WiFi AP '%s' not found, stop OTA check\r\n", cfg->wifi_ssid);
        return -2;
    }

    if (esp8266_join_network((char *)cfg->wifi_ssid,
                             (char *)cfg->wifi_password) != 0) {
        tb_ota_log("WiFi join failed, stop OTA check\r\n");
        return -3;
    }
    joined = 1U;

    tb_ota_log("WiFi joined\r\n");

    if (esp8266_get_ipaddr(ip, gateway, ip_size) != 0) {
        tb_ota_log("WiFi get IP failed, stop OTA check\r\n");
        ret = -4;
        goto fail_after_join;
    }

    tb_ota_log("ip=%s gateway=%s\r\n", ip, gateway);

    if (esp8266_ping_test(gateway) != 0) {
        tb_ota_log("Gateway ping failed, stop OTA check\r\n");
        ret = -5;
        goto fail_after_join;
    }

    if (esp8266_ping_test("www.baidu.com") != 0) {
        tb_ota_log("Public network ping failed, stop OTA check\r\n");
        ret = -6;
        goto fail_after_join;
    }

    if (esp8266_ping_test((char *)cfg->mqtt_host) != 0) {
        tb_ota_log("ThingsBoard server ping failed, stop OTA check\r\n");
        ret = -7;
        goto fail_after_join;
    }

    return 0;

fail_after_join:
    if (joined != 0U) {
        esp8266_wifi_disconnect();
    }
    return ret;
}

static int thingsboard_mqtt_prepare(const net_cfg_t *cfg,
                                    esp8266_tb_firmware_info_t *fw_info)
{
    static unsigned char telemetry[] = "{\"fw_test\":1,\"wifi_rssi\":-34}";
    int sent;

    if (esp8266_mqtt_connect((char *)cfg->mqtt_host,
                             (int)cfg->mqtt_port,
                             (char *)cfg->access_token) != 0) {
        tb_ota_log("ThingsBoard MQTT connect %s:%d failed\r\n",
                   cfg->mqtt_host,
                   (int)cfg->mqtt_port);
        return -1;
    }

    sent = esp8266_thingsboard_publish_telemetry(telemetry,
                                                 (int)(sizeof(telemetry) - 1U));
    tb_ota_log("ThingsBoard telemetry sent %d bytes\r\n", sent);
    if (sent <= 0) {
        esp8266_mqtt_disconnect();
        return -2;
    }

    if (esp8266_thingsboard_request_firmware_info() != 0) {
        tb_ota_log("ThingsBoard firmware info unavailable\r\n");
        esp8266_mqtt_disconnect();
        return -3;
    }

    tb_ota_log("ThingsBoard firmware info received\r\n");

    if (esp8266_thingsboard_get_firmware_info(fw_info) != 0) {
        tb_ota_log("ThingsBoard firmware info read failed\r\n");
        esp8266_mqtt_disconnect();
        return -4;
    }

    return 0;
}

static int thingsboard_ota_receive_cb(void *user_ctx, void *file_info_out)
{
    ota_ctx_t *ota_ctx = (ota_ctx_t *)user_ctx;
    lfs_ctx_t *fs = (ota_ctx != NULL) ? (lfs_ctx_t *)ota_ctx->resource_ctx : NULL;
    YmodemFileInfo *file_info = (YmodemFileInfo *)file_info_out;
    static char ip[32];
    static char gateway[32];
    static char download_path[64];
    static esp8266_tb_firmware_info_t fw_info;
    const net_cfg_t *cfg = net_cfg_get();
    int decision;
    int stage_ret;
    int ret = -1;
    uint8_t wifi_ready = 0U;
    uint8_t mqtt_ready = 0U;

    if (fs == NULL) {
        tb_ota_log("ThingsBoard OTA receive invalid fs context\r\n");
        return -100;
    }

    if (net_cfg_is_valid(cfg) == 0) {
        tb_ota_log("ThingsBoard OTA network config invalid\r\n");
        return -101;
    }

    if (ota_ctx != NULL) {
        memset(ota_ctx->ota_target_version, 0, sizeof(ota_ctx->ota_target_version));
    }

    memset(&fw_info, 0, sizeof(fw_info));

    stage_ret = thingsboard_wifi_prepare(cfg, ip, gateway, (int)sizeof(ip));
    if (stage_ret != 0) {
        tb_ota_log("ThingsBoard OTA WiFi prepare failed: %d\r\n", stage_ret);
        ret = -110;
        goto cleanup;
    }
    wifi_ready = 1U;

    stage_ret = thingsboard_mqtt_prepare(cfg, &fw_info);
    if (stage_ret != 0) {
        tb_ota_log("ThingsBoard OTA MQTT prepare failed: %d\r\n", stage_ret);
        ret = -120;
        goto cleanup;
    }
    mqtt_ready = 1U;

    decision = thingsboard_firmware_should_download(&fw_info);
    if (decision < 0) {
        tb_ota_log("ThingsBoard OTA firmware decision failed: %d\r\n", decision);
        ret = -130;
        goto cleanup;
    }

    if (decision == 0 && ota_ctx != NULL && ota_ctx->active_valid == 0U) {
        tb_ota_log("Active firmware invalid, force download remote firmware [%s]\r\n",
                   fw_info.version);
        decision = 1;
    }

    if (decision == 0) {
        tb_ota_log("ThingsBoard firmware no update, skip OTA transfer\r\n");
        ret = 0;
        goto cleanup;
    }

    thingsboard_build_download_path(&fw_info, download_path, sizeof(download_path));

    if (thingsboard_firmware_download_to_lfs(fs, &fw_info, download_path) != 0) {
        tb_ota_log("ThingsBoard firmware LFS download failed\r\n");
        ret = -140;
        goto cleanup;
    }

    if (thingsboard_firmware_verify_download(fs, &fw_info, download_path) != 0) {
        lfs_remove(&fs->lfs, download_path);
        tb_ota_log("ThingsBoard firmware checksum invalid, removed %s\r\n",
                   download_path);
        ret = -150;
        goto cleanup;
    }

    tb_ota_log("ThingsBoard firmware LFS download ok\r\n");
    if (ota_ctx != NULL) {
        strncpy(ota_ctx->ota_target_version,
                fw_info.version,
                sizeof(ota_ctx->ota_target_version) - 1U);
    }
    if (file_info != NULL) {
        memset(file_info, 0, sizeof(*file_info));
        strncpy(file_info->filename, download_path, sizeof(file_info->filename) - 1U);
        file_info->filesize = fw_info.size;
    }
    ret = (int)fw_info.size;

cleanup:
    if (mqtt_ready != 0U) {
        esp8266_mqtt_disconnect();
    }

    if (wifi_ready != 0U) {
        esp8266_wifi_disconnect();
    }

    tb_ota_log("ThingsBoard OTA receive exit ret=%d\r\n", ret);
    return ret;
}

void thingsboard_ota_context_init(ota_ctx_t *ctx,
                                  transfer_cfg_t *transfer_cfg,
                                  lfs_ctx_t *fs)
{
    memset(transfer_cfg, 0, sizeof(*transfer_cfg));
    memset(ctx, 0, sizeof(*ctx));

    transfer_cfg->write_cb = NULL;
    transfer_cfg->write_user_ctx = NULL;
    transfer_cfg->receive_cb = thingsboard_ota_receive_cb;
    transfer_cfg->recv_user_ctx = ctx;

    ctx->transfer_cfg = transfer_cfg;
    ctx->resource_ctx = fs;
}

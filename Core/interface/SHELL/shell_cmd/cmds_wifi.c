/**
 * @file  cmds_wifi.c
 * @brief WiFi 命令实现（统一走 net_cfg 配置模块）
 */

#include "cmds.h"
#include "bsp_wifi.h"
#include "net_cfg.h"
#include <string.h>
#include <stdlib.h>

static void wifi_mask_secret(const char *src, char *out, uint32_t out_size)
{
    uint32_t len;

    if (out == NULL || out_size == 0U) {
        return;
    }

    memset(out, 0, out_size);
    if (src == NULL || src[0] == '\0') {
        return;
    }

    len = (uint32_t)strlen(src);
    if (len <= 4U) {
        strncpy(out, "****", out_size - 1U);
        return;
    }

    strncpy(out, src, 2U);
    if (out_size > 4U) {
        strncpy(out + 2U, "****", out_size - 3U);
    }
}

static int wifi_cfg_load_or_default(net_cfg_t *cfg)
{
    if (cfg == NULL) {
        return -1;
    }

    if (net_cfg_load(cfg) == 0) {
        return 0;
    }

    net_cfg_load_default(cfg);
    return 0;
}

static int wifi_cfg_save_and_refresh(const net_cfg_t *cfg)
{
    if (net_cfg_save(cfg) != 0) {
        return -1;
    }

    if (net_cfg_reload() != 0) {
        return -1;
    }

    return 0;
}

/* ================================================================
 * 命令实现
 * ================================================================ */

/**
 * wifi ssid <ssid>
 * 设置并持久化 WiFi SSID
 */
int wifi_set_ssid(uint8_t argc, char **argv)
{
    net_cfg_t cfg;

    if (argc < 2) {
        shell_printf("Usage: wifi ssid <ssid>\r\n");
        return -1;
    }

    wifi_cfg_load_or_default(&cfg);
    strncpy(cfg.wifi_ssid, argv[1], sizeof(cfg.wifi_ssid) - 1U);
    cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1U] = '\0';
    if (wifi_cfg_save_and_refresh(&cfg) != 0) {
        shell_printf("Save SSID failed.\r\n");
        return -1;
    }

    shell_printf("SSID saved: %s\r\n", argv[1]);
    return 0;
}

/**
 * wifi pass <password>
 * 设置并持久化 WiFi 密码
 */
int wifi_set_pass(uint8_t argc, char **argv)
{
    net_cfg_t cfg;

    if (argc < 2) {
        shell_printf("Usage: wifi pass <password>\r\n");
        return -1;
    }

    wifi_cfg_load_or_default(&cfg);
    strncpy(cfg.wifi_password, argv[1], sizeof(cfg.wifi_password) - 1U);
    cfg.wifi_password[sizeof(cfg.wifi_password) - 1U] = '\0';
    if (wifi_cfg_save_and_refresh(&cfg) != 0) {
        shell_printf("Save password failed.\r\n");
        return -1;
    }

    shell_printf("Password saved.\r\n");
    return 0;
}

int wifi_set_host(uint8_t argc, char **argv)
{
    net_cfg_t cfg;

    if (argc < 2) {
        shell_printf("Usage: wifi host <host>\r\n");
        return -1;
    }

    wifi_cfg_load_or_default(&cfg);
    strncpy(cfg.mqtt_host, argv[1], sizeof(cfg.mqtt_host) - 1U);
    cfg.mqtt_host[sizeof(cfg.mqtt_host) - 1U] = '\0';
    if (wifi_cfg_save_and_refresh(&cfg) != 0) {
        shell_printf("Save host failed.\r\n");
        return -1;
    }

    shell_printf("Host saved: %s\r\n", cfg.mqtt_host);
    return 0;
}

int wifi_set_port(uint8_t argc, char **argv)
{
    net_cfg_t cfg;
    uint32_t port;

    if (argc < 2) {
        shell_printf("Usage: wifi port <port>\r\n");
        return -1;
    }

    port = strtoul(argv[1], NULL, 0);
    if (port == 0U || port > 65535U) {
        shell_printf("Invalid port.\r\n");
        return -1;
    }

    wifi_cfg_load_or_default(&cfg);
    cfg.mqtt_port = (uint16_t)port;
    if (wifi_cfg_save_and_refresh(&cfg) != 0) {
        shell_printf("Save port failed.\r\n");
        return -1;
    }

    shell_printf("Port saved: %lu\r\n", (unsigned long)port);
    return 0;
}

int wifi_set_token(uint8_t argc, char **argv)
{
    net_cfg_t cfg;

    if (argc < 2) {
        shell_printf("Usage: wifi token <token>\r\n");
        return -1;
    }

    wifi_cfg_load_or_default(&cfg);
    strncpy(cfg.access_token, argv[1], sizeof(cfg.access_token) - 1U);
    cfg.access_token[sizeof(cfg.access_token) - 1U] = '\0';
    if (wifi_cfg_save_and_refresh(&cfg) != 0) {
        shell_printf("Save token failed.\r\n");
        return -1;
    }

    shell_printf("Token saved.\r\n");
    return 0;
}

/**
 * wifi connect [ssid] [pass]
 * 不带参数则使用 EEPROM 里保存的 SSID/密码
 */
int wifi_connect(uint8_t argc, char **argv)
{
    net_cfg_t cfg;

    wifi_cfg_load_or_default(&cfg);

    if (argc >= 3U) {
        strncpy(cfg.wifi_ssid, argv[1], sizeof(cfg.wifi_ssid) - 1U);
        cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1U] = '\0';
        strncpy(cfg.wifi_password, argv[2], sizeof(cfg.wifi_password) - 1U);
        cfg.wifi_password[sizeof(cfg.wifi_password) - 1U] = '\0';
    }

    if (cfg.wifi_ssid[0] == '\0') {
        shell_printf("No SSID set. Use: wifi ssid <ssid>\r\n");
        return -1;
    }

    shell_printf("Connecting to \"%s\"...\r\n", cfg.wifi_ssid);

    if (wifi_init() != WIFI_OK) {
        shell_printf("ESP8266 not responding.\r\n");
        return -1;
    }

    int ret = wifi_connect_ap(cfg.wifi_ssid, cfg.wifi_password);
    if (ret != WIFI_OK) {
        shell_printf("Connect failed (ret=%d).\r\n", ret);
        return -1;
    }

    char ip[24] = {0};
    wifi_get_ip(ip, sizeof(ip));
    shell_printf("Connected. IP: %s\r\n", ip);
    return 0;
}

/**
 * wifi status
 * 打印当前 WiFi 状态和 IP
 */
int wifi_status(uint8_t argc, char **argv)
{
    net_cfg_t cfg;
    char token_mask[16];

    (void)argc; (void)argv;
    wifi_cfg_load_or_default(&cfg);

    wifi_status_t st = wifi_get_status();

    const char *st_str;
    switch (st) {
    case WIFI_STATUS_GOT_IP:       st_str = "CONNECTED (got IP)"; break;
    case WIFI_STATUS_CONNECTED:    st_str = "CONNECTED (no IP)";  break;
    case WIFI_STATUS_DISCONNECTED: st_str = "DISCONNECTED";       break;
    default:                       st_str = "UNKNOWN";            break;
    }

    shell_printf("Status   : %s\r\n", st_str);
    shell_printf("SSID cfg : %s\r\n", cfg.wifi_ssid[0] ? cfg.wifi_ssid : "(not set)");
    shell_printf("MQTT host: %s\r\n", cfg.mqtt_host);
    shell_printf("MQTT port: %u\r\n", (unsigned int)cfg.mqtt_port);
    wifi_mask_secret(cfg.access_token, token_mask, sizeof(token_mask));
    shell_printf("Token    : %s\r\n", token_mask[0] ? token_mask : "(not set)");

    if (st == WIFI_STATUS_GOT_IP) {
        char ip[24] = {0};
        wifi_get_ip(ip, sizeof(ip));
        shell_printf("IP       : %s\r\n", ip);
    }
    return 0;
}


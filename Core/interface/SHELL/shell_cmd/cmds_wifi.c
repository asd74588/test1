/**
 * @file  cmds_wifi.c
 * @brief WiFi 命令实现（基于 bsp_wifi）
 *
 * SSID / 密码持久化到 EEPROM：
 *   每个字段最多 32 字节，按 4 字节一个 EE 变量存储。
 *   SSID：EE_VAR_WIFI_SSID_BASE  ~ +7  （8 个变量，每个存 4 字节）
 *   PASS：EE_VAR_WIFI_PASS_BASE  ~ +7
 */

#include "cmds.h"
#include "bsp_wifi.h"
#include <string.h>
#include <stdlib.h>

extern uint32_t          Read_Flag (uint16_t virt_addr);
extern HAL_StatusTypeDef Write_Flag(uint16_t virt_addr, uint32_t value);

/* ---- EEPROM 地址分配（接续已有变量之后，按需调整）------------ */
#define EE_VAR_WIFI_SSID_BASE   10U   /* 10~17：SSID，8 × 4 字节 = 32 字节 */
#define EE_VAR_WIFI_PASS_BASE   18U   /* 18~25：密码，8 × 4 字节 = 32 字节 */
#define WIFI_STR_EE_WORDS       8U    /* 每个字段占 8 个 EE 变量 */

/* ---- 工具：将字符串按 4 字节写入 / 读出 EEPROM --------------- */
static void ee_write_str(uint16_t base, const char *s, uint32_t max_len)
{
    char buf[32];
    memset(buf, 0, sizeof(buf));
    strncpy(buf, s, sizeof(buf) - 1U);

    uint32_t words = (max_len + 3U) / 4U;
    for (uint32_t i = 0U; i < words && i < WIFI_STR_EE_WORDS; i++) {
        uint32_t val;
        memcpy(&val, &buf[i * 4U], 4U);
        Write_Flag((uint16_t)(base + i), val);
    }
}

static void ee_read_str(uint16_t base, char *out, uint32_t out_sz)
{
    char buf[32];
    memset(buf, 0, sizeof(buf));
    for (uint32_t i = 0U; i < WIFI_STR_EE_WORDS; i++) {
        uint32_t val = Read_Flag((uint16_t)(base + i));
        memcpy(&buf[i * 4U], &val, 4U);
    }
    strncpy(out, buf, out_sz - 1U);
    out[out_sz - 1U] = '\0';
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
    if (argc < 2) {
        shell_printf("Usage: wifi ssid <ssid>\r\n");
        return -1;
    }
    ee_write_str(EE_VAR_WIFI_SSID_BASE, argv[1], 32U);
    shell_printf("SSID saved: %s\r\n", argv[1]);
    return 0;
}

/**
 * wifi pass <password>
 * 设置并持久化 WiFi 密码
 */
int wifi_set_pass(uint8_t argc, char **argv)
{
    if (argc < 2) {
        shell_printf("Usage: wifi pass <password>\r\n");
        return -1;
    }
    ee_write_str(EE_VAR_WIFI_PASS_BASE, argv[1], 32U);
    shell_printf("Password saved.\r\n");
    return 0;
}

/**
 * wifi connect [ssid] [pass]
 * 不带参数则使用 EEPROM 里保存的 SSID/密码
 */
int wifi_connect(uint8_t argc, char **argv)
{
    char ssid[33] = {0};
    char pass[33] = {0};

    if (argc >= 3) {
        strncpy(ssid, argv[1], sizeof(ssid) - 1U);
        strncpy(pass, argv[2], sizeof(pass) - 1U);
    } else {
        ee_read_str(EE_VAR_WIFI_SSID_BASE, ssid, sizeof(ssid));
        ee_read_str(EE_VAR_WIFI_PASS_BASE, pass, sizeof(pass));
    }

    if (ssid[0] == '\0') {
        shell_printf("No SSID set. Use: wifi ssid <ssid>\r\n");
        return -1;
    }

    shell_printf("Connecting to \"%s\"...\r\n", ssid);

    if (wifi_init() != WIFI_OK) {
        shell_printf("ESP8266 not responding.\r\n");
        return -1;
    }

    int ret = wifi_connect_ap(ssid, pass);
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
    (void)argc; (void)argv;

    char ssid[33] = {0};
    ee_read_str(EE_VAR_WIFI_SSID_BASE, ssid, sizeof(ssid));

    wifi_status_t st = wifi_get_status();

    const char *st_str;
    switch (st) {
    case WIFI_STATUS_GOT_IP:       st_str = "CONNECTED (got IP)"; break;
    case WIFI_STATUS_CONNECTED:    st_str = "CONNECTED (no IP)";  break;
    case WIFI_STATUS_DISCONNECTED: st_str = "DISCONNECTED";       break;
    default:                       st_str = "UNKNOWN";            break;
    }

    shell_printf("Status   : %s\r\n", st_str);
    shell_printf("SSID cfg : %s\r\n", ssid[0] ? ssid : "(not set)");

    if (st == WIFI_STATUS_GOT_IP) {
        char ip[24] = {0};
        wifi_get_ip(ip, sizeof(ip));
        shell_printf("IP       : %s\r\n", ip);
    }
    return 0;
}


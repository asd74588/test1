#include "net_cfg.h"

#include <stddef.h>
#include <string.h>

#include "eeprom_emul.h"

#define NET_CFG_WIFI_SSID_DEFAULT      "first"
#define NET_CFG_WIFI_PASSWORD_DEFAULT  "12345678"
#define NET_CFG_MQTT_HOST_DEFAULT      "47.97.214.156"
#define NET_CFG_MQTT_PORT_DEFAULT      1883U
#define NET_CFG_ACCESS_TOKEN_DEFAULT   "59pkoe1t5pba13obc2vx"

#define NET_CFG_PAGE_A_ADDR            0x0803E000U
#define NET_CFG_PAGE_B_ADDR            0x0803E800U
#define NET_CFG_PAGE_SIZE              FLASH_PAGE_SIZE
#define NET_CFG_FLASH_BANK             FLASH_BANK_1

#define NET_CFG_MAGIC                  0x4E434647UL
#define NET_CFG_LAYOUT_VER             0x00000001UL

typedef struct {
    uint32_t ssid_len;
    uint32_t password_len;
    uint32_t host_len;
    uint32_t token_len;
    uint16_t port;
    uint16_t reserved0;
    char     ssid[NET_CFG_WIFI_SSID_MAX + 1U];
    char     password[NET_CFG_WIFI_PASSWORD_MAX + 1U];
    char     host[NET_CFG_MQTT_HOST_MAX + 1U];
    char     token[NET_CFG_ACCESS_TOKEN_MAX + 1U];
} net_cfg_payload_t;

typedef struct {
    uint32_t magic;
    uint32_t layout_ver;
    uint32_t seq;
    uint32_t payload_len;
    uint32_t payload_crc32;
    uint32_t reserved0;
    net_cfg_payload_t payload;
} net_cfg_page_t;

static net_cfg_t s_net_cfg;
static uint8_t s_net_cfg_ready = 0U;

static void net_cfg_copy_string(char *dst, uint32_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0U) {
        return;
    }

    memset(dst, 0, dst_size);
    if (src != NULL) {
        strncpy(dst, src, dst_size - 1U);
    }
}

static uint32_t net_cfg_strnlen_local(const char *text, uint32_t max_len)
{
    uint32_t len = 0U;

    if (text == NULL) {
        return 0U;
    }

    while (len < max_len && text[len] != '\0') {
        len++;
    }

    return len;
}

static uint32_t net_cfg_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t i;
    uint32_t bit;

    if (data == NULL) {
        return 0U;
    }

    for (i = 0U; i < len; i++) {
        crc ^= data[i];
        for (bit = 0U; bit < 8U; bit++) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ 0xEDB88320UL;
            } else {
                crc >>= 1U;
            }
        }
    }

    return ~crc;
}

static void net_cfg_payload_from_cfg(net_cfg_payload_t *payload, const net_cfg_t *cfg)
{
    if (payload == NULL || cfg == NULL) {
        return;
    }

    memset(payload, 0, sizeof(*payload));
    payload->ssid_len = net_cfg_strnlen_local(cfg->wifi_ssid, NET_CFG_WIFI_SSID_MAX);
    payload->password_len = net_cfg_strnlen_local(cfg->wifi_password, NET_CFG_WIFI_PASSWORD_MAX);
    payload->host_len = net_cfg_strnlen_local(cfg->mqtt_host, NET_CFG_MQTT_HOST_MAX);
    payload->token_len = net_cfg_strnlen_local(cfg->access_token, NET_CFG_ACCESS_TOKEN_MAX);
    payload->port = cfg->mqtt_port;

    net_cfg_copy_string(payload->ssid, sizeof(payload->ssid), cfg->wifi_ssid);
    net_cfg_copy_string(payload->password, sizeof(payload->password), cfg->wifi_password);
    net_cfg_copy_string(payload->host, sizeof(payload->host), cfg->mqtt_host);
    net_cfg_copy_string(payload->token, sizeof(payload->token), cfg->access_token);
}

static void net_cfg_cfg_from_payload(net_cfg_t *cfg, const net_cfg_payload_t *payload)
{
    if (cfg == NULL || payload == NULL) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    net_cfg_copy_string(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), payload->ssid);
    net_cfg_copy_string(cfg->wifi_password, sizeof(cfg->wifi_password), payload->password);
    net_cfg_copy_string(cfg->mqtt_host, sizeof(cfg->mqtt_host), payload->host);
    net_cfg_copy_string(cfg->access_token, sizeof(cfg->access_token), payload->token);
    cfg->mqtt_port = payload->port;
}

static void net_cfg_build_page(net_cfg_page_t *page, const net_cfg_t *cfg, uint32_t seq)
{
    if (page == NULL || cfg == NULL) {
        return;
    }

    memset(page, 0, sizeof(*page));
    page->magic = NET_CFG_MAGIC;
    page->layout_ver = NET_CFG_LAYOUT_VER;
    page->seq = seq;
    page->payload_len = sizeof(page->payload);
    net_cfg_payload_from_cfg(&page->payload, cfg);
    page->payload_crc32 = net_cfg_crc32((const uint8_t *)&page->payload, sizeof(page->payload));
}

static int net_cfg_flash_read_page(uint32_t page_addr, net_cfg_page_t *page)
{
    if (page == NULL) {
        return -1;
    }

    memcpy(page, (const void *)page_addr, sizeof(*page));
    return 0;
}

static int net_cfg_flash_page_valid(uint32_t page_addr, net_cfg_page_t *page)
{
    net_cfg_page_t local_page;
    uint32_t calc_crc;

    if (page == NULL) {
        page = &local_page;
    }

    if (net_cfg_flash_read_page(page_addr, page) != 0) {
        return 0;
    }

    if (page->magic != NET_CFG_MAGIC ||
        page->layout_ver != NET_CFG_LAYOUT_VER ||
        page->payload_len != sizeof(page->payload)) {
        return 0;
    }

    calc_crc = net_cfg_crc32((const uint8_t *)&page->payload, sizeof(page->payload));
    if (calc_crc != page->payload_crc32) {
        return 0;
    }

    return 1;
}

static int net_cfg_seq_is_newer(uint32_t lhs, uint32_t rhs)
{
    return ((int32_t)(lhs - rhs) > 0) ? 1 : 0;
}

static HAL_StatusTypeDef net_cfg_flash_erase_page(uint32_t page_addr)
{
    FLASH_EraseInitTypeDef erase_init;
    uint32_t page_error = 0U;

    erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
    erase_init.Banks = NET_CFG_FLASH_BANK;
    erase_init.Page = (page_addr - FLASH_BASE) / NET_CFG_PAGE_SIZE;
    erase_init.NbPages = 1U;

    return HAL_FLASHEx_Erase(&erase_init, &page_error);
}

static HAL_StatusTypeDef net_cfg_flash_write_page(uint32_t page_addr, const net_cfg_page_t *page)
{
    uint32_t offset;
    uint64_t value;

    if (page == NULL) {
        return HAL_ERROR;
    }

    for (offset = 0U; offset < sizeof(*page); offset += 8U) {
        memcpy(&value, ((const uint8_t *)page) + offset, sizeof(value));
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, page_addr + offset, value) != HAL_OK) {
            return HAL_ERROR;
        }
    }

    return HAL_OK;
}

static int net_cfg_flash_load_latest(net_cfg_t *cfg, uint32_t *seq_out)
{
    net_cfg_page_t page_a;
    net_cfg_page_t page_b;
    net_cfg_page_t *selected;
    uint8_t valid_a;
    uint8_t valid_b;

    if (cfg == NULL) {
        return -1;
    }

    valid_a = (uint8_t)net_cfg_flash_page_valid(NET_CFG_PAGE_A_ADDR, &page_a);
    valid_b = (uint8_t)net_cfg_flash_page_valid(NET_CFG_PAGE_B_ADDR, &page_b);

    if (valid_a == 0U && valid_b == 0U) {
        return -2;
    }

    if (valid_a != 0U && valid_b != 0U) {
        selected = net_cfg_seq_is_newer(page_a.seq, page_b.seq) ? &page_a : &page_b;
    } else {
        selected = (valid_a != 0U) ? &page_a : &page_b;
    }

    net_cfg_cfg_from_payload(cfg, &selected->payload);
    if (seq_out != NULL) {
        *seq_out = selected->seq;
    }

    return net_cfg_is_valid(cfg) ? 0 : -3;
}

static void net_cfg_legacy_read_string(uint16_t base,
                                       uint16_t words,
                                       char *out,
                                       uint32_t out_size)
{
    uint32_t i;
    uint8_t has_data = 0U;
    uint8_t raw[(NET_CFG_MQTT_HOST_MAX + 1U) > (NET_CFG_ACCESS_TOKEN_MAX + 1U) ?
                (NET_CFG_MQTT_HOST_MAX + 1U) : (NET_CFG_ACCESS_TOKEN_MAX + 1U)];

    if (out == NULL || out_size == 0U) {
        return;
    }

    memset(raw, 0, sizeof(raw));
    for (i = 0U; i < words; i++) {
        uint32_t word = Read_Flag((uint16_t)(base + i));
        memcpy(&raw[i * 4U], &word, sizeof(word));
    }

    for (i = 0U; i < (words * 4U) && i < sizeof(raw); i++) {
        if (raw[i] == 0xFFU) {
            raw[i] = 0U;
            continue;
        }

        if (raw[i] != 0U) {
            has_data = 1U;
        }
    }

    memset(out, 0, out_size);
    if (has_data != 0U) {
        strncpy(out, (const char *)raw, out_size - 1U);
    }
}

static int net_cfg_try_load_legacy(net_cfg_t *cfg)
{
    uint32_t port_raw;
    uint8_t has_legacy = 0U;
    char legacy_buf[NET_CFG_MQTT_HOST_MAX + 1U];

    if (cfg == NULL) {
        return -1;
    }

    memset(legacy_buf, 0, sizeof(legacy_buf));
    net_cfg_legacy_read_string(EE_VAR_WIFI_SSID_BASE, 8U, legacy_buf, sizeof(legacy_buf));
    if (legacy_buf[0] != '\0') {
        net_cfg_copy_string(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), legacy_buf);
        has_legacy = 1U;
    }

    memset(legacy_buf, 0, sizeof(legacy_buf));
    net_cfg_legacy_read_string(EE_VAR_WIFI_PASS_BASE, 8U, legacy_buf, sizeof(legacy_buf));
    if (legacy_buf[0] != '\0') {
        net_cfg_copy_string(cfg->wifi_password, sizeof(cfg->wifi_password), legacy_buf);
        has_legacy = 1U;
    }

    memset(legacy_buf, 0, sizeof(legacy_buf));
    net_cfg_legacy_read_string(EE_VAR_MQTT_HOST_BASE, 16U, legacy_buf, sizeof(legacy_buf));
    if (legacy_buf[0] != '\0') {
        net_cfg_copy_string(cfg->mqtt_host, sizeof(cfg->mqtt_host), legacy_buf);
        has_legacy = 1U;
    }

    memset(legacy_buf, 0, sizeof(legacy_buf));
    net_cfg_legacy_read_string(EE_VAR_MQTT_TOKEN_BASE, 16U, legacy_buf, sizeof(legacy_buf));
    if (legacy_buf[0] != '\0') {
        net_cfg_copy_string(cfg->access_token, sizeof(cfg->access_token), legacy_buf);
        has_legacy = 1U;
    }

    port_raw = Read_Flag(EE_VAR_MQTT_PORT);
    if (port_raw != 0xFFFFFFFFUL && port_raw > 0U && port_raw <= 65535UL) {
        cfg->mqtt_port = (uint16_t)port_raw;
        has_legacy = 1U;
    }

    if (has_legacy == 0U || net_cfg_is_valid(cfg) == 0) {
        return -1;
    }

    return 0;
}

void net_cfg_load_default(net_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    net_cfg_copy_string(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), NET_CFG_WIFI_SSID_DEFAULT);
    net_cfg_copy_string(cfg->wifi_password, sizeof(cfg->wifi_password), NET_CFG_WIFI_PASSWORD_DEFAULT);
    net_cfg_copy_string(cfg->mqtt_host, sizeof(cfg->mqtt_host), NET_CFG_MQTT_HOST_DEFAULT);
    cfg->mqtt_port = NET_CFG_MQTT_PORT_DEFAULT;
    net_cfg_copy_string(cfg->access_token, sizeof(cfg->access_token), NET_CFG_ACCESS_TOKEN_DEFAULT);
}

int net_cfg_load(net_cfg_t *cfg)
{
    if (cfg == NULL) {
        return -1;
    }

    if (net_cfg_flash_load_latest(cfg, NULL) == 0) {
        return 0;
    }

    net_cfg_load_default(cfg);
    if (net_cfg_try_load_legacy(cfg) == 0) {
        (void)net_cfg_save(cfg);
        return 0;
    }

    return net_cfg_is_valid(cfg) ? 0 : -2;
}

int net_cfg_save(const net_cfg_t *cfg)
{
    net_cfg_page_t page;
    net_cfg_page_t active_page;
    uint32_t active_seq = 0U;
    uint32_t target_addr = NET_CFG_PAGE_A_ADDR;
    uint8_t valid_a;
    uint8_t valid_b;
    HAL_StatusTypeDef hal_ret;

    if (net_cfg_is_valid(cfg) == 0) {
        return -1;
    }

    valid_a = (uint8_t)net_cfg_flash_page_valid(NET_CFG_PAGE_A_ADDR, &active_page);
    valid_b = (uint8_t)net_cfg_flash_page_valid(NET_CFG_PAGE_B_ADDR, &page);

    if (valid_a != 0U && valid_b != 0U) {
        if (net_cfg_seq_is_newer(active_page.seq, page.seq)) {
            active_seq = active_page.seq;
            target_addr = NET_CFG_PAGE_B_ADDR;
        } else {
            active_seq = page.seq;
            target_addr = NET_CFG_PAGE_A_ADDR;
        }
    } else if (valid_a != 0U) {
        active_seq = active_page.seq;
        target_addr = NET_CFG_PAGE_B_ADDR;
    } else if (valid_b != 0U) {
        active_seq = page.seq;
        target_addr = NET_CFG_PAGE_A_ADDR;
    }

    net_cfg_build_page(&page, cfg, active_seq + 1U);

    HAL_FLASH_Unlock();
    hal_ret = net_cfg_flash_erase_page(target_addr);
    if (hal_ret == HAL_OK) {
        hal_ret = net_cfg_flash_write_page(target_addr, &page);
    }
    HAL_FLASH_Lock();

    if (hal_ret != HAL_OK || net_cfg_flash_page_valid(target_addr, &active_page) == 0) {
        return -2;
    }

    s_net_cfg = *cfg;
    s_net_cfg_ready = 1U;
    return 0;
}

int net_cfg_reload(void)
{
    int ret = net_cfg_load(&s_net_cfg);
    s_net_cfg_ready = (ret == 0) ? 1U : 0U;
    return ret;
}

const net_cfg_t *net_cfg_get(void)
{
    if (s_net_cfg_ready == 0U) {
        if (net_cfg_reload() != 0) {
            net_cfg_load_default(&s_net_cfg);
            s_net_cfg_ready = 1U;
        }
    }

    return &s_net_cfg;
}

int net_cfg_is_valid(const net_cfg_t *cfg)
{
    if (cfg == NULL ||
        cfg->wifi_ssid[0] == '\0' ||
        cfg->mqtt_host[0] == '\0' ||
        cfg->mqtt_port == 0U ||
        cfg->access_token[0] == '\0') {
        return 0;
    }

    return 1;
}

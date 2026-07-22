#include "cmds.h"
#include "net_cfg.h"
#include <stdlib.h>
#include <string.h>

extern uint32_t          Read_Flag(uint16_t virt_addr);
extern HAL_StatusTypeDef Write_Flag(uint16_t virt_addr, uint32_t value);

typedef struct
{
    uint16_t    addr;
    const char *name;
} cfg_var_desc_t;

static const cfg_var_desc_t s_cfg_vars[] = {
    {EE_VAR_OTA_STATE, "ota_state"},
    {EE_VAR_ACTIVE_SLOT, "active_slot"},
    {EE_VAR_TARGET_SLOT, "target_slot"},
    {EE_VAR_REVERT_REASON, "revert_reason"},
    {EE_VAR_DEVICE_SN, "device_sn"},
};

static void cfg_mask_secret(const char *src, char *out, uint32_t out_size)
{
    uint32_t len;

    if (out == NULL || out_size == 0U)
    {
        return;
    }

    memset(out, 0, out_size);
    if (src == NULL || src[0] == '\0')
    {
        return;
    }

    len = (uint32_t)strlen(src);
    if (len <= 4U)
    {
        strncpy(out, "****", out_size - 1U);
        return;
    }

    strncpy(out, src, 2U);
    if (out_size > 4U)
    {
        strncpy(out + 2U, "****", out_size - 3U);
    }
}

int cmd_cfg_get(uint8_t argc, char **argv)
{
    if (argc < 2U)
    {
        shell_printf("Usage: cfg get <addr>\r\n");
        return -1;
    }

    uint16_t addr  = (uint16_t)strtoul(argv[1], NULL, 0);
    uint32_t value = Read_Flag(addr);

    shell_printf("EE[%u] = 0x%08lX (%lu)\r\n",
                 (unsigned int)addr,
                 (unsigned long)value,
                 (unsigned long)value);
    return 0;
}

int cmd_cfg_set(uint8_t argc, char **argv)
{
    if (argc < 3U)
    {
        shell_printf("Usage: cfg set <addr> <val>\r\n");
        return -1;
    }

    uint16_t addr  = (uint16_t)strtoul(argv[1], NULL, 0);
    uint32_t value = (uint32_t)strtoul(argv[2], NULL, 0);

    if (Write_Flag(addr, value) != HAL_OK)
    {
        shell_printf("ERROR: write EE[%u] failed\r\n", (unsigned int)addr);
        return -1;
    }

    shell_printf("EE[%u] <- 0x%08lX OK\r\n", (unsigned int)addr, (unsigned long)value);
    return 0;
}

int cmd_cfg_dump(uint8_t argc, char **argv)
{
    const net_cfg_t *cfg;
    char             token_mask[16];

    (void)argc;
    (void)argv;

    cfg = net_cfg_get();
    cfg_mask_secret(cfg->access_token, token_mask, sizeof(token_mask));

    shell_printf("--- Network config (Flash dual-page) ---\r\n");
    shell_printf("wifi_ssid        : %s\r\n", cfg->wifi_ssid[0] ? cfg->wifi_ssid : "(not set)");
    shell_printf("wifi_password    : %s\r\n", cfg->wifi_password[0] ? "(hidden)" : "(empty)");
    shell_printf("mqtt_host        : %s\r\n", cfg->mqtt_host[0] ? cfg->mqtt_host : "(not set)");
    shell_printf("mqtt_port        : %u\r\n", (unsigned int)cfg->mqtt_port);
    shell_printf("access_token     : %s\r\n", token_mask[0] ? token_mask : "(not set)");
    shell_printf("flash_page_a     : 0x0803E000\r\n");
    shell_printf("flash_page_b     : 0x0803E800\r\n");
    shell_printf("\r\n");

    shell_printf("--- EEPROM config ---\r\n");
    for (uint32_t i = 0U; i < (sizeof(s_cfg_vars) / sizeof(s_cfg_vars[0])); i++)
    {
        uint32_t value = Read_Flag(s_cfg_vars[i].addr);
        shell_printf("%-16s EE[%u] = 0x%08lX\r\n",
                     s_cfg_vars[i].name,
                     (unsigned int)s_cfg_vars[i].addr,
                     (unsigned long)value);
    }
    shell_printf("legacy_net_cfg   : only used for one-time migration\r\n");
    return 0;
}

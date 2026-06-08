#include "cmds.h"
#include <stdlib.h>

extern uint32_t Read_Flag(uint16_t virt_addr);
extern HAL_StatusTypeDef Write_Flag(uint16_t virt_addr, uint32_t value);

typedef struct {
    uint16_t addr;
    const char *name;
} cfg_var_desc_t;

static const cfg_var_desc_t s_cfg_vars[] = {
    { EE_VAR_OTA_STATE,       "ota_state" },
    { EE_VAR_ACTIVE_SLOT,     "active_slot" },
    { EE_VAR_TARGET_SLOT,     "target_slot" },
    { EE_VAR_REVERT_REASON,   "revert_reason" },
    { EE_VAR_DEVICE_SN,       "device_sn" },
    { EE_VAR_WIFI_SSID_BASE,  "wifi_ssid[0]" },
    { EE_VAR_WIFI_PASS_BASE,  "wifi_pass[0]" },
};

int cmd_cfg_get(uint8_t argc, char **argv)
{
    if (argc < 2U) {
        shell_printf("Usage: cfg get <addr>\r\n");
        return -1;
    }

    uint16_t addr = (uint16_t)strtoul(argv[1], NULL, 0);
    uint32_t value = Read_Flag(addr);

    shell_printf("EE[%u] = 0x%08lX (%lu)\r\n",
                 (unsigned int)addr,
                 (unsigned long)value,
                 (unsigned long)value);
    return 0;
}

int cmd_cfg_set(uint8_t argc, char **argv)
{
    if (argc < 3U) {
        shell_printf("Usage: cfg set <addr> <val>\r\n");
        return -1;
    }

    uint16_t addr = (uint16_t)strtoul(argv[1], NULL, 0);
    uint32_t value = (uint32_t)strtoul(argv[2], NULL, 0);

    if (Write_Flag(addr, value) != HAL_OK) {
        shell_printf("ERROR: write EE[%u] failed\r\n", (unsigned int)addr);
        return -1;
    }

    shell_printf("EE[%u] <- 0x%08lX OK\r\n",
                 (unsigned int)addr,
                 (unsigned long)value);
    return 0;
}

int cmd_cfg_dump(uint8_t argc, char **argv)
{
    (void)argc;
    (void)argv;

    shell_printf("--- EEPROM config ---\r\n");
    for (uint32_t i = 0U; i < (sizeof(s_cfg_vars) / sizeof(s_cfg_vars[0])); i++) {
        uint32_t value = Read_Flag(s_cfg_vars[i].addr);
        shell_printf("%-16s EE[%u] = 0x%08lX\r\n",
                     s_cfg_vars[i].name,
                     (unsigned int)s_cfg_vars[i].addr,
                     (unsigned long)value);
    }
    return 0;
}

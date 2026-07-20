/**
 * @file  cmds_table.c
 * @brief 命令表 — 库对接层
 *
 * 新增命令：在此追加一行，其余文件不动。
 */

#include "cmds.h"
#include "fs_cmd.h"

static int shell_fs_ls(uint8_t argc, char **argv)
{
    return fs_cmd_ls((int)argc, argv);
}

static int shell_fs_cat(uint8_t argc, char **argv)
{
    return fs_cmd_cat((int)argc, argv);
}

static int shell_fs_write(uint8_t argc, char **argv)
{
    return fs_cmd_write((int)argc, argv);
}

static int shell_fs_rm(uint8_t argc, char **argv)
{
    return fs_cmd_rm((int)argc, argv);
}

static int shell_fs_mkdir(uint8_t argc, char **argv)
{
    return fs_cmd_mkdir((int)argc, argv);
}

static int shell_fs_free(uint8_t argc, char **argv)
{
    return fs_cmd_free((int)argc, argv);
}

struct cmd cmd_table[] = {
    /* 系统 */
    { "help",        cmd_help,        "show this help"                           },
    { "clear",       cmd_clear,       "clear screen"                             },
    { "version",     cmd_version,     "print firmware version and build time"    },
    { "reboot",      cmd_reboot,      "software reset"                           },
    /* 文件传输 */
    { "rz",          ymodem_receive,  "rz [path]         receive file via Ymodem"  },
    { "sz",          ymodem_send,     "sz <path>         send file via Ymodem"     },
    /* 文件系统 */
    { "ls",          shell_fs_ls,      "ls [path]         list LittleFS files"      },
    { "cat",         shell_fs_cat,     "cat <path>        print LittleFS file"      },
    { "write",       shell_fs_write,   "write <file> <data> write LittleFS file"   },
    { "rm",          shell_fs_rm,      "rm <path>         remove LittleFS file"     },
    { "mkdir",       shell_fs_mkdir,   "mkdir <path>      create LittleFS dir"      },
    { "free",        shell_fs_free,    "show LittleFS usage"                       },
    /* 设备配置 */
    { "cfg get",     cmd_cfg_get,     "cfg get <addr>        read EEPROM var"    },
    { "cfg set",     cmd_cfg_set,     "cfg set <addr> <val>  write EEPROM var"   },
    { "cfg dump",    cmd_cfg_dump,    "dump all known EEPROM config vars"        },
    /* WiFi */
    { "wifi ssid",   wifi_set_ssid,   "wifi ssid <ssid>      set & save SSID"   },
    { "wifi pass",   wifi_set_pass,   "wifi pass <pass>      set & save password"},
    { "wifi connect",wifi_connect,    "wifi connect [ssid] [pass]"               },
    { "wifi status", wifi_status,     "show WiFi connection status"              },
    /* OTA */
    { "ota status",  cmd_ota_status,  "show OTA and device info"                 },
    { "ota slot",    cmd_ota_slot,    "ota slot <0|1>        set target slot"    },
    { "ota boot",    cmd_ota_boot,    "experimental: set OTA state to BOOT"      },
    { "ota revert",  cmd_ota_revert,  "revert to previous slot and reboot"       },
    { "ota trigger", cmd_ota_trigger, "set UPGRADING and reboot to bootloader"   },
    { "ota start",   cmd_ota_start,   "ota start <path> [slot]  flash & reboot"  },
};

const uint16_t cmd_table_size =
    sizeof(cmd_table) / sizeof(cmd_table[0]);

char *auto_complete_words[]         = { NULL };
const uint16_t auto_complete_words_size = 0U;



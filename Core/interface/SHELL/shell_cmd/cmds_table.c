/**
 * @file  cmds_table.c
 * @brief 命令表 — 库对接层
 *
 * 新增命令：在此追加一行，其余文件不动。
 */

#include "cmds.h"

struct cmd cmd_table[] = {
    /* 系统 */
    { "help",        cmd_help,        "show this help"                           },
    { "clear",       cmd_clear,       "clear screen"                             },
    { "version",     cmd_version,     "print firmware version and build time"    },
    { "reboot",      cmd_reboot,      "software reset"                           },
    /* 文件传输 */
    { "rz",          ymodem_receive,  "rz [path]         receive file via Ymodem"  },
    { "sz",          ymodem_send,     "sz <path>         send file via Ymodem"     },
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
    { "ota confirm", cmd_ota_confirm, "confirm new fw, clear CONFIRM state"      },
    { "ota revert",  cmd_ota_revert,  "revert to previous slot and reboot"       },
    { "ota trigger", cmd_ota_trigger, "set PENDING and reboot to bootloader"     },
    { "ota start",   cmd_ota_start,   "ota start <path> [slot]  flash & reboot"  },
};

const uint16_t cmd_table_size =
    sizeof(cmd_table) / sizeof(cmd_table[0]);

char *auto_complete_words[]         = { NULL };
const uint16_t auto_complete_words_size = 0U;



/**
 * @file  cmds.h
 * @brief 所有命令函数声明及业务宏定义
 */

#ifndef CMDS_H
#define CMDS_H

#include "nr_micro_shell.h"
#include <stdint.h>


#include "ota_state_machine.h"
#ifdef __cplusplus
extern "C" {
#endif


/* OTA 状态值 */
#define OTA_STATE_IDLE          OTA_STATE_BOOT
#define OTA_STATE_PENDING       OTA_STATE_UPGRADING
#define OTA_STATE_CONFIRM       OTA_STATE_VERIFYING


/* ================================================================
 * 设备信息（cmds_ota.c）
 * ================================================================ */
const char *sys_get_version(void);
uint32_t    sys_get_sn     (void);

/* ================================================================
 * 系统命令（cmds_system.c）
 * ================================================================ */
int cmd_help   (uint8_t argc, char **argv);
int cmd_clear  (uint8_t argc, char **argv);
int cmd_reboot (uint8_t argc, char **argv);
int cmd_version(uint8_t argc, char **argv);

/* ================================================================
 * 配置命令（cmds_cfg.c）
 * ================================================================ */
int cmd_cfg_get (uint8_t argc, char **argv);
int cmd_cfg_set (uint8_t argc, char **argv);
int cmd_cfg_dump(uint8_t argc, char **argv);

/* ================================================================
 * OTA 命令（cmds_ota.c）
 * ================================================================ */
int cmd_ota_status (uint8_t argc, char **argv);
int cmd_ota_slot   (uint8_t argc, char **argv);
int cmd_ota_confirm(uint8_t argc, char **argv);
int cmd_ota_revert (uint8_t argc, char **argv);
int cmd_ota_trigger(uint8_t argc, char **argv);
int cmd_ota_start  (uint8_t argc, char **argv);

/* ================================================================
 * WiFi 命令（cmds_wifi.c）
 * ================================================================ */
int wifi_set_ssid(uint8_t argc, char **argv);
int wifi_set_pass(uint8_t argc, char **argv);
int wifi_connect (uint8_t argc, char **argv);
int wifi_status  (uint8_t argc, char **argv);

/* ================================================================
 * Ymodem 命令（cmds_ymodem.c）
 * ================================================================ */
int ymodem_receive(uint8_t argc, char **argv);
int ymodem_send   (uint8_t argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif /* CMDS_H */


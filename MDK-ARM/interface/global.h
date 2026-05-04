#ifndef __GLOBAL_H
#define __GLOBAL_H


#include "main.h"

#define FLASH_BASE_ADDR          0x08000000U
#define FLASH_TOTAL_SIZE         (256U * 1024U)

#define BOOT_START_ADDR          0x08000000U
#define BOOT_SIZE                (36U * 1024U)    /* 原 32KB + 原 PARAM 区 4KB */

#define APP_A_START_ADDR         0x08009000U
#define APP_A_SIZE               (96U * 1024U)

#define UPGRADE_START_ADDR       0x08021000U
#define UPGRADE_SIZE             (120U * 1024U)    /* 原 124KB，末尾 4KB 划给 EEPROM */

#define EEPROM_START_ADDR        0x0803F000U       /* EEPROM 模拟区，占用最后 2 页共 4KB */

#define OTA_FLAG_ADDR            EEPROM_START_ADDR /* 已迁移至 EEPROM 模块管理 */
#define OTA_FLAG_ERASED          0xFFFFFFFFU       /* 32 位，与 EEPROM 数据宽度一致 */
#define OTA_FLAG_UPGRADE_DONE    0x5A5AA5A5U

/* 保留旧名字，兼容现有调用点 */
#define FlashAddress             APP_A_START_ADDR

extern uint8_t buffer[512];
extern volatile uint8_t data_flag;
extern uint32_t rev_len;
extern volatile int rxlen;
extern volatile uint32_t uart_last_rx_tick;
extern volatile uint8_t uart_stream_active;
extern volatile uint8_t ota_upgrade_done;

#endif


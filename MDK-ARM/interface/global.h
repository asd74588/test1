#ifndef __GLOBAL_H
#define __GLOBAL_H


#include "main.h"

#define FLASH_BASE_ADDR          0x08000000U
#define FLASH_TOTAL_SIZE         (256U * 1024U)

#define BOOT_START_ADDR          0x08000000U
#define BOOT_SIZE                (32U * 1024U)

#define PARAM_START_ADDR         0x08008000U
#define PARAM_SIZE               (4U * 1024U)

#define APP_A_START_ADDR         0x08009000U
#define APP_A_SIZE               (96U * 1024U)

#define UPGRADE_START_ADDR       0x08021000U
#define UPGRADE_SIZE             (124U * 1024U)

#define OTA_FLAG_ADDR            PARAM_START_ADDR
#define OTA_FLAG_ERASED          0xFFFFFFFFFFFFFFFFULL
#define OTA_FLAG_UPGRADE_DONE    0x5A5AA5A5A55A5AA5ULL

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


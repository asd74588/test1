#ifndef __GLOBAL_H
#define __GLOBAL_H

#include "main.h"
#include "lfs.h"
#include "lfs_config.h"
#define FLASH_BASE_ADDR  0x08000000U
#define FLASH_TOTAL_SIZE (256U * 1024U)

#define BOOT_START_ADDR 0x08000000U
#define BOOT_SIZE       (96U * 1024U)

#define APP_A_START_ADDR 0x08018000U
#define APP_A_SIZE       (78U * 1024U)

#define APP_B_START_ADDR 0x0802B800U
#define APP_B_SIZE       (78U * 1024U)

#define EEPROM_START_ADDR 0x0803F000U /* EEPROM 模拟区，占用最后 2 页共 4KB */

typedef struct
{
    lfs_t      lfs;
    lfs_file_t file;
    uint8_t    mounted;   // 1=已挂载
    uint8_t    file_open; // 1=句柄已打开
} lfs_ctx_t;

#define FW_MAGIC 0xAABBCCDD

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t crc32;
    uint8_t  sha256[32];
    uint8_t  signature[64]; // R(32) + S(32)，固定长度
    uint8_t  reserved[144];
} Firmware_Header_t;

#endif

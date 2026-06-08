#ifndef __GLOBAL_H
#define __GLOBAL_H

#include "main.h"
#include "lfs.h"
#include "lfs_config.h"
#define FLASH_BASE_ADDR          0x08000000U
#define FLASH_TOTAL_SIZE         (256U * 1024U)

#define BOOT_START_ADDR          0x08000000U
#define BOOT_SIZE                (48U * 1024U)    /* 原 32KB + 原 PARAM 区 4KB */

#define APP_A_START_ADDR         0x0800C000U      /* A区：原 APP 区，搬运后执行 */
#define APP_A_SIZE               (80U * 1024U)

#define APP_B_START_ADDR         0x08020000U      /* B区：原 UPGRADE 区，直接执行，不搬运 */
#define APP_B_SIZE               (124U * 1024U)   /* 原 124KB，末尾 4KB 划给 EEPROM */

#define EEPROM_START_ADDR        0x0803F000U       /* EEPROM 模拟区，占用最后 2 页共 4KB */


#ifdef Debug
#define dbg_printf(format,args...) printf(format, ##args)
#else
#define dbg_printf(format,args...) do{}while(0)
#endif

typedef struct {
    lfs_t        lfs;
    lfs_file_t   file;
    uint8_t      mounted;       // 1=已挂载
    uint8_t      file_open;     // 1=句柄已打开
}lfs_ctx_t;


#define FW_MAGIC 0xAABBCCDD

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t crc32;
    uint8_t  sha256[32];
    uint8_t  signature[64];   // R(32) + S(32)，固定长度
    uint8_t  reserved[144];
} Firmware_Header_t;


#endif


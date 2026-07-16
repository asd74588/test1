#ifndef __FLASH_BOOTLOADER_H
#define __FLASH_BOOTLOADER_H

#include "stm32l4xx_hal.h"

#include "main.h"
#include "global.h"
#include "usart.h"
#include "eeprom_emul.h"
#include "spi.h"

HAL_StatusTypeDef Write_Flag(uint16_t virt_addr, uint32_t flag_value);

uint32_t Read_Flag(uint16_t virt_addr);


//将bin文件写到flash之前的进行的初始化:擦除指定分区
//slot: SLOT_A 擦除APP_A区, SLOT_B 擦除APP_B区
int Erase_App_Flash(uint8_t slot);

//指定flash的起始位置，将buffer中的数据依次写入。
HAL_StatusTypeDef Write_Buffer_To_Flash(uint32_t* startaddr, const uint8_t *buffer, uint32_t size);

//传输结束后将不足8字节的尾包补0xFF并写入
HAL_StatusTypeDef Flush_Tail_To_Flash(uint32_t* startaddr);

//校验APP区是否有有效固件
int Verify_APP_Integrity_Flash(uint32_t appaddr);

//跳转到指定位置执行烧录的bin文件
int Jump_To_App_Flash(void *resource_ctx, uint32_t appaddr);

typedef enum {
    BOOTLOADER_LOAD_OK             = 0,
    BOOTLOADER_LOAD_ERR_SLOT       = -1,
    BOOTLOADER_LOAD_ERR_READ       = -2,
    BOOTLOADER_LOAD_ERR_PARSE      = -3,
    BOOTLOADER_LOAD_ERR_RELOCATE   = -4,
    BOOTLOADER_LOAD_ERR_ENTRY      = -5,
    BOOTLOADER_LOAD_ERR_FLASH      = -6,
    BOOTLOADER_LOAD_ERR_APP_INVALID = -7,
} bootloader_load_status_t;

//解析并写入目标分区；成功返回前保证目标分区具备合法向量表
bootloader_load_status_t bootloader_load_target(uint8_t target_slot);


#endif


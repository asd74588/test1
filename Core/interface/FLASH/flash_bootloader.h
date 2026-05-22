#ifndef __FLASH_BOOTLOADER_H
#define __FLASH_BOOTLOADER_H

#include "stm32l4xx_hal.h"

#include "main.h"
#include "global.h"
#include "usart.h"
#include "eeprom_emul.h"

HAL_StatusTypeDef Write_Flag(uint16_t virt_addr, uint32_t flag_value);

uint32_t Read_Flag(uint16_t virt_addr);


//将bin文件写到flash之前的进行的初始化:擦除指定分区
//slot: SLOT_A 擦除APP_A区, SLOT_B 擦除APP_B区
void Erase_App_Flash(uint8_t slot);

//指定flash的起始位置，将buffer中的数据依次写入。
HAL_StatusTypeDef Write_Buffer_To_Flash(uint32_t* startaddr, char *buffer);

//传输结束后将不足8字节的尾包补0xFF并写入
HAL_StatusTypeDef Flush_Tail_To_Flash(uint32_t* startaddr);

//校验APP区是否有有效固件
int Verify_APP_Integrity_Flash(uint32_t appaddr);

//跳转到指定位置执行烧录的bin文件
int Jump_To_App_Flash(uint32_t appaddr);

#endif


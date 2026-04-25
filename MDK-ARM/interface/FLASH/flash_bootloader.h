#ifndef __FLASH_BOOTLOADER_H
#define __FLASH_BOOTLOADER_H

#include "stm32l4xx_hal.h"

#include "main.h"
#include "global.h"
#include "usart.h"
//将bin文件写到flash之前的进行的初始化
void Init_Flash(void);

//指定flash的起始位置，将buffer中的数据依次写入。
HAL_StatusTypeDef Write_Buffer_To_Flash(uint32_t* startaddr);

//传输结束后将不足8字节的尾包补0xFF并写入
HAL_StatusTypeDef Flush_Tail_To_Flash(uint32_t* startaddr);

//跳转到指定位置执行烧录的bin文件
int Jump_To_App_Flash(uint32_t appaddr);


#endif


#ifndef __UART_BOOTLOADER_H
#define __UART_BOOTLOADER_H

#include "usart.h"

#include "global.h"

#include "flash_bootloader.h"

//
void Init_Uart(void);

// 清空硬件与软件缓冲区（中止传输/接收、清空寄存器、清零软件缓冲）
void UART_FlushBuffers(UART_HandleTypeDef *huart);

#endif

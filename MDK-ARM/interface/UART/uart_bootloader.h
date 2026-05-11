#ifndef __UART_BOOTLOADER_H
#define __UART_BOOTLOADER_H

#include "usart.h"

#include "global.h"

#include "flash_bootloader.h"

#define Xmodem_Start_Byte 'C'
#define Xmodem_SOH 0x01
#define XMODEM_STX 0x02
#define Xmodem_EOT 0x04
#define Xmodem_ACK 0x06
#define Xmodem_NAK 0x15
#define Xmodem_CAN 0x18


//接收串口发送的bin文件内容到缓冲区中
void Init_Uart(void);

int Xmodem_Start_Transfer(uint32_t startadddr);

#endif


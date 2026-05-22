#ifndef XMODEM_H
#define XMODEM_H

#include "uart_bootloader.h"

#define Xmodem_Start_Byte 'C'
#define Xmodem_SOH 0x01
#define XMODEM_STX 0x02
#define Xmodem_EOT 0x04
#define Xmodem_ACK 0x06
#define Xmodem_NAK 0x15
#define Xmodem_CAN 0x18


int Xmodem_Start_Transfer(uint32_t startadddr);

#endif 

#ifndef SHELL_INIT_H
#define SHELL_INIT_H
#include <stdint.h>
#include "main.h"
#ifdef __cplusplus
extern "C" {
#endif
void Shell_Init(void);
void Shell_Process(void);
void Shell_UartRxCpltCallback(UART_HandleTypeDef *huart);
void Shell_UartErrorCallback(UART_HandleTypeDef *huart);
#ifdef __cplusplus
}
#endif
#endif

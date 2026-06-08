/**
 * @file    shell_init.c
 * @brief   Shell 初始化与 UART 中断接收集成
 *
 * 使用：
 *   main() 里调用 Shell_Init()
 *   HAL_UART_RxCpltCallback() 里调用 Shell_RxCallback(byte)
 *
 * 若项目已有 HAL_UART_RxCpltCallback，请删除本文件末尾的弱符号覆盖，
 * 手动在已有回调里加入 Shell_RxCallback() 调用。
 */

#include "shell_init.h"
#include "nr_micro_shell.h"

extern UART_HandleTypeDef huart1;   /* 与 port.h 保持一致 */

static uint8_t s_rx_byte;

void Shell_Init(void)
{
    shell_init();
    shell_printf("\r\n=== Device Shell (nr_micro_shell v2.0.0) ===\r\n");
    shell_printf("Type 'help' for available commands.\r\n\r\n");
    HAL_UART_Receive_IT(&huart1, &s_rx_byte, 1U);
}

void Shell_RxCallback(uint8_t c)
{
    shell((char)c);
    HAL_UART_Receive_IT(&huart1, &s_rx_byte, 1U);
}

/* 覆盖 HAL 弱符号 — 如项目已有此函数请删除下方代码 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == huart1.Instance) {
        Shell_RxCallback(s_rx_byte);
    }
}



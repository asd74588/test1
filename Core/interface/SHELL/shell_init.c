/**
 * @file    shell_init.c
 * @brief   Shell 初始化与 UART 中断接收集成
 *
 * ISR 只负责把字节压入 ring buffer，shell 处理在主循环完成，
 * 避免在 ISR 中执行阻塞式 HAL_UART_Transmit 导致丢字节 / ORE 卡死。
 *
 * 使用：
 *   main() 里调用 Shell_Init()
 *   主循环里调用 Shell_Process()
 */

#include "shell_init.h"
#include "nr_micro_shell.h"

extern UART_HandleTypeDef huart1;

/* ---- ring buffer ---- */
#define SHELL_RX_BUF_SIZE 32

static volatile uint8_t  s_rx_buf[SHELL_RX_BUF_SIZE];
static volatile uint16_t s_rx_head = 0; /* ISR 写入位置 */
static volatile uint16_t s_rx_tail = 0; /* 主循环读取位置 */
static uint8_t           s_rx_byte;     /* HAL 单字节接收缓存 */

void Shell_Init(void)
{
    shell_init();
    shell_printf("\r\n=== Device Shell (nr_micro_shell v2.0.0) ===\r\n");
    shell_printf("Type 'help' for available commands.\r\n\r\n");
    HAL_UART_Receive_IT(&huart1, &s_rx_byte, 1U);
}

/**
 * @brief  主循环调用，从 ring buffer 取字节交给 shell 处理
 */
void Shell_Process(void)
{
    while (s_rx_tail != s_rx_head)
    {
        uint8_t c = s_rx_buf[s_rx_tail];
        s_rx_tail = (s_rx_tail + 1U) % SHELL_RX_BUF_SIZE;
        shell((char)c);
    }
}

void Shell_UartRxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == huart1.Instance)
    {
        uint16_t next = (s_rx_head + 1U) % SHELL_RX_BUF_SIZE;
        if (next != s_rx_tail)
        {
            s_rx_buf[s_rx_head] = s_rx_byte;
            s_rx_head           = next;
        }
        HAL_UART_Receive_IT(&huart1, &s_rx_byte, 1U);
    }
}

/**
 * @brief  UART 错误回调（ORE / FE / NE）
 *
 * STM32 HAL 在发生 ORE 等错误后不会自动重开接收中断，
 * 必须在此回调中清理错误标志并重新启动接收，否则 shell 卡死。
 */
void Shell_UartErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == huart1.Instance)
    {
        __HAL_UART_CLEAR_OREFLAG(huart);
        huart->RxState = HAL_UART_STATE_READY;
        HAL_UART_Receive_IT(&huart1, &s_rx_byte, 1U);
    }
}

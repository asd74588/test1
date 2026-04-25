#include "uart_bootloader.h"

uint8_t buffer[512];
volatile uint8_t data_flag = 0;
volatile int rxlen = 0;
volatile uint32_t uart_last_rx_tick = 0;
volatile uint8_t uart_stream_active = 0;


void Init_Uart()
{
    memset(buffer, 0, sizeof(buffer));
    data_flag = 0;
    rxlen = 0;
    uart_last_rx_tick = 0;
    uart_stream_active = 0;
    //阻塞式接收
    //HAL_UARTEx_ReceiveToIdle(&huart1,buffer, 512, rxlen, 0xFFFFFFFF);

    //调用中断实现更加灵活的接收
    //HAL_UARTEx_ReceiveToIdle_IT(&huart1, buffer, 512);

    //再加入DMA，降低CPU的负荷，同时需要在回调函数中重新开启DMA接收，保持持续接收能力，
    //而且需要关闭半满中断，避免不必要的中断触发。
    HAL_UARTEx_ReceiveToIdle_DMA(&huart1, buffer, 512);
    __HAL_DMA_DISABLE_IT(&hdma_usart1_rx, DMA_IT_HT);
    
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if(huart == &huart1)
    {
        ota_upgrade_done = 0;
        data_flag = 1;
        rxlen = Size;
        uart_last_rx_tick = HAL_GetTick();
        uart_stream_active = 1;

        // 继续开启下一次DMA接收，保持串口持续接收能力，关闭半满中断，避免不必要的中断触发。
        HAL_UARTEx_ReceiveToIdle_DMA(&huart1, buffer, 512);
        __HAL_DMA_DISABLE_IT(&hdma_usart1_rx, DMA_IT_HT);
    }
}



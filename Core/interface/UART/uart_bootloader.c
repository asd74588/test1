#include "uart_bootloader.h"

void Init_Uart()
{
   
    //阻塞式接收
    //HAL_UARTEx_ReceiveToIdle(&huart1,buffer, 512, rxlen, 0xFFFFFFFF);

    //调用中断实现更加灵活的接收
    //HAL_UARTEx_ReceiveToIdle_IT(&huart1, buffer, 512);

    //再加入DMA，降低CPU的负荷，同时需要在回调函数中重新开启DMA接收，保持持续接收能力，
    //而且需要关闭半满中断，避免不必要的中断触发。
    // HAL_UARTEx_ReceiveToIdle_DMA(&huart1, buffer, 512);
    // __HAL_DMA_DISABLE_IT(&hdma_usart1_rx, DMA_IT_HT);
    

    return ;
}

// 清空硬件与软件缓冲区（中止传输/接收、清空寄存器、清零软件缓冲）
void UART_FlushBuffers(UART_HandleTypeDef *huart)
{
    // 中止 HAL 层的正在进行的发送/接收
    HAL_UART_AbortTransmit(huart);
    HAL_UART_AbortReceive(huart);

    // 如果使用 DMA，需要禁用相关 DMA 通道（如声明了 hdma_usart1_rx/tx）
#ifdef hdma_usart1_rx
    __HAL_DMA_DISABLE(&hdma_usart1_rx);
#endif
#ifdef hdma_usart1_tx
    __HAL_DMA_DISABLE(&hdma_usart1_tx);
#endif

    // 清掉外设数据寄存器残留（读出 RDR）
    __HAL_UART_FLUSH_DRREGISTER(huart);

}

// void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
// {
//     if(huart == &huart1)
//     {
//         ota_upgrade_done = 0;
//         data_flag = 1;
//         rxlen = Size;
//         uart_last_rx_tick = HAL_GetTick();
//         uart_stream_active = 1;

//         // 继续开启下一次DMA接收，保持串口持续接收能力，关闭半满中断，避免不必要的中断触发。
//         HAL_UARTEx_ReceiveToIdle_DMA(&huart1, buffer, 512);
//         __HAL_DMA_DISABLE_IT(&hdma_usart1_rx, DMA_IT_HT);
//     }
// }





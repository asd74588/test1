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
    // HAL_UARTEx_ReceiveToIdle_DMA(&huart1, buffer, 512);
    // __HAL_DMA_DISABLE_IT(&hdma_usart1_rx, DMA_IT_HT);
    
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


//CRC16-CCITT校验，校验成功返回1，失败返回0
//pkt格式: [SOH][block#][~block#][128字节数据][CRC_H][CRC_L]
int Xmodem_Crc_Check(char *buf, int len)
{
    uint16_t crc = 0;
    // 对128字节数据部分计算CRC16-CCITT (多项式0x1021)
    for (int i = 3; i < 131; i++)
    {
        crc ^= ((uint16_t)(uint8_t)buf[i] << 8);
        for (int j = 0; j < 8; j++)
        {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    // 接收到的CRC在pkt[131]和pkt[132]，大端序
    uint16_t recv_crc = ((uint8_t)buf[131] << 8) | (uint8_t)buf[132];
    return (crc == recv_crc) ? 1 : 0;
}



//接收方发送启动包，启动Xmodem传输，返回接收总字节数，失败返回-1
int Xmodem_Start_Transfer(uint32_t startadddr)
{
    uint8_t start_byte = Xmodem_Start_Byte;
    uint8_t ack = Xmodem_ACK;
    uint8_t nak = Xmodem_NAK;
    uint8_t expected = 1;  // Xmodem块号从1开始，255后回绕到0
    int total_received = 0;
    uint8_t pkt[133];

    char prev_buf[128];    // 前一个包的数据缓冲（看前一个包策略）
    int has_prev = 0;      // 是否已缓冲了前一个包
    int retries;

    // 阶段1：发送'C'启动传输，等待发送方回应SOH
    retries = 3;
    while(retries--)
    {
        HAL_UART_Transmit(&huart1, &start_byte, 1, 0xFFFF);
        if(HAL_UART_Receive(&huart1, pkt, 1, 3000) == HAL_OK)
        {
            if(pkt[0] == Xmodem_SOH)
            {
                // 收到SOH，接收剩余132字节(块号+补码+128数据+2CRC)
                // 超时500ms：115200baud下132字节约12ms，留足余量
                if(HAL_UART_Receive(&huart1, &pkt[1], 132, 500) == HAL_OK)
                    break;
                else
                {
                    HAL_UART_AbortReceive(&huart1); // 清除接收状态和RDR残留数据
                    HAL_UART_Transmit(&huart1, &nak, 1, 0xFFFF); // 通知发送方重传
                }
            }
            else if(pkt[0] == Xmodem_EOT)
            {
                HAL_UART_Transmit(&huart1, &ack, 1, 0xFFFF);
                return 0;  // 空传输
            }
        }
    }

    if(retries < 0)
    {
        printf("Failed to start Xmodem transfer.\r\n");
        return -1;
    }

    // 阶段2：循环接收数据包（看前一个包策略：先缓冲，确认不是最后包再写入）
    while(1)
    {
        // 校验块号和补码
        if(pkt[1] != expected || (uint8_t)(pkt[1] + pkt[2]) != 0xFF)
        {
            // 判断是否为重复包（ACK丢失导致发送方重传上一个包）
            if(pkt[1] == (uint8_t)(expected - 1) && (uint8_t)(pkt[1] + pkt[2]) == 0xFF)
            {
                // 补码校验通过，说明确实是上一个包的重传
                // 不写Flash、不改prev_buf、不递增expected，只补发ACK
                HAL_UART_Transmit(&huart1, &ack, 1, 0xFFFF);
            }
            else
            {
                // 真正的校验失败
                HAL_UART_Transmit(&huart1, &nak, 1, 0xFFFF);
            }
        }
        // CRC校验
        else if(Xmodem_Crc_Check((char *)pkt, 133) == 0)
        {
            HAL_UART_Transmit(&huart1, &nak, 1, 0xFFFF);
        }
        else
        {
            // 校验通过：先把前一个缓冲包写入Flash（它不是最后包，128字节全是有效数据）
            if(has_prev)
            {
                Write_Buffer_To_Flash(&startadddr, prev_buf);
                total_received += 128;
            }
            // 当前包先缓冲，等确认不是最后包再写
            memcpy(prev_buf, &pkt[3], 128);
            has_prev = 1;
            HAL_UART_Transmit(&huart1, &ack, 1, 0xFFFF);
            expected++;  // uint8_t自动回绕：255→0
        }

        // 等待下一个包（含重试机制）
        memset(pkt, 0, sizeof(pkt));
        retries = 10;
        while(retries--)
        {
            if(HAL_UART_Receive(&huart1, pkt, 1, 5000) != HAL_OK)
            {
                HAL_UART_Transmit(&huart1, &nak, 1, 0xFFFF);
                continue;
            }

            if(pkt[0] == Xmodem_EOT)
            {
                HAL_UART_Transmit(&huart1, &ack, 1, 0xFFFF);
                // EOT表示传输结束，当前缓冲的就是最后一个包
                if(has_prev)
                {
                    // 剥离Xmodem填充的0x1A，找到有效数据末尾
                    int valid_len = 128;
                    while(valid_len > 0 && (uint8_t)prev_buf[valid_len - 1] == 0x1A)
                    {
                        valid_len--;
                    }
                    if(valid_len > 0)
                    {
                        // 有效数据后的位置补0xFF（Flash擦除状态），凑满128字节对齐写入
                        memset(&prev_buf[valid_len], 0xFF, 128 - valid_len);
                        Write_Buffer_To_Flash(&startadddr, prev_buf);
                        total_received += valid_len;
                    }
                }

                // 确保发送方收到ACK：短暂等待，如果收到重发的EOT就再ACK一次
                uint8_t tmp;
                if(HAL_UART_Receive(&huart1, &tmp, 1, 1000) == HAL_OK && tmp == Xmodem_EOT)
                {
                    HAL_UART_Transmit(&huart1, &ack, 1, 0xFFFF);
                }
                
                printf("Transfer complete, %d bytes written.\r\n", total_received);
                return total_received;
            }
            else if(pkt[0] == Xmodem_CAN)
            {
                // 标准要求检测两个连续CAN字节，防止单字节干扰误判取消
                uint8_t second_byte;
                if(HAL_UART_Receive(&huart1, &second_byte, 1, 1000) == HAL_OK && second_byte == Xmodem_CAN)
                {
                    uint8_t can = Xmodem_CAN;
                    HAL_UART_Transmit(&huart1, &can, 1, 0xFFFF);
                    HAL_UART_Transmit(&huart1, &can, 1, 0xFFFF);
                    printf("Transfer cancelled by sender.\r\n");
                    return -1;
                }
                // 单个CAN可能是线路干扰，忽略继续重试
                continue;
            }
            else if(pkt[0] == Xmodem_SOH)
            {
                // 超时500ms：115200baud下132字节约12ms，留足余量
                if(HAL_UART_Receive(&huart1, &pkt[1], 132, 500) == HAL_OK)
                    break;  // 收到完整包，回到外层循环处理
                else
                {
                    HAL_UART_AbortReceive(&huart1); // 清除接收状态和RDR残留数据
                    HAL_UART_Transmit(&huart1, &nak, 1, 0xFFFF); // 通知发送方重传
                }
            }
        }

        if(retries < 0)
        {
            printf("Timeout waiting for packet.\r\n");
            return -1;
        }
    }

}




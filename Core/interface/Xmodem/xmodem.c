#include "xmodem.h"




//CRC16-CCITT校验，校验成功返回1，失败返回0
//pkt格式: [SOH][block#][~block#][128字节数据][CRC_H][CRC_L]
static int Xmodem_Crc_Check(char *buf, int len)
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

//握手阶段：只负责发送'C'并等待发送方返回SOH
//返回值：1=收到SOH，-1=握手失败
static int Xmodem_Handshake(uint8_t *start_byte, uint8_t *first_byte)
{
    int retries = 3;

    while(retries--)
    {
        HAL_UART_Transmit(&huart1, start_byte, 1, 0xFFFF);
        if(HAL_UART_Receive(&huart1, first_byte, 1, 5000) == HAL_OK)
        {
            if(first_byte[0] == Xmodem_SOH)
            {
                return 1;
            }
        }
    }

    return -1;
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
    uint8_t tmp;


    UART_FlushBuffers(&huart1);  // 确保接收缓冲区干净，避免残留数据干扰握手和后续接收

    // 阶段1：握手，仅负责发送'C'并拿到首包控制字节
    retries = Xmodem_Handshake(&start_byte, pkt);
    if(retries < 0)
    {
        printf("Failed to start Xmodem transfer.\r\n");
        return -1;
    }

    // 收到SOH后，再进入完整包接收阶段
    if(HAL_UART_Receive(&huart1, &pkt[1], 132, 5000) != HAL_OK)
    {
        UART_FlushBuffers(&huart1);
        HAL_UART_Transmit(&huart1, &nak, 1, 0xFFFF);
        printf("Timeout waiting for first packet.\r\n");
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
            if(HAL_UART_Receive(&huart1, pkt, 1, 500) != HAL_OK)
            {
                UART_FlushBuffers(&huart1);
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
                    UART_FlushBuffers(&huart1);
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



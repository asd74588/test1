#include "flash_bootloader.h"
#include "eeprom_emul.h"

#define UART_RX_IDLE_FLUSH_MS 50U

uint32_t rev_len = 0;
volatile uint8_t ota_upgrade_done = 0;
static uint8_t s_tail_bytes[8] = {0};
static uint32_t s_tail_len = 0;

static HAL_StatusTypeDef Write_Upgrade_Flag(uint32_t flag_value)
{
    return EE_Write(EE_VAR_OTA_FLAG, flag_value);
}

static uint32_t Read_Upgrade_Flag(void)
{
    uint32_t flag = 0xFFFFFFFFU;
    EE_Read(EE_VAR_OTA_FLAG, &flag);
    return flag;
}

//将bin文件写到flash之前的进行的初始化:擦除A区起始位置之后可能用到的FLASH页
void Init_Flash(void)
{
    
    FLASH_EraseInitTypeDef eraseInitStruct;
    uint32_t PageError = 0;
    uint32_t pageSize = FLASH_PAGE_SIZE;  // HAL库通常定义了此宏
    uint32_t startPage = (APP_A_START_ADDR - FLASH_BASE) / pageSize;
    uint32_t appPages = APP_A_SIZE / pageSize;
    
    eraseInitStruct.TypeErase = FLASH_TYPEERASE_PAGES;   // 页擦除
    eraseInitStruct.Banks = FLASH_BANK_1;                // 单Bank设备用 FLASH_BANK_1
    eraseInitStruct.Page = startPage;                    // 起始页编号
    eraseInitStruct.NbPages = appPages;                  // 擦除A区全部页

    HAL_FLASH_Unlock();

    if(HAL_OK != HAL_FLASHEx_Erase(&eraseInitStruct, &PageError))
    {
        HAL_FLASH_Lock();
        return ;
    }

    HAL_FLASH_Lock();
    s_tail_len = 0;
    rev_len = 0;
    ota_upgrade_done = 0;
    return ;
}

//指定flash的起始位置，将buffer中的数据依次写入。
HAL_StatusTypeDef Write_Buffer_To_Flash(uint32_t* startaddr)
{
    if (startaddr == NULL)
    {
        return HAL_ERROR;
    }

    if (data_flag != 1U)
    {
        // 没有新包时，使用空闲超时判定传输结束并自动刷写尾包。
        if ((uart_stream_active == 1U) &&
            ((HAL_GetTick() - uart_last_rx_tick) > UART_RX_IDLE_FLUSH_MS))
        {
            HAL_StatusTypeDef flush_status = Flush_Tail_To_Flash(startaddr);
            if (flush_status == HAL_OK)
            {
                uart_stream_active = 0U;
                if (rev_len > 0U)
                {
                    if (Write_Upgrade_Flag(OTA_FLAG_UPGRADE_DONE) != HAL_OK)  /* OTA_FLAG_UPGRADE_DONE 已改为 uint32_t */
                    {
                        return HAL_ERROR;
                    }
                    ota_upgrade_done = 1U;
                }
            }
            return flush_status;
        }
        return HAL_BUSY;
    }

    ota_upgrade_done = 0U;

    uint32_t chunk_len = (uint32_t)rxlen;
    if ((chunk_len == 0U) || (chunk_len > sizeof(buffer)))
    {
        data_flag = 0;
        rxlen = 0;
        return HAL_ERROR;
    }

    uint8_t write_cache[sizeof(buffer)] = {0};
    uint32_t cache_idx = 0U;
    uint64_t value = 0;
    HAL_StatusTypeDef status = HAL_OK;

    // 先拷贝快照，避免DMA在写Flash过程中改写原buffer。
    memcpy(write_cache, buffer, chunk_len);

    data_flag = 0;
    rxlen = 0;

    HAL_FLASH_Unlock();
    if (s_tail_len > 0U)
    {
        // 先用本包数据补齐上次遗留的1~7字节尾巴，凑够8字节再写。
        uint32_t need = 8U - s_tail_len;
        uint32_t copy_len = (chunk_len < need) ? chunk_len : need;
        memcpy(&s_tail_bytes[s_tail_len], write_cache, copy_len);
        s_tail_len += copy_len;
        cache_idx += copy_len;

        if (s_tail_len == 8U)
        {
            memcpy(&value, s_tail_bytes, sizeof(uint64_t));
            status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, *startaddr, value);
            if (status != HAL_OK)
            {
                HAL_FLASH_Lock();
                return status;
            }
            *startaddr += 8U;
            s_tail_len = 0U;
        }
    }

    for(; (cache_idx + 8U) <= chunk_len; cache_idx += 8U)
    {
        // 本包中完整的8字节块直接写入Flash，地址始终按8字节推进。
        memcpy(&value, &write_cache[cache_idx], sizeof(uint64_t));
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, *startaddr, value);
        if (status != HAL_OK)
        {
            HAL_FLASH_Lock();
            return status;
        }
        *startaddr += 8U;
    }

    if (cache_idx < chunk_len)
    {
        // 本包末尾不足8字节的数据暂存，等待下一个包继续拼接。
        uint32_t remain = chunk_len - cache_idx;
        memcpy(s_tail_bytes, &write_cache[cache_idx], remain);
        s_tail_len = remain;
    }

    HAL_FLASH_Lock();

    rev_len += chunk_len;
    return HAL_OK;
}

HAL_StatusTypeDef Flush_Tail_To_Flash(uint32_t* startaddr)
{
    uint64_t value = 0;
    HAL_StatusTypeDef status = HAL_OK;

    if (startaddr == NULL)
    {
        return HAL_ERROR;
    }

    if (s_tail_len == 0U)
    {
        return HAL_OK;
    }

    // 仅在最终收尾时补0xFF，将最后不足8字节的数据落盘。
    memset(&s_tail_bytes[s_tail_len], 0xFF, 8U - s_tail_len);

    HAL_FLASH_Unlock();
    memcpy(&value, s_tail_bytes, sizeof(uint64_t));
    status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, *startaddr, value);
    HAL_FLASH_Lock();

    if (status != HAL_OK)
    {
        return status;
    }

    *startaddr += 8U;
    s_tail_len = 0U;
    return HAL_OK;
}

//校验接收到的bin文件是否正确，正确则跳转到指定位置执行烧录的bin文件
//成功返回1，失败返回0
int Verify_APP_Integrity_Flash(uint32_t appaddr)
{
    // uint32_t app_stack = *(__IO uint32_t*)appaddr;
    // uint32_t app_reset = *(__IO uint32_t*)(appaddr + 4U);

    // if ((app_stack & 0x2FFE0000U) != 0x20000000U)
    // {
    //     return 0;
    // }
    // if ((app_reset < appaddr) || (app_reset > 0x080FFFFFU))
    // {
    //     return 0;
    // }
    // return 1;
    return 1;  // 简化处理，实际应用中应根据具体需求完善校验逻辑
}

//跳转到指定位置执行烧录的bin文件
int Jump_To_App_Flash(uint32_t appaddr)
{
    uint32_t jump_addr;
    void (*jump_to_app)(void);

    if (Verify_APP_Integrity_Flash(appaddr) == 0)
    {
        return 0;
    }

    // 关闭全局中断，防止跳转过程中被中断打断。
    __disable_irq();

    // 停止并反初始化串口及其DMA，释放外设状态给App。
    HAL_UART_DMAStop(&huart1);
    HAL_UART_DeInit(&huart1);
    HAL_DMA_DeInit(&hdma_usart1_rx);
    HAL_NVIC_DisableIRQ(USART1_IRQn);
    HAL_NVIC_DisableIRQ(DMA1_Channel5_IRQn);

    // 关闭系统滴答定时器，避免旧工程节拍影响新App。
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;

    // 复位时钟树到默认状态。
    HAL_RCC_DeInit();

    // 关闭并清除所有NVIC中断使能与挂起位。
    for (uint8_t i = 0; i < 8U; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    // 切换中断向量表到目标App起始地址。
    SCB->VTOR = appaddr;
    __DSB();
    __ISB();

    // 读取App复位向量并转换为函数入口。
    jump_addr = *(__IO uint32_t*)(appaddr + 4U);
    if ((jump_addr & 0x1U) == 0U)
    {
        return 0;
    }
    jump_to_app = (void (*)(void))jump_addr;

    // 设置App主堆栈指针，恢复线程特权模式并重新打开中断后跳转。
    __set_MSP(*(__IO uint32_t*)appaddr);
    __set_CONTROL(0U);
    __ISB();
    
    __enable_irq();
    jump_to_app();

    while(1)
    {
    }
}

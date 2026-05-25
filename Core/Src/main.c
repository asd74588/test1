/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include "stdio.h"
#include "global.h"
#include "uart_bootloader.h"
#include "flash_bootloader.h"
#include "w25qxx.h"
#include "eeprom_emul.h"
#include "xmodem.h"
#include "../interface/Littlefs/lfs.h"
#include "lfs_config.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define XMODEM_MAX_RETRY  3
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
#define HEAP_WATERMARK 0xCD
extern char __heap_base[];
extern char __heap_limit[];

void Heap_InitWatermark(void) {
    uint8_t *p = (uint8_t *)__heap_base;
    uint8_t *end = (uint8_t *)__heap_limit;
    while (p < end) *p++ = HEAP_WATERMARK;
}

// 🔑 全堆扫描版：无视碎片，统计所有非 0xCD 字节
uint32_t Heap_GetUsage(uint32_t *p_used, uint32_t *p_percent, uint32_t *p_total) {
    // volatile 防止编译器优化掉内存读取
    volatile uint8_t *base  = (volatile uint8_t *)__heap_base;
    volatile uint8_t *limit = (volatile uint8_t *)__heap_limit;
    
    uint32_t total = (uint32_t)(limit - base);
    uint32_t free_bytes = 0;
    
    // 遍历整个堆区，统计未被覆盖的水印字节
    for (volatile uint8_t *p = base; p < limit; p++) {
        if (*p == HEAP_WATERMARK) {
            free_bytes++;
        }
    }
    
    uint32_t used = total - free_bytes;
    
    if (p_used)   *p_used   = used;
    if (p_total)  *p_total  = total;
    if (p_percent && total > 0) {
        *p_percent = (used * 100UL + total/2) / total; // 四舍五入
    }
    return used;
}
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
int is_heap_available(size_t size) {
    void *ptr = malloc(size);
    if (ptr != NULL) {
        free(ptr);
        return 1;  // 有空间
    }
    return 0;      // 空间不足
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  Heap_InitWatermark();

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_SPI1_Init();
  MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */
uint32_t p_max_used;
uint32_t p_percent;
uint32_t p_total;

  // 🔍 打印堆边界验证符号解析
    printf("=== Heap Debug ===\r\n");
    printf("Base: 0x%08X, Limit: 0x%08X\r\n", 
           (uint32_t)__heap_base, (uint32_t)__heap_limit);
    printf("Total: %lu bytes\r\n", (uint32_t)(__heap_limit - __heap_base));
    
    // 🔍 验证水印是否写入成功（打印前 16 字节）
    printf("Watermark check: ");
    for (int i = 0; i < 16; i++) {
        printf("%02X ", ((uint8_t *)__heap_base)[i]);
    }
    printf("\r\n");  // 期望输出: CD CD CD CD ...

  EE_Init();

  void *r_buf = malloc(LFS_CACHE_SIZE);
  void *p_buf = malloc(LFS_CACHE_SIZE);
  void *l_buf = malloc(LFS_LOOKAHEAD_SIZE);

  printf("read_buf  : %p (usable: )\n", r_buf);
  printf("prog_buf  : %p (usable: )\n", p_buf);
  printf("look_buf  : %p (usable: )\n", l_buf);

  // 计算实际物理跨度
  uint32_t span = (uint32_t)l_buf - (uint32_t)r_buf + LFS_LOOKAHEAD_SIZE;
  printf("Heap physical span: %lu bytes\n", span);


  Heap_GetUsage(&p_max_used, &p_percent, &p_total); // 预热堆栈，获取初始使用情况
  printf("Initial Heap Usage: %lu bytes (%.2f%%)\r\n", p_max_used, (float)p_percent);
  lfs_t lfs;
  extern const struct lfs_config my_lfs_config;

  //is_heap_available(10) ? printf("Heap is available for 512 bytes.\r\n") : printf("Heap is NOT available for 512 bytes.\r\n");

  if( spinor_init(&spinor) < 0 )
    return 1;

  //is_heap_available(512) ? printf("Heap is available for 512 bytes.\r\n") : printf("Heap is NOT available for 512 bytes.\r\n");

  Heap_GetUsage(&p_max_used, &p_percent, &p_total); // 预热堆栈，获取初始使用情况
  printf("Initial Heap Usage: %lu bytes (%.2f%%)\r\n", p_max_used, (float)p_percent);

  int err = lfs_mount(&lfs, &my_lfs_config);
  printf("lfs_mount: %d\r\n", err);

  Heap_GetUsage(&p_max_used, &p_percent, &p_total ); // 预热堆栈，获取初始使用情况
  printf("Initial Heap Usage: %lu bytes (%.2f%%)\r\n", p_max_used, (float)p_percent);


  if (err) {
    int fmt_err = lfs_format(&lfs, &my_lfs_config);
    printf("lfs_format: %d\r\n", fmt_err);
    if (fmt_err == 0) {
      err = lfs_mount(&lfs, &my_lfs_config);
      printf("lfs_mount after format: %d\r\n", err);
    }
  }

  if (err) {
    printf("LittleFS init failed, stop here.\r\n");
    return 1;
  }

  // 4. 测试文件操作 (读写文件)
  lfs_file_t file;
  static uint8_t file_buffer[512];

  int file_err = lfs_file_open(&lfs, &file, "test.txt", LFS_O_WRONLY | LFS_O_CREAT);
  printf("lfs_file_open(write): %d\r\n", file_err);
  if (file_err == 0) {
    lfs_file_write(&lfs, &file, "Hello LittleFS", 15);
    lfs_file_sync(&lfs, &file);
    lfs_file_close(&lfs, &file);
  }

  char read_buf[20] = {0};
  file_err = lfs_file_open(&lfs, &file, "test.txt", LFS_O_RDONLY);
  printf("lfs_file_open(read): %d\r\n", file_err);
  if (file_err == 0) {
    lfs_ssize_t bytes_read = lfs_file_read(&lfs, &file, read_buf, 15);
    lfs_file_close(&lfs, &file);
    printf("Read from LittleFS: %s (bytes read: %d)\r\n", read_buf, (int)bytes_read);
  }


  return 0;
  /* 在 EE_Init() 后统一声明将要使用的局部变量（避免在循环/块中间再声明） */
  // uint32_t ota_state = 0;
  // uint32_t active_slot = 0;
  // uint32_t app_addr = 0;
  // uint8_t rx_byte = 0;
  // uint32_t start_tick = 0;
  // uint32_t other_slot = 0;
  // uint32_t other_addr = 0;
  // uint32_t target_slot = 0;
  // uint32_t write_addr = 0;
  // uint32_t active_addr = 0;
  // uint8_t active_valid = 0;
  // uint8_t xmodem_retry = 0;
  // int received = 0;
  // uint32_t first_word = 0;
  // uint32_t new_addr = 0;
  // uint32_t old_active = 0;
  // uint32_t reason = 0;

  // /* ========== A/B双区OTA Bootloader 状态机 ========== */
  // ota_state = Read_Flag(EE_VAR_OTA_STATE);
  // active_slot = Read_Flag(EE_VAR_ACTIVE_SLOT);

  // /* 首次上电EEPROM无数据时，EE_Read返回失败，Read_Flag返回0xFFFFFFFF */
  // if (active_slot != SLOT_A && active_slot != SLOT_B)
  // {
  //   active_slot = SLOT_A;
  //   Write_Flag(EE_VAR_ACTIVE_SLOT, SLOT_A);
  //   Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_BOOT);
  //   ota_state = OTA_STATE_BOOT;
  // }




  /*
   * 状态流转：
   *   BOOT ──(升级触发)──→ UPGRADING ──(接收成功)──→ VERIFYING
   *     ↑ ↑                                       │
   *     │ │                                (校验通过)→ 切分区，跳转
   *     │ │                                (校验失败)→ REVERT ←─┐
   *     │ │                                        ↑            │
   *     │ └────────────────────────────────────────┘   (升级回退)
   *     └───(活跃分区无效)──→ REVERT (启动降级)
   */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  // while (1)
  // {
  //   switch (ota_state)
  //   {
  //     case OTA_STATE_BOOT:
  //     {
  //       app_addr = (active_slot == SLOT_B) ? APP_B_START_ADDR : APP_A_START_ADDR;

  //       /* 当前活跃分区无效，转REVERT处理降级启动 */
  //       if (Verify_APP_Integrity_Flash(app_addr) == 0)
  //       {
  //         printf("Slot %s invalid, entering revert for fallback.\r\n",
  //                (active_slot == SLOT_B) ? "B" : "A");
  //         /* 预检查另一分区，设置回退原因，REVERT内零Flash校验 */
  //         other_slot = (active_slot == SLOT_A) ? SLOT_B : SLOT_A;
  //         other_addr = (other_slot == SLOT_B) ? APP_B_START_ADDR : APP_A_START_ADDR;
  //         if (Verify_APP_Integrity_Flash(other_addr) != 0)
  //         {
  //           Write_Flag(EE_VAR_REVERT_REASON, REVERT_OTHER_VALID);
  //         }
  //         else
  //         {
  //           Write_Flag(EE_VAR_REVERT_REASON, REVERT_BOTH_INVALID);
  //         }
  //         ota_state = OTA_STATE_REVERT;
  //         Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_REVERT);
  //         continue;
  //       }

  //       /* APP有效：3秒等待窗口，收到'U'进升级，否则跳转 */
  //       printf("Press 'U' within 3s to enter upgrade mode...\r\n");
  //       start_tick = HAL_GetTick();
  //       while ((HAL_GetTick() - start_tick) < 3000U)
  //       {
  //         if (HAL_UART_Receive(&huart1, &rx_byte, 1, 100) == HAL_OK)
  //         {
  //           if (rx_byte == 'U' || rx_byte == 'u')
  //           {
  //             printf("Upgrade mode triggered.\r\n");
  //             ota_state = OTA_STATE_UPGRADING;
  //             Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_UPGRADING);
  //             Write_Flag(EE_VAR_TARGET_SLOT, (active_slot == SLOT_A) ? SLOT_B : SLOT_A);
  //             break;
  //           }
  //         }
  //       }

  //       if (ota_state == OTA_STATE_UPGRADING)
  //       {
  //         continue;  /* 状态已变，重新走switch */
  //       }

  //       /* 超时无升级请求，跳转APP */
  //       Jump_To_App_Flash(app_addr);

  //     }

  //     case OTA_STATE_UPGRADING:
  //     {
  //       target_slot = Read_Flag(EE_VAR_TARGET_SLOT);
  //       /* 防御：target非法或等于active时，强制修正为active的对侧 */
  //       if (target_slot != SLOT_A && target_slot != SLOT_B)
  //       {
  //         target_slot = SLOT_B;
  //         Write_Flag(EE_VAR_TARGET_SLOT, target_slot);
  //       }
  //       else if (target_slot == active_slot && reason != REVERT_BOTH_INVALID)
  //       {
  //         target_slot = (active_slot == SLOT_A) ? SLOT_B : SLOT_A;
  //         Write_Flag(EE_VAR_TARGET_SLOT, target_slot);
  //       }

  //       write_addr = (target_slot == SLOT_B) ? APP_B_START_ADDR : APP_A_START_ADDR;
  //       active_addr = (active_slot == SLOT_B) ? APP_B_START_ADDR : APP_A_START_ADDR;

  //       /* 一次性校验活跃分区有效性，存入RAM标志，避免每次重试都校验 */
  //       active_valid = (Verify_APP_Integrity_Flash(active_addr) != 0) ? 1 : 0;

  //       printf("OTA upgrading to slot %s, addr 0x%08lX (active %s %s)\r\n",
  //              (target_slot == SLOT_B) ? "B" : "A", write_addr,
  //              (active_slot == SLOT_B) ? "B" : "A",
  //              active_valid ? "valid" : "invalid");

  //       xmodem_retry = 0;

  //       Erase_App_Flash(target_slot);

  //       while (1)
  //       {
  //         received = Xmodem_Start_Transfer(write_addr);

  //         if (received > 0)
  //         {
  //           printf("OTA received %d bytes, verifying...\r\n", received);
  //           ota_state = OTA_STATE_VERIFYING;
  //           Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_VERIFYING);
  //           break;
  //         }

  //         xmodem_retry++;
  //         if (active_valid)
  //         {
  //           /* 活跃分区有效：达到重试上限后退回BOOT，避免无限困在升级模式 */
  //           printf("OTA receive failed, retry %d/%d\r\n", xmodem_retry, XMODEM_MAX_RETRY);
  //           if (xmodem_retry >= XMODEM_MAX_RETRY)
  //           {
  //             printf("Max retry reached, active slot valid, falling back to BOOT.\r\n");
  //             ota_state = OTA_STATE_BOOT;
  //             Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_BOOT);
  //             break;
  //           }
  //         }
  //         else
  //         {
  //           /* 活跃分区无效：必须留在升级模式等待固件，但避免无意义擦除 */
  //           printf("OTA receive failed, active slot invalid, staying in UPGRADING.\r\n");
  //           /* 检测目标分区是否有部分写入：首字非0xFF说明有残留数据，需重新擦除 */
  //           first_word = *(volatile uint32_t *)write_addr;
  //           if (first_word != 0xFFFFFFFFU)
  //           {
  //             printf("Partial write detected, re-erasing slot %s\r\n",
  //                    (target_slot == SLOT_B) ? "B" : "A");
  //             Erase_App_Flash(target_slot);
  //           }
  //           else
  //           {
  //             printf("No data written, retrying without erase.\r\n");
  //           }
  //           xmodem_retry = 0;  /* 无有效分区时不计次，持续等待 */
  //         }
  //       }

  //       continue;
  //     }
     
  //     case OTA_STATE_VERIFYING:
  //     {
  //       target_slot = Read_Flag(EE_VAR_TARGET_SLOT);
  //       if (target_slot != SLOT_A && target_slot != SLOT_B)
  //       {
  //         target_slot = (active_slot == SLOT_A) ? SLOT_B : SLOT_A;
  //       }
  //       new_addr = (target_slot == SLOT_B) ? APP_B_START_ADDR : APP_A_START_ADDR;

  //       /* 校验新写入的分区 */
  //       if (Verify_APP_Integrity_Flash(new_addr) != 0)
  //       {
  //         /* 校验通过：先保存旧active，再切活跃分区，跳转新分区 */
  //         old_active = active_slot;
  //         active_slot = target_slot;
  //         Write_Flag(EE_VAR_ACTIVE_SLOT, target_slot);
  //         Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_BOOT);

  //         printf("OTA success, jumping to slot %s\r\n",
  //                (target_slot == SLOT_B) ? "B" : "A");
  //         Jump_To_App_Flash(new_addr);

  //       }

  //       /* 校验失败 → 升级回退 */
  //       printf("Slot %s verification failed, rolling back.\r\n",
  //              (target_slot == SLOT_B) ? "B" : "A");
  //       Write_Flag(EE_VAR_REVERT_REASON, REVERT_ACTIVE_VALID);
  //       ota_state = OTA_STATE_REVERT;
  //       Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_REVERT);
  //       continue;
  //     }

  //     case OTA_STATE_REVERT:
  //     {
  //       reason = Read_Flag(EE_VAR_REVERT_REASON);

  //       /*
  //        * REVERT纯标志驱动，零Flash校验（进入前已预写原因）：
  //        *   - REVERT_ACTIVE_VALID：升级回退，active有效，直接回BOOT
  //        *   - REVERT_OTHER_VALID：启动降级，other有效，切分区回BOOT
  //        *   - REVERT_BOTH_INVALID：两分区都无效，进UPGRADING
  //        */
  //       if (reason == REVERT_ACTIVE_VALID)
  //       {
  //         printf("Upgrade revert: active slot %s valid, returning to BOOT.\r\n",
  //                (active_slot == SLOT_B) ? "B" : "A");
  //         ota_state = OTA_STATE_BOOT;
  //         Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_BOOT);
  //         continue;
  //       }
  //       else if (reason == REVERT_OTHER_VALID)
  //       {
  //         other_slot = (active_slot == SLOT_A) ? SLOT_B : SLOT_A;
  //         printf("Boot fallback: switching from slot %s to slot %s.\r\n",
  //                (active_slot == SLOT_B) ? "B" : "A",
  //                (other_slot == SLOT_B) ? "B" : "A");
  //         active_slot = other_slot;
  //         Write_Flag(EE_VAR_ACTIVE_SLOT, other_slot);
  //         Write_Flag(EE_VAR_TARGET_SLOT, other_slot == SLOT_A ? SLOT_B : SLOT_A);
  //         ota_state = OTA_STATE_BOOT;
  //         Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_BOOT);
  //         continue;
  //       }
  //       else
  //       {
  //         /* REVERT_BOTH_INVALID 或异常值：两分区都无效，进升级模式 */
  //         printf("No valid app in any slot, entering upgrade mode.\r\n");
  //         ota_state = OTA_STATE_UPGRADING;
  //         Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_UPGRADING);
  //         Write_Flag(EE_VAR_TARGET_SLOT, SLOT_B);
  //         continue;
  //       }
  //     }

  //     default:
  //     {
  //       /* 异常状态，重置为BOOT */
  //       ota_state = OTA_STATE_BOOT;
  //       Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_BOOT);
  //       continue;
  //     }
  //   }
  //   /* USER CODE END WHILE */

  //   /* USER CODE BEGIN 3 */
  // }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure LSE Drive Capability
  */
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSE|RCC_OSCILLATORTYPE_MSI;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.MSIState = RCC_MSI_ON;
  RCC_OscInitStruct.MSICalibrationValue = 0;
  RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_6;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_MSI;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 40;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV7;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }

  /** Enable MSI Auto calibration
  */
  HAL_RCCEx_EnableMSIPLLMode();
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

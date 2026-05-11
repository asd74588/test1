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
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

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

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
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
  /* USER CODE BEGIN 2 */

  EE_Init();

  /* ========== A/B双区OTA Bootloader 状态机 ========== */
  uint32_t ota_state = Read_Flag(EE_VAR_OTA_STATE);
  uint32_t active_slot = Read_Flag(EE_VAR_ACTIVE_SLOT);

  /* 首次上电EEPROM无数据时，EE_Read返回失败，Read_Flag返回0xFFFFFFFF */
  if (active_slot != SLOT_A && active_slot != SLOT_B)
  {
    active_slot = SLOT_A;
    Write_Flag(EE_VAR_ACTIVE_SLOT, SLOT_A);
    Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_BOOT);
    ota_state = OTA_STATE_BOOT;
  }

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
  while (1)
  {
    switch (ota_state)
    {
      case OTA_STATE_BOOT:
      {
        uint32_t app_addr = (active_slot == SLOT_B) ? APP_B_START_ADDR : APP_A_START_ADDR;

        /* 当前活跃分区无效，转REVERT处理降级启动 */
        if (Verify_APP_Integrity_Flash(app_addr) == 0)
        {
          printf("Slot %s invalid, entering revert for fallback.\r\n",
                 (active_slot == SLOT_B) ? "B" : "A");
          /* 预检查另一分区，设置回退原因，REVERT内零Flash校验 */
          uint32_t other_slot = (active_slot == SLOT_A) ? SLOT_B : SLOT_A;
          uint32_t other_addr = (other_slot == SLOT_B) ? APP_B_START_ADDR : APP_A_START_ADDR;
          if (Verify_APP_Integrity_Flash(other_addr) != 0)
          {
            Write_Flag(EE_VAR_REVERT_REASON, REVERT_OTHER_VALID);
          }
          else
          {
            Write_Flag(EE_VAR_REVERT_REASON, REVERT_BOTH_INVALID);
          }
          ota_state = OTA_STATE_REVERT;
          Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_REVERT);
          continue;
        }

        /* APP有效：3秒等待窗口，收到'U'进升级，否则跳转 */
        printf("Press 'U' within 3s to enter upgrade mode...\r\n");
        uint8_t rx_byte;
        uint32_t start_tick = HAL_GetTick();
        while ((HAL_GetTick() - start_tick) < 3000U)
        {
          if (HAL_UART_Receive(&huart1, &rx_byte, 1, 100) == HAL_OK)
          {
            if (rx_byte == 'U' || rx_byte == 'u')
            {
              printf("Upgrade mode triggered.\r\n");
              ota_state = OTA_STATE_UPGRADING;
              Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_UPGRADING);
              Write_Flag(EE_VAR_TARGET_SLOT, (active_slot == SLOT_A) ? SLOT_B : SLOT_A);
              break;
            }
          }
        }

        if (ota_state == OTA_STATE_UPGRADING)
        {
          continue;  /* 状态已变，重新走switch */
        }

        /* 超时无升级请求，跳转APP */
        Jump_To_App_Flash(app_addr);

        
        /* 跳转失败（不应到达此处），进升级模式 */
        printf("Jump failed, entering upgrade mode.\r\n");
        ota_state = OTA_STATE_UPGRADING;
        Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_UPGRADING);
        Write_Flag(EE_VAR_TARGET_SLOT, (active_slot == SLOT_A) ? SLOT_B : SLOT_A);
        continue;
      }

      case OTA_STATE_UPGRADING:
      {
        uint32_t target_slot = Read_Flag(EE_VAR_TARGET_SLOT);
        /* 防御：target非法或等于active时，强制修正为active的对侧 */
        if (target_slot != SLOT_A && target_slot != SLOT_B)
        {
          target_slot = (active_slot == SLOT_A) ? SLOT_B : SLOT_A;
          Write_Flag(EE_VAR_TARGET_SLOT, target_slot);
        }
        else if (target_slot == active_slot)
        {
          target_slot = (active_slot == SLOT_A) ? SLOT_B : SLOT_A;
          Write_Flag(EE_VAR_TARGET_SLOT, target_slot);
        }

        uint32_t write_addr = (target_slot == SLOT_B) ? APP_B_START_ADDR : APP_A_START_ADDR;
        uint32_t active_addr = (active_slot == SLOT_B) ? APP_B_START_ADDR : APP_A_START_ADDR;

        /* 一次性校验活跃分区有效性，存入RAM标志，避免每次重试都校验 */
        uint8_t active_valid = (Verify_APP_Integrity_Flash(active_addr) != 0) ? 1 : 0;

        printf("OTA upgrading to slot %s, addr 0x%08lX (active %s %s)\r\n",
               (target_slot == SLOT_B) ? "B" : "A", write_addr,
               (active_slot == SLOT_B) ? "B" : "A",
               active_valid ? "valid" : "invalid");

        uint8_t xmodem_retry = 0;
        #define XMODEM_MAX_RETRY  3

        Erase_App_Flash(target_slot);

        while (1)
        {
          int received = Xmodem_Start_Transfer(write_addr);

          if (received > 0)
          {
            printf("OTA received %d bytes, verifying...\r\n", received);
            ota_state = OTA_STATE_VERIFYING;
            Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_VERIFYING);
            break;
          }

          xmodem_retry++;
          if (active_valid)
          {
            /* 活跃分区有效：达到重试上限后退回BOOT，避免无限困在升级模式 */
            printf("OTA receive failed, retry %d/%d\r\n", xmodem_retry, XMODEM_MAX_RETRY);
            if (xmodem_retry >= XMODEM_MAX_RETRY)
            {
              printf("Max retry reached, active slot valid, falling back to BOOT.\r\n");
              ota_state = OTA_STATE_BOOT;
              Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_BOOT);
              break;
            }
          }
          else
          {
            /* 活跃分区无效：必须留在升级模式等待固件，但避免无意义擦除 */
            printf("OTA receive failed, active slot invalid, staying in UPGRADING.\r\n");
            /* 检测目标分区是否有部分写入：首字非0xFF说明有残留数据，需重新擦除 */
            uint32_t first_word = *(volatile uint32_t *)write_addr;
            if (first_word != 0xFFFFFFFFU)
            {
              printf("Partial write detected, re-erasing slot %s\r\n",
                     (target_slot == SLOT_B) ? "B" : "A");
              Erase_App_Flash(target_slot);
            }
            else
            {
              printf("No data written, retrying without erase.\r\n");
            }
            xmodem_retry = 0;  /* 无有效分区时不计次，持续等待 */
          }
        }

        continue;
      }

         
      case OTA_STATE_VERIFYING:
      {
        uint32_t target_slot = Read_Flag(EE_VAR_TARGET_SLOT);
        if (target_slot != SLOT_A && target_slot != SLOT_B)
        {
          target_slot = (active_slot == SLOT_A) ? SLOT_B : SLOT_A;
        }
        uint32_t new_addr = (target_slot == SLOT_B) ? APP_B_START_ADDR : APP_A_START_ADDR;

        /* 校验新写入的分区 */
        if (Verify_APP_Integrity_Flash(new_addr) != 0)
        {
          /* 校验通过：先保存旧active，再切活跃分区，跳转新分区 */
          uint32_t old_active = active_slot;
          active_slot = target_slot;
          Write_Flag(EE_VAR_ACTIVE_SLOT, target_slot);
          Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_BOOT);

          printf("OTA success, jumping to slot %s\r\n",
                 (target_slot == SLOT_B) ? "B" : "A");
          Jump_To_App_Flash(new_addr);

          /* 跳转失败：新固件校验通过但无法运行，切回旧分区回退 */
          printf("Jump to slot %s failed, reverting to old slot %s.\r\n",
                 (target_slot == SLOT_B) ? "B" : "A",
                 (old_active == SLOT_B) ? "B" : "A");
          active_slot = old_active;
          Write_Flag(EE_VAR_ACTIVE_SLOT, old_active);
          Write_Flag(EE_VAR_REVERT_REASON, REVERT_ACTIVE_VALID);
          ota_state = OTA_STATE_REVERT;
          Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_REVERT);
          continue;
        }

        /* 校验失败 → 升级回退 */
        printf("Slot %s verification failed, rolling back.\r\n",
               (target_slot == SLOT_B) ? "B" : "A");
        Write_Flag(EE_VAR_REVERT_REASON, REVERT_ACTIVE_VALID);
        ota_state = OTA_STATE_REVERT;
        Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_REVERT);
        continue;
      }

      case OTA_STATE_REVERT:
      {
        uint32_t reason = Read_Flag(EE_VAR_REVERT_REASON);

        /*
         * REVERT纯标志驱动，零Flash校验（进入前已预写原因）：
         *   - REVERT_ACTIVE_VALID：升级回退，active有效，直接回BOOT
         *   - REVERT_OTHER_VALID：启动降级，other有效，切分区回BOOT
         *   - REVERT_BOTH_INVALID：两分区都无效，进UPGRADING
         */
        if (reason == REVERT_ACTIVE_VALID)
        {
          printf("Upgrade revert: active slot %s valid, returning to BOOT.\r\n",
                 (active_slot == SLOT_B) ? "B" : "A");
          ota_state = OTA_STATE_BOOT;
          Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_BOOT);
          continue;
        }
        else if (reason == REVERT_OTHER_VALID)
        {
          uint32_t other_slot = (active_slot == SLOT_A) ? SLOT_B : SLOT_A;
          printf("Boot fallback: switching from slot %s to slot %s.\r\n",
                 (active_slot == SLOT_B) ? "B" : "A",
                 (other_slot == SLOT_B) ? "B" : "A");
          active_slot = other_slot;
          Write_Flag(EE_VAR_ACTIVE_SLOT, other_slot);
          Write_Flag(EE_VAR_TARGET_SLOT, other_slot == SLOT_A ? SLOT_B : SLOT_A);
          ota_state = OTA_STATE_BOOT;
          Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_BOOT);
          continue;
        }
        else
        {
          /* REVERT_BOTH_INVALID 或异常值：两分区都无效，进升级模式 */
          printf("No valid app in any slot, entering upgrade mode.\r\n");
          ota_state = OTA_STATE_UPGRADING;
          Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_UPGRADING);
          Write_Flag(EE_VAR_TARGET_SLOT, SLOT_A);
          continue;
        }
      }

      default:
      {
        /* 异常状态，重置为BOOT */
        ota_state = OTA_STATE_BOOT;
        Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_BOOT);
        continue;
      }
    }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
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

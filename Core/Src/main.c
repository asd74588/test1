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
#include "w25qxx.h"
#include "eeprom_emul.h"
#include "ota_state_machine.h"
#include "data_storage.h"
#include "cmox_crypto.h"
#include "shell_init.h"
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
static uint8_t storage_mount(lfs_ctx_t *fs);
static void ota_context_init(ota_ctx_t *ctx, transfer_cfg_t *transfer_cfg);
static uint8_t boot_wait_for_shell(uint32_t timeout_ms);
static void enter_shell_forever(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

extern const struct lfs_config my_lfs_config;
extern spinor_info_t          spinor;

lfs_ctx_t lfs_ctx;

static uint8_t storage_mount(lfs_ctx_t *fs)
{
  if (fs == NULL) {
    return 1;
  }

  if (spinor_init(&spinor) < 0) {
    return 1;
  }

  int mount_err = lfs_mount(&fs->lfs, &my_lfs_config);
  dbg_printf("lfs_mount: %d\r\n", mount_err);
  
  if (mount_err) 
  {
    lfs_format(&fs->lfs, &my_lfs_config);
    mount_err = lfs_mount(&fs->lfs, &my_lfs_config);
    dbg_printf("LittleFS formatted and mounted,err: %d\r\n", mount_err);
  }

  if (mount_err != 0) 
  {
    return 1;
  }

  fs->mounted = 1;
  fs->file_open = 0;

  return 0;
}

static void ota_context_init(ota_ctx_t *ctx, transfer_cfg_t *transfer_cfg)
{
  memset(transfer_cfg, 0, sizeof(*transfer_cfg));
  memset(ctx, 0, sizeof(*ctx));

  transfer_cfg->write_cb = (storage_callback_t)lfs_storage_callback;
  transfer_cfg->write_user_ctx = (void *)&lfs_ctx;
  transfer_cfg->receive_cb = (receive_callback_t)Proto_Start_Receive;
  transfer_cfg->recv_user_ctx = (void *)transfer_cfg;

  ctx->transfer_cfg = transfer_cfg;
  ctx->resource_ctx = &lfs_ctx;
}

static uint8_t boot_wait_for_shell(uint32_t timeout_ms)
{
  uint8_t boot_key = 0U;
  uint32_t start_tick = HAL_GetTick();

  while ((HAL_GetTick() - start_tick) < timeout_ms) {
    if (HAL_UART_Receive(&huart3, &boot_key, 1U, 100U) == HAL_OK) {
      if (boot_key == 'S' || boot_key == 's') {
        return 1U;
      }
    }
  }

  return 0U;
}

static void enter_shell_forever(void)
{
  Shell_Init();
  while (1) {
    Shell_Process();
  }
}
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
  MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */
  EE_Init();

  ota_ctx_t ctx;
  transfer_cfg_t transfer_cfg;

  memset(&lfs_ctx, 0, sizeof(lfs_ctx_t));
  ota_context_init(&ctx, &transfer_cfg);

  cmox_initialize(NULL);

  if (storage_mount(&lfs_ctx) != 0U) {
    dbg_printf("LittleFS mount failed\r\n");
    Error_Handler();
  }

  dbg_printf("System initialized. Press 'S' within 5s to enter shell...\r\n");
  if (boot_wait_for_shell(5000U) != 0U) {
    enter_shell_forever();
  }

  dbg_printf("No shell request, starting OTA state machine...\r\n");
  ota_run(&ctx);
  /* USER CODE END 2 */
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
     ex: dbg_printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

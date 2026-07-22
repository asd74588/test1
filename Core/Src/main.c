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

#include <string.h>
#include "stdio.h"
#include "global.h"
#include "w25qxx.h"
#include "eeprom_emul.h"
#include "ota_state_machine.h"
#include "cmox_crypto.h"
#include "log_config.h"
#include "thingsboard_ota.h"
#include "esp8266.h"
#include "net_cfg.h"
#include "core_mqtt.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

#if LOG_MAIN_ENABLE
#define main_log(...) printf(__VA_ARGS__)
#else
#define main_log(...) ((void)0)
#endif

#if LOG_MAIN_TRACE_ENABLE
#define main_trace(...) printf(__VA_ARGS__)
#else
#define main_trace(...) ((void)0)
#endif

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
#if ESP8266_MQTT_BACKEND_AT_ENABLE
static uint8_t spinor_lfs_mount(lfs_ctx_t *fs);
#else
static void    wifi_mqtt_smoke_test(void);
#endif

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

lfs_ctx_t lfs_ctx;

#if ESP8266_MQTT_BACKEND_AT_ENABLE
extern const struct lfs_config my_lfs_config;
extern spinor_info_t           spinor;

static uint8_t spinor_lfs_mount(lfs_ctx_t *fs)
{
    if (fs == NULL)
    {
        return 1;
    }

    if (spinor_init(&spinor) < 0)
    {
        return 1;
    }

    int mount_err = lfs_mount(&fs->lfs, &my_lfs_config);
    main_trace("lfs_mount: %d\r\n", mount_err);

    if (mount_err)
    {
        lfs_format(&fs->lfs, &my_lfs_config);
        mount_err = lfs_mount(&fs->lfs, &my_lfs_config);
        main_log("LittleFS formatted and mounted, err: %d\r\n", mount_err);
    }

    if (mount_err != 0)
    {
        return 1;
    }

    fs->mounted   = 1;
    fs->file_open = 0;

    return 0;
}
#else
static void wifi_mqtt_smoke_test(void)
{
    const net_cfg_t *cfg;
    static char      ip[32];
    static char      gateway[32];
    static char      client_id[64];
    static char      telemetry[96];
    int              rv;

    cfg = net_cfg_get();
    if (net_cfg_is_valid(cfg) == 0)
    {
        main_log("WiFi MQTT test: network config invalid\r\n");
        return;
    }

    if (esp8266_module_init() != 0)
    {
        main_log("WiFi MQTT test: ESP8266 init failed\r\n");
        return;
    }

    if (esp8266_scan_ap((char *)cfg->wifi_ssid) != 0)
    {
        main_log("WiFi MQTT test: AP [%s] not found\r\n", cfg->wifi_ssid);
        return;
    }

    if (esp8266_join_network((char *)cfg->wifi_ssid, (char *)cfg->wifi_password) != 0)
    {
        main_log("WiFi MQTT test: join AP failed\r\n");
        return;
    }

    if (esp8266_get_ipaddr(ip, gateway, (int)sizeof(ip)) != 0)
    {
        main_log("WiFi MQTT test: get IP failed\r\n");
        goto ExitWiFi;
    }

    main_log("WiFi MQTT test: ip=%s gateway=%s\r\n", ip, gateway);

    if (esp8266_ping_test((char *)cfg->mqtt_host) != 0)
    {
        main_log("WiFi MQTT test: ping host [%s] failed\r\n", cfg->mqtt_host);
        goto ExitWiFi;
    }

    memset(client_id, 0, sizeof(client_id));
    snprintf(client_id,
             sizeof(client_id),
             "stm32_ota_%08lX%08lX%08lX_%lu",
             (unsigned long)HAL_GetUIDw0(),
             (unsigned long)HAL_GetUIDw1(),
             (unsigned long)HAL_GetUIDw2(),
             (unsigned long)HAL_GetTick());

    rv = mqtt_connect(
        (char *)cfg->mqtt_host, (int)cfg->mqtt_port, client_id, (char *)cfg->access_token, "");
    if (rv != 0)
    {
        main_log("WiFi MQTT test: mqtt_connect [%s:%u] failed, rv=%d\r\n",
                 cfg->mqtt_host,
                 (unsigned int)cfg->mqtt_port,
                 rv);
        goto ExitWiFi;
    }

    main_log("WiFi MQTT test: mqtt_connect [%s:%u] ok\r\n",
             cfg->mqtt_host,
             (unsigned int)cfg->mqtt_port);

    rv = mqtt_subscribe_topic("v1/devices/me/attributes", Qos0, 1);
    if (rv != 0)
    {
        main_log("WiFi MQTT test: subscribe attributes failed, rv=%d\r\n", rv);
        goto ExitMqtt;
    }

    main_log("WiFi MQTT test: subscribe attributes ok\r\n");

    memset(telemetry, 0, sizeof(telemetry));
    snprintf(
        telemetry, sizeof(telemetry), "{\"fw_test\":1,\"tick\":%lu}", (unsigned long)HAL_GetTick());

    rv = mqtt_publish("v1/devices/me/telemetry", Qos0, telemetry);
    if (rv != 0)
    {
        main_log("WiFi MQTT test: publish telemetry failed, rv=%d\r\n", rv);
        goto ExitMqtt;
    }

    main_log("WiFi MQTT test: publish telemetry ok\r\n");

    rv = mqtt_pingreq();
    if (rv != 0)
    {
        main_log("WiFi MQTT test: pingreq failed, rv=%d\r\n", rv);
        goto ExitMqtt;
    }

    main_log("WiFi MQTT test: pingreq sent\r\n");

ExitMqtt:
    mqtt_disconnect();

ExitWiFi:
    esp8266_wifi_disconnect();
}
#endif
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
    MX_USART2_UART_Init();
    /* USER CODE BEGIN 2 */

    EE_Init();
    cmox_initialize(NULL);

#if ESP8266_MQTT_BACKEND_AT_ENABLE
    {
        ota_ctx_t      ctx;
        transfer_cfg_t transfer_cfg;

        memset(&lfs_ctx, 0, sizeof(lfs_ctx_t));
        thingsboard_ota_bind_transport(&ctx, &transfer_cfg, &lfs_ctx);

        if (spinor_lfs_mount(&lfs_ctx) != 0U)
        {
            main_log("LittleFS mount failed\r\n");
            Error_Handler();
        }

        main_log("Starting ThingsBoard OTA state machine...\r\n");
        ota_run(&ctx);
    }
#else
    main_log("Starting WiFi MQTT smoke test...\r\n");
    wifi_mqtt_smoke_test();
#endif
    /* USER CODE END 2 */

    /* Infinite loop */
    /* USER CODE BEGIN WHILE */
    while (1)
    {
        /* USER CODE END WHILE */

        /* USER CODE BEGIN 3 */
        HAL_Delay(10U);
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
    RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_LSE | RCC_OSCILLATORTYPE_MSI;
    RCC_OscInitStruct.LSEState            = RCC_LSE_ON;
    RCC_OscInitStruct.MSIState            = RCC_MSI_ON;
    RCC_OscInitStruct.MSICalibrationValue = 0;
    RCC_OscInitStruct.MSIClockRange       = RCC_MSIRANGE_6;
    RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_MSI;
    RCC_OscInitStruct.PLL.PLLM            = 1;
    RCC_OscInitStruct.PLL.PLLN            = 40;
    RCC_OscInitStruct.PLL.PLLP            = RCC_PLLP_DIV7;
    RCC_OscInitStruct.PLL.PLLQ            = RCC_PLLQ_DIV2;
    RCC_OscInitStruct.PLL.PLLR            = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    /** Initializes the CPU, AHB and APB buses clocks
  */
    RCC_ClkInitStruct.ClockType =
        RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
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

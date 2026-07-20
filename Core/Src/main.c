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

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "stdio.h"
#include "global.h"
#include "w25qxx.h"
#include "eeprom_emul.h"
#include "ota_state_machine.h"
#include "data_storage.h"
#include "cmox_crypto.h"
#include "shell_init.h"
#include "log_config.h"
#include "esp8266.h"
#include "fs_cmd.h"
#include "cmds.h"
#include "app_verify.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define TB_FW_DOWNLOAD_PATH "a.elf"
#define TB_FW_CHUNK_SIZE    512U
#define WIFI_TEST_SSID      "first"
#define WIFI_TEST_PASS      "12345678"
#define TB_MQTT_HOST        "47.97.214.156"
#define TB_MQTT_PORT        1883
#define TB_ACCESS_TOKEN     "59pkoe1t5pba13obc2vx"
#define MAIN_THINGSBOARD_OTA_ENABLE 1U
#define MAIN_THINGSBOARD_FORCE_CHECK_ENABLE 1U
#define MAIN_THINGSBOARD_SHELL_ENABLE 0U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

#if LOG_MAIN_ENABLE
#define dbg_printf(format,args...) printf(format, ##args)
#else
#define dbg_printf(format,args...) do{}while(0)
#endif

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static uint8_t storage_mount(lfs_ctx_t *fs);
#if (MAIN_THINGSBOARD_OTA_ENABLE == 0U)
static void ota_context_init(ota_ctx_t *ctx, transfer_cfg_t *transfer_cfg);
#endif
static void ota_context_init_thingsboard(ota_ctx_t *ctx, transfer_cfg_t *transfer_cfg);
#if (MAIN_THINGSBOARD_OTA_ENABLE == 0U) || (MAIN_THINGSBOARD_SHELL_ENABLE != 0U)
static uint8_t boot_wait_for_shell(uint32_t timeout_ms);
static void enter_shell_forever(void);
#endif
static int thingsboard_firmware_download_to_lfs(lfs_ctx_t *fs,
                                                const esp8266_tb_firmware_info_t *fw_info);
static int thingsboard_firmware_verify_download(lfs_ctx_t *fs,
                                                const esp8266_tb_firmware_info_t *fw_info);
static int thingsboard_ota_receive_cb(void *user_ctx, void *file_info_out);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

extern const struct lfs_config my_lfs_config;
extern const struct lfs_file_config lfs_file_cfg;
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

static void ota_context_init_thingsboard(ota_ctx_t *ctx, transfer_cfg_t *transfer_cfg)
{
  memset(transfer_cfg, 0, sizeof(*transfer_cfg));
  memset(ctx, 0, sizeof(*ctx));

  transfer_cfg->write_cb = NULL;
  transfer_cfg->write_user_ctx = NULL;
  transfer_cfg->receive_cb = thingsboard_ota_receive_cb;
  transfer_cfg->recv_user_ctx = &lfs_ctx;

  ctx->transfer_cfg = transfer_cfg;
  ctx->resource_ctx = &lfs_ctx;
}

#if (MAIN_THINGSBOARD_OTA_ENABLE == 0U)
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
#endif

#if (MAIN_THINGSBOARD_OTA_ENABLE == 0U) || (MAIN_THINGSBOARD_SHELL_ENABLE != 0U)
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
#endif

static int thingsboard_firmware_download_to_lfs(lfs_ctx_t *fs,
                                                const esp8266_tb_firmware_info_t *fw_info)
{
  static uint8_t chunk_buf[TB_FW_CHUNK_SIZE];
  uint16_t chunk_len = 0U;
  lfs_ssize_t written;
  uint32_t total_written = 0U;
  uint32_t chunk_index = 0U;
  uint32_t request_size;
  int err;

  if (fs == NULL || fw_info == NULL || fs->mounted == 0U || fw_info->size == 0U) {
    printf("Invalid firmware download arguments\r\n");
    return -1;
  }

  if (fs->file_open != 0U) {
    lfs_file_close(&fs->lfs, &fs->file);
    fs->file_open = 0U;
  }

  lfs_remove(&fs->lfs, TB_FW_DOWNLOAD_PATH);
  err = lfs_file_opencfg(&fs->lfs,
                         &fs->file,
                         TB_FW_DOWNLOAD_PATH,
                         LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC,
                         &lfs_file_cfg);
  if (err != 0) {
    printf("LittleFS open %s failed: %d\r\n", TB_FW_DOWNLOAD_PATH, err);
    return -3;
  }
  fs->file_open = 1U;

  printf("ThingsBoard firmware download start: %lu bytes -> %s\r\n",
         (unsigned long)fw_info->size,
         TB_FW_DOWNLOAD_PATH);

  if (esp8266_thingsboard_subscribe_firmware_chunks() != 0) {
    printf("ThingsBoard firmware chunk topic subscribe failed\r\n");
    lfs_file_close(&fs->lfs, &fs->file);
    fs->file_open = 0U;
    return -4;
  }

  while (total_written < fw_info->size) {
    request_size = fw_info->size - total_written;
    if (request_size > TB_FW_CHUNK_SIZE) {
      request_size = TB_FW_CHUNK_SIZE;
    }

    if (esp8266_thingsboard_request_firmware_chunk(chunk_index,
                                                   request_size,
                                                   chunk_buf,
                                                   sizeof(chunk_buf),
                                                   &chunk_len) != 0) {
      printf("ThingsBoard firmware chunk %lu receive failed\r\n",
             (unsigned long)chunk_index);
      lfs_file_close(&fs->lfs, &fs->file);
      fs->file_open = 0U;
      return -5;
    }

    if (chunk_len != (uint16_t)request_size) {
      printf("ThingsBoard firmware chunk %lu length mismatch, expect %lu got %u\r\n",
             (unsigned long)chunk_index,
             (unsigned long)request_size,
             chunk_len);
      lfs_file_close(&fs->lfs, &fs->file);
      fs->file_open = 0U;
      return -6;
    }

    written = lfs_file_write(&fs->lfs, &fs->file, chunk_buf, chunk_len);
    if (written < 0 || (uint16_t)written != chunk_len) {
      printf("LittleFS write %s failed at chunk %lu: %d\r\n",
             TB_FW_DOWNLOAD_PATH,
             (unsigned long)chunk_index,
             (int)written);
      lfs_file_close(&fs->lfs, &fs->file);
      fs->file_open = 0U;
      return -7;
    }

    total_written += chunk_len;
    printf("ThingsBoard firmware download progress: %lu/%lu bytes\r\n",
           (unsigned long)total_written,
           (unsigned long)fw_info->size);
    chunk_index++;
  }

  err = lfs_file_sync(&fs->lfs, &fs->file);
  if (err != 0) {
    printf("LittleFS sync %s failed: %d\r\n", TB_FW_DOWNLOAD_PATH, err);
    lfs_file_close(&fs->lfs, &fs->file);
    fs->file_open = 0U;
    return -8;
  }

  err = lfs_file_close(&fs->lfs, &fs->file);
  fs->file_open = 0U;
  if (err != 0) {
    printf("LittleFS close %s failed: %d\r\n", TB_FW_DOWNLOAD_PATH, err);
    return -9;
  }

  printf("ThingsBoard firmware saved to %s, %lu bytes, chunks %lu\r\n",
         TB_FW_DOWNLOAD_PATH,
         (unsigned long)total_written,
         (unsigned long)chunk_index);
  return 0;
}

static const char *version_skip_to_digit(const char *text)
{
  while (text != NULL && *text != '\0' && isdigit((unsigned char)*text) == 0) {
    text++;
  }

  return text;
}

static uint8_t version_tail_has_nonzero(const char *text)
{
  const char *cursor = text;

  while (cursor != NULL) {
    unsigned long value;
    char *end;

    cursor = version_skip_to_digit(cursor);
    if (*cursor == '\0') {
      return 0U;
    }

    value = strtoul(cursor, &end, 10);
    if (value != 0U) {
      return 1U;
    }

    cursor = end;
  }

  return 0U;
}

static int firmware_version_compare(const char *remote_version, const char *local_version)
{
  const char *remote = remote_version;
  const char *local = local_version;

  if (remote == NULL || local == NULL) {
    return 0;
  }

  while (1) {
    unsigned long remote_value;
    unsigned long local_value;
    char *remote_end;
    char *local_end;

    remote = version_skip_to_digit(remote);
    local = version_skip_to_digit(local);

    if (*remote == '\0' && *local == '\0') {
      return 0;
    }

    if (*remote == '\0') {
      return version_tail_has_nonzero(local) ? -1 : 0;
    }

    if (*local == '\0') {
      return version_tail_has_nonzero(remote) ? 1 : 0;
    }

    remote_value = strtoul(remote, &remote_end, 10);
    local_value = strtoul(local, &local_end, 10);

    if (remote_value > local_value) {
      return 1;
    }

    if (remote_value < local_value) {
      return -1;
    }

    remote = remote_end;
    local = local_end;
  }
}

static int thingsboard_firmware_should_download(const esp8266_tb_firmware_info_t *fw_info)
{
  const char *local_version = sys_get_version();
  int version_cmp;

  if (fw_info == NULL || fw_info->valid == 0U || fw_info->size == 0U) {
    printf("ThingsBoard firmware info invalid, skip download\r\n");
    return -1;
  }

  if (fw_info->version[0] == '\0') {
    printf("ThingsBoard firmware version empty, skip download\r\n");
    return -2;
  }

  if (fw_info->checksum_algorithm[0] != '\0' &&
      strcmp(fw_info->checksum_algorithm, "SHA256") != 0) {
    printf("ThingsBoard checksum algorithm [%s] unsupported, skip download\r\n",
           fw_info->checksum_algorithm);
    return -3;
  }

  version_cmp = firmware_version_compare(fw_info->version, local_version);
  printf("ThingsBoard firmware version remote[%s], local[%s]\r\n",
         fw_info->version,
         local_version);

  if (version_cmp <= 0) {
    printf("ThingsBoard firmware is not newer, skip download\r\n");
    return 0;
  }

  printf("ThingsBoard firmware update available, size %lu bytes\r\n",
         (unsigned long)fw_info->size);
  return 1;
}

static int thingsboard_firmware_verify_download(lfs_ctx_t *fs,
                                                const esp8266_tb_firmware_info_t *fw_info)
{
  char calc_checksum[65];
  firmware_verify_status_t verify_ret;

  if (fs == NULL || fw_info == NULL || fw_info->checksum[0] == '\0') {
    printf("ThingsBoard firmware checksum missing\r\n");
    return -1;
  }

  if (strcmp(fw_info->checksum_algorithm, "SHA256") != 0) {
    printf("ThingsBoard checksum algorithm [%s] unsupported\r\n",
           fw_info->checksum_algorithm);
    return -2;
  }

  memset(calc_checksum, 0, sizeof(calc_checksum));
  verify_ret = verify_lfs_file_sha256(fs,
                                      TB_FW_DOWNLOAD_PATH,
                                      fw_info->checksum,
                                      fw_info->size,
                                      calc_checksum,
                                      sizeof(calc_checksum));
  if (verify_ret != FIRMWARE_VERIFY_OK) {
    printf("ThingsBoard firmware checksum verify failed: %d\r\n", verify_ret);
    printf("expected=%s\r\n", fw_info->checksum);
    if (calc_checksum[0] != '\0') {
      printf("actual  =%s\r\n", calc_checksum);
    }
    return -3;
  }

  printf("ThingsBoard firmware checksum verify ok: SHA256 %s\r\n",
         calc_checksum);
  return 0;
}

static int thingsboard_wifi_prepare(const char *ssid,
                                    const char *password,
                                    const char *server_host,
                                    char *ip,
                                    char *gateway,
                                    int ip_size)
{
  uint8_t joined = 0U;
  int ret = -1;

  if (esp8266_module_init() != 0) {
    printf("ESP8266 init failed\r\n");
    return -1;
  }

  printf("ESP8266 init ok\r\n");

  if (esp8266_scan_ap((char *)ssid) != 0) {
    printf("WiFi AP '%s' not found, stop OTA check\r\n", ssid);
    return -2;
  }

  if (esp8266_join_network((char *)ssid, (char *)password) != 0) {
    printf("WiFi join failed, stop OTA check\r\n");
    return -3;
  }
  joined = 1U;

  printf("WiFi joined\r\n");

  if (esp8266_get_ipaddr(ip, gateway, ip_size) != 0) {
    printf("WiFi get IP failed, stop OTA check\r\n");
    ret = -4;
    goto FailAfterJoin;
  }

  printf("ip=%s gateway=%s\r\n", ip, gateway);

  if (esp8266_ping_test(gateway) != 0) {
    printf("Gateway ping failed, stop OTA check\r\n");
    ret = -5;
    goto FailAfterJoin;
  }

  if (esp8266_ping_test("www.baidu.com") != 0) {
    printf("Public network ping failed, stop OTA check\r\n");
    ret = -6;
    goto FailAfterJoin;
  }

  if (esp8266_ping_test((char *)server_host) != 0) {
    printf("ThingsBoard server ping failed, stop OTA check\r\n");
    ret = -7;
    goto FailAfterJoin;
  }

  return 0;

FailAfterJoin:
  if (joined != 0U) {
    esp8266_wifi_disconnect();
  }
  return ret;
}

static int thingsboard_mqtt_prepare(const char *host,
                                    int port,
                                    const char *access_token,
                                    esp8266_tb_firmware_info_t *fw_info)
{
  static unsigned char telemetry[] = "{\"fw_test\":1,\"wifi_rssi\":-34}";
  int sent;

  if (esp8266_mqtt_connect((char *)host, port, (char *)access_token) != 0) {
    printf("ThingsBoard MQTT connect %s:%d failed\r\n", host, port);
    return -1;
  }

  sent = esp8266_thingsboard_publish_telemetry(telemetry,
                                               (int)(sizeof(telemetry) - 1U));
  printf("ThingsBoard telemetry sent %d bytes\r\n", sent);
  if (sent <= 0) {
    esp8266_mqtt_disconnect();
    return -2;
  }

  if (esp8266_thingsboard_request_firmware_info() != 0) {
    printf("ThingsBoard firmware info unavailable\r\n");
    esp8266_mqtt_disconnect();
    return -3;
  }

  printf("ThingsBoard firmware info received\r\n");

  if (esp8266_thingsboard_get_firmware_info(fw_info) != 0) {
    printf("ThingsBoard firmware info read failed\r\n");
    esp8266_mqtt_disconnect();
    return -4;
  }

  return 0;
}

static int thingsboard_ota_receive_cb(void *user_ctx, void *file_info_out)
{
  lfs_ctx_t *fs = (lfs_ctx_t *)user_ctx;
  YmodemFileInfo *file_info = (YmodemFileInfo *)file_info_out;
  char ip[32];
  char gateway[32];
  esp8266_tb_firmware_info_t fw_info;
  int decision;
  int ret = -1;
  uint8_t wifi_ready = 0U;
  uint8_t mqtt_ready = 0U;

  if (fs == NULL) {
    return -1;
  }

  memset(&fw_info, 0, sizeof(fw_info));

  if (thingsboard_wifi_prepare(WIFI_TEST_SSID,
                               WIFI_TEST_PASS,
                               TB_MQTT_HOST,
                               ip,
                               gateway,
                               (int)sizeof(ip)) != 0) {
    goto CleanUp;
  }
  wifi_ready = 1U;

  if (thingsboard_mqtt_prepare(TB_MQTT_HOST,
                               TB_MQTT_PORT,
                               TB_ACCESS_TOKEN,
                               &fw_info) != 0) {
    goto CleanUp;
  }
  mqtt_ready = 1U;

  decision = thingsboard_firmware_should_download(&fw_info);
  if (decision < 0) {
    goto CleanUp;
  }

  if (decision == 0) {
    printf("ThingsBoard firmware no update, skip OTA transfer\r\n");
    ret = 0;
    goto CleanUp;
  }

  if (thingsboard_firmware_download_to_lfs(fs, &fw_info) != 0) {
    printf("ThingsBoard firmware LFS download failed\r\n");
    goto CleanUp;
  }

  if (thingsboard_firmware_verify_download(fs, &fw_info) != 0) {
    lfs_remove(&fs->lfs, TB_FW_DOWNLOAD_PATH);
    printf("ThingsBoard firmware checksum invalid, removed %s\r\n",
           TB_FW_DOWNLOAD_PATH);
    goto CleanUp;
  }

  printf("ThingsBoard firmware LFS download ok\r\n");
  if (file_info != NULL) {
    memset(file_info, 0, sizeof(*file_info));
    strncpy(file_info->filename, TB_FW_DOWNLOAD_PATH, sizeof(file_info->filename) - 1U);
    file_info->filesize = fw_info.size;
  }
  ret = (int)fw_info.size;

CleanUp:
  if (mqtt_ready != 0U) {
    esp8266_mqtt_disconnect();
  }

  if (wifi_ready != 0U) {
    esp8266_wifi_disconnect();
  }

  return ret;
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
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */

  EE_Init();
  cmox_initialize(NULL);

#if (MAIN_THINGSBOARD_OTA_ENABLE == 0U)
  {

    ota_ctx_t ctx;
    transfer_cfg_t transfer_cfg;

    memset(&lfs_ctx, 0, sizeof(lfs_ctx_t));
    ota_context_init(&ctx, &transfer_cfg);

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
  }
#else
  {
    ota_ctx_t ctx;
    transfer_cfg_t transfer_cfg;

    memset(&lfs_ctx, 0, sizeof(lfs_ctx_t));
    ota_context_init_thingsboard(&ctx, &transfer_cfg);

    if (storage_mount(&lfs_ctx) != 0U) {
      printf("LittleFS mount failed, stop ThingsBoard chunk test\r\n");
      Error_Handler();
    }

#if (MAIN_THINGSBOARD_SHELL_ENABLE != 0U)
    fs_cmd_init(&lfs_ctx);

    dbg_printf("ThingsBoard OTA mode. Press 'S' within 3s to enter shell...\r\n");
    if (boot_wait_for_shell(3000U) != 0U) {
      enter_shell_forever();
    }
#endif

    if (MAIN_THINGSBOARD_FORCE_CHECK_ENABLE != 0U) {
      Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_UPGRADING);
    }

    dbg_printf("No shell request, starting ThingsBoard OTA state machine...\r\n");
    ota_run(&ctx);
  }
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

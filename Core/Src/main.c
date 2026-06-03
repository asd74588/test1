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
#include "ota_state_machine.h"
#include "data_storage.h"
#include "fs_cmd.h"
#include "shell.h"
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

#define PATH "a.elf"
extern const struct lfs_config my_lfs_config;
extern const struct lfs_file_config lfs_file_cfg;
extern spinor_info_t          spinor;




uint8_t lfs_init(lfs_ctx_t *lfs_ctx)
{
  if( lfs_ctx == NULL )
    return 1;

  if( spinor_init(&spinor) < 0 )
    return 1;

  int mount_err;
  int file_err;


  mount_err = lfs_mount(&lfs_ctx->lfs, &my_lfs_config);
  dbg_printf("lfs_mount: %d\r\n", mount_err);
  
  if (mount_err) 
  {
    lfs_format(&lfs_ctx->lfs, &my_lfs_config);
    mount_err = lfs_mount(&lfs_ctx->lfs, &my_lfs_config);
    dbg_printf("LittleFS formatted and mounted,err: %d\r\n", mount_err);
  }

  if (mount_err != 0) 
  {
    return 1;
  }

  /*到这里的话文件系统肯定挂载上了*/
  lfs_ctx->mounted = 1;

  file_err = lfs_file_opencfg(&lfs_ctx->lfs, &lfs_ctx->file, PATH, LFS_O_WRONLY | LFS_O_CREAT, &lfs_file_cfg);
  dbg_printf("lfs_file_open(write): %d\r\n", file_err);

  if (file_err != 0) 
  {
    return 1;
  }

  /*到这里的话文件句柄已经打开*/
  lfs_ctx->file_open = 1;

  return 0;
}

/* --- Ymodem (rz/sz) ----------------------------------------------- */
int  ymodem_receive(void)          /* rz：从串口接收文件存入 FS */
{
  return 0;
}

int  ymodem_send(const char *path) /* sz：从 FS 发送文件      */
{
  return 0;
}

/* --- 设备信息 ------------------------------------------------------ */
const char *sys_get_version(void)   /* 返回版本字符串            */
{
  return "1.0.0"; /* 示例版本 */
}

uint32_t    sys_get_sn     (void)   /* 返回设备序列号            */
{
  return 0x12345678; /* 示例序列号 */
}

/* --- 无线配置 ------------------------------------------------------ */
int  wifi_set_ssid(int argc, char *argv[])
{
  return 0;
}

int  wifi_set_pass(int argc, char *argv[])
{
  return 0;
}

int  wifi_connect (int argc, char *argv[])
{
  return 0;
}

int  wifi_status  (int argc, char *argv[])
{
  return 0;
}

/* --- OTA ---------------------------------------------------------- */
int  ota_start(int argc, char *argv[])     /* 传入 URL 或文件路径       */
{
  return 0;
}
int  ota_status(int argc, char *argv[])    /* 查询 OTA 状态             */
{
  uint32_t flag = Read_Flag(EE_VAR_OTA_STATE);
  printf("OTA State: %u\r\n", flag);
  return 0;
}

#define SIZEOF(arr) (sizeof(arr) / sizeof((arr)[0]))
#define RX_BUF_SIZE 256

static volatile uint8_t  rx_buf[RX_BUF_SIZE];
static volatile uint16_t rx_head = 0;
static volatile uint16_t rx_tail = 0;

static uint8_t rx_byte;   /* HAL 中断写入目标，必须全局/static */

/* 中断回调：收到字符存入环形缓冲区 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        uint16_t next = (rx_head + 1) % RX_BUF_SIZE;
        if (next != rx_tail) {          /* 缓冲未满才写入 */
            rx_buf[rx_head] = rx_byte;
            rx_head = next;
        }
        HAL_UART_Receive_IT(&huart1, &rx_byte, 1);  /* 重新挂起 */
    }
}

// 底层完全由用户控制，Shell 不知道是 UART 还是别的
static void my_putc(char c) {
    HAL_UART_Transmit(&huart3, (uint8_t*)&c, 1, 10);
}

static char my_getc(void) {
    // 从环形缓冲区阻塞读取（shell_rx_feed 在中断里喂）
    while (rx_head == rx_tail);
    char c = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
    return c;
}


lfs_ctx_t lfs_ctx;
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
  shell_config_t shell_cfg;





  static const shell_cmd_t my_cmds[] = {
    { "ls",      "list files",        fs_cmd_ls      },
    { "cat",     "print file",        fs_cmd_cat     },
    { "write",   "write to file",     fs_cmd_write   },
    { "rm",      "remove file",       fs_cmd_rm      },
    { "mkdir",   "make directory",    fs_cmd_mkdir   },
    { "free",    "show fs usage",     fs_cmd_free    },
    { "wifi",    "wifi config",       wifi_set_ssid  },
    { "ota",     "OTA upgrade",       ota_start      },
    { "version", "show version",      ota_status     },
    { "sn",      "show serial no",    ota_status     },
  };

  memset(&transfer_cfg, 0, sizeof(transfer_cfg_t));
  memset(&lfs_ctx, 0, sizeof(lfs_ctx_t));
  memset(&ctx, 0, sizeof(ota_ctx_t));
  memset(&shell_cfg, 0, sizeof(shell_config_t));

  /* 传输配置初始化：写回调指向 LittleFS，写上下文为 lfs_ctx */
  transfer_cfg.write_cb = (storage_callback_t)lfs_storage_callback;                 // 选择存储后端的回调函数
  transfer_cfg.write_user_ctx = (void *)&lfs_ctx;              // ctx透传给回调，Xmodem 只做中转

  /* 接收入口：receive_cb 指向 Proto_Start_Receive，recv_user_ctx 传入 transfer_cfg 本身 */
  transfer_cfg.receive_cb = (receive_callback_t)Proto_Start_Receive;                 // 协议层接收入口
  transfer_cfg.recv_user_ctx = (void *)&transfer_cfg;              // 第一个参数会被传给 Proto_Start_Receive

  /* 业务相关指针引用 */
  ctx.transfer_cfg = &transfer_cfg;                         // 传输配置放入 ctx，方便 handler 访问
  ctx.resource_ctx  = &lfs_ctx;


  /*Shell 配置*/
  shell_cfg.putc = (void (*)(char))my_putc;  // 简单适配，直接调用 my_putc 发送一个字节
  shell_cfg.getc = (char (*)(void))my_getc;   // 简单适配，直接调用 my_getc 接收一个字节（阻塞）
  shell_cfg.commands = my_cmds;                    // 命令表，定义在 fs
  shell_cfg.cmd_count = SIZEOF(my_cmds);           // 命令数量
  shell_cfg.prompt = "> ";                         // 提示符
  shell_cfg.history_size = 8;                      // 历史记录条数

  lfs_init(&lfs_ctx);

  fs_cmd_init(&lfs_ctx); // 注入文件系统上下文，供 fs_cmd 使用
  

  while(HAL_UART_Receive(&huart1, &rx_byte, 1, 5000) == HAL_OK)
  {
    if(rx_byte == 's' || rx_byte == 'S')
    {
        HAL_UART_Receive_IT(&huart1, &rx_byte, 1);
        shell_init(&shell_cfg);
        shell_run();
        break;
    }
    if(rx_byte == 'f' || rx_byte == 'F')
    {
        if (lfs_ctx.mounted) {
            lfs_unmount(&lfs_ctx.lfs);
        }
        int fmt_err = lfs_format(&lfs_ctx.lfs, &my_lfs_config);
        dbg_printf("lfs_format: %d\r\n", fmt_err);
        if (fmt_err == 0) {
            int mount_err = lfs_mount(&lfs_ctx.lfs, &my_lfs_config);
            dbg_printf("lfs_mount after format: %d\r\n", mount_err);
        }
        break;
    }
  }

  HAL_UART_Receive_IT(&huart1, &rx_byte, 1);
  shell_init(&shell_cfg);
  ota_run(&ctx);

  // uint8_t rx_byte = 0;
  // dbg_printf("Press 'f' or 'F' to format external flash...\r\n");

  
  // if (HAL_UART_Receive(&huart1, &rx_byte, 1, 5000) == HAL_OK) {
  //     if (rx_byte == 'f' || rx_byte == 'F') {
  //       if (err == 0) {
  //         lfs_unmount(&lfs_ctx.lfs);
  //       }

  //       int fmt_err = lfs_format(&lfs_ctx.lfs, &my_lfs_config);
  //       dbg_printf("lfs_format: %d\r\n", fmt_err);
  //       if (fmt_err == 0) {
  //         err = lfs_mount(&lfs_ctx.lfs, &my_lfs_config);
  //        dbg_printf("lfs_mount after format: %d\r\n", err);
  //      }  
  //   }
  // }
  


  // // 4. 测试文件操作 (读写文件)

  // static uint8_t file_buffer[512];
  


  // int rv = Proto_Start_Receive(APP_B_START_ADDR, NULL);
  // if(rv > 0)
  // {
  //   dbg_printf("Xmodem transfer complete, %d bytes received.\r\n", rv);
  // }
  // else
  // {
  //   dbg_printf("Xmodem transfer failed.\r\n");
  // }
  

// uint32_t crc = 0xFFFFFFFF;
// crc = crc32_update(crc, test, 9);
// crc ^= 0xFFFFFFFF;
// dbg_printf("CRC32 = 0x%08X\r\n", crc);

  // uint8_t buf[16];
  // lfs_file_seek(&lfs_ctx.lfs, &lfs_ctx.file, 0, LFS_SEEK_SET);
  // lfs_file_read(&lfs_ctx.lfs, &lfs_ctx.file, buf, 16);
  // for(int i = 0; i < 16; i++) {
  //     dbg_printf("%02X ", buf[i]);
  // }

  // 使用串口接收时记录的真实大小，而不是lfs_file_size
  // uint32_t real_size = rv; // 从Xmodem传输结果获取，存在某个变量里
  // uint8_t buf[1024];
  // uint32_t remaining = real_size;  // ← 关键：用真实大小，不用lfs文件大小
  // uint32_t crc = 0xFFFFFFFF;

  // while (remaining > 0) {
  //     int to_read = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
  //     int read_len = lfs_file_read(&lfs_ctx.lfs, &lfs_ctx.file, buf, to_read);
  //     if (read_len <= 0) break;
  //     crc = crc32_update(crc, buf, read_len);
  //     remaining -= read_len;
  // }

  // crc ^= 0xFFFFFFFF;
  // dbg_printf("File CRC32: 0x%08X\r\n", crc);

  // // 同时打印文件实际大小
  // lfs_file_seek(&lfs_ctx.lfs, &lfs_ctx.file, 0, LFS_SEEK_END);
  // int32_t size = lfs_file_tell(&lfs_ctx.lfs, &lfs_ctx.file);
  // dbg_printf("LFS file size: %d\r\n", size);

  // lfs_file_close(&lfs_ctx.lfs, &lfs_ctx.file);

  return 0;
  /* USER CODE END 2 */
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
     ex: dbg_printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

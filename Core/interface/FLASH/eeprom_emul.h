#ifndef __EEPROM_EMUL_H
#define __EEPROM_EMUL_H

#include "stm32l4xx_hal.h"

/* ======================== 移植配置（按项目修改） ======================== */

/* EEPROM 占用 Flash 末尾 2 页（4KB） */
#define EE_PAGE0_BASE 0x0803F000U
#define EE_PAGE1_BASE 0x0803F800U

/* Flash 页大小（STM32L4 单 Bank = 2KB） */
#define EE_FLASH_PAGE_SIZE FLASH_PAGE_SIZE

/* Flash 基地址 & Bank */
#define EE_FLASH_BASE FLASH_BASE
#define EE_FLASH_BANK FLASH_BANK_1

/* ======================== 页状态定义 ======================== */
/*
 * 双状态字方案：每个页头占 2 个双字（16 字节）
 *   status1   status2     含义
 *   0xFFFF..  0xFFFF..    页已擦除 (ERASED)
 *   0xAAAA..  0xFFFF..    页正在接收数据 (RECEIVE)
 *   0xAAAA..  0x5555..    页为活跃页 (ACTIVE)
 *
 * 所有状态转换均为 1→0，无需擦除即可写入。
 */

#define EE_STATUS_ERASED  0xFFFFFFFFFFFFFFFFULL
#define EE_STATUS_RECEIVE 0xAAAAAAAAAAAAAAAAULL
#define EE_STATUS_ACTIVE  0x5555555555555555ULL

/* ======================== 条目定义 ======================== */
/*
 * 每条目 8 字节（与 STM32L4 双字编程对齐）：
 *   virt_addr(16) + reserved(16) + data(32)
 * 页头 16 字节，剩余 (2048-16)/8 = 254 个条目
 */
#define EE_ENTRY_SIZE  8U
#define EE_HEADER_SIZE 16U
#define EE_MAX_ENTRIES ((EE_FLASH_PAGE_SIZE - EE_HEADER_SIZE) / EE_ENTRY_SIZE)

/* 无效虚拟地址（擦除态标记） */
#define EE_ADDR_INVALID 0xFFFFU

/* ================================================================
 * 分区定义
 * ================================================================ */
#define SLOT_A           0x00000001U
#define SLOT_B           0x00000002U
#define OPPOSITE_SLOT(s) (((s) == SLOT_A) ? SLOT_B : SLOT_A)
#define SLOT_NAME(s)     (((s) == SLOT_B) ? "B" : "A")

/* ================================================================
 * EEPROM 变量索引（根据实际EE_VAR定义调整）
 * ================================================================ */
#define EE_VAR_OTA_STATE     0U
#define EE_VAR_ACTIVE_SLOT   1U
#define EE_VAR_TARGET_SLOT   2U
#define EE_VAR_REVERT_REASON 3U
#define EE_VAR_DEVICE_SN     4U /* 设备序列号（出厂写入）         */

#define EE_VAR_WIFI_SSID_BASE  10U /* 10~17：WiFi SSID（8×4 字节）  */
#define EE_VAR_WIFI_PASS_BASE  18U /* 18~25：WiFi 密码（8×4 字节）  */
#define EE_VAR_SLOT_A_VER_BASE 26U /* 26~33：Slot A 版本（8×4字节） */
#define EE_VAR_SLOT_B_VER_BASE 34U /* 34~41：Slot B 版本（8×4字节） */
#define EE_VAR_MQTT_HOST_BASE  42U /* 42~57：MQTT Host（16×4字节） */
#define EE_VAR_MQTT_TOKEN_BASE 58U /* 58~73：Access Token（16×4字节） */
#define EE_VAR_MQTT_PORT       74U /* 74：MQTT Port                */

/* ================================================================
 * 回退原因标志
 * ================================================================ */
#define REVERT_ACTIVE_VALID 0x00000001U /**< 升级回退，active有效   */
#define REVERT_OTHER_VALID  0x00000002U /**< 启动降级，other有效    */
#define REVERT_BOTH_INVALID 0x00000003U /**< 双区均无效             */

/* ======================== 函数声明 ======================== */

/**
 * @brief  EEPROM 初始化，检测页状态并恢复异常
 * @retval HAL_OK / HAL_ERROR
 */
HAL_StatusTypeDef EE_Init(void);

/**
 * @brief  写入变量
 * @param  virt_addr    虚拟地址 (0x0001 ~ 0xFFFE)
 * @param  flag_value   标志值  
 * @retval HAL_OK / HAL_ERROR
 */
HAL_StatusTypeDef Write_Flag(uint16_t virt_addr, uint32_t flag_value);

/**
 * @brief  读取变量
 * @param  virt_addr    虚拟地址 (0x0001 ~ 0xFFFE)
 * @param  flag_value   标志值  
 * @retval HAL_OK / HAL_ERROR
 */
uint32_t Read_Flag(uint16_t virt_addr);

#endif /* __EEPROM_EMUL_H */

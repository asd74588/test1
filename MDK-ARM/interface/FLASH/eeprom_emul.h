#ifndef __EEPROM_EMUL_H
#define __EEPROM_EMUL_H

#include "stm32l4xx_hal.h"

/* ======================== 移植配置（按项目修改） ======================== */

/* EEPROM 占用 Flash 末尾 2 页（4KB） */
#define EE_PAGE0_BASE       0x0803F000U
#define EE_PAGE1_BASE       0x0803F800U

/* Flash 页大小（STM32L4 单 Bank = 2KB） */
#define EE_FLASH_PAGE_SIZE  0x800U

/* Flash 基地址 & Bank */
#define EE_FLASH_BASE       0x08000000U
#define EE_FLASH_BANK       FLASH_BANK_1

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

#define EE_STATUS_ERASED    0xFFFFFFFFFFFFFFFFULL
#define EE_STATUS_RECEIVE   0xAAAAAAAAAAAAAAAAULL
#define EE_STATUS_ACTIVE    0x5555555555555555ULL

/* ======================== 条目定义 ======================== */
/*
 * 每条目 8 字节（与 STM32L4 双字编程对齐）：
 *   virt_addr(16) + reserved(16) + data(32)
 * 页头 16 字节，剩余 (2048-16)/8 = 254 个条目
 */
#define EE_ENTRY_SIZE       8U
#define EE_HEADER_SIZE      16U
#define EE_MAX_ENTRIES      ((EE_FLASH_PAGE_SIZE - EE_HEADER_SIZE) / EE_ENTRY_SIZE)

/* 无效虚拟地址（擦除态标记） */
#define EE_ADDR_INVALID     0xFFFFU

/* ======================== 用户虚拟地址 ======================== */
/* 用户在此定义变量地址，范围 0x0001 ~ 0xFFFE */

/* OTA 状态机 — 驱动整个 Bootloader 流程，一个变量搞定 */
#define EE_VAR_OTA_STATE            0x0001U
#define OTA_STATE_BOOT              0x00U   // 正常启动：检查分区完整性，降级回退，等待升级指令，跳转APP
#define OTA_STATE_UPGRADING         0x01U   // 升级中：Bootloader等待Xmodem接收固件
#define OTA_STATE_VERIFYING         0x02U   // 校验中：新固件已写入，校验通过则切换分区，失败则回退
#define OTA_STATE_REVERT            0x03U   // 升级回退：新分区校验失败，回到旧分区执行

/* 当前活跃分区 — 决定Bootloader跳转哪个分区 */
#define EE_VAR_ACTIVE_SLOT          0x0002U
#define SLOT_A                      0x00U   // 当前运行APP_A
#define SLOT_B                      0x01U   // 当前运行APP_B

/* 升级目标分区 — 记录正在往哪个分区写（掉电恢复用，= !ACTIVE_SLOT） */
#define EE_VAR_TARGET_SLOT          0x0003U

/* 固件大小（字节）— 由App端在请求升级时写入，用于完整性校验 */
#define EE_VAR_FIRMWARE_SIZE        0x0004U

/* 回退原因 — 进入REVERT前写入，REVERT内零Flash校验 */
#define EE_VAR_REVERT_REASON        0x0005U
#define REVERT_ACTIVE_VALID         0x00U   // 升级回退：active有效，直接回BOOT
#define REVERT_OTHER_VALID          0x01U   // 启动降级：active无效，other有效，切分区
#define REVERT_BOTH_INVALID         0x02U   // 两分区都无效，进UPGRADING

/* ======================== 函数声明 ======================== */

/**
 * @brief  EEPROM 初始化，检测页状态并恢复异常
 * @retval HAL_OK / HAL_ERROR
 */
HAL_StatusTypeDef EE_Init(void);

/**
 * @brief  格式化 EEPROM（擦除两页并将 Page0 设为 ACTIVE）
 * @retval HAL_OK / HAL_ERROR
 */
HAL_StatusTypeDef EE_Format(void);

/**
 * @brief  读取变量
 * @param  virt_addr  虚拟地址 (0x0001 ~ 0xFFFE)
 * @param  p_data     数据输出指针
 * @retval HAL_OK 成功，HAL_ERROR 未找到或参数错误
 */
HAL_StatusTypeDef EE_Read(uint16_t virt_addr, uint32_t *p_data);

/**
 * @brief  写入变量（页满时自动执行页转移）
 * @param  virt_addr  虚拟地址 (0x0001 ~ 0xFFFE)
 * @param  data       数据
 * @retval HAL_OK / HAL_ERROR
 */
HAL_StatusTypeDef EE_Write(uint16_t virt_addr, uint32_t data);

#endif /* __EEPROM_EMUL_H */

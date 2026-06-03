/**
 * @file    ota_state_machine.h
 * @brief   A/B双区OTA Bootloader — 表驱动状态机头文件
 */

#ifndef OTA_STATE_MACHINE_H
#define OTA_STATE_MACHINE_H

#include <stdint.h>

#include "global.h"
#include "xmodem.h"
#include "lfs_config.h"
#include "eeprom_emul.h"
#include "flash_bootloader.h"
/* ================================================================
 * 状态枚举
 * ================================================================ */
typedef enum {
    OTA_STATE_BOOT      = 0U,
    OTA_STATE_UPGRADING = 1U,
    OTA_STATE_VERIFYING = 2U,
    OTA_STATE_REVERT    = 3U,
    OTA_STATE_SAME      = 0xFFU,  /**< handler返回此值表示不迁移 */
} ota_state_t;


/* ================================================================
 * 类型定义
 * ================================================================ */

/** 状态机上下文：所有运行时数据集中在此结构体，禁止全局散变量 */
typedef struct {
    ota_state_t  state;           /**< 当前状态（内存镜像，与EEPROM同步） */
    uint32_t     active_slot;     /**< 当前活跃分区 SLOT_A / SLOT_B        */
    uint32_t     target_slot;     /**< 升级目标分区                         */
    uint8_t      active_valid;    /**< 活跃分区完整性（0=无效，1=有效）      */
    uint8_t      xmodem_retry;    /**< Xmodem重试计数                       */
    uint32_t     revert_reason;   /**< 回退原因标志                          */

    /* 业务相关指针，方便handler访问，非必要可删除 */
    void    *resource_ctx ;        /**< 系统资源相关上下文指针，方便handler访问，非必要可删除 */
    transfer_cfg_t *transfer_cfg;  /**< 传输回调配置，OTA状态机不关心细节，仅传递给Xmodem */

} ota_ctx_t;

/**
 * 状态表项：
 *   guard_fn   — 返回1表示允许进入本状态，返回0跳过
 *   handler_fn — 执行业务，返回期望迁移到的下一状态
 *                返回 OTA_STATE_SAME 表示留在当前状态（不做迁移）
 */
typedef struct {
    ota_state_t   state;
    uint8_t     (*guard_fn)  (const ota_ctx_t *ctx);
    ota_state_t (*handler_fn)(ota_ctx_t *ctx);
} ota_state_entry_t;




/* ================================================================
 * Xmodem 重试上限
 * ================================================================ */
#define XMODEM_MAX_RETRY    3U

/* ================================================================
 * 公共接口
 * ================================================================ */
void ota_run(ota_ctx_t *ctx);      /**< OTA主循环入口，替换main()中的while(1)+switch */

#endif /* OTA_STATE_MACHINE_H */


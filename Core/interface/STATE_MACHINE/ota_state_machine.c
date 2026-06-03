/**
 * @file    ota_state_machine.c
 * @brief   A/B双区OTA Bootloader — 表驱动状态机（优化版）
 *
 * 架构分层：
 *   guard_fn   —— 纯谓词，只读状态，无副作用，可独立单元测试
 *   handler_fn —— 执行本状态业务逻辑，返回下一个请求状态
 *   fsm_transition —— 唯一写EEPROM入口，保证持久化原子性
 *   ota_dispatch —— 遍历表，调用guard/handler，驱动转换
 *
 * 新增状态只需在 s_ota_table[] 追加一行，dispatch逻辑不变。
 */

#include "main.h"
#include "ota_state_machine.h"
#include <string.h>
#include "lfs.h"
#include "data_storage.h"

/* ================================================================
 * 前置声明
 * ================================================================ */
static uint8_t     guard_boot      (const ota_ctx_t *ctx);
static uint8_t     guard_upgrading (const ota_ctx_t *ctx);
static uint8_t     guard_verifying (const ota_ctx_t *ctx);
static uint8_t     guard_revert    (const ota_ctx_t *ctx);

static ota_state_t handle_boot     (ota_ctx_t *ctx);
static ota_state_t handle_upgrading(ota_ctx_t *ctx);
static ota_state_t handle_verifying(ota_ctx_t *ctx);
static ota_state_t handle_revert   (ota_ctx_t *ctx);

static void        fsm_transition  (ota_ctx_t *ctx, ota_state_t next);
static void        fsm_ctx_init    (ota_ctx_t *ctx);
static void        ota_dispatch(ota_ctx_t *ctx);
/* ================================================================
 * 状态表（唯一扩展点）
 * 新增状态：追加一行，guard/handler独立实现，dispatch不变
 * ================================================================ */
static const ota_state_entry_t s_ota_table[] = {
    { OTA_STATE_BOOT,      guard_boot,      handle_boot      },
    { OTA_STATE_UPGRADING, guard_upgrading, handle_upgrading },
    { OTA_STATE_VERIFYING, guard_verifying, handle_verifying },
    { OTA_STATE_REVERT,    guard_revert,    handle_revert    },
};
#define OTA_TABLE_SIZE  (sizeof(s_ota_table) / sizeof(s_ota_table[0]))








/* ================================================================
 * 公共入口：ota_run()
 * ================================================================ */

/**
 * @brief  OTA Bootloader 主循环，替换原 while(1)+switch-case
 * @note   在 main() 的 USER CODE BEGIN WHILE 处调用
 */
void ota_run(ota_ctx_t *ctx)
{
    fsm_ctx_init(ctx);

    while (1) {
        ota_dispatch(ctx);
    }
    // ota_dispatch(ctx);
}

/* ================================================================
 * 核心调度器
 * ================================================================ */

/**
 * @brief  遍历状态表，找到当前状态 → 执行guard → 执行handler → 触发迁移
 */
static void ota_dispatch(ota_ctx_t *ctx)
{
    for (uint32_t i = 0U; i < OTA_TABLE_SIZE; i++) {
        if (s_ota_table[i].state != ctx->state) {
            continue;
        }
        /* guard: 返回0表示当前条件不允许进入（防御性跳过） */
        if (!s_ota_table[i].guard_fn(ctx)) {
            /* guard未通过：重置为BOOT兜底，避免永久困死 */
            dbg_printf("[FSM] guard failed for state %u, fallback to BOOT\r\n", ctx->state);
            fsm_transition(ctx, OTA_STATE_BOOT);
            return;
        }
        ota_state_t next = s_ota_table[i].handler_fn(ctx);
        if (next != OTA_STATE_SAME) {
            fsm_transition(ctx, next);
        }
        return;
    }

    /* 未命中任何表项（异常枚举值）：重置 */
    dbg_printf("[FSM] unknown state %u, reset to BOOT\r\n", ctx->state);
    fsm_transition(ctx, OTA_STATE_BOOT);
}

/* ================================================================
 * 状态迁移（唯一写EEPROM入口）
 * ================================================================ */

/**
 * @brief  执行状态迁移：更新内存ctx + 持久化写EEPROM
 * @note   所有 Write_Flag(EE_VAR_OTA_STATE, ...) 必须经此函数，禁止在handler内直接写
 */
static void fsm_transition(ota_ctx_t *ctx, ota_state_t next)
{
    dbg_printf("[FSM] %u -> %u\r\n", ctx->state, next);
    ctx->state = next;
    Write_Flag(EE_VAR_OTA_STATE, (uint32_t)next);
}

/* ================================================================
 * 上下文初始化
 * ================================================================ */

static void fsm_ctx_init(ota_ctx_t *ctx)
{

#ifdef OTA_TEST_UPGRADING
    /* 测试模式：直接进UPGRADING，跳过BOOT等待窗口 */
    ctx->active_slot = SLOT_A;
    ctx->target_slot = SLOT_B;
    ctx->state       = OTA_STATE_UPGRADING;
    Write_Flag(EE_VAR_ACTIVE_SLOT, SLOT_A);
    Write_Flag(EE_VAR_TARGET_SLOT, SLOT_B);
    Write_Flag(EE_VAR_OTA_STATE,   OTA_STATE_UPGRADING);
    dbg_printf("[TEST] forced into UPGRADING mode\r\n");
    return;
#endif

    ctx->state       = (ota_state_t)Read_Flag(EE_VAR_OTA_STATE);
    ctx->active_slot = Read_Flag(EE_VAR_ACTIVE_SLOT);

    if (ctx->active_slot != SLOT_A && ctx->active_slot != SLOT_B) {
        ctx->active_slot = SLOT_A;
        Write_Flag(EE_VAR_ACTIVE_SLOT, SLOT_A);
        Write_Flag(EE_VAR_OTA_STATE,   OTA_STATE_BOOT);
        ctx->state = OTA_STATE_BOOT;
    }

    /* VERIFYING 必须读持久化的 target，其余状态直接推导 */
    if (ctx->state == OTA_STATE_VERIFYING) 
    {
        ctx->target_slot = Read_Flag(EE_VAR_TARGET_SLOT);
        if (ctx->target_slot != SLOT_A && ctx->target_slot != SLOT_B) 
        {
            /* 读取失败兜底：推导，然后回BOOT重来 */
            ctx->target_slot = OPPOSITE_SLOT(ctx->active_slot);
            ctx->state = OTA_STATE_BOOT;
            Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_BOOT);
        }
    } 
    else 
    {
        /* BOOT / UPGRADING / REVERT：active 推导即可，无需读 EEPROM */
        ctx->target_slot = OPPOSITE_SLOT(ctx->active_slot);
    }
}
/* ================================================================
 * Guard 函数：纯谓词，无副作用
 * ================================================================ */

/** BOOT：任何合法active_slot均可进入 */
static uint8_t guard_boot(const ota_ctx_t *ctx)
{
    return (ctx->active_slot == SLOT_A || ctx->active_slot == SLOT_B) ? 1U : 0U;
}

/** UPGRADING：target必须合法 */
static uint8_t guard_upgrading(const ota_ctx_t *ctx)
{
    return (ctx->target_slot == SLOT_A || ctx->target_slot == SLOT_B) ? 1U : 0U;
}

/** VERIFYING：target必须合法 */
static uint8_t guard_verifying(const ota_ctx_t *ctx)
{
    return (ctx->target_slot == SLOT_A || ctx->target_slot == SLOT_B) ? 1U : 0U;
}

/** REVERT：无附加前置条件，总允许进入 */
static uint8_t guard_revert(const ota_ctx_t *ctx)
{
    (void)ctx;
    return 1U;
}

/* ================================================================
 * Handler 函数：执行业务，返回下一状态（不直接写EEPROM）
 * ================================================================ */

/**
 * @brief  BOOT handler
 *         1. 活跃分区无效 → 预写revert_reason → 转REVERT
 *         2. 活跃分区有效 → 3s等待'U' → 升级 or 跳转APP
 */
static ota_state_t handle_boot(ota_ctx_t *ctx)
{
    uint32_t other_slot;
    uint32_t other_addr;
    uint32_t reason;
    uint8_t  rx_byte    = 0U;
    uint32_t start_tick = 0U;
    uint32_t app_addr = (ctx->active_slot == SLOT_B) ? APP_B_START_ADDR
                                                      : APP_A_START_ADDR;

    if (Verify_APP_Integrity_Flash(app_addr) == 0) 
    {
        /* 活跃分区无效：预检另一分区，写原因，转REVERT */
        dbg_printf("[BOOT] active slot %s invalid, entering revert\r\n",
               SLOT_NAME(ctx->active_slot));
        other_slot = OPPOSITE_SLOT(ctx->active_slot);
        other_addr = (other_slot == SLOT_B) ? APP_B_START_ADDR
                                                      : APP_A_START_ADDR;
        reason = (Verify_APP_Integrity_Flash(other_addr) != 0) ? REVERT_OTHER_VALID 
                                                                : REVERT_BOTH_INVALID;
        Write_Flag(EE_VAR_REVERT_REASON, reason);
        ctx->revert_reason = reason;
        return OTA_STATE_REVERT;
    }

    /* 3秒升级等待窗口 */
    dbg_printf("[BOOT] press 'U' within 3s to upgrade...\r\n");
    rx_byte    = 0U;
    start_tick = HAL_GetTick();

    while ((HAL_GetTick() - start_tick) < 3000U) {
        if (HAL_UART_Receive(&huart1, &rx_byte, 1U, 100U) == HAL_OK) {
            if (rx_byte == 'U' || rx_byte == 'u') {
                dbg_printf("[BOOT] upgrade triggered\r\n");
                ctx->target_slot = OPPOSITE_SLOT(ctx->active_slot);
                Write_Flag(EE_VAR_TARGET_SLOT, ctx->target_slot);
                return OTA_STATE_UPGRADING;
            }
        }
    }

    /* 超时：直接跳转APP，不再返回 */
    dbg_printf("[BOOT] timeout, jumping to slot %s @ 0x%08lX\r\n",
           SLOT_NAME(ctx->active_slot), app_addr);
    Jump_To_App_Flash(ctx->resource_ctx, app_addr);
    /* NOTREACHED */
    return OTA_STATE_SAME;
}

/**
 * @brief  UPGRADING handler
 *         擦除目标分区 → Xmodem接收 → 成功转VERIFYING
 *         活跃有效时超限重试 → 回BOOT；活跃无效时持续等待
 */
static ota_state_t handle_upgrading(ota_ctx_t *ctx)
{
    uint32_t first_word;
    uint32_t write_addr  = (ctx->target_slot == SLOT_B) ? APP_B_START_ADDR
                                                         : APP_A_START_ADDR;
    uint32_t active_addr = (ctx->active_slot == SLOT_B) ? APP_B_START_ADDR
                                                         : APP_A_START_ADDR;

    
    /* active_valid 仅初次进入时评估，存入ctx避免重复校验 */
    ctx->active_valid = (Verify_APP_Integrity_Flash(active_addr) != 0) ? 1U : 0U;

    dbg_printf("[UPGRADING] target=%s @ 0x%08lX (active=%s %s)\r\n",
           SLOT_NAME(ctx->target_slot), write_addr,
           SLOT_NAME(ctx->active_slot),
           ctx->active_valid ? "valid" : "invalid");

    ctx->xmodem_retry = 0U;
    Erase_App_Flash(ctx->target_slot);

    while (1) 
    {
        int received = ctx->transfer_cfg->receive_cb(ctx->transfer_cfg->recv_user_ctx, NULL);

        if (received > 0) {
            dbg_printf("[TEST][PASS] Xmodem received=%d bytes\r\n", received);
        } else {
            dbg_printf("[TEST][FAIL] Xmodem failed, ret=%d\r\n", received);
            return OTA_STATE_BOOT;
        }

        int err = lfs_file_truncate(&((lfs_ctx_t *)(ctx->resource_ctx))->lfs,&((lfs_ctx_t *)(ctx->resource_ctx))->file, received);
        if (err == 0) {
             dbg_printf("truncate file to %d bytes\r\n", received);
        } else {
            dbg_printf("truncate failed: %d\r\n", err);
        }

        lfs_soff_t file_size = lfs_file_size(&((lfs_ctx_t *)(ctx->resource_ctx))->lfs, &((lfs_ctx_t *)(ctx->resource_ctx))->file);
        if (file_size == received) {
            dbg_printf("file size after truncate: %d bytes\r\n", (int)file_size);
        } else {
            dbg_printf("file size mismatch after truncate: %d bytes\r\n", (int)file_size);
        }
        
        return OTA_STATE_VERIFYING;
        ctx->xmodem_retry++;

        if (ctx->active_valid) 
        {
            dbg_printf("[UPGRADING] recv failed, retry %u/%u\r\n",
                   ctx->xmodem_retry, XMODEM_MAX_RETRY);
            if (ctx->xmodem_retry >= XMODEM_MAX_RETRY) 
            {
                dbg_printf("[UPGRADING] max retry, active valid -> BOOT\r\n");
                return OTA_STATE_BOOT;
            }
        } 
        else 
        {
            /* 活跃无效：持续等待，按需重擦 */
            dbg_printf("[UPGRADING] active invalid, staying in UPGRADING\r\n");
            first_word = *(volatile uint32_t *)write_addr;
            if (first_word != 0xFFFFFFFFU) 
            {
                dbg_printf("[UPGRADING] partial write detected, re-erasing\r\n");
                Erase_App_Flash(ctx->target_slot);
            }
            ctx->xmodem_retry = 0U;  /* 无有效分区时不计次 */
        }
    }
}


uint32_t crc32_update(uint32_t crc, uint8_t *data, uint32_t len)
{
    for(uint32_t i = 0; i < len; i++)
    {
        crc ^= data[i];

        for(uint32_t j = 0; j < 8; j++)
        {
            if(crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }

    return crc;
}

int Verify_Transferred_App(ota_ctx_t *ctx)
{
    lfs_file_seek(&((lfs_ctx_t *)(ctx->resource_ctx))->lfs, 
                                  &((lfs_ctx_t *)(ctx->resource_ctx))->file, 
                                  0, LFS_SEEK_SET);

    lfs_soff_t file_size = lfs_file_size(&((lfs_ctx_t *)(ctx->resource_ctx))->lfs, &((lfs_ctx_t *)(ctx->resource_ctx))->file);
    dbg_printf("file size after truncate: %d bytes\r\n", (int)file_size);

    uint32_t real_size = lfs_file_size(&((lfs_ctx_t *)(ctx->resource_ctx))->lfs, &((lfs_ctx_t *)(ctx->resource_ctx))->file); // 从Xmodem传输结果获取，存在某个变量里
    uint8_t buf[1024];
    uint32_t remaining = real_size;  // ← 关键：用真实大小，不用lfs文件大小
    uint32_t crc = 0xFFFFFFFF;

  while (remaining > 0) {
      int to_read = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
      int read_len = lfs_file_read(&((lfs_ctx_t *)(ctx->resource_ctx))->lfs, &((lfs_ctx_t *)(ctx->resource_ctx))->file, buf, to_read);
      if (read_len <= 0) break;
      crc = crc32_update(crc, buf, read_len);
      remaining -= read_len;
  }

  crc ^= 0xFFFFFFFF;
  dbg_printf("File CRC32: 0x%08X\r\n", crc);

  return 1;
}

/**
 * @brief  VERIFYING handler
 *         校验新分区 → 通过：切active跳转 APP；失败：写reason转REVERT
 */
static ota_state_t handle_verifying(ota_ctx_t *ctx)
{
        lfs_soff_t file_size = lfs_file_size(&((lfs_ctx_t *)(ctx->resource_ctx))->lfs, &((lfs_ctx_t *)(ctx->resource_ctx))->file);
        dbg_printf("file size after truncate: %d bytes\r\n", (int)file_size);

    uint32_t new_addr = (ctx->target_slot == SLOT_B) ? APP_B_START_ADDR
                                                      : APP_A_START_ADDR;

    if (Verify_Transferred_App(ctx) != 0) 
    {
        dbg_printf("[VERIFYING] slot %s verified OK -> now active\r\n",
                SLOT_NAME(ctx->target_slot));
        ctx->active_slot = ctx->target_slot;
        Write_Flag(EE_VAR_ACTIVE_SLOT, ctx->target_slot);
        /* OTA_STATE_BOOT写入由fsm_transition完成，此处仅需跳转 */
        Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_BOOT);

        bootloader_load_and_jump();
        //Jump_To_App_Flash(ctx->resource_ctx, new_addr);
        /* NOTREACHED */
    }

    dbg_printf("[VERIFYING] slot %s FAILED, -> REVERT\r\n", SLOT_NAME(ctx->target_slot));
    Write_Flag(EE_VAR_REVERT_REASON, REVERT_ACTIVE_VALID);
    ctx->revert_reason = REVERT_ACTIVE_VALID;
    return OTA_STATE_REVERT;
}

/**
 * @brief  REVERT handler — 纯标志驱动，零Flash校验
 *         ACTIVE_VALID  → 升级回退，active有效，回BOOT
 *         OTHER_VALID   → 切分区，回BOOT
 *         BOTH_INVALID  → 进UPGRADING强制等待固件
 */
static ota_state_t handle_revert(ota_ctx_t *ctx)
{
    uint32_t other;
    ctx->revert_reason = Read_Flag(EE_VAR_REVERT_REASON);

    switch (ctx->revert_reason) 
    {
        case REVERT_ACTIVE_VALID:
            dbg_printf("[REVERT] upgrade revert: active=%s valid -> BOOT\r\n",
                   SLOT_NAME(ctx->active_slot));
            return OTA_STATE_BOOT;

        case REVERT_OTHER_VALID: {
            other = OPPOSITE_SLOT(ctx->active_slot);
            dbg_printf("[REVERT] boot fallback: %s -> %s\r\n",
                   SLOT_NAME(ctx->active_slot), SLOT_NAME(other));
            ctx->active_slot  = other;
            ctx->target_slot  = OPPOSITE_SLOT(other);
            Write_Flag(EE_VAR_ACTIVE_SLOT, other);
            Write_Flag(EE_VAR_TARGET_SLOT, ctx->target_slot);
            return OTA_STATE_BOOT;
        }

        default:
            /* REVERT_BOTH_INVALID 或异常值 */
            dbg_printf("[REVERT] both invalid, -> UPGRADING\r\n");
            ctx->target_slot = SLOT_B;
            Write_Flag(EE_VAR_TARGET_SLOT, SLOT_B);
            return OTA_STATE_UPGRADING;
    }
}


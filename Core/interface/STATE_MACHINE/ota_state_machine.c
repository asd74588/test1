/**
 * @file    ota_state_machine.c
 * @brief   A/B双区OTA Bootloader — 表驱动状态机（优化版）
 *
 * 架构分层：
 *   guard_fn   —— 纯谓词，只读状态，无副作用，可独立单元测试
 *   handler_fn —— 执行本状态业务逻辑，返回下一个请求状态
 *   fsm_transition —— 常规状态迁移与持久化入口
 *   commit_verified_target —— 新镜像装载完成后的提交入口
 *   ota_dispatch —— 遍历表，调用guard/handler，驱动转换
 *
 * 新增状态只需在 s_ota_table[] 追加一行，dispatch逻辑不变。
 */

#include "main.h"
#include "ota_state_machine.h"
#include <string.h>
#include "lfs.h"
#include "data_storage.h"
#include "flash_bootloader.h"
#include "app_verify.h"
#include "log_config.h"
#include "cmds.h"

#if LOG_OTA_STATE_MACHINE_ENABLE
#include <stdio.h>
#define dbg_printf(format,args...) printf(format, ##args)
#else
#define dbg_printf(format,args...) do{}while(0)
#endif

#define OTA_MAX_PROTOCOL_PADDING  1023U

extern const struct lfs_file_config lfs_file_cfg;
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
static int         commit_verified_target(ota_ctx_t *ctx);
static int         normalize_received_package(lfs_ctx_t *fs,
                                              const char *path,
                                              int received,
                                              const YmodemFileInfo *file_info);
static const char *ctx_ota_path(const ota_ctx_t *ctx);
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
 * 常规状态迁移
 * ================================================================ */

/**
 * @brief  执行状态迁移：更新内存ctx + 持久化写EEPROM
 * @note   普通状态迁移经此函数；VERIFYING 成功提交由 commit_verified_target() 处理
 */
static void fsm_transition(ota_ctx_t *ctx, ota_state_t next)
{
    dbg_printf("[FSM] %u -> %u\r\n", ctx->state, next);
    ctx->state = next;
    Write_Flag(EE_VAR_OTA_STATE, (uint32_t)next);
}

/*
 * 先写 BOOT、后写 active。若两次写入之间掉电，设备仍会按旧 active
 * 启动；新镜像可能暂时不生效，但不会把未提交的 target 当作活动固件。
 */
static int commit_verified_target(ota_ctx_t *ctx)
{
    if (Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_BOOT) != HAL_OK) {
        dbg_printf("[VERIFYING] failed to persist BOOT state\r\n");
        return -1;
    }

    if (Write_Flag(EE_VAR_ACTIVE_SLOT, ctx->target_slot) != HAL_OK) {
        dbg_printf("[VERIFYING] failed to persist active slot\r\n");
        return -1;
    }

    ctx->state = OTA_STATE_BOOT;
    ctx->active_slot = ctx->target_slot;
    return 0;
}

static const char *ctx_ota_path(const ota_ctx_t *ctx)
{
    if (ctx != NULL && ctx->ota_file_path[0] != '\0') {
        return ctx->ota_file_path;
    }

    return "a.elf";
}

/*
 * 将协议写入的物理文件规范为 Firmware_Header_t + hdr.size。
 * Xmodem 的最后一帧可能包含 0x1A 填充，因此物理文件允许比逻辑包长
 * 多至一个帧尾；任何短包、额外有效数据或非法填充都在这里拒绝。
 */
static int normalize_received_package(lfs_ctx_t *fs,
                                      const char *path,
                                      int received,
                                      const YmodemFileInfo *file_info)
{
    static Firmware_Header_t hdr;
    lfs_soff_t stored_size = -1;
    uint32_t expected_size = 0U;
    uint32_t padding_size;
    int err;
    int result = -1;

    if (fs == NULL || path == NULL || path[0] == '\0' || received <= 0) {
        return -1;
    }

    if (fs->file_open != 0U) {
        err = lfs_file_close(&fs->lfs, &fs->file);
        fs->file_open = 0U;
        if (err != LFS_ERR_OK) {
            return -1;
        }
    }

    err = lfs_file_opencfg(&fs->lfs, &fs->file, path,
                           LFS_O_RDWR, &lfs_file_cfg);
    if (err != LFS_ERR_OK) {
        return -1;
    }
    fs->file_open = 1U;

    stored_size = lfs_file_size(&fs->lfs, &fs->file);
    if (stored_size < (lfs_soff_t)sizeof(Firmware_Header_t)) {
        goto out;
    }

    if (lfs_file_seek(&fs->lfs, &fs->file, 0, LFS_SEEK_SET) < 0 ||
        lfs_file_read(&fs->lfs, &fs->file, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        goto out;
    }

    if (hdr.magic != FW_MAGIC) {
        goto out;
    }

    if (hdr.size == 0U ||
        hdr.size > ((uint32_t)LFS_FILE_MAX - (uint32_t)sizeof(Firmware_Header_t))) {
        goto out;
    }

    expected_size = (uint32_t)sizeof(Firmware_Header_t) + hdr.size;
    if (stored_size < (lfs_soff_t)expected_size) {
        goto out;
    }

    if (file_info != NULL && file_info->filesize != 0U &&
        file_info->filesize != expected_size) {
        goto out;
    }

    if ((uint32_t)received > expected_size) {
        goto out;
    }

    if ((uint32_t)received < expected_size) {
        uint32_t recovered = expected_size - (uint32_t)received;
        if ((file_info != NULL && file_info->filesize != 0U) ||
            recovered > 1024U) {
            goto out;
        }
    }

    /* received 已剥除末尾连续 0x1A，因此额外物理字节只可能是末帧填充。 */
    padding_size = (uint32_t)stored_size - expected_size;
    if (padding_size > OTA_MAX_PROTOCOL_PADDING) {
        goto out;
    }

    err = lfs_file_truncate(&fs->lfs, &fs->file, (lfs_off_t)expected_size);
    if (err != LFS_ERR_OK) {
        goto out;
    }

    result = 0;

out:
    err = lfs_file_close(&fs->lfs, &fs->file);
    fs->file_open = 0U;
    if (err != LFS_ERR_OK) {
        result = -1;
    }

    return result;
}

/* ================================================================
 * 上下文初始化
 * ================================================================ */


 
static void fsm_ctx_init(ota_ctx_t *ctx)
{
    memset(ctx->ota_file_path, 0, sizeof(ctx->ota_file_path));
    strncpy(ctx->ota_file_path, "a.elf", sizeof(ctx->ota_file_path) - 1U);
    memset(ctx->ota_target_version, 0, sizeof(ctx->ota_target_version));
    ctx->skip_boot_window_once = 0U;

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
#else
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
#endif
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
    return ((ctx->target_slot == SLOT_A || ctx->target_slot == SLOT_B) &&
            ctx->target_slot != ctx->active_slot) ? 1U : 0U;
}

/** VERIFYING：target必须合法 */
static uint8_t guard_verifying(const ota_ctx_t *ctx)
{
    return ((ctx->target_slot == SLOT_A || ctx->target_slot == SLOT_B) &&
            ctx->target_slot != ctx->active_slot) ? 1U : 0U;
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

    if (ctx->skip_boot_window_once != 0U) {
        ctx->skip_boot_window_once = 0U;
        dbg_printf("[BOOT] skip upgrade window once, jumping to slot %s @ 0x%08lX\r\n",
                   SLOT_NAME(ctx->active_slot), app_addr);
        Jump_To_App_Flash(ctx->resource_ctx, app_addr);
        return OTA_STATE_SAME;
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
    firmware_verify_status_t verify_status;
    static YmodemFileInfo file_info;
    const char *ota_path = ctx_ota_path(ctx);
    uint32_t write_addr  = (ctx->target_slot == SLOT_B) ? APP_B_START_ADDR
                                                         : APP_A_START_ADDR;
#if !LOG_OTA_STATE_MACHINE_ENABLE
    (void)write_addr;
#endif
    uint32_t active_addr = (ctx->active_slot == SLOT_B) ? APP_B_START_ADDR
                                                         : APP_A_START_ADDR;
    lfs_ctx_t *fs = (lfs_ctx_t *)ctx->resource_ctx;

    
    /* active_valid 仅初次进入时评估，存入ctx避免重复校验 */
    ctx->active_valid = (Verify_APP_Integrity_Flash(active_addr) != 0) ? 1U : 0U;

    dbg_printf("[UPGRADING] target=%s @ 0x%08lX (active=%s %s)\r\n",
           SLOT_NAME(ctx->target_slot), write_addr,
           SLOT_NAME(ctx->active_slot),
           ctx->active_valid ? "valid" : "invalid");

    ctx->xmodem_retry = 0U;

    while (1) 
    {
        if (fs == NULL || fs->mounted == 0U) {
            dbg_printf("[UPGRADING] LittleFS not ready\r\n");
            return ctx->active_valid ? OTA_STATE_BOOT : OTA_STATE_UPGRADING;
        }

        if (ctx->transfer_cfg->write_cb != NULL) {
            if (fs->file_open) {
                lfs_file_close(&fs->lfs, &fs->file);
                fs->file_open = 0U;
            }

            int open_err = lfs_file_opencfg(&fs->lfs, &fs->file, ota_path,
                                            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC,
                                            &lfs_file_cfg);
            if (open_err != 0) {
                dbg_printf("[UPGRADING] open %s failed: %d\r\n", ota_path, open_err);
                return ctx->active_valid ? OTA_STATE_BOOT : OTA_STATE_UPGRADING;
            }
            fs->file_open = 1U;
        }

        memset(&file_info, 0, sizeof(file_info));
        int received = ctx->transfer_cfg->receive_cb(ctx->transfer_cfg->recv_user_ctx,
                                                     &file_info);

        if (file_info.filename[0] != '\0') {
            memset(ctx->ota_file_path, 0, sizeof(ctx->ota_file_path));
            strncpy(ctx->ota_file_path, file_info.filename, sizeof(ctx->ota_file_path) - 1U);
            ota_path = ctx_ota_path(ctx);
        }

        if (received > 0) {
            dbg_printf("[UPGRADING] received=%d bytes\r\n", received);
        } else if (received == 0) {
            dbg_printf("[UPGRADING] no new firmware received\r\n");
            ctx->skip_boot_window_once = 1U;
            if (fs->file_open) {
                lfs_file_close(&fs->lfs, &fs->file);
                fs->file_open = 0U;
            }
            lfs_remove(&fs->lfs, ota_path);
            return ctx->active_valid ? OTA_STATE_BOOT : OTA_STATE_UPGRADING;
        } else {
            dbg_printf("[UPGRADING] transfer failed, ret=%d\r\n", received);
            if (fs->file_open) {
                lfs_file_close(&fs->lfs, &fs->file);
                fs->file_open = 0U;
            }
            lfs_remove(&fs->lfs, ota_path);
            return ctx->active_valid ? OTA_STATE_BOOT : OTA_STATE_UPGRADING;
        }

        if (ctx->transfer_cfg->write_cb != NULL) {
            if (normalize_received_package(fs, ota_path, received, &file_info) != 0) {
                dbg_printf("[UPGRADING] received package length validation failed\r\n");
                Write_Flag(EE_VAR_REVERT_REASON,
                           ctx->active_valid ? REVERT_ACTIVE_VALID : REVERT_BOTH_INVALID);
                return ctx->active_valid ? OTA_STATE_REVERT : OTA_STATE_UPGRADING;
            }
        } else {
            dbg_printf("[UPGRADING] exact-size transport, skip protocol padding normalize\r\n");
        }
        
        dbg_printf("[UPGRADING] verify firmware begin\r\n");
        verify_status = verify_firmware(ota_path);
        dbg_printf("[UPGRADING] verify firmware end: %d\r\n", (int)verify_status);
        if (verify_status != FIRMWARE_VERIFY_OK)
        {
            dbg_printf("Failed to verify firmware: %d\r\n", (int)verify_status);
            Write_Flag(EE_VAR_REVERT_REASON,
                       ctx->active_valid ? REVERT_ACTIVE_VALID : REVERT_BOTH_INVALID);
            return ctx->active_valid ? OTA_STATE_REVERT : OTA_STATE_UPGRADING;
        }

        return OTA_STATE_VERIFYING;
    }
}


/**
 * @brief  VERIFYING handler
 *         验签 → ELF装载 → 校验目标分区 → 提交active → 跳转
 */
static ota_state_t handle_verifying(ota_ctx_t *ctx)
{
    firmware_verify_status_t verify_status;
    bootloader_load_status_t load_status;
    const char *ota_path = ctx_ota_path(ctx);
    uint32_t active_addr = (ctx->active_slot == SLOT_B) ? APP_B_START_ADDR
                                                         : APP_A_START_ADDR;
    uint32_t target_addr = (ctx->target_slot == SLOT_B) ? APP_B_START_ADDR
                                                         : APP_A_START_ADDR;

    verify_status = verify_firmware(ota_path);
    if (verify_status != FIRMWARE_VERIFY_OK) {
        dbg_printf("[VERIFYING] firmware verification failed: %d\r\n",
                   (int)verify_status);
        goto load_failed;
    }

    load_status = bootloader_load_target((uint8_t)ctx->target_slot, ota_path);
    if (load_status != BOOTLOADER_LOAD_OK) {
        dbg_printf("[VERIFYING] target load failed: %d\r\n", (int)load_status);
        goto load_failed;
    }

    if (ctx->ota_target_version[0] != '\0') {
        if (sys_set_slot_version(ctx->target_slot, ctx->ota_target_version) != 0) {
            dbg_printf("[VERIFYING] persist slot %s version [%s] failed\r\n",
                       SLOT_NAME(ctx->target_slot),
                       ctx->ota_target_version);
            goto load_failed;
        }
        dbg_printf("[VERIFYING] slot %s version updated to [%s]\r\n",
                   SLOT_NAME(ctx->target_slot),
                   ctx->ota_target_version);
    }

    if (commit_verified_target(ctx) != 0) {
        dbg_printf("[VERIFYING] target commit failed\r\n");
        goto load_failed;
    }

    dbg_printf("[VERIFYING] slot %s committed, jumping to App\r\n",
               SLOT_NAME(ctx->active_slot));
    if (Jump_To_App_Flash(ctx->resource_ctx, target_addr) == 0) {
        /* 目标已提交后若跳转前复检异常，REVERT_OTHER_VALID 会切回旧分区。 */
        ctx->revert_reason = (Verify_APP_Integrity_Flash(active_addr) != 0)
                             ? REVERT_OTHER_VALID : REVERT_BOTH_INVALID;
        Write_Flag(EE_VAR_REVERT_REASON, ctx->revert_reason);
        return OTA_STATE_REVERT;
    }

    /* NOTREACHED */
    return OTA_STATE_SAME;

load_failed:
    ctx->revert_reason = (Verify_APP_Integrity_Flash(active_addr) != 0)
                         ? REVERT_ACTIVE_VALID : REVERT_BOTH_INVALID;
    Write_Flag(EE_VAR_REVERT_REASON, ctx->revert_reason);
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
            ctx->target_slot = OPPOSITE_SLOT(ctx->active_slot);
            Write_Flag(EE_VAR_TARGET_SLOT, ctx->target_slot);
            return OTA_STATE_UPGRADING;
    }
}

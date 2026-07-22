/**
 * @file  cmds_ota.c
 * @brief OTA 命令：ota status / slot / boot / revert / trigger / start
 *        以及设备信息：sys_get_version / sys_get_sn
 *
 * ota_start 流程：
 *   1. 解析目标 slot（从参数或 EE_VAR_TARGET_SLOT）
 *   2. 打开 LittleFS 上的固件文件
 *   3. 读取文件内容写入目标 slot 的 Flash 分区
 *   4. 写 EE_VAR_OTA_STATE = UPGRADING，触发 bootloader 搬运
 *   5. 复位
 *
 * Flash 分区地址（根据实际 scatter 文件调整）：
 *   Slot A: APP_A_START_ADDR  APP_A_SIZE
 *   Slot B: APP_B_START_ADDR  APP_B_SIZE
 */

#include "cmds.h"
#include "lfs.h"
#include <string.h>
#include <stdlib.h>

extern uint32_t          Read_Flag(uint16_t virt_addr);
extern HAL_StatusTypeDef Write_Flag(uint16_t virt_addr, uint32_t value);
extern lfs_ctx_t         lfs_ctx;

#define FW_VERSION_EE_WORDS 8U
#define FW_VERSION_STR_LEN  32U

/* ---- Flash 分区表（根据实际芯片/scatter 调整）---------------- */
#define SLOT_B_FLASH_ADDR     APP_B_START_ADDR
#define SLOT_B_FLASH_SIZE     APP_B_SIZE
#define SHELL_FLASH_PAGE_SIZE 2048U /* STM32L431 每页 2KB */

static int parse_slot_arg(const char *arg, uint32_t *slot)
{
    if (arg == NULL || slot == NULL)
    {
        return -1;
    }

    if (arg[0] == 'A' || arg[0] == 'a')
    {
        *slot = SLOT_A;
        return 0;
    }

    if (arg[0] == 'B' || arg[0] == 'b')
    {
        *slot = SLOT_B;
        return 0;
    }

    uint32_t value = (uint32_t)strtoul(arg, NULL, 0);
    if (value == SLOT_A || value == SLOT_B)
    {
        *slot = value;
        return 0;
    }

    return -1;
}

/* ================================================================
 * 设备信息
 * ================================================================ */

/**
 * 固件版本字符串（由编译期 -DFW_VERSION_STR="x.y.z" 注入）
 */
static uint16_t version_slot_base(uint32_t slot)
{
    if (slot == SLOT_A)
    {
        return EE_VAR_SLOT_A_VER_BASE;
    }

    if (slot == SLOT_B)
    {
        return EE_VAR_SLOT_B_VER_BASE;
    }

    return EE_ADDR_INVALID;
}

static void ee_write_fixed_str(uint16_t base, const char *s, uint32_t max_len)
{
    char     buf[FW_VERSION_STR_LEN];
    uint32_t words = (max_len + 3U) / 4U;

    memset(buf, 0, sizeof(buf));
    if (s != NULL)
    {
        strncpy(buf, s, sizeof(buf) - 1U);
    }

    for (uint32_t i = 0U; i < words && i < FW_VERSION_EE_WORDS; i++)
    {
        uint32_t val = 0U;
        memcpy(&val, &buf[i * 4U], 4U);
        Write_Flag((uint16_t)(base + i), val);
    }
}

static void ee_read_fixed_str(uint16_t base, char *out, uint32_t out_sz)
{
    char buf[FW_VERSION_STR_LEN];

    memset(buf, 0, sizeof(buf));
    for (uint32_t i = 0U; i < FW_VERSION_EE_WORDS; i++)
    {
        uint32_t val = Read_Flag((uint16_t)(base + i));
        memcpy(&buf[i * 4U], &val, 4U);
    }

    if (out == NULL || out_sz == 0U)
    {
        return;
    }

    strncpy(out, buf, out_sz - 1U);
    out[out_sz - 1U] = '\0';
}

int sys_get_slot_version(uint32_t slot, char *out, uint32_t out_sz)
{
    uint16_t base = version_slot_base(slot);

    if (base == EE_ADDR_INVALID || out == NULL || out_sz == 0U)
    {
        return -1;
    }

    ee_read_fixed_str(base, out, out_sz);
    return (out[0] != '\0') ? 0 : -2;
}

int sys_set_slot_version(uint32_t slot, const char *version)
{
    uint16_t base = version_slot_base(slot);

    if (base == EE_ADDR_INVALID || version == NULL || version[0] == '\0')
    {
        return -1;
    }

    ee_write_fixed_str(base, version, FW_VERSION_STR_LEN);
    return 0;
}

const char *sys_get_version(void)
{
    static char slot_version[FW_VERSION_STR_LEN];
    uint32_t    active_slot = Read_Flag(EE_VAR_ACTIVE_SLOT);

#ifndef FW_VERSION_STR
#define FW_VERSION_STR "1.0.0"
#endif

    memset(slot_version, 0, sizeof(slot_version));
    if (sys_get_slot_version(active_slot, slot_version, sizeof(slot_version)) == 0)
    {
        return slot_version;
    }

    return FW_VERSION_STR;
}

/**
 * 设备序列号：从 EEPROM 读取
 * 虚拟地址定义在 cmds.h（EE_VAR_DEVICE_SN），
 * 出厂时由产线工具写入。
 */
uint32_t sys_get_sn(void)
{
    return Read_Flag(EE_VAR_DEVICE_SN);
}

/* ================================================================
 * 内部工具：Flash 写入
 * 使用 STM32 HAL Flash 接口，按页擦除后逐 doubleword 编程。
 * ================================================================ */

static int flash_write_slot(uint32_t flash_addr, const uint8_t *data, uint32_t len)
{
    if (flash_addr < SLOT_B_FLASH_ADDR || flash_addr + len > SLOT_B_FLASH_ADDR + SLOT_B_FLASH_SIZE)
    {
        shell_printf("Flash addr out of Slot B range\r\n");
        return -1;
    }

    HAL_FLASH_Unlock();

    /* 按页擦除 */
    uint32_t pages    = (len + SHELL_FLASH_PAGE_SIZE - 1U) / SHELL_FLASH_PAGE_SIZE;
    uint32_t page_num = (flash_addr - 0x08000000U) / SHELL_FLASH_PAGE_SIZE;

    FLASH_EraseInitTypeDef erase = {
        .TypeErase = FLASH_TYPEERASE_PAGES,
        .Banks     = FLASH_BANK_1,
        .Page      = page_num,
        .NbPages   = pages,
    };
    uint32_t page_err = 0U;
    if (HAL_FLASHEx_Erase(&erase, &page_err) != HAL_OK)
    {
        HAL_FLASH_Lock();
        shell_printf("Flash erase error, page=%lu\r\n", (unsigned long)page_err);
        return -1;
    }

    /* 逐 doubleword（8 字节）编程 */
    static uint8_t prog_buf[8];
    for (uint32_t off = 0U; off < len; off += 8U)
    {
        memset(prog_buf, 0xFF, 8U);
        uint32_t chunk = ((len - off) < 8U) ? (len - off) : 8U;
        memcpy(prog_buf, data + off, chunk);

        uint64_t dw;
        memcpy(&dw, prog_buf, 8U);

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, flash_addr + off, dw) != HAL_OK)
        {
            HAL_FLASH_Lock();
            shell_printf("Flash program error at 0x%08lX\r\n", (unsigned long)(flash_addr + off));
            return -1;
        }
    }

    HAL_FLASH_Lock();
    return 0;
}

/* ================================================================
 * OTA 状态辅助
 * ================================================================ */
static const char *ota_state_name(uint32_t s)
{
    switch (s)
    {
        case OTA_STATE_BOOT:
            return "BOOT";
        case OTA_STATE_UPGRADING:
            return "UPGRADING";
        case OTA_STATE_VERIFYING:
            return "VERIFYING";
        case OTA_STATE_REVERT:
            return "REVERT";
        default:
            return "UNKNOWN";
    }
}

/* ================================================================
 * 命令实现
 * ================================================================ */

int cmd_ota_status(uint8_t argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint32_t state  = Read_Flag(EE_VAR_OTA_STATE);
    uint32_t active = Read_Flag(EE_VAR_ACTIVE_SLOT);
    uint32_t target = Read_Flag(EE_VAR_TARGET_SLOT);
    uint32_t reason = Read_Flag(EE_VAR_REVERT_REASON);

    shell_printf("\r\n--- OTA status ---\r\n");
    shell_printf("  ota_state     = %u (%s)\r\n", state, ota_state_name(state));
    shell_printf("  active_slot   = Slot%s (%u)\r\n", active == SLOT_A ? "A" : "B", active);
    shell_printf("  target_slot   = Slot%s (%u)\r\n", target == SLOT_A ? "A" : "B", target);
    shell_printf("  revert_reason = %u\r\n", reason);
    shell_printf("  fw_version    = %s\r\n", sys_get_version());
    shell_printf("  device_sn     = 0x%08lX\r\n", (unsigned long)sys_get_sn());
    shell_printf("------------------\r\n");
    return 0;
}

int cmd_ota_slot(uint8_t argc, char **argv)
{
    if (argc < 2)
    {
        shell_printf("Usage: ota slot <A|B|1|2>\r\n");
        return -1;
    }
    uint32_t slot = 0U;
    if (parse_slot_arg(argv[1], &slot) != 0)
    {
        shell_printf("ERROR: slot must be A/B or 1/2\r\n");
        return -1;
    }
    if (Write_Flag(EE_VAR_TARGET_SLOT, slot) != HAL_OK)
    {
        shell_printf("ERROR: write failed\r\n");
        return -1;
    }
    shell_printf("target_slot = Slot%s  OK\r\n", slot == SLOT_A ? "A" : "B");
    return 0;
}

int cmd_ota_boot(uint8_t argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint32_t state = Read_Flag(EE_VAR_OTA_STATE);
    if (state != OTA_STATE_VERIFYING)
        shell_printf("WARN: ota_state = %u (%s), not VERIFYING\r\n", state, ota_state_name(state));

    if (Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_BOOT) != HAL_OK)
    {
        shell_printf("ERROR: write failed\r\n");
        return -1;
    }
    shell_printf("ota_state -> BOOT (experimental command)\r\n");
    return 0;
}

int cmd_ota_revert(uint8_t argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint32_t active = Read_Flag(EE_VAR_ACTIVE_SLOT);
    uint32_t target = (active == SLOT_A) ? SLOT_B : SLOT_A;

    if (Write_Flag(EE_VAR_TARGET_SLOT, target) != HAL_OK ||
        Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_REVERT) != HAL_OK)
    {
        shell_printf("ERROR: write failed\r\n");
        return -1;
    }
    shell_printf("Revert: Slot%s -> Slot%s. Rebooting...\r\n",
                 active == SLOT_A ? "A" : "B",
                 target == SLOT_A ? "A" : "B");
    HAL_Delay(50U);
    NVIC_SystemReset();
    return 0;
}

int cmd_ota_trigger(uint8_t argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint32_t target = Read_Flag(EE_VAR_TARGET_SLOT);
    if (Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_UPGRADING) != HAL_OK)
    {
        shell_printf("ERROR: write failed\r\n");
        return -1;
    }
    shell_printf("OTA trigger: target=Slot%s, ota_state->UPGRADING. Rebooting...\r\n",
                 target == SLOT_A ? "A" : "B");
    HAL_Delay(50U);
    NVIC_SystemReset();
    return 0;
}

/**
 * ota start <path> [slot]
 *
 * 从 LittleFS 读取固件文件，烧写到目标 slot 的 Flash，
 * 然后设置 UPGRADING 并复位，由 bootloader 完成切换。
 *
 * 示例：
 *   ota start /fw/app_v2.bin        → 烧写到 EE_VAR_TARGET_SLOT 指定的 slot
 *   ota start /fw/app_v2.bin B      → 强制烧写到 Slot B
 */
int cmd_ota_start(uint8_t argc, char **argv)
{
    if (argc < 2)
    {
        shell_printf("Usage: ota start <path> [slot]\r\n");
        return -1;
    }

    const char *path        = argv[1];
    uint32_t    target_slot = Read_Flag(EE_VAR_TARGET_SLOT);

    if (argc >= 3 && parse_slot_arg(argv[2], &target_slot) != 0)
    {
        shell_printf("ERROR: invalid slot %lu\r\n", (unsigned long)strtoul(argv[2], NULL, 0));
        return -1;
    }

    if (target_slot != SLOT_A && target_slot != SLOT_B)
    {
        target_slot = SLOT_B;
    }

    /* 当前只支持写 Slot B（不允许自己覆盖自己运行的分区）*/
    uint32_t active = Read_Flag(EE_VAR_ACTIVE_SLOT);
    if (target_slot == active)
    {
        shell_printf("ERROR: target slot == active slot, refused.\r\n");
        return -1;
    }

    /* ── 打开固件文件 ────────────────────────────────────── */
    lfs_file_t file;
    int        err = lfs_file_open(&lfs_ctx.lfs, &file, path, LFS_O_RDONLY);
    if (err < 0)
    {
        shell_printf("LFS open error: %d  path=%s\r\n", err, path);
        return -1;
    }

    lfs_soff_t fsize = lfs_file_size(&lfs_ctx.lfs, &file);
    if (fsize <= 0 || (uint32_t)fsize > SLOT_B_FLASH_SIZE)
    {
        lfs_file_close(&lfs_ctx.lfs, &file);
        shell_printf("Invalid file size: %ld\r\n", (long)fsize);
        return -1;
    }

    shell_printf("OTA start: %s -> Slot%s (%ld bytes)\r\n",
                 path,
                 target_slot == SLOT_A ? "A" : "B",
                 (long)fsize);

    /* ── 分块读取并写 Flash ──────────────────────────────── */
    static uint8_t fbuf[SHELL_FLASH_PAGE_SIZE];
    uint32_t       flash_addr = SLOT_B_FLASH_ADDR; /* 目前只支持 Slot B */
    uint32_t       total      = 0U;
    int            ret        = 0;

    /* 先整体擦除目标分区 */
    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef erase = {
        .TypeErase = FLASH_TYPEERASE_PAGES,
        .Banks     = FLASH_BANK_1,
        .Page      = (SLOT_B_FLASH_ADDR - 0x08000000U) / SHELL_FLASH_PAGE_SIZE,
        .NbPages   = SLOT_B_FLASH_SIZE / SHELL_FLASH_PAGE_SIZE,
    };
    uint32_t page_err = 0U;
    if (HAL_FLASHEx_Erase(&erase, &page_err) != HAL_OK)
    {
        HAL_FLASH_Lock();
        lfs_file_close(&lfs_ctx.lfs, &file);
        shell_printf("Flash erase failed, page=%lu\r\n", (unsigned long)page_err);
        return -1;
    }
    HAL_FLASH_Lock();

    /* 逐块读写 */
    while (total < (uint32_t)fsize)
    {
        lfs_ssize_t nread = lfs_file_read(&lfs_ctx.lfs, &file, fbuf, sizeof(fbuf));
        if (nread < 0)
        {
            shell_printf("LFS read error: %d\r\n", (int)nread);
            ret = -1;
            break;
        }
        if (nread == 0)
            break;

        if (flash_write_slot(flash_addr + total, fbuf, (uint32_t)nread) < 0)
        {
            ret = -1;
            break;
        }
        total += (uint32_t)nread;

        /* 进度（每 8KB 打一个点）*/
        if ((total % (8U * 1024U)) == 0U)
            shell_printf(".");
    }
    shell_printf("\r\n");

    lfs_file_close(&lfs_ctx.lfs, &file);

    if (ret < 0)
    {
        shell_printf("OTA write failed after %lu bytes.\r\n", (unsigned long)total);
        return -1;
    }

    shell_printf("Flash written: %lu bytes  OK\r\n", (unsigned long)total);

    /* ── 写 EEPROM，触发 bootloader ──────────────────────── */
    Write_Flag(EE_VAR_TARGET_SLOT, target_slot);
    Write_Flag(EE_VAR_OTA_STATE, OTA_STATE_UPGRADING);

    shell_printf("ota_state -> UPGRADING. Rebooting...\r\n");
    HAL_Delay(50U);
    NVIC_SystemReset();
    return 0;
}

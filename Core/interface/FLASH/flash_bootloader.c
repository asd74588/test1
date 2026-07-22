
/**
 * flash_bootloader.c — STM32L431 + LittleFS Bootloader
 *
 * 职责：
 *   1. Erase_App_Flash()          擦除指定 slot 的 App Flash 分区
 *   2. Write_Buffer_To_Flash()    将任意长度数据写入 Flash（8字节对齐处理）
 *   3. Verify_APP_Integrity_Flash() 校验 Flash 中是否有合法固件
 *   4. Jump_To_App_Flash()        清理外设并跳转到 App
 *   5. bootloader_load_target()   读ELF→解析→重定向→烧写→校验目标分区
 */

#include "flash_bootloader.h"
#include "eeprom_emul.h"
#include "elf_loader.h"
#include "global.h"
#include "lfs_config.h"
#include "lfs.h"
#include "log_config.h"
#include <string.h>
#include <stdio.h>

/* ===================================================================
 * 调试输出
 * =================================================================== */
#define BL_PREFIX "[boot] "
#if LOG_BOOTLOADER_ENABLE
#define BL_INFO(fmt, ...) printf(BL_PREFIX fmt "\r\n", ##__VA_ARGS__)
#define BL_ERR(fmt, ...)  printf(BL_PREFIX "ERROR: " fmt "\r\n", ##__VA_ARGS__)
#define BL_WARN(fmt, ...) printf(BL_PREFIX "WARN: " fmt "\r\n", ##__VA_ARGS__)
#else
#define BL_INFO(...) ((void)0)
#define BL_ERR(...)  ((void)0)
#define BL_WARN(...) ((void)0)
#endif

/* ===================================================================
 * Internal RAM buffers
 *
 * elf_buf and elf_reloc_scratch are ordinary .bss objects. The scatter
 * linker places them in SRAM1/SRAM2; no fixed RAM address is required.
 * Their memory can be reused by the App after the bootloader jumps.
 * =================================================================== */
#define ELF_BUF_SIZE (40U * 1024U)

/* One bitmap bit per 32-bit ELF word for fallback relocation scanning. */
#define ELF_RELOC_SCRATCH_SIZE ((ELF_BUF_SIZE + 31U) / 32U)

/* ===================================================================
 * ELF buffer（全局 .bss，启动时已清零，4字节对齐）
 * =================================================================== */
static uint8_t elf_buf[ELF_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t elf_reloc_scratch[ELF_RELOC_SCRATCH_SIZE] __attribute__((aligned(4)));

/* ===================================================================
 * 尾字节暂存（Write_Buffer_To_Flash 跨包拼接用）
 * =================================================================== */
static uint8_t s_tail_bytes[8] = {0};
static uint8_t s_tail_len      = 0U;

/* ===================================================================
 * LittleFS 句柄（外部初始化）
 * =================================================================== */
extern lfs_ctx_t                    lfs_ctx;
extern const struct lfs_file_config lfs_file_cfg;

/* ===================================================================
 * 第一部分：Flash 擦写接口
 * =================================================================== */

/**
 * Erase_App_Flash() — 擦除指定 slot 的 App 分区
 *
 * @param slot  SLOT_A 或 SLOT_B
 * @return 0 成功，-1 失败
 */
int Erase_App_Flash(uint8_t slot)
{
    FLASH_EraseInitTypeDef eraseInitStruct;
    uint32_t               PageError = 0;

    uint32_t startAddr;
    uint32_t appSize;
    if (slot == SLOT_B)
    {
        startAddr = APP_B_START_ADDR;
        appSize   = APP_B_SIZE;
    }
    else
    {
        startAddr = APP_A_START_ADDR;
        appSize   = APP_A_SIZE;
    }

    uint32_t startPage = (startAddr - FLASH_BASE) / FLASH_PAGE_SIZE;
    uint32_t appPages  = appSize / FLASH_PAGE_SIZE;

    BL_INFO("Erase_App_Flash: slot=%s addr=0x%08x pages=%u~%u",
            slot == SLOT_B ? "B" : "A",
            startAddr,
            startPage,
            startPage + appPages - 1U);

    eraseInitStruct.TypeErase = FLASH_TYPEERASE_PAGES;
    eraseInitStruct.Banks     = FLASH_BANK_1;
    eraseInitStruct.Page      = startPage;
    eraseInitStruct.NbPages   = appPages;

    HAL_FLASH_Unlock();
    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&eraseInitStruct, &PageError);
    HAL_FLASH_Lock();

    if (status != HAL_OK)
    {
        BL_ERR("Erase_App_Flash: HAL_FLASHEx_Erase failed at page %u", PageError);
        return -1;
    }

    BL_INFO("Erase_App_Flash: OK");
    return 0;
}

/**
 * Write_Buffer_To_Flash() — 将 size 字节数据写入 Flash
 *
 * STM32L4 要求 64-bit（8字节）对齐写入。
 * 跨包调用时，上一包末尾不足 8 字节的数据通过 s_tail_bytes 暂存，
 * 由下一包开头补齐后再写入。
 *
 * 注意：第一次调用前应将 s_tail_len 清零（reset_tail_state() 或直接赋0）。
 * 最后一包写完后如仍有尾字节残留，需调用 Flush_Tail_To_Flash() 冲洗。
 *
 * @param startaddr  [in/out] 当前写入 Flash 地址，写入后自动推进
 * @param buffer     源数据指针
 * @param size       本次写入字节数
 * @return HAL_OK 成功，其他失败
 */
HAL_StatusTypeDef Write_Buffer_To_Flash(uint32_t *startaddr, const uint8_t *buffer, uint32_t size)
{
    if (startaddr == NULL || buffer == NULL || size == 0U)
    {
        return HAL_ERROR;
    }

    HAL_StatusTypeDef status = HAL_OK;
    uint64_t          value  = 0U;
    uint32_t          idx    = 0U;

    HAL_FLASH_Unlock();

    /* ---- 先把上一包遗留的尾字节补齐 ---- */
    if (s_tail_len > 0U)
    {
        uint32_t need     = 8U - s_tail_len;
        uint32_t copy_len = (size < need) ? size : need;

        memcpy(&s_tail_bytes[s_tail_len], buffer, copy_len);
        s_tail_len += (uint8_t)copy_len;
        idx += copy_len;

        if (s_tail_len == 8U)
        {
            memcpy(&value, s_tail_bytes, 8U);
            status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, *startaddr, value);
            if (status != HAL_OK)
            {
                HAL_FLASH_Lock();
                BL_ERR("Write_Buffer_To_Flash: write failed at 0x%08x", *startaddr);
                return status;
            }
            *startaddr += 8U;
            s_tail_len = 0U;
        }
        /* 本包数据全部用于补尾，且凑不够 8 字节，等下一包 */
        if (idx >= size)
        {
            HAL_FLASH_Lock();
            return HAL_OK;
        }
    }

    /* ---- 写完整的 8 字节块 ---- */
    for (; (idx + 8U) <= size; idx += 8U)
    {
        memcpy(&value, buffer + idx, 8U);
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, *startaddr, value);
        if (status != HAL_OK)
        {
            HAL_FLASH_Lock();
            BL_ERR("Write_Buffer_To_Flash: write failed at 0x%08x", *startaddr);
            return status;
        }
        *startaddr += 8U;
    }

    /* ---- 暂存本包末尾不足 8 字节的数据 ---- */
    if (idx < size)
    {
        uint32_t remain = size - idx;
        memcpy(s_tail_bytes, buffer + idx, remain);
        s_tail_len = (uint8_t)remain;
    }

    HAL_FLASH_Lock();
    return HAL_OK;
}

/**
 * Flush_Tail_To_Flash() — 冲洗最后一包遗留的尾字节
 *
 * 最后一个 section 写完后调用。
 * 不足 8 字节的部分用 0xFF 填充（Flash 擦后默认值，等效于不写）。
 *
 * @param startaddr  [in/out] 当前写入地址，写入后推进
 * @return HAL_OK 成功，其他失败
 */
HAL_StatusTypeDef Flush_Tail_To_Flash(uint32_t *startaddr)
{
    if (s_tail_len == 0U)
    {
        return HAL_OK;
    }

    /* 末尾补 0xFF */
    memset(&s_tail_bytes[s_tail_len], 0xFF, 8U - s_tail_len);

    uint64_t value = 0U;
    memcpy(&value, s_tail_bytes, 8U);

    HAL_FLASH_Unlock();
    HAL_StatusTypeDef status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, *startaddr, value);
    HAL_FLASH_Lock();

    if (status != HAL_OK)
    {
        BL_ERR("Flush_Tail_To_Flash: write failed at 0x%08x", *startaddr);
        return status;
    }

    *startaddr += 8U;
    s_tail_len = 0U;
    memset(s_tail_bytes, 0, sizeof(s_tail_bytes));

    BL_INFO("Flush_Tail_To_Flash: OK");
    return HAL_OK;
}

/* ===================================================================
 * 第二部分：App 完整性校验
 * =================================================================== */

/**
 * Verify_APP_Integrity_Flash() — 检查 Flash 中是否有合法 App
 *
 * 规则：
 *   1. 向量表[0]（初始 MSP）必须在 STM32L431 SRAM1/SRAM2 范围内
 *   2. 向量表[1]（Reset_Handler）必须在当前 App 分区内，且 bit0=1（Thumb）
 *
 * @param appaddr  App 分区起始地址（向量表地址）
 * @return 1 合法，0 非法
 */
int Verify_APP_Integrity_Flash(uint32_t appaddr)
{
    uint32_t app_size;
    uint32_t msp;
    uint32_t reset_handler;

    if (appaddr == APP_A_START_ADDR)
    {
        app_size = APP_A_SIZE;
    }
    else if (appaddr == APP_B_START_ADDR)
    {
        app_size = APP_B_SIZE;
    }
    else
    {
        BL_WARN("  unsupported App base 0x%08x", appaddr);
        return 0;
    }

    msp           = *(__IO uint32_t *)(appaddr);
    reset_handler = *(__IO uint32_t *)(appaddr + 4U);

    /* 初始 MSP 允许等于 RAM 尾地址，因为栈按递减方向生长。 */
    int msp_valid =
        (((msp > 0x20000000U && msp <= 0x2000C000U) || (msp > 0x10000000U && msp <= 0x10004000U)) &&
         ((msp & 0x7U) == 0U));

    if (!msp_valid)
    {
        BL_WARN("  invalid MSP 0x%08x (not in RAM)", msp);
        return 0;
    }

    if ((reset_handler & 0x1U) == 0U)
    {
        BL_WARN("  Reset_Handler 0x%08x not Thumb (bit0=0)", reset_handler);
        return 0;
    }

    uint32_t handler_addr = reset_handler & ~0x1U;
    if (handler_addr < appaddr || handler_addr >= (appaddr + app_size))
    {
        BL_WARN("  Reset_Handler 0x%08x out of App range 0x%08x~0x%08x",
                reset_handler,
                appaddr,
                appaddr + app_size - 1U);
        return 0;
    }

    BL_INFO("  App integrity OK");
    return 1;
}

/* ===================================================================
 * 第三部分：外设清理 + 跳转
 * =================================================================== */

/**
 * Jump_To_App_Flash() — 清理外设并跳转到指定地址的 App
 *
 * 操作顺序：
 *   1. 校验 App 合法性
 *   2. 关闭 LittleFS 文件 / 卸载文件系统
 *   3. 关全局中断
 *   4. 反初始化各外设（UART1/DMA、SPI1、UART3）
 *   5. 关 SysTick
 *   6. 复位时钟树
 *   7. 清除所有 NVIC 使能和挂起位
 *   8. 设置 VTOR
 *   9. 设置 MSP，恢复线程特权模式
 *  10. 开中断，跳转
 *
 * @param resource_ctx  lfs_ctx_t 指针，用于关闭文件系统
 * @param appaddr       App 向量表起始地址
 * @return 0 校验失败（正常跳转后不返回）
 */
int Jump_To_App_Flash(void *resource_ctx, uint32_t appaddr)
{
    if (Verify_APP_Integrity_Flash(appaddr) == 0)
    {
        BL_ERR("Jump_To_App_Flash: App integrity check failed");
        return 0;
    }

    BL_INFO("Jump_To_App_Flash: preparing to jump to 0x%08x", appaddr);

    /* ---- 关闭 LittleFS ---- */
    lfs_ctx_t *lfs_ctx_ptr = (lfs_ctx_t *)resource_ctx;
    if (lfs_ctx_ptr != NULL)
    {
        if (lfs_ctx_ptr->file_open)
        {
            lfs_file_close(&lfs_ctx_ptr->lfs, &lfs_ctx_ptr->file);
            lfs_ctx_ptr->file_open = 0U;
            BL_INFO("  LittleFS file closed");
        }
        if (lfs_ctx_ptr->mounted)
        {
            lfs_unmount(&lfs_ctx_ptr->lfs);
            lfs_ctx_ptr->mounted = 0U;
            BL_INFO("  LittleFS unmounted");
        }
    }

    /* ---- 关闭外设，恢复到干净状态 ---- */
    __disable_irq();

    /* 1. 反初始化外设
     *    HAL_UART_DeInit 内部调用 HAL_UART_MspDeInit，
     *    自动完成 DMA 反初始化、NVIC 禁用、GPIO 复位、关闭外设时钟 */
    HAL_UART_DeInit(&huart1);
    HAL_UART_DeInit(&huart3);
    HAL_SPI_DeInit(&hspi1);

    /* 2. 复位时钟树到上电默认状态
     *    必须在 HAL_DeInit() 之前调用，因为 HAL_RCC_DeInit() 内部
     *    使用 HAL_GetTick() 做超时判断，需要 SysTick 仍在运行。
     *    HAL_RCC_DeInit 将：
     *    - SYSCLK 切回 MSI 4MHz
     *    - 关闭 HSE/HSI/PLL
     *    - 清零 PLLCFGR（防止 App 误判"PLL 配置未变"跳过重配）
     *    - 清除 RCC 中断 */
    HAL_RCC_DeInit();

    /* 3. 反初始化 HAL（停止 SysTick、清零 HAL 状态） */
    HAL_DeInit();

    /* 4. 清除所有 NVIC 中断使能和挂起位（兜底，防止遗漏） */
    for (uint32_t i = 0; i < 8; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    /* Set VTOR and MSP, then jump */
    SCB->VTOR = appaddr;
    __DSB();
    __ISB();

    __set_MSP(*(__IO uint32_t *)appaddr);
    __ISB();

    uint32_t app_entry = *(__IO uint32_t *)(appaddr + 4U);
    /* Keep the Thumb LSB bit set in the entry address. Clearing it causes
         an invalid processor state (UsageFault INVSTATE) on Cortex-M. */
    void (*jump_to_app)(void) = (void (*)(void))app_entry;
    /* 不在此处 __enable_irq()：中断由 App Reset Handler 负责开启。
     * 若在此处开中断，VTOR/MSP 已切换但尚未跳转的窗口期内，
     * 中断会以 Bootloader 栈帧进入 App 向量表，造成不可预测行为。 */
    jump_to_app();

    /* 不可达 */
    while (1)
    {
    }
}

/* ===================================================================
 * 第四部分：从 LittleFS 读取 ELF（内部函数）
 * =================================================================== */

static int read_elf_from_lfs(const char *path, uint8_t *buf, uint32_t buf_size, uint32_t *out_size)
{
    Firmware_Header_t hdr;
    int               err;
    uint32_t          total_read = 0U;

    if (path == NULL || buf == NULL || out_size == NULL || buf_size == 0U)
    {
        return -1;
    }
    *out_size = 0U;

    BL_INFO("opening \"%s\" from LittleFS...", path);

    if (lfs_ctx.file_open == 0U)
    {
        err = lfs_file_opencfg(&lfs_ctx.lfs, &lfs_ctx.file, path, LFS_O_RDONLY, &lfs_file_cfg);
        lfs_ctx.file_open = (err == LFS_ERR_OK) ? 1U : 0U;
        if (err < 0)
        {
            BL_ERR("lfs_file_opencfg failed: %d", err);
            return err;
        }
    }

    /* 文件总大小 */
    lfs_soff_t fsize = lfs_file_size(&lfs_ctx.lfs, &lfs_ctx.file);
    BL_INFO("file size = %d bytes", (int)fsize);

    if (fsize < (lfs_soff_t)sizeof(Firmware_Header_t))
    {
        BL_ERR("file too small: %d bytes", (int)fsize);
        lfs_file_close(&lfs_ctx.lfs, &lfs_ctx.file);
        lfs_ctx.file_open = 0U;
        return -1;
    }

    if (lfs_file_seek(&lfs_ctx.lfs, &lfs_ctx.file, 0, LFS_SEEK_SET) < 0 ||
        lfs_file_read(&lfs_ctx.lfs, &lfs_ctx.file, &hdr, sizeof(hdr)) != sizeof(hdr))
    {
        BL_ERR("failed to read firmware header");
        lfs_file_close(&lfs_ctx.lfs, &lfs_ctx.file);
        lfs_ctx.file_open = 0U;
        return -1;
    }

    if (hdr.magic != FW_MAGIC || hdr.size == 0U ||
        hdr.size > ((uint32_t)LFS_FILE_MAX - (uint32_t)sizeof(Firmware_Header_t)) ||
        fsize != (lfs_soff_t)((uint32_t)sizeof(Firmware_Header_t) + hdr.size))
    {
        BL_ERR(
            "invalid header/file size: payload=%lu file=%d", (unsigned long)hdr.size, (int)fsize);
        lfs_file_close(&lfs_ctx.lfs, &lfs_ctx.file);
        lfs_ctx.file_open = 0U;
        return -1;
    }

    /* hdr.size is the exact ELF boundary established by package verification. */
    if (lfs_file_seek(&lfs_ctx.lfs, &lfs_ctx.file, sizeof(Firmware_Header_t), LFS_SEEK_SET) < 0)
    {
        BL_ERR("failed to seek to ELF payload");
        lfs_file_close(&lfs_ctx.lfs, &lfs_ctx.file);
        lfs_ctx.file_open = 0U;
        return -1;
    }

    uint32_t payload_size = hdr.size;
    BL_INFO("payload size from header = %lu bytes", (unsigned long)payload_size);

    if (payload_size > buf_size)
    {
        BL_ERR("payload too large: %lu > %u bytes", (unsigned long)payload_size, buf_size);
        lfs_file_close(&lfs_ctx.lfs, &lfs_ctx.file);
        lfs_ctx.file_open = 0U;
        return -1;
    }

    BL_INFO("reading complete ELF to internal RAM 0x%08X...", (uint32_t)(uintptr_t)buf);

    while (total_read < payload_size)
    {
        lfs_size_t  request = (lfs_size_t)(payload_size - total_read);
        lfs_ssize_t nread   = lfs_file_read(&lfs_ctx.lfs, &lfs_ctx.file, buf + total_read, request);
        if (nread <= 0)
        {
            BL_ERR("read stopped at %u / %u bytes (err=%d)", total_read, payload_size, (int)nread);
            lfs_file_close(&lfs_ctx.lfs, &lfs_ctx.file);
            lfs_ctx.file_open = 0U;
            return -1;
        }
        total_read += (uint32_t)nread;
    }
    lfs_file_close(&lfs_ctx.lfs, &lfs_ctx.file);
    lfs_ctx.file_open = 0U;

    if (total_read != payload_size)
    {
        BL_ERR("read incomplete: %u / %u bytes", total_read, payload_size);
        return -1;
    }

    *out_size = total_read;
    BL_INFO("read OK: %u bytes", *out_size);
    return 0;
}

static int write_sections_to_flash(elf_ctx_t *ctx, uint8_t slot)
{
    uint32_t flash_start = (slot == SLOT_B) ? APP_B_START_ADDR : APP_A_START_ADDR;
    uint32_t flash_size  = (slot == SLOT_B) ? APP_B_SIZE : APP_A_SIZE;
    uint32_t flash_end   = flash_start + flash_size;
    int32_t  offset      = ctx->offset;

    if (Erase_App_Flash(slot) != 0)
    {
        BL_ERR("flash erase failed");
        return -1;
    }

    s_tail_len = 0U;
    memset(s_tail_bytes, 0, sizeof(s_tail_bytes));

    /*
     * prev_shdr: 上一个 Flash section 的节头（提供 sh_addralign）
     * prev_dst:  上一个 Flash section 写入 Flash 的起始地址
     * 两者配合可计算 RAM section（.data）的 LMA：
     *   prev_flash_end = prev_dst + prev_shdr->sh_size
     */
    elf32_shdr *prev_shdr    = NULL;
    uint32_t    prev_dst     = flash_start;
    uint32_t    last_written = flash_start; /* 用于最终打印 */

    for (uint32_t i = 0; i < ctx->load_count; i++)
    {
        elf32_shdr *shdr = ctx->load_shdrs[i];

        /* SHT_NOBITS (.bss) 不占文件空间，无需写 Flash */
        if (shdr->sh_type == SHT_NOBITS)
            continue;
        /* 前面入表时已过滤，这里是双重保险 */
        if (!(shdr->sh_flags & SHF_ALLOC))
            continue;
        if (shdr->sh_size == 0U)
            continue;

        const char *name = (ctx->shstrtab && shdr->sh_name) ? ctx->shstrtab + shdr->sh_name : "(?)";
#if !LOG_BOOTLOADER_ENABLE
        (void)name;
#endif

        /* -------------------------------------------------------
         * 判断 Flash section 还是 RAM section
         *   RAM section：sh_addr 是 VMA（SRAM 地址），≥ 0x10000000
         *   Flash section：sh_addr 是链接地址（VMA == LMA），< 0x10000000
         *                  或 ET_EXEC 时在 Flash 范围（0x08000000+）
         * ------------------------------------------------------- */
        int      is_ram = (shdr->sh_addr >= 0x10000000U);
        uint32_t dst;

        if (!is_ram)
        {
            /* Flash section：dst = sh_addr + offset
             * 0-based ELF：offset = app_start，sh_addr 是从 0 起的偏移
             * ET_EXEC ELF：offset = app_start - link_base，sh_addr 是绝对地址
             * 两种情况同一个公式均正确 */
            dst = (uint32_t)((int32_t)shdr->sh_addr + offset);
        }
        else
        {
            /* RAM section (.data)：sh_addr 是 VMA（SRAM 地址），需推算 LMA
             *
             * 链接脚本约定：.data 的 LMA 紧跟上一个 Flash section 之后，
             * 双重对齐取整：
             *   Step 1：上一 Flash section 末尾按其自身对齐取整
             *   Step 2：再按 .data 的对齐要求取整 → LMA
             */
            if (prev_shdr == NULL)
            {
                BL_ERR("RAM section \"%s\" has no preceding Flash section", name);
                return -1;
            }

            uint32_t prev_flash_end = prev_dst + prev_shdr->sh_size;

            uint32_t prev_align = prev_shdr->sh_addralign;
            if (prev_align < 1U)
                prev_align = 1U;
            uint32_t aligned_end = (prev_flash_end + prev_align - 1U) & ~(prev_align - 1U);

            uint32_t data_align = shdr->sh_addralign;
            if (data_align < 1U)
                data_align = 1U;
            dst = (aligned_end + data_align - 1U) & ~(data_align - 1U);
        }

        /* 刷新上一 section 遗留的尾字节（< 8 字节的未对齐余量）
         * 必须在写新 section 之前执行，否则尾字节会挂在错误地址 */
        if (s_tail_len > 0U)
        {
            uint32_t flush_addr = prev_dst + (prev_shdr ? prev_shdr->sh_size : 0U);
            if (Flush_Tail_To_Flash(&flush_addr) != HAL_OK)
            {
                BL_ERR("tail flush failed before \"%s\"", name);
                return -1;
            }
        }

        /* Flash 范围检查 */
        if (dst < flash_start || dst >= flash_end || shdr->sh_size > (flash_end - dst))
        {
            BL_ERR("section \"%s\" out of flash: dst=0x%08x size=%u end=0x%08x",
                   name,
                   dst,
                   shdr->sh_size,
                   flash_end);
            return -1;
        }

        BL_INFO("  \"%s\" [%s]: file+0x%05x  %u bytes  -> flash 0x%08x",
                name,
                is_ram ? "RAM→LMA" : "Flash",
                shdr->sh_offset,
                shdr->sh_size,
                dst);

        /* 写入 Flash */
        uint8_t *src        = elf_buf + shdr->sh_offset;
        uint32_t write_addr = dst;
        if (Write_Buffer_To_Flash(&write_addr, src, shdr->sh_size) != HAL_OK)
        {
            BL_ERR("flash write FAILED for \"%s\"", name);
            return -1;
        }

        last_written = write_addr; /* Write_Buffer_To_Flash 会推进 write_addr */

        /* 仅 Flash section 更新 prev，RAM section 不影响 LMA 推算链 */
        if (!is_ram)
        {
            prev_shdr = shdr;
            prev_dst  = dst;
        }
    }

    /* 刷新最后遗留的尾字节 */
    if (s_tail_len > 0U)
    {
        if (Flush_Tail_To_Flash(&last_written) != HAL_OK)
        {
            BL_ERR("final tail flush FAILED");
            return -1;
        }
    }

    BL_INFO("write OK: flash 0x%08x ~ 0x%08x (%u bytes)",
            flash_start,
            last_written - 1U,
            last_written - flash_start);
    return 0;
}

/* ===================================================================
 * 第六部分：目标分区装载流程
 * =================================================================== */

/**
 * bootloader_load_target() — 将 LittleFS 中的 ELF 装载到目标分区
 *
 * 调用前提：
 *   - lfs_ctx 已挂载
 *   - UART/printf 可用
 *
 * 本函数只执行仍可能失败的步骤，不修改 OTA active/state，也不跳转。
 * 状态机必须在本函数返回成功后再提交 active，然后调用跳转接口。
 */
bootloader_load_status_t bootloader_load_target(uint8_t target_slot, const char *path)
{
    BL_INFO("elf_buf actual address: 0x%08x", (uint32_t)(uintptr_t)elf_buf);
    /* ---- 打印上次复位原因，便于定位 APP 是否触发了复位 ---- */
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST))
        BL_INFO("Reset cause: IWDG");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST))
        BL_INFO("Reset cause: WWDG");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_LPWRRST))
        BL_INFO("Reset cause: LowPower");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST))
        BL_INFO("Reset cause: BOR");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))
        BL_INFO("Reset cause: PIN/PWR");
#ifdef RCC_FLAG_PORRST
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST))
        BL_INFO("Reset cause: POR");
#endif
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))
        BL_INFO("Reset cause: Software");
    __HAL_RCC_CLEAR_RESET_FLAGS();
    int       ret;
    uint32_t  elf_size = 0U;
    uint32_t  entry    = 0U;
    elf_ctx_t ctx;
    uint32_t  link_base    = UINT32_MAX;
    int32_t   reloc_offset = 0;
    uint32_t  app_max      = 0U;

    if (path == NULL || path[0] == '\0')
    {
        BL_ERR("invalid OTA file path");
        return BOOTLOADER_LOAD_ERR_READ;
    }

    if (target_slot != SLOT_A && target_slot != SLOT_B)
    {
        BL_ERR("invalid target slot: %u", target_slot);
        return BOOTLOADER_LOAD_ERR_SLOT;
    }

    uint32_t app_start = (target_slot == SLOT_B) ? APP_B_START_ADDR : APP_A_START_ADDR;
    uint32_t app_size  = (target_slot == SLOT_B) ? APP_B_SIZE : APP_A_SIZE;

    BL_INFO("=========================================");
    BL_INFO("  STM32L431 Bootloader");
    BL_INFO("  target slot  : %s", target_slot == SLOT_B ? "B" : "A");
    BL_INFO("  App Flash    : 0x%08x ~ 0x%08x (%uKB)",
            app_start,
            app_start + ((target_slot == SLOT_B) ? APP_B_SIZE : APP_A_SIZE) - 1U,
            ((target_slot == SLOT_B) ? APP_B_SIZE : APP_A_SIZE) / 1024U);
    BL_INFO("  RAM buffers (linker managed):");
    BL_INFO("    ELF buffer : 0x%08x  %uKB", (uint32_t)(uintptr_t)elf_buf, ELF_BUF_SIZE / 1024U);
    BL_INFO("    ELF scratch: 0x%08x  %u bytes",
            (uint32_t)(uintptr_t)elf_reloc_scratch,
            ELF_RELOC_SCRATCH_SIZE);
    BL_INFO("=========================================");

    /* ---- Step 1: 从 LittleFS 读 ELF ---- */
    BL_INFO("[1/5] reading ELF \"%s\" from LittleFS...", path);

    ret = read_elf_from_lfs(path, elf_buf, ELF_BUF_SIZE, &elf_size);
    if (ret != 0)
    {
        BL_ERR("failed to read ELF from LittleFS: %d", ret);
        return BOOTLOADER_LOAD_ERR_READ;
    }
    /* ---- Step 2: Parse the complete ELF directly from internal RAM. ---- */
    BL_INFO("[2/5] parsing ELF...");

    ret = elf_parse(&ctx, elf_buf, elf_size);
    if (ret != ELF_OK)
    {
        BL_ERR("elf_parse failed: %d", ret);
        return BOOTLOADER_LOAD_ERR_PARSE;
    }

    /* ---- Step 3: 重定向 ---- */
    link_base = ctx.link_base;

    reloc_offset = (int32_t)app_start - (int32_t)link_base;
    app_max      = (target_slot == SLOT_B) ? APP_B_SIZE : APP_A_SIZE;

    BL_INFO("[3/5] relocating (link_base=0x%08x offset=%d)...", link_base, reloc_offset);

    ret = elf_relocate(&ctx, reloc_offset, app_max, elf_reloc_scratch, ELF_RELOC_SCRATCH_SIZE);
    if (ret != ELF_OK)
    {
        BL_ERR("elf_relocate failed: %d", ret);
        return BOOTLOADER_LOAD_ERR_RELOCATE;
    }

    /* 入口地址 */
    entry = elf_get_entry(&ctx);
    if ((entry & 0x1U) == 0U || (entry & ~1U) < app_start ||
        (entry & ~1U) >= (app_start + app_size))
    {
        BL_ERR("invalid entry point: 0x%08x", entry);
        return BOOTLOADER_LOAD_ERR_ENTRY;
    }
    BL_INFO("entry point: 0x%08x", entry);

    /* ---- Step 4: 写入 Flash ---- */
    BL_INFO("[4/5] writing sections to Flash...");

    ret = write_sections_to_flash(&ctx, target_slot);
    if (ret != 0)
    {
        BL_ERR("write_sections_to_flash FAILED! App Flash may be corrupted.");
        return BOOTLOADER_LOAD_ERR_FLASH;
    }

    /* 写入成功后可删除 LittleFS 中的 ELF（节省空间，可选）*/
    /* lfs_remove(&lfs_ctx.lfs, path); */

    /* ---- Step 5: 写入后的目标 App 合法性检查 ---- */
    BL_INFO("[5/5] validating target App at 0x%08x...", app_start);
    if (Verify_APP_Integrity_Flash(app_start) == 0)
    {
        BL_ERR("target App integrity check failed after loading");
        return BOOTLOADER_LOAD_ERR_APP_INVALID;
    }

    BL_INFO("target slot %s is ready to boot", target_slot == SLOT_B ? "B" : "A");
    return BOOTLOADER_LOAD_OK;
}


/**
 * flash_bootloader.c — STM32L431 + LittleFS Bootloader
 *
 * 职责：
 *   1. Erase_App_Flash()          擦除指定 slot 的 App Flash 分区
 *   2. Write_Buffer_To_Flash()    将任意长度数据写入 Flash（8字节对齐处理）
 *   3. Verify_APP_Integrity_Flash() 校验 Flash 中是否有合法固件
 *   4. Jump_To_App_Flash()        清理外设并跳转到 App
 *   5. bootloader_load_and_jump() 完整主流程：读ELF→解析→重定向→烧写→跳转
 */

#include "flash_bootloader.h"
#include "eeprom_emul.h"
#include "elf_loader_stream.h"
#include "global.h"
#include "lfs_config.h"
#include "lfs.h"
#include <string.h>
#include <stdio.h>

/* ===================================================================
 * 调试输出
 * =================================================================== */
#define BL_PREFIX  "[boot] "
#define BL_INFO(fmt, ...)  printf(BL_PREFIX fmt "\r\n", ##__VA_ARGS__)
#define BL_ERR(fmt, ...)   printf(BL_PREFIX "ERROR: " fmt "\r\n", ##__VA_ARGS__)
#define BL_WARN(fmt, ...)  printf(BL_PREFIX "WARN: "  fmt "\r\n", ##__VA_ARGS__)

/* ===================================================================
 * RAM / Flash 布局
 *
 * RAM 64KB (0x20000000 ~ 0x20010000):
 *   0x20000000 ~ 0x200017FF   6KB   Bootloader .data/.bss + 栈
 *   0x20001800 ~ 0x2000B7FF  40KB   ELF 文件 buffer (elf_buf)
 *   0x2000B800 ~ 0x2000FFFF  18KB   App .data/.bss 运行区
 *
 * 注意：App 链接脚本的 RAM ORIGIN 必须与 APP_RAM_BASE 一致。
 * =================================================================== */
#define BL_RAM_BASE    0x20000000UL
#define BL_RAM_SIZE    (6U  * 1024U)

#define ELF_BUF_BASE   (BL_RAM_BASE + BL_RAM_SIZE)   /* 0x20001800 */
#define ELF_BUF_SIZE   (40U * 1024U)

#define APP_RAM_BASE   (ELF_BUF_BASE + ELF_BUF_SIZE) /* 0x2000B800 */
#define APP_RAM_SIZE   (18U * 1024U)

/* LittleFS 中 App ELF 的路径 */
#define APP_ELF_PATH   "a.elf"

/* ===================================================================
 * ELF buffer（全局 .bss，启动时已清零，4字节对齐）
 * =================================================================== */
static uint8_t elf_buf[ELF_BUF_SIZE] __attribute__((aligned(4)));

/* ===================================================================
 * Stream I/O 回调 — 以 elf_buf 为后端存储
 * =================================================================== */

/** elf_buf 内实际 ELF 文件大小（read_elf_from_lfs 写入后设置） */
static uint32_t s_elf_file_size = 0U;

static int elf_buf_read(uint32_t file_offset, void *dst, uint32_t len, void *user)
{
    (void)user;
    if (len > s_elf_file_size || file_offset > s_elf_file_size - len) return -1;
    memcpy(dst, elf_buf + file_offset, len);
    return 0;
}

static int elf_buf_writeback(uint32_t file_offset, const void *src, uint32_t len, void *user)
{
    (void)user;
    if (len > s_elf_file_size || file_offset > s_elf_file_size - len) return -1;
    memcpy(elf_buf + file_offset, src, len);
    return 0;
}

/* ===================================================================
 * 尾字节暂存（Write_Buffer_To_Flash 跨包拼接用）
 * =================================================================== */
static uint8_t s_tail_bytes[8] = {0};
static uint8_t s_tail_len      = 0U;

/* ===================================================================
 * LittleFS 句柄（外部初始化）
 * =================================================================== */
extern lfs_ctx_t lfs_ctx;

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
    uint32_t PageError = 0;

    uint32_t startAddr;
    uint32_t appSize;
    if (slot == SLOT_B) {
        startAddr = APP_B_START_ADDR;
        appSize   = APP_B_SIZE;
    } else {
        startAddr = APP_A_START_ADDR;
        appSize   = APP_A_SIZE;
    }

    uint32_t startPage = (startAddr - FLASH_BASE) / FLASH_PAGE_SIZE;
    uint32_t appPages  = appSize / FLASH_PAGE_SIZE;

    BL_INFO("Erase_App_Flash: slot=%s addr=0x%08x pages=%u~%u",
            slot == SLOT_B ? "B" : "A",
            startAddr, startPage, startPage + appPages - 1U);

    eraseInitStruct.TypeErase = FLASH_TYPEERASE_PAGES;
    eraseInitStruct.Banks     = FLASH_BANK_1;
    eraseInitStruct.Page      = startPage;
    eraseInitStruct.NbPages   = appPages;

    HAL_FLASH_Unlock();
    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&eraseInitStruct, &PageError);
    HAL_FLASH_Lock();

    if (status != HAL_OK) {
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
HAL_StatusTypeDef Write_Buffer_To_Flash(uint32_t *startaddr,
                                        const uint8_t *buffer,
                                        uint32_t size)
{
    if (startaddr == NULL || buffer == NULL || size == 0U) {
        return HAL_ERROR;
    }

    HAL_StatusTypeDef status   = HAL_OK;
    uint64_t          value    = 0U;
    uint32_t          idx      = 0U;

    HAL_FLASH_Unlock();

    /* ---- 先把上一包遗留的尾字节补齐 ---- */
    if (s_tail_len > 0U) {
        uint32_t need     = 8U - s_tail_len;
        uint32_t copy_len = (size < need) ? size : need;

        memcpy(&s_tail_bytes[s_tail_len], buffer, copy_len);
        s_tail_len += (uint8_t)copy_len;
        idx        += copy_len;

        if (s_tail_len == 8U) {
            memcpy(&value, s_tail_bytes, 8U);
            status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                                       *startaddr, value);
            if (status != HAL_OK) {
                HAL_FLASH_Lock();
                BL_ERR("Write_Buffer_To_Flash: write failed at 0x%08x", *startaddr);
                return status;
            }
            *startaddr += 8U;
            s_tail_len  = 0U;
        }
        /* 本包数据全部用于补尾，且凑不够 8 字节，等下一包 */
        if (idx >= size) {
            HAL_FLASH_Lock();
            return HAL_OK;
        }
    }

    /* ---- 写完整的 8 字节块 ---- */
    for (; (idx + 8U) <= size; idx += 8U) {
        memcpy(&value, buffer + idx, 8U);
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                                   *startaddr, value);
        if (status != HAL_OK) {
            HAL_FLASH_Lock();
            BL_ERR("Write_Buffer_To_Flash: write failed at 0x%08x", *startaddr);
            return status;
        }
        *startaddr += 8U;
    }

    /* ---- 暂存本包末尾不足 8 字节的数据 ---- */
    if (idx < size) {
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
    if (s_tail_len == 0U) {
        return HAL_OK;
    }

    /* 末尾补 0xFF */
    memset(&s_tail_bytes[s_tail_len], 0xFF, 8U - s_tail_len);

    uint64_t value = 0U;
    memcpy(&value, s_tail_bytes, 8U);

    HAL_FLASH_Unlock();
    HAL_StatusTypeDef status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                                                  *startaddr, value);
    HAL_FLASH_Lock();

    if (status != HAL_OK) {
        BL_ERR("Flush_Tail_To_Flash: write failed at 0x%08x", *startaddr);
        return status;
    }

    *startaddr += 8U;
    s_tail_len  = 0U;
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
 *   1. 向量表[0]（初始 MSP）必须在 STM32L431 RAM 范围内
 *   2. 向量表[1]（Reset_Handler）必须在 Flash 范围内，且 bit0=1（Thumb）
 *
 * @param appaddr  App 分区起始地址（向量表地址）
 * @return 1 合法，0 非法
 */
int Verify_APP_Integrity_Flash(uint32_t appaddr)
{
    uint32_t msp           = *(__IO uint32_t *)(appaddr);
    uint32_t reset_handler = *(__IO uint32_t *)(appaddr + 4U);

    /* MSP 应在 STM32L431 整片 RAM 范围内
     * RAM1: 0x20000000 ~ 0x20010000 (64KB)
     * RAM2: 0x10000000 ~ 0x10004000 (16KB CCM)
     * 只要落在任意一块 RAM 里就合法 */
    int msp_valid = (msp >= 0x20000000U && msp <= 0x20010000U) ||
                    (msp >= 0x10000000U && msp <= 0x10004000U);

    if (!msp_valid) {
        BL_WARN("  invalid MSP 0x%08x (not in RAM)", msp);
        return 0;
    }

    if ((reset_handler & 0x1U) == 0U) {
        BL_WARN("  Reset_Handler 0x%08x not Thumb (bit0=0)", reset_handler);
        return 0;
    }

    uint32_t handler_addr = reset_handler & ~0x1U;
    if (handler_addr < FLASH_BASE_ADDR ||
        handler_addr > (FLASH_BASE_ADDR + FLASH_TOTAL_SIZE)) {
        BL_WARN("  Reset_Handler 0x%08x out of Flash range", reset_handler);
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
    if (Verify_APP_Integrity_Flash(appaddr) == 0) {
        BL_ERR("Jump_To_App_Flash: App integrity check failed");
        return 0;
    }

    BL_INFO("Jump_To_App_Flash: preparing to jump to 0x%08x", appaddr);

    /* ---- 关闭 LittleFS ---- */
    lfs_ctx_t *lfs_ctx_ptr = (lfs_ctx_t *)resource_ctx;
    if (lfs_ctx_ptr != NULL) {
        if (lfs_ctx_ptr->file_open) {
            lfs_file_close(&lfs_ctx_ptr->lfs, &lfs_ctx_ptr->file);
            lfs_ctx_ptr->file_open = 0U;
            BL_INFO("  LittleFS file closed");
        }
        if (lfs_ctx_ptr->mounted) {
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
    for (uint32_t i = 0; i < 8; i++) {
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
    while (1) {}
}

/* ===================================================================
 * 第四部分：从 LittleFS 读取 ELF（内部函数）
 * =================================================================== */

static int read_elf_from_lfs(const char *path,
                              uint8_t    *buf,
                              uint32_t    buf_size,
                              uint32_t   *out_size)
{
    int err;

    BL_INFO("opening \"%s\" from LittleFS...", path);

    if (lfs_ctx.file_open == 0U) {
        err = lfs_file_open(&lfs_ctx.lfs, &lfs_ctx.file, path, LFS_O_RDONLY);
        lfs_ctx.file_open = (err == LFS_ERR_OK) ? 1U : 0U;
        if (err < 0) {
            BL_ERR("lfs_file_open failed: %d", err);
            return err;
        }
    }

    /* 文件总大小 */
    lfs_soff_t fsize = lfs_file_size(&lfs_ctx.lfs, &lfs_ctx.file);
    BL_INFO("file size = %d bytes", (int)fsize);

    if (fsize <= (lfs_soff_t)sizeof(Firmware_Header_t)) {
        BL_ERR("file too small: %d bytes", (int)fsize);
        lfs_file_close(&lfs_ctx.lfs, &lfs_ctx.file);
        lfs_ctx.file_open = 0U;
        return -1;
    }

    /* 跳过 Header，只读 payload */
    lfs_file_seek(&lfs_ctx.lfs, &lfs_ctx.file, sizeof(Firmware_Header_t), LFS_SEEK_SET);

    lfs_soff_t payload_size = fsize - (lfs_soff_t)sizeof(Firmware_Header_t);
    BL_INFO("payload size = %d bytes", (int)payload_size);

    if ((uint32_t)payload_size > buf_size) {
        BL_ERR("payload too large: %d > %u bytes", (int)payload_size, buf_size);
        lfs_file_close(&lfs_ctx.lfs, &lfs_ctx.file);
        lfs_ctx.file_open = 0U;
        return -1;
    }

    BL_INFO("reading to RAM 0x%08X...", (uint32_t)(uintptr_t)buf);

    lfs_ssize_t nread = lfs_file_read(&lfs_ctx.lfs, &lfs_ctx.file,
                                       buf, (lfs_size_t)payload_size);
    lfs_file_close(&lfs_ctx.lfs, &lfs_ctx.file);
    lfs_ctx.file_open = 0U;

    if (nread != payload_size) {
        BL_ERR("read incomplete: %d / %d bytes", (int)nread, (int)payload_size);
        return -1;
    }

    *out_size = (uint32_t)nread;
    BL_INFO("read OK: %u bytes", *out_size);
    return 0;
}
/* ===================================================================
 * 第五部分：按 section 写入 Flash（内部函数）
 * =================================================================== */
static int write_sections_to_flash(elf_ctx_stream_t *ctx, uint8_t slot)
{
    uint32_t flash_start = (slot == SLOT_B) ? APP_B_START_ADDR : APP_A_START_ADDR;
    uint32_t flash_size  = (slot == SLOT_B) ? APP_B_SIZE       : APP_A_SIZE;
    uint32_t flash_end   = flash_start + flash_size;

    if (Erase_App_Flash(slot) != 0) {
        BL_ERR("flash erase failed");
        return -1;
    }

    s_tail_len = 0U;
    memset(s_tail_bytes, 0, sizeof(s_tail_bytes));

    /*
     * 逐 section 写入 Flash，每个 section 写到其正确的 Flash 地址：
     *   - Flash section (sh_addr < 0x10000000): 目标 = sh_addr + offset
     *   - RAM section (.data, sh_addr >= 0x10000000): 目标 = 紧接上一个 section 之后
     *
     * 这保证了 .data 初始值在 Flash 中的位置与 startup code 的 _sidata 一致。
     * 连续写入方式对 GCC ELF 不适用：GCC 将 .data 放在独立的 segment，
     * file offset 与 Flash load address 之间有对齐间隙，
     * 导致 .data 被写到错误位置。
     */
    uint32_t next_flash = flash_start;

    for (uint32_t i = 0; i < ctx->load_count; i++) {
        elf32_shdr *shdr = &ctx->shdrs[ctx->load_shidx[i]];
        if (shdr->sh_type == SHT_NOBITS) continue;
        if (!(shdr->sh_flags & SHF_ALLOC)) continue;
        if (shdr->sh_size == 0) continue;

        const char *name = (ctx->shstrtab && shdr->sh_name)
                           ? ctx->shstrtab + shdr->sh_name : "(?)";

        /* 刷新上一 section 遗留的尾字节（避免跨间隙拼接） */
        if (s_tail_len > 0U) {
            uint32_t flush_addr = next_flash;
            if (Flush_Tail_To_Flash(&flush_addr) != HAL_OK) {
                BL_ERR("flush tail failed at 0x%08x", next_flash);
                return -1;
            }
            next_flash = flush_addr;
        }

        /* 确定目标 Flash 地址（RAM section 的 target_addr 必须在 flush
         * 之后计算，否则 flush 推进 next_flash 会导致重叠误判）
         *
         * 判断 Flash section 的标准：
         *   - ARMCC ET_EXEC: sh_addr 在 Flash 范围 (0x08000000+)
         *   - GCC 0-based:   sh_addr 在低地址 (< 0x10000000)，不含 RAM
         *   两种情况都排除 RAM section (sh_addr >= 0x10000000 且不在 Flash 范围)
         *
         * 目标地址计算：
         *   - ET_EXEC: sh_addr 已在 Flash 范围，target = sh_addr + offset
         *   - 0-based: sh_addr 是虚拟偏移（从 0 起），target = app_start + sh_addr
         *     不能用 sh_addr + offset，因为 offset = app_start - link_base，
         *     对 link_base 之前的 section（如 .isr_vector addr=0）会映射到
         *     flash_start 之前 */
        uint32_t target_addr;
        int is_flash_sec = (shdr->sh_addr >= FLASH_BASE_ADDR &&
                            shdr->sh_addr <  FLASH_BASE_ADDR + FLASH_TOTAL_SIZE) ||
                           (shdr->sh_addr <  0x10000000U);
        if (is_flash_sec) {
            if (shdr->sh_addr >= FLASH_BASE_ADDR) {
                /* ET_EXEC: sh_addr 在 Flash 范围，用 reloc offset */
                target_addr = shdr->sh_addr + ctx->offset;
            } else {
                /* 0-based: sh_addr 是虚拟偏移，直接加 app_start */
                target_addr = flash_start + shdr->sh_addr;
            }
        } else {
            /* RAM section (.data): load address 紧接上一个 section 之后 */
            target_addr = next_flash;
        }

        /* Flash 已擦除为 0xFF，间隙无需写入，直接跳过 */
        if (target_addr > next_flash) {
            BL_INFO("gap: 0x%08x ~ 0x%08x (%u bytes)",
                    next_flash, target_addr - 1U, target_addr - next_flash);
            next_flash = target_addr;
        }

        /* 重叠检测：tail-flush 推进 next_flash 后，target_addr 可能落在
         * 已写入区域内（非 8 字节对齐 section 间间隙 < padding 字节数），
         * 此时静默继续会导致数据写到错误位置 */
        if (target_addr < next_flash) {
            BL_ERR("overlap: section \"%s\" target 0x%08x < next_flash 0x%08x",
                   name, target_addr, next_flash);
            return -1;
        }

        if (target_addr + shdr->sh_size > flash_end) {
            BL_ERR("section \"%s\" overflows flash (0x%08x + %u > 0x%08x)",
                   name, target_addr, shdr->sh_size, flash_end);
            return -1;
        }

        BL_INFO("  \"%s\": buf+0x%05x, %u bytes -> flash 0x%08x",
                name, shdr->sh_offset, shdr->sh_size, target_addr);

        uint32_t write_addr = target_addr;
        uint8_t *src = elf_buf + shdr->sh_offset;

        HAL_StatusTypeDef status = Write_Buffer_To_Flash(&write_addr, src, shdr->sh_size);
        if (status != HAL_OK) {
            BL_ERR("flash write FAILED for \"%s\"", name);
            return -1;
        }
        next_flash = write_addr;
    }

    /* 刷新最后的尾字节 */
    if (s_tail_len > 0U) {
        uint32_t flush_addr = next_flash;
        if (Flush_Tail_To_Flash(&flush_addr) != HAL_OK) {
            BL_ERR("final flush FAILED");
            return -1;
        }
        next_flash = flush_addr;
    }

    BL_INFO("write OK: flash 0x%08x ~ 0x%08x (%u bytes)",
            flash_start, next_flash - 1U, next_flash - flash_start);

    return 0;
}

/* ===================================================================
 * 第六部分：主流程
 * =================================================================== */

/**
 * bootloader_load_and_jump() — 完整加载并跳转流程
 *
 * 调用前提：
 *   - lfs_ctx 已挂载
 *   - UART/printf 可用
 */
int bootloader_load_and_jump(void)
{
    BL_INFO("elf_buf actual address: 0x%08x", (uint32_t)(uintptr_t)elf_buf);
    /* ---- 打印上次复位原因，便于定位 APP 是否触发了复位 ---- */
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST))  BL_INFO("Reset cause: IWDG");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST))  BL_INFO("Reset cause: WWDG");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_LPWRRST))  BL_INFO("Reset cause: LowPower");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST))   BL_INFO("Reset cause: BOR");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))   BL_INFO("Reset cause: PIN/PWR");
#ifdef RCC_FLAG_PORRST
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST))   BL_INFO("Reset cause: POR");
#endif
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))   BL_INFO("Reset cause: Software");
    __HAL_RCC_CLEAR_RESET_FLAGS();
    int      ret;
    uint32_t elf_size = 0U;
    uint32_t entry    = 0U;
    uint32_t avail_after = 0U;
    uint8_t *meta_buf_ptr = NULL, *work_buf_ptr = NULL;
    elf_ctx_stream_t ctx;
    int load_ok = 0;

    /* 从 EEPROM 仿真读取目标 slot（SLOT_A=1，SLOT_B=2）*/
    uint32_t stored_slot = Read_Flag(EE_VAR_TARGET_SLOT);
    uint8_t target_slot = (stored_slot == SLOT_B) ? SLOT_B : SLOT_A;
    uint32_t app_start  = (target_slot == SLOT_B) ? APP_B_START_ADDR : APP_A_START_ADDR;

    BL_INFO("=========================================");
    BL_INFO("  STM32L431 Bootloader");
    BL_INFO("  target slot  : %s", target_slot == SLOT_B ? "B" : "A");
    BL_INFO("  App Flash    : 0x%08x ~ 0x%08x (%uKB)",
            app_start,
            app_start + ((target_slot == SLOT_B) ? APP_B_SIZE : APP_A_SIZE) - 1U,
            ((target_slot == SLOT_B) ? APP_B_SIZE : APP_A_SIZE) / 1024U);
    BL_INFO("  RAM layout:");
    BL_INFO("    Bootloader : 0x%08x  %uKB", BL_RAM_BASE,  BL_RAM_SIZE  / 1024U);
    BL_INFO("    ELF buffer : 0x%08x  %uKB", ELF_BUF_BASE, ELF_BUF_SIZE / 1024U);
    BL_INFO("    App RAM    : 0x%08x  %uKB", APP_RAM_BASE, APP_RAM_SIZE  / 1024U);
    BL_INFO("=========================================");

    /* ---- Step 1: 从 LittleFS 读 ELF ---- */
    BL_INFO("[1/5] reading ELF \"%s\" from LittleFS...", APP_ELF_PATH);

    ret = read_elf_from_lfs(APP_ELF_PATH, elf_buf, ELF_BUF_SIZE, &elf_size);
    if (ret != 0) {
        BL_WARN("no valid ELF in LittleFS (err=%d), trying existing App...", ret);
        goto try_existing;
    }
    s_elf_file_size = elf_size;

    /* ---- Step 2: 解析 ELF（stream 模式）---- */
    BL_INFO("[2/5] parsing ELF...");

    /*
     * meta_buf / work_buf 分配策略：
     *   ELF 文件已读入 elf_buf[40KB]，重定向时 writeback 直接修改 elf_buf。
     *   meta_buf（~10KB）和 work_buf（~4KB）共约 14KB，放在栈上会溢出，
     *   所以复用 elf_buf 中 ELF 文件末尾之后的空闲区域。
     *   只要 ELF 文件 < 40KB - 14KB = 26KB 就安全（实际 AXF 通常 < 20KB）。
     */
    avail_after = (elf_size < ELF_BUF_SIZE) ? (ELF_BUF_SIZE - elf_size) : 0U;

    if (avail_after >= ELF_STREAM_META_BUF_MIN + 4096U) {
        meta_buf_ptr = elf_buf + elf_size;
        work_buf_ptr = meta_buf_ptr + ELF_STREAM_META_BUF_MIN;
    } else {
        BL_ERR("ELF too large, no room for stream buffers");
        goto try_existing;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.io.read       = elf_buf_read;
    ctx.io.user       = NULL;
    ctx.meta_buf      = meta_buf_ptr;
    ctx.meta_buf_size = ELF_STREAM_META_BUF_MIN;
    ctx.work_buf      = work_buf_ptr;
    ctx.work_buf_size = avail_after - ELF_STREAM_META_BUF_MIN;

    ret = elf_parse_stream(&ctx);
    if (ret != ELF_OK) {
        BL_ERR("elf_parse_stream failed: %d", ret);
        goto try_existing;
    }

    /* ---- Step 3: 重定向 ---- */
    /* 推导 link_base：取第一个 exec section 的 sh_addr */
    uint32_t link_base = 0U;
    int found_exec = 0;
    for (uint32_t si = 0; si < ctx.load_count; si++) {
        elf32_shdr *s = &ctx.shdrs[ctx.load_shidx[si]];
        if ((s->sh_flags & SHF_EXECINSTR) && !(s->sh_flags & SHF_WRITE)) {
            link_base = s->sh_addr;
            found_exec = 1;
            break;
        }
    }
    if (!found_exec) {
        BL_ERR("no exec section found, cannot relocate");
        goto try_existing;
    }
    /* 0-based ELF 修正：
     * 0-based ELF 的虚拟地址空间从 0 开始，第一个 exec section 的
     * sh_addr（如 0x190）只是段偏移，不是真正的基地址。
     * 如果用 section offset 作为 link_base，reloc_offset = app_start - 0x190，
     * 所有重定位值都会偏少 0x190。正确的 link_base 应为 0。
     * 注意：Flash 地址（0x08000000+）虽然 < 0x10000000，但不是 0-based，
     * 必须用 FLASH_BASE_ADDR 作为分界线。 */
    if (link_base != 0U && link_base < FLASH_BASE_ADDR) {
        BL_INFO("0-based ELF detected, link_base 0x%08x -> 0", link_base);
        link_base = 0U;
    }
    /* link_base 合法性检查：
     *   - 0-based:     link_base == 0（已修正），合法
     *   - ET_EXEC:     link_base 在 Flash 范围（0x08000000+），合法
     *   拒绝的情况：link_base > 0 但不在 Flash 范围，
     *   这说明 .ARM.extab 等非代码 section 带了 SHF_EXECINSTR 被误取为基准 */
    if (link_base > 0U &&
        (link_base < FLASH_BASE_ADDR || link_base >= FLASH_BASE_ADDR + FLASH_TOTAL_SIZE)) {
        BL_ERR("link_base 0x%08x out of Flash range, cannot relocate", link_base);
        goto try_existing;
    }
    /* offset = 运行时基地址 − 链接时基地址 */
    uint32_t reloc_offset = app_start - link_base;
    uint32_t app_max      = (target_slot == SLOT_B) ? APP_B_SIZE : APP_A_SIZE;

    BL_INFO("[3/5] relocating (link_base=0x%08x offset=0x%08x)...", link_base, reloc_offset);

    ret = elf_relocate_stream(&ctx, reloc_offset, app_max, elf_buf_writeback);
    if (ret != ELF_OK) {
        BL_ERR("elf_relocate_stream failed: %d", ret);
        goto try_existing;
    }

    /* 入口地址 */
    entry = elf_stream_get_entry(&ctx);
    if (entry == 0U) {
        BL_ERR("invalid entry point (e_entry=0)");
        goto try_existing;
    }
    BL_INFO("entry point: 0x%08x", entry);

    /* ---- Step 4: 写入 Flash ---- */
    BL_INFO("[4/5] writing sections to Flash...");

    ret = write_sections_to_flash(&ctx, target_slot);
    if (ret != 0) {
        BL_ERR("write_sections_to_flash FAILED! App Flash may be corrupted.");
        while (1) {}   /* Flash 写失败不应跳转，等看门狗复位重试 */
    }

    /* 写入成功后可删除 LittleFS 中的 ELF（节省空间，可选）*/
    /* lfs_remove(&lfs_ctx.lfs, APP_ELF_PATH); */

    load_ok = 1;

try_existing:
    if (!load_ok) {
        /* ---- 降级：跳转到 Flash 中已有的 App ---- */
        BL_WARN("[4/5] skip Flash write, trying existing App at 0x%08x...", app_start);

        if (!Verify_APP_Integrity_Flash(app_start)) {
            while (1) {}
        }

        entry = *(__IO uint32_t *)(app_start + 4U);
        BL_INFO("fallback entry: 0x%08x", entry);
    }

    /* ---- Step 5: 跳转（通过 Jump_To_App_Flash 统一清理）---- */
    BL_INFO("[5/5] jumping to App at 0x%08x...", entry);

    /*
     * Jump_To_App_Flash 内部会再次校验合法性、关闭 LittleFS、
     * 反初始化外设、清 NVIC、设置 VTOR/MSP，最后跳转。
     * 正常情况下不会返回。
     */
    Jump_To_App_Flash(&lfs_ctx, app_start);

    /* 仅在校验失败时返回到这里 */
    BL_ERR("Jump_To_App_Flash returned unexpectedly");
    while (1) {}
}


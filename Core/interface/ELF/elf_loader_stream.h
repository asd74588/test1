/**
 * elf_loader_stream.h — 流式 ELF 解析与重定向接口
 *
 * 设计目标：
 *   全程不需要将整个 ELF 文件映射进 RAM；
 *   只在 work_buf 内分块读取 section 内容，
 *   重定向后通过 writeback 回写（通常是写 Flash）。
 *
 * ── 与 elf_loader.c 的差异 ──
 *
 *   elf_loader.c（值域猜测模式）假设：
 *     • ELF 链接基地址 = 0（sh_addr 从 0 开始）
 *     • 无 REL/RELA 表，或不使用 REL 表
 *     • scan_exec 白名单条件：sh_addr == 0
 *
 *   elf_loader_stream.c（双路模式）：
 *     路径 A — REL 表存在（标准 ARMCC AXF）：精确重定向
 *       支持 R_ARM_ABS32 / R_ARM_TARGET1（直接加 offset）
 *       支持 R_ARM_THM_MOVW_ABS_NC + R_ARM_THM_MOVT_ABS（修改 Thumb2 MOVW/MOVT imm16）
 *       忽略 R_ARM_THM_CALL / R_ARM_CALL / R_ARM_JUMP24（PC-relative，自校正）
 *       忽略 R_ARM_THM_MOVW_PREL_NC / R_ARM_THM_ALU_PREL_11_0（PC-relative）
 *     路径 B — 无 REL 表（stripped ELF）：值域猜测（保持与 elf_loader.c 兼容）
 *       scan_exec 条件改为：is_exec && !is_write（去掉 sh_addr==0 限制）
 *       LINK_MIN/LINK_END 改为相对于 sh_addr 的范围
 *
 * ── meta_buf 布局 ──
 *
 *   [0,              sizeof(elf32_ehdr))          — ELF header
 *   [sizeof(ehdr),   +shnum*sizeof(elf32_shdr))   — section header table
 *   [上述末尾,       +shstrtab.sh_size)            — shstrtab（若可用）
 *
 * meta_buf 最小大小：ELF_STREAM_META_BUF_MIN
 * work_buf 最小大小：ELF_STREAM_WORK_BUF_MIN
 */

#ifndef ELF_LOADER_STREAM_H
#define ELF_LOADER_STREAM_H

#include "elf_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * 容量常量
 * =================================================================== */

enum
{
    R_ARM_THM_MOVW_ABS_NC = 47,
    R_ARM_THM_MOVT_ABS    = 48,
};


#define ELF_STREAM_MAX_SHDRS      64U
#define ELF_STREAM_SHSTRTAB_MAX   4096U
#define ELF_STREAM_META_BUF_MIN   \
    ((uint32_t)(sizeof(elf32_ehdr)) + \
     ELF_STREAM_MAX_SHDRS * (uint32_t)(sizeof(elf32_shdr)) + \
     ELF_STREAM_SHSTRTAB_MAX)

/**
 * work_buf 最小值：
 *   exec section Pass-1/2 需要：bitmap(bmp_bytes) + 至少 4 个 word(16 字节)
 *   write section 需要：至少 1 个 word(4 字节)
 *   建议 4 KB 以上以提高分块效率。
 */
#define ELF_STREAM_WORK_BUF_MIN   256U

#define ELF_STREAM_MAX_LOAD       16U
#define ELF_STREAM_MAX_REL        16U

/* ===================================================================
 * I/O 回调
 * =================================================================== */

/**
 * elf_read_fn — 从存储介质随机读取
 *
 * @param file_offset  ELF 文件内的字节偏移（0 = 文件头）
 * @param dst          目标缓冲区
 * @param len          字节数
 * @param user         调用方透传指针
 * @return 0 = 成功，负值 = 错误
 */
typedef int (*elf_read_fn)(uint32_t file_offset,
                           void    *dst,
                           uint32_t len,
                           void    *user);

/**
 * elf_writeback_fn — 将重定向后数据写回存储
 *
 * @param file_offset  ELF 文件内的字节偏移
 * @param src          已修改的数据
 * @param len          字节数
 * @param user         调用方透传指针
 * @return 0 = 成功，负值 = 错误
 */
typedef int (*elf_writeback_fn)(uint32_t    file_offset,
                                const void *src,
                                uint32_t    len,
                                void       *user);

typedef struct {
    elf_read_fn  read;
    void        *user;
} elf_io_t;

/* ===================================================================
 * 流式上下文
 * =================================================================== */

typedef struct {
    /* ── I/O ── */
    elf_io_t    io;

    /* ── meta_buf ── */
    uint8_t    *meta_buf;
    uint32_t    meta_buf_size;

    /* ── work_buf ── */
    uint8_t    *work_buf;
    uint32_t    work_buf_size;

    /* ── 解析结果（elf_parse_stream 填充）── */
    elf32_ehdr *ehdr;
    elf32_shdr *shdrs;
    const char *shstrtab;

    uint16_t    load_shidx[ELF_STREAM_MAX_LOAD];
    uint32_t    load_count;

    uint16_t    rel_shidx[ELF_STREAM_MAX_REL];
    uint32_t    rel_count;

    uint32_t    sym_count;

    /* ── 重定向参数（elf_relocate_stream 填充）── */
    int32_t    offset;

} elf_ctx_stream_t;

/* ===================================================================
 * 公开 API
 * =================================================================== */

/**
 * elf_parse_stream() — 流式解析 ELF header / section table
 *
 * 调用前须初始化：
 *   ctx->io.read / ctx->io.user
 *   ctx->meta_buf  （大小 >= ELF_STREAM_META_BUF_MIN）
 *   ctx->work_buf  （大小 >= ELF_STREAM_WORK_BUF_MIN）
 */
int elf_parse_stream(elf_ctx_stream_t *ctx);

/**
 * elf_relocate_stream() — 流式重定向
 *
 * 须在 elf_parse_stream() 成功后调用。
 *
 * 路径 A（有 REL 表）：
 *   对每条 REL 记录精确重定向，exec/write section 均单遍扫描。
 * 路径 B（无 REL 表）：
 *   exec section 两遍（Pass-1 build litpool bitmap，Pass-2 reloc+writeback）。
 *   write section 单遍。
 *
 * @param offset        运行时 Flash 起始地址与链接时基地址之差
 *                      例：链接在 0x08020000，运行在 0x08040000，offset = 0x20000
 * @param app_max_size  Flash 分区大小（字节），用于值域过滤（路径 B）
 * @param writeback     重定向后写回回调
 */
int elf_relocate_stream(elf_ctx_stream_t *ctx,
                        uint32_t          link_base,
                        int32_t          offset,
                        uint32_t          app_max_size,
                        elf_writeback_fn  writeback);

/** 返回运行时入口地址（parse + relocate 后调用）*/
uint32_t elf_stream_get_entry(const elf_ctx_stream_t *ctx);

/** 获取第 idx 个 loadable section 的元信息（data 指针始终为 NULL）*/
int elf_stream_get_section(const elf_ctx_stream_t *ctx,
                           uint32_t                idx,
                           elf_section_info_t     *info);

#ifdef __cplusplus
}
#endif

#endif /* ELF_LOADER_STREAM_H */

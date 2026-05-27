/**
 * elf_loader.h — ELF 解析与静态重定向接口
 *
 * 功能：从 buffer 中解析 ELF 文件，对所有绝对地址引用施加
 *       目标分区偏移，使同一份 ELF 可烧写到不同 Flash 地址运行。
 *
 * 使用流程：
 *   1. elf_parse()        — 解析 ELF，填充上下文
 *   2. elf_relocate()     — 对 buffer 中的内容施加地址偏移
 *   3. elf_get_section()  — 逐 section 取出数据，由调用者写入 Flash
 */

#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stdint.h>
#include <stddef.h>
#include "elf.h"

/* ===== 错误码 ===== */
#define ELF_OK              0
#define ELF_ERR_MAGIC      -1   /* 不是有效的 ELF 文件 */
#define ELF_ERR_CLASS      -2   /* 不是 32-bit ELF */
#define ELF_ERR_MACHINE    -3   /* 不是 ARM 架构 */
#define ELF_ERR_NO_SYMTAB  -4   /* 未找到符号表 */
#define ELF_ERR_RELOC      -5   /* 遇到不支持的重定向类型 */
#define ELF_ERR_PARAM      -6   /* 参数错误 */

/* ===== 可加载 section 的描述 ===== */
/* 调用 elf_get_section() 遍历时返回此结构，由调用者决定写入目标地址 */
typedef struct {
    uint32_t  load_addr;   /* 该 section 在目标 Flash 中的地址（已加 offset）*/
    uint8_t  *data;        /* 指向 buffer 中该 section 数据的指针 */
    uint32_t  size;        /* section 大小（字节）*/
    uint8_t   is_bss;      /* 1 = .bss 类型，data 为 NULL，需要清零 */
} elf_section_info_t;

/* ===== ELF 解析上下文 ===== */
/* 所有字段均指向 buf 内部，不额外分配内存 */
typedef struct {
    /* 原始 buffer */
    uint8_t   *buf;         /* ELF 文件的内存起始地址 */
    uint32_t   buf_size;    /* buffer 总大小（字节）*/

    /* 解析后的关键指针（全部指向 buf 内部）*/
    elf32_ehdr *ehdr;       /* ELF 文件头 */
    elf32_shdr *shdrs;      /* Section header table 起始 */
    elf32_sym  *symtab;     /* 符号表起始 */
    uint32_t    sym_count;  /* 符号表条目数量 */
    const char *strtab;     /* 符号名字符串表（调试用，可为 NULL）*/
    const char *shstrtab;   /* Section 名字符串表 */

    /* 重定向表信息（最多 16 个 rel/rela section，通常远少于此）*/
    elf32_shdr *rel_shdrs[16];   /* 指向各个 REL/RELA section header */
    uint32_t    rel_count;       /* 共有多少个重定向 section */

    /* 可加载 section 信息（最多 16 个）*/
    elf32_shdr *load_shdrs[16];  /* 指向各个需要加载的 section header */
    uint32_t    load_count;      /* 共有多少个可加载 section */

    /* 地址偏移（由 elf_relocate() 使用）*/
    uint32_t    offset;     /* 目标分区起始地址（因为链接基址为0，即等于偏移）*/
} elf_ctx_t;

/* ===== 公开接口 ===== */

/**
 * elf_parse() — 解析 ELF 文件，填充上下文
 *
 * @param ctx      上下文（由调用者提供，此函数填充）
 * @param buf      ELF 文件内容的起始地址（SRAM 或外挂 RAM 中的 buffer）
 * @param buf_size buffer 大小
 * @return ELF_OK 或负数错误码
 *
 * 注意：此函数只做指针计算和校验，不分配内存，不修改 buf 内容。
 */
int elf_parse(elf_ctx_t *ctx, uint8_t *buf, uint32_t buf_size);

/**
 * elf_relocate() — 对 buffer 中的内容施加地址偏移
 *
 * 遍历所有 REL/RELA section，对每一条重定向记录：
 * 查找符号值 → 加上 offset → 写回 buf 中对应位置
 *
 * @param ctx     已由 elf_parse() 填充的上下文
 * @param offset  目标分区起始地址（链接基址为0时即为偏移量）
 *                例：分区A起始地址 0x08010000，offset = 0x08010000
 * @return ELF_OK 或负数错误码
 *
 * 注意：此函数会修改 buf 中的内容（写回修正后的地址），
 *       调用后 buf 中的数据即可直接烧写到 Flash。
 */
int elf_relocate(elf_ctx_t *ctx, uint32_t offset);

/**
 * elf_get_section() — 遍历可加载 section，逐个取出
 *
 * 典型用法：
 *   elf_section_info_t sec;
 *   for (uint32_t i = 0; i < ctx.load_count; i++) {
 *       elf_get_section(&ctx, i, &sec);
 *       if (sec.is_bss) {
 *           flash_erase_and_zero(sec.load_addr, sec.size);
 *       } else {
 *           flash_write(sec.load_addr, sec.data, sec.size);
 *       }
 *   }
 *
 * @param ctx   已由 elf_parse() + elf_relocate() 处理的上下文
 * @param idx   section 索引（0 到 ctx->load_count - 1）
 * @param info  输出：section 信息
 * @return ELF_OK 或 ELF_ERR_PARAM
 */
int elf_get_section(const elf_ctx_t *ctx, uint32_t idx, elf_section_info_t *info);

/**
 * elf_get_entry() — 获取程序入口地址（已加 offset）
 *
 * @param ctx    已处理的上下文
 * @return 入口虚拟地址 + offset（即目标 Flash 中的 Reset_Handler 地址）
 */
uint32_t elf_get_entry(const elf_ctx_t *ctx);

#endif /* ELF_LOADER_H */



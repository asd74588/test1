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
typedef struct {
    uint32_t  load_addr;   /* 该 section 在目标 Flash/RAM 中的地址（已加 offset）*/
    uint8_t  *data;        /* 指向 buffer 中该 section 数据的指针 */
    uint32_t  size;        /* section 大小（字节）*/
    uint8_t   is_bss;      /* 1 = .bss 类型，data 为 NULL，需要清零 */
    const char *name;      /* section 名称（调试用，可能为 NULL）*/
} elf_section_info_t;

/* ===== ELF 解析上下文 ===== */
typedef struct {
    uint8_t   *buf;
    uint32_t   buf_size;

    elf32_ehdr *ehdr;
    elf32_shdr *shdrs;
    elf32_sym  *symtab;
    uint32_t    sym_count;
    const char *strtab;
    const char *shstrtab;

    elf32_shdr *rel_shdrs[16];
    uint32_t    rel_count;

    elf32_shdr *load_shdrs[16];
    uint32_t    load_count;

    uint32_t    offset;
} elf_ctx_t;

/* ===== 公开接口 ===== */
int      elf_parse      (elf_ctx_t *ctx, uint8_t *buf, uint32_t buf_size);
int      elf_relocate   (elf_ctx_t *ctx, uint32_t offset);
int      elf_get_section(const elf_ctx_t *ctx, uint32_t idx, elf_section_info_t *info);
uint32_t elf_get_entry  (const elf_ctx_t *ctx);

/* 调试：打印所有 section 信息 */
void     elf_dump_sections(const elf_ctx_t *ctx);

#endif /* ELF_LOADER_H */



/**
 * elf_loader.h — ELF32 整包解析与原地重定位接口
 *
 * 调用方必须先将完整 ELF 文件放入连续内存缓冲区。解析过程只保存指向
 * 该缓冲区的指针，不会复制 section 内容或分配额外的元数据缓冲区。
 */

#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stdint.h>

#include "elf.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* 返回码：0 表示成功，负值表示失败。 */
#define ELF_OK            0
#define ELF_ERR_MAGIC     -1
#define ELF_ERR_CLASS     -2
#define ELF_ERR_MACHINE   -3
#define ELF_ERR_NO_SYMTAB -4
#define ELF_ERR_RELOC     -5
#define ELF_ERR_PARAM     -6
#define ELF_ERR_BOUNDS    -7
#define ELF_ERR_FORMAT    -8
#define ELF_ERR_CAPACITY  -9

#define ELF_MAX_LOAD_SECTIONS 32U
#define ELF_MAX_REL_SECTIONS  32U

/* 一个可加载 section 的摘要信息。data 直接指向 ELF 输入缓冲区。 */
typedef struct
{
    uint32_t    load_addr; /* 重定位后的加载地址。 */
    uint8_t    *data;      /* section 数据；NOBITS section 为 NULL。 */
    uint32_t    size;      /* section 大小，单位为字节。 */
    uint8_t     is_bss;    /* 是否为 NOBITS section。 */
    const char *name;      /* section 名称，指向 ELF 缓冲区。 */
} elf_section_info_t;

typedef struct
{
    /* ELF 完整镜像；该缓冲区由调用方持有，解析器不会释放。 */
    uint8_t *buf;
    uint32_t file_size;

    /* 以下指针均直接指向 buf 内部，ctx 的生命周期不能超过 buf。 */
    elf32_ehdr *ehdr;
    elf32_shdr *shdrs;
    const char *shstrtab;
    uint32_t    shstrtab_size;
    elf32_sym  *symtab;
    uint32_t    sym_count;
    const char *strtab;
    uint32_t    strtab_size;

    elf32_shdr *load_shdrs[ELF_MAX_LOAD_SECTIONS];
    uint32_t    load_count;
    elf32_shdr *rel_shdrs[ELF_MAX_REL_SECTIONS];
    uint32_t    rel_count;

    uint32_t link_base; /* 所有相对地址 section 的最小链接地址。 */
    int32_t  offset;    /* 最近一次 elf_relocate 使用的地址偏移。 */
} elf_ctx_t;

/**
 * 解析已经存放在 buf[0..file_size) 中的完整 ELF 文件。
 *
 * @param ctx  输出的解析上下文。
 * @param buf  调用方持有的 ELF 连续缓冲区。
 * @param file_size  缓冲区长度，单位为字节。
 * @return ELF_OK 或对应的 ELF_ERR_* 错误码。
 */
int elf_parse(elf_ctx_t *ctx, uint8_t *buf, uint32_t file_size);

/**
 * 在原缓冲区内重定位 ELF 镜像。
 *
 * 存在重定位 section 时优先使用重定位表；没有重定位表的精简镜像则使用
 * scratch 作为字面量池位图，执行保守扫描。
 *
 * @param ctx  已成功解析的 ELF 上下文。
 * @param offset  链接地址到运行地址的有符号偏移。
 * @param app_max_size  目标应用区域的最大大小，单位为字节。
 * @param scratch  fallback 扫描使用的临时位图，可为 NULL（存在重定位表时）。
 * @param scratch_size  scratch 的容量，单位为字节。
 * @return ELF_OK 或对应的 ELF_ERR_* 错误码。
 */
int elf_relocate(
    elf_ctx_t *ctx, int32_t offset, uint32_t app_max_size, uint8_t *scratch, uint32_t scratch_size);

/**
 * 获取第 idx 个可加载 section 的地址、大小、数据指针和名称。
 *
 * @param ctx  已成功解析的 ELF 上下文。
 * @param idx  可加载 section 的索引。
 * @param info  输出的 section 摘要信息。
 * @return ELF_OK 或对应的 ELF_ERR_* 错误码。
 */
int elf_get_section(const elf_ctx_t *ctx, uint32_t idx, elf_section_info_t *info);

/** 获取重定位后的入口地址；参数无效或溢出时返回 0。 */
uint32_t elf_get_entry(const elf_ctx_t *ctx);

/** 按当前日志配置输出所有 section 的摘要信息。 */
void elf_dump_sections(const elf_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ELF_LOADER_H */

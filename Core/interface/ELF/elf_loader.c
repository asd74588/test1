/**
 * elf_loader.c — ELF 解析与静态重定向实现（含完整调试输出）
 *
 * 逻辑来源：
 *   - elf_parse()     → Zephyr subsys/llext/llext_load.c : llext_find_tables()
 *   - elf_relocate()  → Zephyr subsys/llext/llext_link.c : llext_link()
 *   - arm_relocate()  → Zephyr arch/arm/core/elf.c       : arch_elf_relocate()
 *
 * 调试输出分级：
 *   ELF_LOG_INFO  — 关键步骤（默认开启）
 *   ELF_LOG_VERB  — 每条重定向记录（详细模式，量大时关闭）
 *   ELF_LOG_WARN  — 警告（跳过的符号等，默认开启）
 */

#include "elf_loader.h"
#include <string.h>
#include <stdio.h>

/* ===================================================================
 * 调试输出控制
 * =================================================================== */

/* 开启 INFO 级别：关键解析步骤、section列表、重定向统计 */
#define ELF_LOG_LEVEL_INFO  1

/* 开启 VERBOSE 级别：每条重定向记录的详细信息（量很大，调试时开启）*/
/* #define ELF_LOG_LEVEL_VERB  1 */

/* 开启 WARN 级别：未定义符号、越界等警告 */
#define ELF_LOG_LEVEL_WARN  1

/* 输出前缀 */
#define ELF_PREFIX  "[elf] "

#ifdef ELF_LOG_LEVEL_INFO
  #define ELF_INFO(fmt, ...)  printf(ELF_PREFIX fmt "\r\n", ##__VA_ARGS__)
#else
  #define ELF_INFO(fmt, ...)  do {} while(0)
#endif

#ifdef ELF_LOG_LEVEL_VERB
  #define ELF_VERB(fmt, ...)  printf(ELF_PREFIX "  " fmt "\r\n", ##__VA_ARGS__)
#else
  #define ELF_VERB(fmt, ...)  do {} while(0)
#endif

#ifdef ELF_LOG_LEVEL_WARN
  #define ELF_WARN(fmt, ...)  printf(ELF_PREFIX "WARN: " fmt "\r\n", ##__VA_ARGS__)
#else
  #define ELF_WARN(fmt, ...)  do {} while(0)
#endif

/* 始终输出的错误信息 */
#define ELF_ERR(fmt, ...)   printf(ELF_PREFIX "ERROR: " fmt "\r\n", ##__VA_ARGS__)

/* ===================================================================
 * 内部工具宏
 * =================================================================== */

/* 检查指针是否在 buf 范围内，防止越界访问 */
#define IN_BUF(ctx, ptr, size) \
    ((uint8_t*)(ptr) >= (ctx)->buf && \
     (uint8_t*)(ptr) + (size) <= (ctx)->buf + (ctx)->buf_size)

/* section 类型名称（调试用）*/
static const char *shtype_name(uint32_t type)
{
    switch (type) {
    case SHT_NULL:     return "NULL";
    case SHT_PROGBITS: return "PROGBITS";
    case SHT_SYMTAB:   return "SYMTAB";
    case SHT_STRTAB:   return "STRTAB";
    case SHT_RELA:     return "RELA";
    case SHT_NOBITS:   return "NOBITS";
    case SHT_REL:      return "REL";
    default:           return "OTHER";
    }
}

/* ARM 重定向类型名称（调试用）*/
static const char *reloc_type_name(uint8_t type)
{
    switch (type) {
    case R_ARM_NONE:      return "NONE";
    case R_ARM_ABS32:     return "ABS32";
    case R_ARM_RELATIVE:  return "RELATIVE";
    case R_ARM_THM_CALL:  return "THM_CALL";
    case R_ARM_CALL:      return "CALL";
    case R_ARM_JUMP24:    return "JUMP24";
    case R_ARM_TARGET1:   return "TARGET1";
    case R_ARM_V4BX:      return "V4BX";
    default:              return "UNKNOWN";
    }
}

/* ===================================================================
 * 第一部分：ARM 重定向计算（来自 arch/arm/core/elf.c）
 * =================================================================== */

/**
 * arm_relocate() — 对单条重定向记录施加修正
 *
 * @param type     重定向类型（R_ARM_ABS32 等）
 * @param loc      指向 buf 中需要修正的位置
 * @param sym_val  符号的运行时地址（已加 offset）
 * @param addend   加数（RELA 类型为 r_addend，REL 类型为 *loc 原始值）
 *
 * ARM ELF ABI 符号约定（IHI0044）：
 *   S = sym_val，A = addend，P = 被修正位置的运行时地址
 */
static int arm_relocate(uint8_t type, uint32_t *loc,
                        uint32_t sym_val, uint32_t addend)
{
    uint32_t old_val = *loc;   /* 保存原始值，供调试输出对比 */

    switch (type) {

    /* ------------------------------------------------------------------
     * R_ARM_ABS32 / R_ARM_TARGET1: *loc = S + A
     *
     * 最常见类型，出现于：
     *   - 向量表条目（Reset_Handler、IRQ handler 地址）
     *   - 全局变量地址
     *   - 函数指针
     *   - .data 段中的指针初始值
     *
     * 处理：sym_val 已是运行时绝对地址，加上 addend 后直接写入。
     * ------------------------------------------------------------------ */
    case R_ARM_ABS32:
    case R_ARM_TARGET1:
        *loc = sym_val + addend;
        ELF_VERB("ABS32    loc=0x%08x: 0x%08x -> 0x%08x  (S=0x%08x A=0x%08x)",
                 (uint32_t)(uintptr_t)loc, old_val, *loc, sym_val, addend);
        break;

    /* ------------------------------------------------------------------
     * R_ARM_RELATIVE: *loc = B(S) + A
     *
     * 出现于：--emit-relocs 或 PIC 编译产生的相对地址引用。
     * 符号索引为 0 时，sym_val 传入的是 offset 本身（基址偏移）。
     * addend 从 *loc 读取（即链接时写入的原始值）。
     *
     * 处理：在 *loc 原有值基础上加 offset。
     * ------------------------------------------------------------------ */
    case R_ARM_RELATIVE:
        *loc += addend;
        ELF_VERB("RELATIVE loc=0x%08x: 0x%08x -> 0x%08x  (addend=0x%08x)",
                 (uint32_t)(uintptr_t)loc, old_val, *loc, addend);
        break;

    /* ------------------------------------------------------------------
     * R_ARM_THM_CALL: Thumb BL/BLX 指令的目标地址修正
     *
     * 出现于：所有 Thumb 模式下的函数调用（bl 指令）。
     *
     * Thumb-2 BL 编码格式（两个 16-bit half-word，共 32-bit）：
     *   上半字: [15:11]=11110  [10]=S  [9:0]=imm10
     *   下半字: [15:14]=11     [13]=J1 [12]=1 [11]=J2 [10:0]=imm11
     *
     * 最终偏移 = SignExtend(S:I1:I2:imm10:imm11:'0', 25)
     * 其中 I1 = !(J1 XOR S)，I2 = !(J2 XOR S)
     * ------------------------------------------------------------------ */
    case R_ARM_THM_CALL: {
        uint16_t upper = ((uint16_t *)loc)[0];
        uint16_t lower = ((uint16_t *)loc)[1];

        /* 目标地址：sym_val 已是运行时绝对地址，确保 bit0=1（Thumb）*/
        uint32_t target = (sym_val + addend) | 1U;

        /*
         * 计算相对偏移（相对于当前指令 PC+4）
         *
         * 注意：此处 loc 是 buf 中的地址（非运行时地址），
         * 对于 --emit-relocs 基址0链接的 ELF，loc 在 buf 中的偏移
         * 恰好等于其链接时虚拟地址，所以加上 offset 即得运行时地址。
         *
         * 但由于 arm_relocate 不知道 offset，这里直接使用：
         *   PC_runtime = (loc 在 section 中的虚拟地址) + offset + 4
         * 对于基址0链接：loc 虚拟地址 = loc 在 buf 中的偏移 - section文件偏移 + section虚拟地址
         * 这个计算在调用层（elf_relocate）需要传入，此处简化为：
         * 相对偏移 = target - (loc_runtime + 4)
         * 其中 loc_runtime 由调用者通过 sym_val 已隐含处理。
         *
         * 实用简化：对于静态重定向，直接重算 BL 的 imm 字段。
         * 需要知道 loc 的运行时地址，这里用 loc 指针值代替
         *（在嵌入式裸机 RAM 中运行时，指针值 == 运行时地址）。
         */
        uint32_t loc_runtime = (uint32_t)(uintptr_t)loc;
        uint32_t pc = loc_runtime + 4U;

        /* 相对偏移（带符号，字节，向下对齐到2）*/
        int32_t  rel = (int32_t)(target - pc) & ~1;

        /* 重新编码 S / I1 / I2 / imm10 / imm11 */
        uint32_t s     = (rel < 0) ? 1U : 0U;
        uint32_t urel  = (uint32_t)(rel < 0 ? -rel : rel);
        uint32_t imm10 = (urel >> 12) & 0x3ffU;
        uint32_t imm11 = (urel >>  1) & 0x7ffU;
        uint32_t i1    = (urel >> 23) & 0x1U;
        uint32_t i2    = (urel >> 22) & 0x1U;
        uint32_t j1    = (!(i1 ^ s)) & 0x1U;
        uint32_t j2    = (!(i2 ^ s)) & 0x1U;

        /* 保留上下半字的固定位，更新可变位 */
        ((uint16_t *)loc)[0] = (uint16_t)((upper & 0xf800U) | (s << 10) | imm10);
        ((uint16_t *)loc)[1] = (uint16_t)((lower & 0xc000U) | (j1 << 13) |
                                           (1U << 12) | (j2 << 11) | imm11);

        ELF_VERB("THM_CALL loc=0x%08x: upper=0x%04x lower=0x%04x -> "
                 "upper=0x%04x lower=0x%04x  target=0x%08x rel=%d",
                 loc_runtime,
                 (uint32_t)upper, (uint32_t)lower,
                 (uint32_t)((uint16_t *)loc)[0],
                 (uint32_t)((uint16_t *)loc)[1],
                 target, rel);
        break;
    }

    /* ------------------------------------------------------------------
     * R_ARM_CALL / R_ARM_JUMP24: ARM 模式 BL/B 指令（非 Thumb）
     *
     * Cortex-M（包括 M4）只运行 Thumb 模式，正常情况下不会出现。
     * 遇到时输出警告并跳过，不影响其余重定向处理。
     * ------------------------------------------------------------------ */
    case R_ARM_CALL:
    case R_ARM_JUMP24:
        ELF_WARN("ARM mode CALL/JUMP24 at loc=0x%08x (Cortex-M is Thumb-only, skip)",
                 (uint32_t)(uintptr_t)loc);
        break;

    /* ------------------------------------------------------------------
     * R_ARM_V4BX: ARMv4T 的 BX 指令修正，Cortex-M 不使用
     * ------------------------------------------------------------------ */
    case R_ARM_V4BX:
        ELF_VERB("V4BX     loc=0x%08x: ignored (Cortex-M)", (uint32_t)(uintptr_t)loc);
        break;

    /* ------------------------------------------------------------------
     * R_ARM_NONE: 无操作
     * ------------------------------------------------------------------ */
    case R_ARM_NONE:
        break;

    default:
        ELF_WARN("unsupported reloc type %d (%s) at loc=0x%08x, skip",
                 type, reloc_type_name(type), (uint32_t)(uintptr_t)loc);
        break;
    }

    return ELF_OK;
}

/* ===================================================================
 * 第二部分：ELF 解析（来自 llext_load.c : llext_find_tables()）
 * =================================================================== */

int elf_parse(elf_ctx_t *ctx, uint8_t *buf, uint32_t buf_size)
{
    if (!ctx || !buf || buf_size < sizeof(elf32_ehdr)) {
        ELF_ERR("elf_parse: invalid params (ctx=%p buf=%p size=%u)",
                (void*)ctx, (void*)buf, buf_size);
        return ELF_ERR_PARAM;
    }

    memset(ctx, 0, sizeof(elf_ctx_t));
    ctx->buf      = buf;
    ctx->buf_size = buf_size;

    ELF_INFO("=== elf_parse start: buf=0x%08x size=%u bytes ===",
             (uint32_t)(uintptr_t)buf, buf_size);

    /* ---- 校验 ELF header ---- */
    elf32_ehdr *ehdr = (elf32_ehdr *)buf;
    ctx->ehdr = ehdr;

    /* 魔数校验 */
    if (*(uint32_t *)ehdr->e_ident != ELF_MAGIC) {
        ELF_ERR("bad magic: 0x%08x (expected 0x%08x)",
                *(uint32_t *)ehdr->e_ident, ELF_MAGIC);
        return ELF_ERR_MAGIC;
    }

    /* 32-bit 校验 */
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
        ELF_ERR("not 32-bit ELF (EI_CLASS=%d)", ehdr->e_ident[EI_CLASS]);
        return ELF_ERR_CLASS;
    }

    /* ARM 架构校验 */
    if (ehdr->e_machine != EM_ARM) {
        ELF_ERR("not ARM ELF (e_machine=0x%04x, expected 0x%04x=EM_ARM)",
                ehdr->e_machine, EM_ARM);
        return ELF_ERR_MACHINE;
    }

    ELF_INFO("ELF header OK:");
    ELF_INFO("  e_type    = %u (%s)",
             ehdr->e_type,
             ehdr->e_type == ET_EXEC ? "ET_EXEC" :
             ehdr->e_type == ET_DYN  ? "ET_DYN"  : "OTHER");
    ELF_INFO("  e_machine = 0x%04x (ARM)", ehdr->e_machine);
    ELF_INFO("  e_entry   = 0x%08x", ehdr->e_entry);
    ELF_INFO("  e_shoff   = 0x%08x", ehdr->e_shoff);
    ELF_INFO("  e_shnum   = %u sections", ehdr->e_shnum);
    ELF_INFO("  e_shstrndx= %u", ehdr->e_shstrndx);
    ELF_INFO("  e_flags   = 0x%08x", ehdr->e_flags);

    /* ---- 定位 section header table ---- */
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) {
        ELF_ERR("no section headers (shoff=%u shnum=%u)",
                ehdr->e_shoff, ehdr->e_shnum);
        return ELF_ERR_NO_SYMTAB;
    }

    elf32_shdr *shdrs = (elf32_shdr *)(buf + ehdr->e_shoff);
    if (!IN_BUF(ctx, shdrs, (uint32_t)ehdr->e_shnum * sizeof(elf32_shdr))) {
        ELF_ERR("section header table out of buffer");
        return ELF_ERR_PARAM;
    }
    ctx->shdrs = shdrs;

    /* ---- 找 section 名称字符串表（shstrtab）---- */
    if (ehdr->e_shstrndx != 0 && ehdr->e_shstrndx < ehdr->e_shnum) {
        elf32_shdr *shstr_shdr = &shdrs[ehdr->e_shstrndx];
        ctx->shstrtab = (const char *)(buf + shstr_shdr->sh_offset);
        ELF_INFO("shstrtab at file offset 0x%08x", shstr_shdr->sh_offset);
    } else {
        ELF_WARN("no shstrtab, section names unavailable");
    }

    /* ---- 遍历所有 section，分类记录 ---- */
    ELF_INFO("--- section table (%u entries) ---", ehdr->e_shnum);

    for (uint32_t i = 0; i < ehdr->e_shnum; i++) {
        elf32_shdr *shdr = &shdrs[i];

        const char *name = "(no name)";
        if (ctx->shstrtab && shdr->sh_name) {
            name = ctx->shstrtab + shdr->sh_name;
        }

        /* 打印所有 section 的基本信息 */
        ELF_INFO("  [%2u] %-20s type=%-8s addr=0x%08x off=0x%06x size=%-6u flags=0x%x",
                 i, name, shtype_name(shdr->sh_type),
                 shdr->sh_addr, shdr->sh_offset, shdr->sh_size,
                 shdr->sh_flags);

        switch (shdr->sh_type) {

        case SHT_SYMTAB:
            ctx->symtab    = (elf32_sym *)(buf + shdr->sh_offset);
            ctx->sym_count = shdr->sh_size / sizeof(elf32_sym);
            /* sh_link 指向对应的字符串表 */
            if (shdr->sh_link < ehdr->e_shnum) {
                elf32_shdr *str_shdr = &shdrs[shdr->sh_link];
                ctx->strtab = (const char *)(buf + str_shdr->sh_offset);
            }
            ELF_INFO("        ^-- SYMTAB: %u symbols, strtab section=%u",
                     ctx->sym_count, shdr->sh_link);
            break;

        case SHT_REL:
        case SHT_RELA:
            if (ctx->rel_count < 16) {
                ctx->rel_shdrs[ctx->rel_count++] = shdr;
                ELF_INFO("        ^-- %s: %u entries, targets section[%u]",
                         shdr->sh_type == SHT_REL ? "REL" : "RELA",
                         shdr->sh_size / shdr->sh_entsize,
                         shdr->sh_info);
            } else {
                ELF_WARN("too many REL/RELA sections (>16), section[%u] ignored", i);
            }
            break;

        case SHT_PROGBITS:
        case SHT_NOBITS:
            if ((shdr->sh_flags & SHF_ALLOC) && shdr->sh_size > 0) {
                if (ctx->load_count < 16) {
                    ctx->load_shdrs[ctx->load_count++] = shdr;
                    ELF_INFO("        ^-- LOAD[%u]: %s%s%s",
                             ctx->load_count - 1,
                             (shdr->sh_flags & SHF_EXECINSTR) ? "EXEC " : "",
                             (shdr->sh_flags & SHF_WRITE)     ? "WRITE " : "",
                             shdr->sh_type == SHT_NOBITS      ? "(bss, zero-fill)" : "");
                } else {
                    ELF_WARN("too many loadable sections (>16), section[%u] ignored", i);
                }
            }
            break;

        default:
            break;
        }
    }

    /* ---- 结果汇总 ---- */
    if (!ctx->symtab) {
        /* ARMCC 不加 --emit_relocs 时无 SYMTAB，值域重定向模式下不需要它 */
        ELF_WARN("no SYMTAB section (ARMCC normal build), value-range reloc mode");
    }

    ELF_INFO("--- parse summary ---");
    ELF_INFO("  loadable sections : %u", ctx->load_count);
    ELF_INFO("  reloc sections    : %u", ctx->rel_count);
    ELF_INFO("  symbols           : %u", ctx->sym_count);
    ELF_INFO("  entry point       : 0x%08x", ehdr->e_entry);
    ELF_INFO("=== elf_parse done ===");

    return ELF_OK;   /* 不再因为缺少 SYMTAB 而报错 */
}

/* ===================================================================
 * 第三部分：重定向遍历（来自 llext_link.c : llext_link()）
 * =================================================================== */
int elf_relocate(elf_ctx_t *ctx, uint32_t offset)
{
    if (!ctx || !ctx->buf) {
        ELF_ERR("elf_relocate: invalid context");
        return ELF_ERR_PARAM;
    }

    ctx->offset = offset;

    ELF_INFO("=== elf_relocate start: offset=0x%08x (value-range mode) ===", offset);

    uint32_t total_fixed = 0;

    for (uint32_t i = 0; i < ctx->load_count; i++) {
        elf32_shdr *shdr = ctx->load_shdrs[i];

        /* .bss 在文件中无内容，跳过 */
        if (shdr->sh_type == SHT_NOBITS) continue;

        /* 只处理有 SHF_ALLOC 标志的段 */
        if (!(shdr->sh_flags & SHF_ALLOC)) continue;

        uint32_t *base       = (uint32_t *)(ctx->buf + shdr->sh_offset);
        uint32_t  word_count = shdr->sh_size / 4U;

        const char *name = "(?)";
        if (ctx->shstrtab && shdr->sh_name) {
            name = ctx->shstrtab + shdr->sh_name;
        }

        ELF_INFO("  scanning section \"%s\": %u words", name, word_count);

        uint32_t sec_fixed = 0;

        for (uint32_t w = 0; w < word_count; w++) {
            uint32_t val = base[w];

            /*
             * scatter 文件：
             *   Flash ORIGIN = 0x00000000，链接时 flash 符号落在
             *   [0x00000001, 0x0FFFFFFF]
             *
             *   RAM1 ORIGIN = 0x20000000（已是真实地址，不动）
             *   RAM2 ORIGIN = 0x10000000（已是真实地址，不动）
             *
             * 只对 flash 范围的值加 offset。
             */
            if (val >= 0x00000001U && val < 0x10000000U) {
                ELF_VERB("  [%u] 0x%08x -> 0x%08x", w, val, val + offset);
                base[w] = val + offset;
                sec_fixed++;
            }
        }

        ELF_INFO("    fixed %u words", sec_fixed);
        total_fixed += sec_fixed;
    }

    ELF_INFO("=== elf_relocate done: total fixed=%u ===", total_fixed);
    return ELF_OK;
}
/* ===================================================================
 * 第四部分：辅助接口
 * =================================================================== */
int elf_get_section(const elf_ctx_t *ctx, uint32_t idx, elf_section_info_t *info)
{
    if (!ctx || !info || idx >= ctx->load_count) {
        return ELF_ERR_PARAM;
    }

    elf32_shdr *shdr = ctx->load_shdrs[idx];

    /*
     * sh_addr 是链接时虚拟地址：
     *   flash section：base-0，如 0x00000000，需要加 offset
     *   RAM section：  真实物理地址，如 0x20000000，不能加 offset
     *
     * 用值域判断区分：< 0x10000000 的是 flash，否则是 RAM
     */
    if (shdr->sh_addr < 0x10000000U) {
        info->load_addr = shdr->sh_addr + ctx->offset;
    } else {
        info->load_addr = shdr->sh_addr;  /* RAM 地址，保持不变 */
    }

    info->size   = shdr->sh_size;
    info->is_bss = (shdr->sh_type == SHT_NOBITS) ? 1 : 0;
    info->name   = "";

    if (ctx->shstrtab && shdr->sh_name) {
        info->name = ctx->shstrtab + shdr->sh_name;
    }

    info->data = info->is_bss ? NULL : ctx->buf + shdr->sh_offset;

    return ELF_OK;
}

uint32_t elf_get_entry(const elf_ctx_t *ctx)
{
    if (!ctx || !ctx->ehdr) {
        return 0;
    }
    /* e_entry 是链接时虚拟地址，加 offset 得运行时地址 */
    return ctx->ehdr->e_entry + ctx->offset;
}

/**
 * elf_dump_sections() — 打印所有可加载 section 的详细信息
 *
 * 在 write_sections_to_flash() 之前调用，用于核查地址布局。
 */
void elf_dump_sections(const elf_ctx_t *ctx)
{
    if (!ctx) return;

    ELF_INFO("=== loadable section dump (offset=0x%08x) ===", ctx->offset);
    ELF_INFO("  %-4s  %-20s  %-12s  %-10s  %-6s  %s",
             "idx", "name", "load_addr", "file_off", "size", "type");
    ELF_INFO("  %-4s  %-20s  %-12s  %-10s  %-6s  %s",
             "---", "--------------------", "----------", "----------",
             "------", "----");

    for (uint32_t i = 0; i < ctx->load_count; i++) {
        elf_section_info_t sec;
        if (elf_get_section(ctx, i, &sec) != ELF_OK) continue;

        elf32_shdr *shdr = ctx->load_shdrs[i];

        ELF_INFO("  [%2u]  %-20s  0x%08x    0x%08x  %-6u  %s",
                 i,
                 sec.name,
                 sec.load_addr,
                 shdr->sh_offset,
                 sec.size,
                 sec.is_bss ? "BSS (zero-fill, App startup clears)"
                            : "DATA (write to Flash)");
    }

    ELF_INFO("  entry point: 0x%08x", elf_get_entry(ctx));
    ELF_INFO("=== dump end ===");
}



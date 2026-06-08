/**
 * elf_loader.c — ELF 解析与重定向实现
 *
 * 逻辑来源：
 *   elf_parse()        → Zephyr subsys/llext/llext_load.c : llext_find_tables()
 *   elf_relocate_ex()  → Zephyr subsys/llext/llext_link.c : llext_link()
 *   arm_relocate()     → Zephyr arch/arm/core/elf.c       : arch_elf_relocate()
 *
 * 调试输出分级：
 *   ELF_LOG_LEVEL_INFO  — 关键步骤（默认开启）
 *   ELF_LOG_LEVEL_VERB  — 每条重定向记录（量大时关闭）
 *   ELF_LOG_LEVEL_WARN  — 警告（跳过的符号等，默认开启）
 *
 * elf_relocate_ex() 重定向策略（v4）：
 *
 *   exec 段（ER_IROM1）：
 *     ① Scatter Table 精确处理（优先，4-word 滑动窗口检测）
 *     ② 向量表  [0, e_entry_stripped+4)    — 逐 word 值域过滤后重定向
 *     ③ Literal Pool  由 LDR PC-relative 指令反向追踪精确定位
 *        — 16-bit: LDR Rn,[PC,#imm8*4]  opcode 0x48xx
 *        — 32-bit: LDR Rn,[PC,#imm12]   opcode 0xF85F....
 *        — ADR / VLDR 等其他 PC-relative 指令不产生需重定向的地址常量，忽略
 *     函数体内所有其余 word 一律跳过，不做值域猜测
 *
 *   write 段（RW_IRAM1 / .data）：
 *     全段扫描，值域过滤（高16位为0、4字节对齐、范围校验）
 *
 *   过滤⑤ Thumb 指针范围收紧（v4 新增）：
 *     奇数 Thumb 指针用实际代码大小（actual_code_end）而非分区大小校验，
 *     防止大数值常量（如 0x0001387f）被误判为合法函数指针。
 */

#include "elf_loader.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ===================================================================
 * 调试输出控制
 * =================================================================== */

#define ELF_LOG_LEVEL_INFO  1
/* #define ELF_LOG_LEVEL_VERB  1 */
#define ELF_LOG_LEVEL_WARN  1

#define ELF_PREFIX  "[elf] "

#ifdef ELF_LOG_LEVEL_INFO
  #define ELF_INFO(fmt, ...)  printf(ELF_PREFIX fmt "\r\n", ##__VA_ARGS__)
#else
  #define ELF_INFO(fmt, ...)  do {} while (0)
#endif

#ifdef ELF_LOG_LEVEL_VERB
  #define ELF_VERB(fmt, ...)  printf(ELF_PREFIX "  " fmt "\r\n", ##__VA_ARGS__)
#else
  #define ELF_VERB(fmt, ...)  do {} while (0)
#endif

#ifdef ELF_LOG_LEVEL_WARN
  #define ELF_WARN(fmt, ...)  printf(ELF_PREFIX "WARN: " fmt "\r\n", ##__VA_ARGS__)
#else
  #define ELF_WARN(fmt, ...)  do {} while (0)
#endif

#define ELF_ERR(fmt, ...)   printf(ELF_PREFIX "ERROR: " fmt "\r\n", ##__VA_ARGS__)

/* ===================================================================
 * 内部工具宏
 * =================================================================== */

#define IN_BUF(ctx, ptr, size) \
    ((uint8_t *)(ptr) >= (ctx)->buf && \
     (uint8_t *)(ptr) + (size) <= (ctx)->buf + (ctx)->buf_size)

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

// /* ===================================================================
//  * 第一部分：ARM 重定向计算（来自 arch/arm/core/elf.c）
//  * =================================================================== */

// static int arm_relocate(uint8_t type, uint32_t *loc,
//                         uint32_t sym_val, uint32_t addend)
// {
//     uint32_t old_val = *loc;

//     switch (type) {

//     case R_ARM_ABS32:
//     case R_ARM_TARGET1:
//         *loc = sym_val + addend;
//         ELF_VERB("ABS32    loc=0x%08x: 0x%08x -> 0x%08x  (S=0x%08x A=0x%08x)",
//                  (uint32_t)(uintptr_t)loc, old_val, *loc, sym_val, addend);
//         break;

//     case R_ARM_RELATIVE:
//         *loc += addend;
//         ELF_VERB("RELATIVE loc=0x%08x: 0x%08x -> 0x%08x  (addend=0x%08x)",
//                  (uint32_t)(uintptr_t)loc, old_val, *loc, addend);
//         break;

//     case R_ARM_THM_CALL: {
//         uint16_t upper = ((uint16_t *)loc)[0];
//         uint16_t lower = ((uint16_t *)loc)[1];

//         uint32_t target      = (sym_val + addend) | 1U;
//         uint32_t loc_runtime = (uint32_t)(uintptr_t)loc;
//         uint32_t pc          = loc_runtime + 4U;
//         int32_t  rel         = (int32_t)(target - pc) & ~1;

//         uint32_t s     = (rel < 0) ? 1U : 0U;
//         uint32_t urel  = (uint32_t)(rel < 0 ? -rel : rel);
//         uint32_t imm10 = (urel >> 12) & 0x3ffU;
//         uint32_t imm11 = (urel >>  1) & 0x7ffU;
//         uint32_t i1    = (urel >> 23) & 0x1U;
//         uint32_t i2    = (urel >> 22) & 0x1U;
//         uint32_t j1    = (!(i1 ^ s)) & 0x1U;
//         uint32_t j2    = (!(i2 ^ s)) & 0x1U;

//         ((uint16_t *)loc)[0] = (uint16_t)((upper & 0xf800U) | (s << 10) | imm10);
//         ((uint16_t *)loc)[1] = (uint16_t)((lower & 0xc000U) | (j1 << 13) |
//                                            (1U << 12) | (j2 << 11) | imm11);

//         ELF_VERB("THM_CALL loc=0x%08x: upper=0x%04x lower=0x%04x -> "
//                  "upper=0x%04x lower=0x%04x  target=0x%08x rel=%d",
//                  loc_runtime,
//                  (uint32_t)upper, (uint32_t)lower,
//                  (uint32_t)((uint16_t *)loc)[0],
//                  (uint32_t)((uint16_t *)loc)[1],
//                  target, rel);
//         break;
//     }

//     case R_ARM_CALL:
//     case R_ARM_JUMP24:
//         ELF_WARN("ARM mode CALL/JUMP24 at loc=0x%08x (Cortex-M is Thumb-only, skip)",
//                  (uint32_t)(uintptr_t)loc);
//         break;

//     case R_ARM_V4BX:
//         ELF_VERB("V4BX     loc=0x%08x: ignored (Cortex-M)", (uint32_t)(uintptr_t)loc);
//         break;

//     case R_ARM_NONE:
//         break;

//     default:
//         ELF_WARN("unsupported reloc type %d (%s) at loc=0x%08x, skip",
//                  type, reloc_type_name(type), (uint32_t)(uintptr_t)loc);
//         break;
//     }

//     return ELF_OK;
// }

/* ===================================================================
 * 第二部分：ELF 解析（来自 llext_load.c : llext_find_tables()）
 * =================================================================== */

int elf_parse(elf_ctx_t *ctx, uint8_t *buf, uint32_t buf_size)
{
    if (!ctx || !buf || buf_size < sizeof(elf32_ehdr)) {
        ELF_ERR("elf_parse: invalid params (ctx=%p buf=%p size=%u)",
                (void *)ctx, (void *)buf, buf_size);
        return ELF_ERR_PARAM;
    }

    memset(ctx, 0, sizeof(elf_ctx_t));
    ctx->buf      = buf;
    ctx->buf_size = buf_size;

    ELF_INFO("=== elf_parse start: buf=0x%08x size=%u bytes ===",
             (uint32_t)(uintptr_t)buf, buf_size);

    elf32_ehdr *ehdr = (elf32_ehdr *)buf;
    ctx->ehdr = ehdr;

    if (*(uint32_t *)ehdr->e_ident != ELF_MAGIC) {
        ELF_ERR("bad magic: 0x%08x (expected 0x%08x)",
                *(uint32_t *)ehdr->e_ident, ELF_MAGIC);
        return ELF_ERR_MAGIC;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
        ELF_ERR("not 32-bit ELF (EI_CLASS=%d)", ehdr->e_ident[EI_CLASS]);
        return ELF_ERR_CLASS;
    }

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

    if (ehdr->e_shstrndx != 0 && ehdr->e_shstrndx < ehdr->e_shnum) {
        elf32_shdr *shstr_shdr = &shdrs[ehdr->e_shstrndx];
        ctx->shstrtab = (const char *)(buf + shstr_shdr->sh_offset);
        ELF_INFO("shstrtab at file offset 0x%08x", shstr_shdr->sh_offset);
    } else {
        ELF_WARN("no shstrtab, section names unavailable");
    }

    ELF_INFO("--- section table (%u entries) ---", ehdr->e_shnum);

    for (uint32_t i = 0; i < ehdr->e_shnum; i++) {
        elf32_shdr *shdr = &shdrs[i];

        const char *name = "(no name)";
        if (ctx->shstrtab && shdr->sh_name) {
            name = ctx->shstrtab + shdr->sh_name;
        }

        ELF_INFO("  [%2u] %-20s type=%-8s addr=0x%08x off=0x%06x size=%-6u flags=0x%x",
                 i, name, shtype_name(shdr->sh_type),
                 shdr->sh_addr, shdr->sh_offset, shdr->sh_size,
                 shdr->sh_flags);

        switch (shdr->sh_type) {

        case SHT_SYMTAB:
            ctx->symtab    = (elf32_sym *)(buf + shdr->sh_offset);
            ctx->sym_count = shdr->sh_size / sizeof(elf32_sym);
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

    if (!ctx->symtab) {
        ELF_WARN("no SYMTAB section (ARMCC normal build), value-range reloc mode");
    }

    ELF_INFO("--- parse summary ---");
    ELF_INFO("  loadable sections : %u", ctx->load_count);
    ELF_INFO("  reloc sections    : %u", ctx->rel_count);
    ELF_INFO("  symbols           : %u", ctx->sym_count);
    ELF_INFO("  entry point       : 0x%08x", ehdr->e_entry);
    ELF_INFO("=== elf_parse done ===");

    return ELF_OK;
}

/* ===================================================================
 * 第三部分：重定向实现
 * =================================================================== */

/* ---- RAM 范围（STM32L431，根据实际芯片修改） ---- */
#define RELOC_RAM1_BASE  0x20000000U
#define RELOC_RAM1_END   0x20010000U
#define RELOC_RAM2_BASE  0x10000000U
#define RELOC_RAM2_END   0x10004000U

static int reloc_in_ram(uint32_t addr)
{
    return (addr >= RELOC_RAM1_BASE && addr < RELOC_RAM1_END) ||
           (addr >= RELOC_RAM2_BASE && addr < RELOC_RAM2_END);
}

/* ------------------------------------------------------------------
 * is_scatter_entry() — 4-word 滑动窗口检测 ARMCC scatter 条目
 *
 * ARMCC __scatterload 区域描述符格式（每条目 4 word）：
 *   [0] src : Flash 源地址，偶数，4 字节对齐  ← 需要重定向
 *   [1] dst : RAM 目标地址                    ← 保留
 *   [2] len : 字节数                          ← 保留
 *   [3] fn  : __scatterload_copy/_zeroinit    ← 需要重定向
 * ------------------------------------------------------------------ */
static int is_scatter_entry(const uint32_t *base,
                            uint32_t        w,
                            uint32_t        word_end,
                            uint32_t        link_end)
{
    if (w + 4U > word_end) return 0;

    uint32_t src = base[w + 0U];
    uint32_t dst = base[w + 1U];
    uint32_t len = base[w + 2U];
    uint32_t fn  = base[w + 3U];

    if (src == 0U || (src & 3U) != 0U || src >= link_end)  return 0;
    if (!reloc_in_ram(dst))                                  return 0;
    if (len == 0U || len >= 0x10000U)                        return 0;
    if (fn == 0U || (fn & 3U) != 0U || fn >= link_end || fn == src) return 0;

    return 1;
}

static uint32_t handle_scatter_entry(uint32_t *base,
                                     uint32_t  w,
                                     uint32_t  offset)
{
    uint32_t old_src = base[w + 0U];
    uint32_t old_fn  = base[w + 3U];

    base[w + 0U] = old_src + offset;
    base[w + 3U] = old_fn  + offset;

    ELF_INFO("scatter [w=%u] src:0x%08x->0x%08x  fn:0x%08x->0x%08x  "
             "dst=0x%08x(keep)  len=0x%08x(keep)",
             w,
             old_src, base[w + 0U],
             old_fn,  base[w + 3U],
             base[w + 1U],
             base[w + 2U]);

    return 4U;
}

/* ------------------------------------------------------------------
 * build_litpool_bitmap() — 扫描 Thumb LDR PC-relative 指令，
 *                          精确标记所有 Literal Pool word 的位置。
 *
 * 覆盖指令：
 *   16-bit  LDR Rn, [PC, #imm8*4]   编码: 0x48xx .. 0x4Fxx
 *   32-bit  LDR Rn, [PC, #imm12]    编码: 0xF85F 0x?nnn
 *
 * 不覆盖（不产生需重定向的地址常量）：
 *   ADR、VLDR、LDR 非 PC-base 等
 *
 * @param base      exec section 的 uint32_t 数组
 * @param word_end  section 总 word 数
 * @param bitmap    调用方分配的位图，大小 >= (word_end+7)/8 字节，
 *                  调用前须已清零
 * @param lp_count  输出：标记的 Literal Pool word 总数
 * ------------------------------------------------------------------ */
static void build_litpool_bitmap(const uint32_t *base,
                                 uint32_t        word_end,
                                 uint8_t        *bitmap,
                                 uint32_t       *lp_count)
{
    const uint16_t *h16 = (const uint16_t *)base;
    uint32_t half_end   = word_end * 2U;
    uint32_t count      = 0U;

    for (uint32_t h = 0U; h < half_end; h++) {
        uint16_t instr = h16[h];

        /* ── Thumb PC 规则：PC = 当前指令地址 + 4，再向下对齐到 4 字节 ── */
        uint32_t instr_addr = h * 2U;          /* 指令在 section 内的字节偏移 */
        uint32_t pc_align   = (instr_addr + 4U) & ~3U;

        /* ── 16-bit LDR Rn, [PC, #imm8*4]  opcode[15:11]=01001 ── */
        if ((instr & 0xF800U) == 0x4800U) {
            uint32_t imm     = (uint32_t)(instr & 0x00FFU) * 4U;
            uint32_t lp_byte = pc_align + imm;
            uint32_t lp_w    = lp_byte / 4U;

            if (lp_w < word_end) {
                if (!((bitmap[lp_w / 8U] >> (lp_w % 8U)) & 1U)) {
                    bitmap[lp_w / 8U] |= (uint8_t)(1U << (lp_w % 8U));
                    count++;
                    ELF_VERB("litpool [h=%u] 16-bit LDR -> word[%u]=0x%08x",
                             h, lp_w, base[lp_w]);
                }
            }
            continue;
        }

        /* ── 32-bit LDR Rn, [PC, #imm12]
         *    上半字: 0xF85F
         *    下半字: 0x?nnn  (bits[11:0] = imm12, bits[15:12] = Rn)
         * ── */
        if (instr == 0xF85FU && h + 1U < half_end) {
            uint16_t lower   = h16[h + 1U];
            uint32_t imm     = (uint32_t)(lower & 0x0FFFU);
            uint32_t lp_byte = pc_align + imm;
            uint32_t lp_w    = lp_byte / 4U;

            if (lp_w < word_end) {
                if (!((bitmap[lp_w / 8U] >> (lp_w % 8U)) & 1U)) {
                    bitmap[lp_w / 8U] |= (uint8_t)(1U << (lp_w % 8U));
                    count++;
                    ELF_VERB("litpool [h=%u] 32-bit LDR -> word[%u]=0x%08x",
                             h, lp_w, base[lp_w]);
                }
            }
            h++;    /* 跳过下半字，避免误识别 */
            continue;
        }
    }

    *lp_count = count;
}

/* ------------------------------------------------------------------
 * relocate_word() — 对单个 word 施加值域过滤后重定向
 *
 * 返回 1 = 已修改，0 = 跳过
 *
 * 过滤层次：
 *   ② 双界收紧   [LINK_MIN, LINK_END)
 *   ③ 对齐校验   偶数必须 4 字节对齐
 *   ④ IEEE 754   指数/尾数特征匹配则跳过
 *   ⑤ Thumb 范围  奇数：stripped < LINK_END；偶数：< LINK_END
 *   ⑦ 重定向后越界回退
 * ------------------------------------------------------------------ */
static int relocate_word(uint32_t *slot,
                         uint32_t  offset,
                         uint32_t  LINK_MIN,
                         uint32_t  LINK_END,
                         uint32_t  LINK_END_STRICT, /* 实际代码上界，用于 Thumb 指针校验 */
                         uint32_t  RT_BASE,
                         uint32_t  RT_SIZE,
                         uint32_t  w,              /* 仅用于 VERB 日志 */
                         uint32_t *skip_bounds,
                         uint32_t *skip_align,
                         uint32_t *skip_float,
                         uint32_t *skip_thumb,
                         uint32_t *skip_postchk)
{
    uint32_t val = *slot;

    /* ② 双界收紧 */
    if (val == 0U || val < LINK_MIN || val >= LINK_END) {
        (*skip_bounds)++;
        return 0;
    }

    /* ③ 对齐校验（偶数指针必须 4 字节对齐）*/
    if ((val & 1U) == 0U && (val & 3U) != 0U) {
        ELF_VERB("[%3u] skip 0x%08x not 4-aligned", w, val);
        (*skip_align)++;
        return 0;
    }

    /* ④ IEEE 754 浮点排除 */
    {
        uint32_t exp  = (val >> 23) & 0xFFU;
        uint32_t mant = val & 0x7FFFFFU;
        if ((exp >= 0x01U && exp <= 0xFEU && mant != 0U) ||
            (exp >= 0x7EU && exp <= 0x9EU && mant == 0U)) {
            ELF_VERB("[%3u] skip 0x%08x IEEE754", w, val);
            (*skip_float)++;
            return 0;
        }
    }

    /* ⑤ Thumb / 数据指针范围二次校验 */
    if (val & 1U) {
        uint32_t stripped = val & ~1U;
        /* Thumb 函数指针：stripped 必须在实际代码范围内 */
        if (stripped == 0U || stripped >= LINK_END_STRICT) {
            ELF_VERB("[%3u] skip 0x%08x Thumb out-of-range (strict)", w, val);
            (*skip_thumb)++;
            return 0;
        }
    } else {
        /* 数据指针：在链接 Flash 范围内即可 */
        if (val >= LINK_END) {
            ELF_VERB("[%3u] skip 0x%08x even out-of-range", w, val);
            (*skip_thumb)++;
            return 0;
        }
    }

    /* ⑦ 重定向后越界回退 */
    uint32_t new_val = val + offset;
    {
        uint32_t addr    = new_val & ~1U;
        int ok_flash = (addr >= RT_BASE && addr < RT_BASE + RT_SIZE);
        int ok_ram   = reloc_in_ram(addr);
        if (!ok_flash && !ok_ram) {
            ELF_VERB("[%3u] skip 0x%08x postchk fail (new=0x%08x)", w, val, new_val);
            (*skip_postchk)++;
            return 0;
        }
    }

    ELF_VERB("[%3u] FIX 0x%08x -> 0x%08x", w, val, new_val);
    *slot = new_val;
    return 1;
}

/* ------------------------------------------------------------------
 * elf_relocate_ex() — 主重定向函数
 *
 * @param ctx          已 elf_parse() 完成的上下文
 * @param offset       运行时 Flash 起始地址（如 0x08020000）
 * @param app_max_size Flash 分区大小（字节）
 * ------------------------------------------------------------------ */
int elf_relocate_ex(elf_ctx_t *ctx, uint32_t offset, uint32_t app_max_size)
{
    if (!ctx || !ctx->buf) {
        ELF_ERR("elf_relocate_ex: invalid context");
        return ELF_ERR_PARAM;
    }

    ctx->offset = offset;

    const uint32_t LINK_MIN = 0x00000100U;
    const uint32_t LINK_END = app_max_size;
    const uint32_t RT_BASE  = offset;
    const uint32_t RT_SIZE  = app_max_size;

    uint32_t e_entry_stripped = ctx->ehdr ? (ctx->ehdr->e_entry & ~1U) : 0U;

    /* 计算实际代码上界：取所有 exec section 的 sh_addr + sh_size 最大值。
     * 用于 Thumb 指针校验（filter⑤），防止大数值常量被误判为函数指针。
     * 若无 exec section，退回 app_max_size。 */
    uint32_t actual_code_end = 0U;
    for (uint32_t i = 0; i < ctx->load_count; i++) {
        elf32_shdr *s = ctx->load_shdrs[i];
        if ((s->sh_flags & SHF_EXECINSTR) && !(s->sh_flags & SHF_WRITE)) {
            uint32_t end = s->sh_addr + s->sh_size;
            if (end > actual_code_end) actual_code_end = end;
        }
    }
    if (actual_code_end == 0U) actual_code_end = app_max_size;

    ELF_INFO("=== elf_relocate_ex ===");
    ELF_INFO("  offset          = 0x%08x", offset);
    ELF_INFO("  app_max_size    = 0x%08x  (%u KB)", app_max_size, app_max_size / 1024U);
    ELF_INFO("  link range      = [0x%08x, 0x%08x)", LINK_MIN, LINK_END);
    ELF_INFO("  link code end   = 0x%08x  (actual, for Thumb ptr check)", actual_code_end);
    ELF_INFO("  runtime range   = [0x%08x, 0x%08x)", RT_BASE, RT_BASE + RT_SIZE);
    ELF_INFO("  e_entry         = 0x%08x  (vec table end)", e_entry_stripped);

    uint32_t total_scanned   = 0U;
    uint32_t total_fixed     = 0U;
    uint32_t skip_section    = 0U;
    uint32_t skip_bounds     = 0U;
    uint32_t skip_align      = 0U;
    uint32_t skip_float      = 0U;
    uint32_t skip_thumb      = 0U;
    uint32_t skip_litpool    = 0U;
    uint32_t skip_postchk    = 0U;
    uint32_t scatter_entries = 0U;

    for (uint32_t i = 0; i < ctx->load_count; i++) {
        elf32_shdr *shdr = ctx->load_shdrs[i];

        if (shdr->sh_type == SHT_NOBITS) continue;

        uint32_t flags    = shdr->sh_flags;
        int      is_exec  = (flags & SHF_EXECINSTR) != 0;
        int      is_write = (flags & SHF_WRITE)     != 0;
        int      is_alloc = (flags & SHF_ALLOC)     != 0;

        if (!is_alloc) continue;

        const char *name = (ctx->shstrtab && shdr->sh_name)
                           ? ctx->shstrtab + shdr->sh_name : "(?)";

        /* ============================================================
         * 过滤①：Section 白名单
         *
         * 处理：
         *   exec  段（ER_IROM1）：向量表 + LDR-literal 标记的 Literal Pool
         *   write 段（RW_IRAM1）：全段值域扫描
         * 跳过：
         *   rodata（ALLOC only，无 EXEC 无 WRITE）
         * ============================================================ */
        int scan_exec  = (is_exec  && !is_write && shdr->sh_addr == 0U);
        int scan_write = (is_write && !is_exec);

        if (!scan_exec && !scan_write) {
            ELF_INFO("  [skip] \"%s\" (not exec@0 or write)", name);
            skip_section += shdr->sh_size / 4U;
            continue;
        }

        uint32_t *base     = (uint32_t *)(ctx->buf + shdr->sh_offset);
        uint32_t  word_end = shdr->sh_size / 4U;
        uint32_t  sec_fixed        = 0U;
        uint32_t  sec_scatter_hits = 0U;

        /* ============================================================
         * exec 段处理
         * ============================================================ */
        if (scan_exec) {

            /* ── 第一步：构建 Literal Pool 位图 ── */
            uint32_t bitmap_bytes = (word_end + 7U) / 8U;
            uint8_t *litpool_bmp  = (uint8_t *)calloc(bitmap_bytes, 1U);
            if (!litpool_bmp) {
                ELF_ERR("OOM: litpool bitmap alloc failed (%u bytes)", bitmap_bytes);
                return ELF_ERR_PARAM;
            }

            uint32_t lp_count = 0U;
            build_litpool_bitmap(base, word_end, litpool_bmp, &lp_count);
            ELF_INFO("  scanning exec \"%s\": %u words, litpool=%u entries",
                     name, word_end, lp_count);

            /* ── 第二步：Scatter + 向量表 + Literal Pool 三路处理 ── */
            uint32_t w = 0U;
            while (w < word_end) {

                /* ── Scatter Table 检测（优先级最高）── */
                if (is_scatter_entry(base, w, word_end, LINK_END)) {
                    uint32_t step = handle_scatter_entry(base, w, offset);
                    total_scanned += step;
                    total_fixed   += 2U;
                    sec_fixed     += 2U;
                    scatter_entries++;
                    sec_scatter_hits++;
                    w += step;
                    continue;
                }

                total_scanned++;

                /* ── word[0] = MSP，跳过 ── */
                if (w == 0U) {
                    ELF_VERB("[  0] skip MSP=0x%08x (RAM addr)", base[0]);
                    skip_litpool++;
                    w++;
                    continue;
                }

                int in_vec = (w * 4U < e_entry_stripped + 4U);
                int in_lp  = (int)((litpool_bmp[w / 8U] >> (w % 8U)) & 1U);

                if (!in_vec && !in_lp) {
                    /* 函数体内，不是地址，直接跳过 */
                    skip_litpool++;
                    w++;
                    continue;
                }

                /* ── 向量表内偶数 word 跳过（向量表只含奇数 Thumb 指针）── */
                if (in_vec && (base[w] & 1U) == 0U) {
                    ELF_VERB("[%3u] skip 0x%08x even in vec table", w, base[w]);
                    skip_litpool++;
                    w++;
                    continue;
                }

                /* ── 值域过滤 + 重定向 ── */
                int fixed = relocate_word(
                    &base[w], offset,
                    LINK_MIN, LINK_END, actual_code_end,
                    RT_BASE, RT_SIZE, w,
                    &skip_bounds, &skip_align,
                    &skip_float,  &skip_thumb, &skip_postchk);

                if (fixed) {
                    sec_fixed++;
                    total_fixed++;
                }
                w++;
            }

            free(litpool_bmp);

        } /* end scan_exec */

        /* ============================================================
         * write 段处理（.data / RW_IRAM1）
         *
         * 链接时 .data 里需要重定向的值是指向 Flash 的地址常量，
         * 特征：高16位为 0（链接基地址 0x00000000），4字节对齐，偶数。
         * 奇数（Thumb 函数指针）在 .data 里极少出现，但也用值域过滤处理。
         * ============================================================ */
        else { /* scan_write */

            ELF_INFO("  scanning write \"%s\": %u words", name, word_end);

            for (uint32_t w = 0U; w < word_end; w++) {
                total_scanned++;

                uint32_t val = base[w];

                /* 偶数地址：高16位必须为0（链接时 Flash 偏移 < 64KB）*/
                if ((val & 1U) == 0U && (val >> 16) != 0U) {
                    skip_bounds++;
                    continue;
                }

                int fixed = relocate_word(
                    &base[w], offset,
                    LINK_MIN, LINK_END, actual_code_end,
                    RT_BASE, RT_SIZE, w,
                    &skip_bounds, &skip_align,
                    &skip_float,  &skip_thumb, &skip_postchk);

                if (fixed) {
                    sec_fixed++;
                    total_fixed++;
                }
            }

        } /* end scan_write */

        ELF_INFO("    fixed %u words (%u scatter entries) in \"%s\"",
                 sec_fixed, sec_scatter_hits, name);

    } /* end section loop */

    ELF_INFO("=== elf_relocate_ex done ===");
    ELF_INFO("  scanned          : %u", total_scanned);
    ELF_INFO("  fixed (total)    : %u", total_fixed);
    ELF_INFO("  fixed (scatter)  : %u entries (%u words)", scatter_entries, scatter_entries * 2U);
    ELF_INFO("  skip[1-section]  : %u", skip_section);
    ELF_INFO("  skip[2-bounds ]  : %u", skip_bounds);
    ELF_INFO("  skip[3-align  ]  : %u", skip_align);
    ELF_INFO("  skip[4-float  ]  : %u", skip_float);
    ELF_INFO("  skip[5-thumb  ]  : %u", skip_thumb);
    ELF_INFO("  skip[6-litpool]  : %u", skip_litpool);
    ELF_INFO("  skip[7-postchk]  : %u", skip_postchk);

    return ELF_OK;
}

/* ------------------------------------------------------------------
 * elf_relocate() — 向后兼容包装
 * ------------------------------------------------------------------ */
int elf_relocate(elf_ctx_t *ctx, uint32_t offset)
{
#ifndef APP_A_SIZE
  #define APP_A_SIZE  (256U * 1024U)
#endif
    return elf_relocate_ex(ctx, offset, APP_A_SIZE);
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

    if (shdr->sh_addr < 0x10000000U) {
        info->load_addr = shdr->sh_addr + ctx->offset;
    } else {
        info->load_addr = shdr->sh_addr;
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
    if (!ctx || !ctx->ehdr) return 0U;
    return ctx->ehdr->e_entry + ctx->offset;
}

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



/**
 * elf_loader_stream.c — 流式 ELF 解析与重定向实现
 *
 * ── AXF 实测校验结论（LED.axf / ARMCC 6.19 / STM32L431）──
 *
 *   1. exec section sh_addr = 0x08020000（非 0），原 scan_exec 条件
 *      `sh_addr == 0U` 会导致整个 exec section 被跳过。
 *      修正：exec 判断改为 `is_exec && !is_write`。
 *
 *   2. AXF 含完整 REL 表（243 条），类型分布：
 *        R_ARM_ABS32           (type=2)  ×81  — 向量表 + literal pool 绝对地址
 *        R_ARM_THM_CALL        (type=10) ×120 — BL 指令，PC-relative，自校正
 *        R_ARM_THM_MOVW_ABS_NC (type=47) ×20  — MOVW 绝对低16位，需重定向
 *        R_ARM_THM_MOVT_ABS    (type=48) ×20  — MOVT 绝对高16位，需重定向
 *        R_ARM_THM_MOVW_PREL   (type=54) ×1   — PC-relative，忽略
 *        R_ARM_THM_ALU_PREL    (type=102)×1   — PC-relative，忽略
 *
 *   3. MOVW/MOVT 目标地址：4 对指向 Flash（0x08022xxx），16 对指向 RAM，
 *      Flash 目标必须重定向；RAM 目标（SRAM 地址不随 Flash offset 变化）
 *      按 REL 驱动处理，sym=0 时 addend=当前 imm16，offset 仅加到 Flash 范围。
 *      实现采用：解码 → 全值加 offset → 若结果落入运行时 Flash 则写回，否则保留。
 *
 *   4. LINK_MIN/LINK_END 在路径 B（值域猜测）中必须以实际链接基地址为基准，
 *      而非固定从 0 算起，否则 [LINK_MIN=0x100, LINK_END=app_max_size] 覆盖不到
 *      0x0802xxxx 的向量表条目。
 *
 *   5. write section 的 4 个 word 均为 RAM/常量（0x003d0900, 0x1, 0x10, 0），
 *      无需重定向，但代码流程仍正确通过并 writeback（zero-delta）。
 *
 * ── 内存开销（典型 256 KB 分区）──
 *
 *   meta_buf：~10 KB（可配置上限）
 *   work_buf：建议 4–16 KB；exec Pass-1 bitmap 占 256K/4/8 = 8 KB
 *             若 work_buf < 8 KB，bitmap 压缩到 work_buf 一半，chunk 变小但正确
 *
* ── 三遍 exec 扫描说明 ──
*
*   Pass-1（build litpool bitmap）：
*     以 half-word 分块扫描，识别 16-bit LDR Rn,[PC,#imm8*4] 和
*     32-bit LDR Rn,[PC,#imm12]，在 bitmap 中标记 literal pool word 位置。
*     需处理 32-bit LDR 跨块边界（上半字 0xF85F 在块尾）：pending_f85f 状态位。
*
*   Pass-2（relocate + writeback）：
*     以 word 分块，携带 3-word carry 处理 scatter 跨块检测。
*     scatter 处理优先（4-word 滑动窗口），其次向量表，再次 literal pool，
*     最后函数体内部一律跳过。
*
*   Pass-3（MOVW/MOVT pair relocation）：
*     以 half-word 粒度扫描，检测 Thumb2 MOVW(0xF240)/MOVT(0xF2C0) 指令对。
*     MOVW/MOVT 将 32 位地址编码为两个 16 位立即数嵌入指令中，
*     不在 literal pool 中，Pass-2 的 word 级扫描无法检测。
*     按 Rd 寄存器配对，解码组合地址，若落入 link Flash 范围则重定向写回。
*/

#include "elf_loader_stream.h"
#include <string.h>
#include <stdio.h>

/* ===================================================================
 * 调试输出
 * =================================================================== */

#define ELF_LOG_LEVEL_INFO 1
/* #define ELF_LOG_LEVEL_VERB 1 */
#define ELF_LOG_LEVEL_WARN 1

#define ELF_PREFIX "[elf_s] "

#ifdef ELF_LOG_LEVEL_INFO
  #define ELF_INFO(fmt,...) printf(ELF_PREFIX fmt "\r\n",##__VA_ARGS__)
#else
  #define ELF_INFO(fmt,...) do{}while(0)
#endif
#ifdef ELF_LOG_LEVEL_VERB
  #define ELF_VERB(fmt,...) printf(ELF_PREFIX "  " fmt "\r\n",##__VA_ARGS__)
#else
  #define ELF_VERB(fmt,...) do{}while(0)
#endif
#ifdef ELF_LOG_LEVEL_WARN
  #define ELF_WARN(fmt,...) printf(ELF_PREFIX "WARN: " fmt "\r\n",##__VA_ARGS__)
#else
  #define ELF_WARN(fmt,...) do{}while(0)
#endif
#define ELF_ERR(fmt,...) printf(ELF_PREFIX "ERROR: " fmt "\r\n",##__VA_ARGS__)

/* ===================================================================
 * 内部工具
 * =================================================================== */

#ifndef MIN
  #define MIN(a,b) ((a)<(b)?(a):(b))
#endif

/* RAM 范围 (STM32L431) */
#define RELOC_RAM1_BASE 0x20000000U
#define RELOC_RAM1_END  0x20010000U
#define RELOC_RAM2_BASE 0x10000000U
#define RELOC_RAM2_END  0x10004000U

static inline int reloc_in_ram_s(uint32_t a) {
    return (a>=RELOC_RAM1_BASE && a<RELOC_RAM1_END) ||
           (a>=RELOC_RAM2_BASE && a<RELOC_RAM2_END);
}

static const char *shtype_name_s(uint32_t t) {
    switch(t){
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

/* ===================================================================
 * scatter 检测（与 elf_loader.c 完全一致）
 * =================================================================== */

static int is_scatter_entry_s(const uint32_t *base, uint32_t w,
                               uint32_t word_end, uint32_t link_end)
{
    if (w+4U > word_end) return 0;
    uint32_t src=base[w], dst=base[w+1], len=base[w+2], fn=base[w+3];
    if (src==0||( src&3)!=0||src>=link_end)           return 0;
    if (!reloc_in_ram_s(dst))                          return 0;
    if (len==0||len>=0x10000U)                         return 0;
    if (fn==0||(fn&3)!=0||fn>=link_end||fn==src)       return 0;
    return 1;
}

static void handle_scatter_entry_s(uint32_t *base, uint32_t w, uint32_t offset)
{
    uint32_t old_src=base[w], old_fn=base[w+3];
    base[w]   = old_src+offset;
    base[w+3] = old_fn +offset;
    ELF_INFO("scatter [w=%u] src:0x%08x->0x%08x  fn:0x%08x->0x%08x",
             w, old_src, base[w], old_fn, base[w+3]);
}

/* ===================================================================
 * relocate_word_s() — 值域过滤 + 重定向（路径 B 专用）
 *
 * Fix-B1: LINK_MIN/LINK_END 以链接基地址（sh_addr）为基准
 * =================================================================== */
static int relocate_word_s(uint32_t *slot,
                            uint32_t  offset,
                            uint32_t  LINK_MIN,
                            uint32_t  LINK_END,
                            uint32_t  LINK_END_STRICT,
                            uint32_t  RT_BASE,
                            uint32_t  RT_SIZE,
                            uint32_t  w,
                            uint32_t *skip_bounds,
                            uint32_t *skip_align,
                            uint32_t *skip_float,
                            uint32_t *skip_thumb,
                            uint32_t *skip_postchk)
{
    uint32_t val = *slot;

    if (val<LINK_MIN || val>=LINK_END) { (*skip_bounds)++; return 0; }

    if ((val&1U)==0U && (val&3U)!=0U) {
        ELF_VERB("[%3u] skip 0x%08x not 4-aligned", w, val);
        (*skip_align)++; return 0;
    }

    /* ④ IEEE 754 浮点排除
     *
     * 仅当 val < LINK_MIN 时浮点误判才成立。
     * LINK_MIN 是 link_base + 0x100：
     *   link_base = 0       (PIC/0-based)：LINK_MIN = 0x100，val 通过 bounds 才到这里，
     *                        可能与 float 编码重叠，需要过滤。
     *   link_base = 0x08020000 (ARMCC ET_EXEC)：LINK_MIN = 0x08020100，
     *                        bits[30:23] 固定为 0x10，对所有 Flash 地址一律触发 IEEE754
     *                        误判 → 完全跳过 float check（已由 bounds 保证 val 合法）。
     * 判据：val < 0x01000000 才做 float check。
     *   原因：ARM Cortex-M 实际使用的 Flash/SRAM 地址均 >= 0x08000000 / 0x20000000，
     *   而真正可能与指针混淆的 float（如 1.0f = 0x3F800000）均 < 0x01000000 已被
     *   bounds 拒绝，或在 [0x01000000, 0x08000000) 的过渡区中 float check 仍有意义。
     */
    if (val < 0x01000000U) {
        uint32_t exp=(val>>23)&0xFFU, mant=val&0x7FFFFFU;
        if ((exp>=0x01U&&exp<=0xFEU&&mant!=0U)||(exp>=0x7EU&&exp<=0x9EU&&mant==0U)){
            ELF_VERB("[%3u] skip 0x%08x IEEE754", w, val);
            (*skip_float)++; return 0;
        }
    }

    if (val&1U) {
        uint32_t stripped=val&~1U;
        if (stripped==0U||stripped>=LINK_END_STRICT){
            ELF_VERB("[%3u] skip 0x%08x Thumb OOB", w, val);
            (*skip_thumb)++; return 0;
        }
    } else {
        if (val>=LINK_END){ (*skip_thumb)++; return 0; }
    }

    uint32_t nv = val+offset;
    {
        uint32_t a=nv&~1U;
        if (!(a>=RT_BASE&&a<RT_BASE+RT_SIZE)&&!reloc_in_ram_s(a)){
            ELF_VERB("[%3u] skip 0x%08x postchk fail", w, val);
            (*skip_postchk)++; return 0;
        }
    }

    ELF_VERB("[%3u] FIX 0x%08x -> 0x%08x", w, val, nv);
    *slot=nv; return 1;
}

/* ===================================================================
 * Thumb2 MOVW/MOVT 编解码
 *
 * 内存字节布局（小端）：
 *   addr+0,+1 : upper halfword（含 imm4, i, S 等）
 *   addr+2,+3 : lower halfword（含 imm3, Rd, imm8）
 *
 * imm16 = imm4[3:0] : i[10] : imm3[14:12] : imm8[7:0]
 *         (15:12)     (11)    (10:8)         (7:0)
 * =================================================================== */

static uint16_t movw_decode_imm16(uint16_t upper, uint16_t lower)
{
    uint16_t imm4 = upper & 0x000FU;
    uint16_t i    = (upper >> 10) & 0x0001U;
    uint16_t imm3 = (lower >> 12) & 0x0007U;
    uint16_t imm8 = lower & 0x00FFU;
    return (uint16_t)((imm4<<12)|(i<<11)|(imm3<<8)|imm8);
}

/* movt_decode_imm16 与 movw 编码相同，复用 */
#define movt_decode_imm16 movw_decode_imm16

static void movw_encode_imm16(uint16_t imm16, uint16_t *upper, uint16_t *lower)
{
    uint16_t imm4 = (imm16 >> 12) & 0x000FU;
    uint16_t i    = (imm16 >> 11) & 0x0001U;
    uint16_t imm3 = (imm16 >>  8) & 0x0007U;
    uint16_t imm8 =  imm16        & 0x00FFU;
    *upper = (uint16_t)((*upper & 0xFBF0U) | (i<<10) | imm4);
    *lower = (uint16_t)((*lower & 0x8F00U) | (imm3<<12) | imm8);
}

#define movt_encode_imm16 movw_encode_imm16

/* ===================================================================
 * litpool bitmap 更新（Pass-1 分块调用）
 *
 * @param h16          chunk 首 half-word 指针
 * @param chunk_halfs  本块 half-word 数
 * @param half_base    本块首 half 在 section 内的绝对 half 索引
 * @param sec_words    section 总 word 数
 * @param bitmap       位图缓冲区（已清零，全 section 大小）
 * @param pending_32bit 跨块状态：上块末尾遗留的 32-bit LDR 上半字
 * @param pending_hw   跨块时上半字内容（0xF8DF 或 0xF85F）
 * =================================================================== */
static void litpool_bitmap_update(const uint16_t *h16,
                                  uint32_t        chunk_halfs,
                                  uint32_t        half_base,
                                  uint32_t        sec_words,
                                  uint8_t        *bitmap,
                                  int            *pending_32bit,
                                  uint16_t       *pending_hw)
{
#define SET_LP(lp_w) do { \
    uint32_t _w=(lp_w); \
    if(_w<sec_words && !(( bitmap[_w/8U]>>(_w%8U))&1U)){ \
        bitmap[_w/8U]|=(uint8_t)(1U<<(_w%8U)); \
        ELF_VERB("litpool h=%u -> word[%u]",(unsigned)(half_base+i),_w); \
    } \
} while(0)

    for (uint32_t i=0; i<chunk_halfs; i++) {
        uint32_t h          = half_base + i;
        uint32_t instr_byte = h * 2U;
        uint32_t pc_align   = (instr_byte + 4U) & ~3U;

        /* ── 处理跨块遗留的 32-bit LDR 上半字 ── */
        if (*pending_32bit) {
            *pending_32bit = 0;
            /* 上半字位于 h-1，PC = 上半字地址 + 4（Thumb2 流水线） */
            uint32_t pend_pc = ((h-1U)*2U + 4U) & ~3U;
            if (*pending_hw == 0xF8DFU) {
                /* LDR Rt,[PC,#+imm12] */
                uint32_t imm = (uint32_t)(h16[i] & 0x0FFFU);
                SET_LP((pend_pc + imm) / 4U);
            } else {
                /* LDR Rt,[PC,#-imm8]  (0xF85F): imm8 是字节偏移，负方向 */
                uint32_t imm8 = (uint32_t)(h16[i] & 0x00FFU);
                if (imm8 <= pend_pc) {
                    SET_LP((pend_pc - imm8) / 4U);
                }
            }
            continue;
        }

        uint16_t instr = h16[i];

        /* 16-bit LDR Rn,[PC,#imm8*4]  opcode[15:11]=01001 */
        if ((instr & 0xF800U) == 0x4800U) {
            uint32_t imm = (uint32_t)(instr & 0x00FFU) * 4U;
            SET_LP((pc_align + imm) / 4U);
            continue;
        }

        /* 32-bit LDR Rt,[PC,#+imm12]  upper=0xF8DF */
        if (instr == 0xF8DFU) {
            if (i+1U < chunk_halfs) {
                uint32_t imm = (uint32_t)(h16[i+1U] & 0x0FFFU);
                SET_LP((pc_align + imm) / 4U);
                i++;
            } else {
                *pending_32bit = 1;
                *pending_hw = instr;
            }
            continue;
        }

        /* 32-bit LDR Rt,[PC,#-imm8]  upper=0xF85F
         * T1 编码：imm8 为字节偏移，目标 = PC - imm8 */
        if (instr == 0xF85FU) {
            if (i+1U < chunk_halfs) {
                uint32_t imm8 = (uint32_t)(h16[i+1U] & 0x00FFU);
                if (imm8 <= pc_align) {
                    SET_LP((pc_align - imm8) / 4U);
                }
                i++;
            } else {
                *pending_32bit = 1;
                *pending_hw = instr;
            }
            continue;
        }
    }
#undef SET_LP
}

/* ===================================================================
 * elf_parse_stream()
 * =================================================================== */
int elf_parse_stream(elf_ctx_stream_t *ctx)
{
    if (!ctx || !ctx->io.read ||
        !ctx->meta_buf || ctx->meta_buf_size < ELF_STREAM_META_BUF_MIN ||
        !ctx->work_buf || ctx->work_buf_size < ELF_STREAM_WORK_BUF_MIN) {
        ELF_ERR("invalid params");
        return ELF_ERR_PARAM;
    }

    memset(ctx->load_shidx, 0, sizeof(ctx->load_shidx));
    memset(ctx->rel_shidx,  0, sizeof(ctx->rel_shidx));
    ctx->load_count = ctx->rel_count = ctx->sym_count = ctx->offset = 0;
    ctx->ehdr=NULL; ctx->shdrs=NULL; ctx->shstrtab=NULL;

    uint8_t *meta = ctx->meta_buf;

    /* ── 1. ELF header ── */
    if (ctx->io.read(0U, meta, sizeof(elf32_ehdr), ctx->io.user) != 0) {
        ELF_ERR("read ELF header failed"); return ELF_ERR_PARAM;
    }
    elf32_ehdr *ehdr = (elf32_ehdr *)meta;
    ctx->ehdr = ehdr;

    if (*(uint32_t *)ehdr->e_ident != ELF_MAGIC){ ELF_ERR("bad magic"); return ELF_ERR_MAGIC; }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32)  { ELF_ERR("not 32-bit"); return ELF_ERR_CLASS; }
    if (ehdr->e_machine != EM_ARM)              { ELF_ERR("not ARM"); return ELF_ERR_MACHINE; }
    if (ehdr->e_shoff==0||ehdr->e_shnum==0)     { ELF_ERR("no shdrs"); return ELF_ERR_NO_SYMTAB; }
    if (ehdr->e_shnum > ELF_STREAM_MAX_SHDRS)   {
        ELF_ERR("too many shdrs (%u)", ehdr->e_shnum); return ELF_ERR_PARAM;
    }

    ELF_INFO("=== elf_parse_stream ===");
    ELF_INFO("  e_entry=0x%08x  e_shnum=%u  e_flags=0x%08x",
             ehdr->e_entry, ehdr->e_shnum, ehdr->e_flags);

    /* ── 2. section header table ── */
    uint8_t  *shdr_ptr  = meta + sizeof(elf32_ehdr);
    uint32_t  shdr_size = (uint32_t)ehdr->e_shnum * (uint32_t)sizeof(elf32_shdr);
    if ((uint32_t)sizeof(elf32_ehdr) + shdr_size > ctx->meta_buf_size) {
        ELF_ERR("meta_buf too small"); return ELF_ERR_PARAM;
    }
    if (ctx->io.read(ehdr->e_shoff, shdr_ptr, shdr_size, ctx->io.user) != 0) {
        ELF_ERR("read shdrs failed"); return ELF_ERR_PARAM;
    }
    elf32_shdr *shdrs = (elf32_shdr *)shdr_ptr;
    ctx->shdrs = shdrs;

    /* ── 3. shstrtab ── */
    uint8_t *strtab_ptr = shdr_ptr + shdr_size;
    uint32_t strtab_cap = ctx->meta_buf_size
                          - (uint32_t)sizeof(elf32_ehdr) - shdr_size;

    if (ehdr->e_shstrndx != 0U && ehdr->e_shstrndx < ehdr->e_shnum) {
        elf32_shdr *ss = &shdrs[ehdr->e_shstrndx];
        if (ss->sh_size > 0U && ss->sh_size <= strtab_cap) {
            if (ctx->io.read(ss->sh_offset, strtab_ptr,
                             ss->sh_size, ctx->io.user) == 0) {
                ctx->shstrtab = (const char *)strtab_ptr;
            } else {
                ELF_WARN("read shstrtab failed");
            }
        } else if (ss->sh_size > strtab_cap) {
            ELF_WARN("shstrtab too large (%u > %u cap)", ss->sh_size, strtab_cap);
        }
    } else {
        ELF_WARN("no shstrtab (e_shstrndx=%u)", ehdr->e_shstrndx);
    }

    /* ── 4. 遍历 section table ── */
    ELF_INFO("--- sections ---");
    for (uint32_t i=0; i<ehdr->e_shnum; i++) {
        elf32_shdr *s = &shdrs[i];
        const char *name = (ctx->shstrtab && s->sh_name)
                           ? ctx->shstrtab+s->sh_name : "(?)";

        ELF_INFO("  [%2u] %-20s type=%-8s addr=0x%08x off=0x%06x size=%-6u flags=0x%x",
                 i, name, shtype_name_s(s->sh_type),
                 s->sh_addr, s->sh_offset, s->sh_size, s->sh_flags);

        switch(s->sh_type) {
        case SHT_SYMTAB:
            ctx->sym_count = s->sh_size / (uint32_t)sizeof(elf32_sym);
            ELF_INFO("        ^-- SYMTAB %u syms", ctx->sym_count);
            break;
        case SHT_REL: case SHT_RELA:
            if (ctx->rel_count < ELF_STREAM_MAX_REL)
                ctx->rel_shidx[ctx->rel_count++] = (uint16_t)i;
            else ELF_WARN("too many REL/RELA, sec[%u] ignored", i);
            break;
        case SHT_PROGBITS: case SHT_NOBITS:
        case 14:  /* SHT_INIT_ARRAY */
        case 15:  /* SHT_FINI_ARRAY */
        case 16:  /* SHT_PREINIT_ARRAY */
        case 0x70000001U: /* SHT_ARM_EXIDX */
            if ((s->sh_flags & SHF_ALLOC) && s->sh_size>0) {
                if (ctx->load_count < ELF_STREAM_MAX_LOAD)
                    ctx->load_shidx[ctx->load_count++]=(uint16_t)i;
                else ELF_WARN("too many load secs, sec[%u] ignored", i);
            }
            break;
        default: break;
        }
    }

    ELF_INFO("  load=%u  rel=%u  sym=%u  entry=0x%08x",
             ctx->load_count, ctx->rel_count, ctx->sym_count, ehdr->e_entry);
    ELF_INFO("=== elf_parse_stream done ===");
    return ELF_OK;
}

/* ===================================================================
 * 路径 A — 精确 REL 重定向
 *
 * 支持的类型：
 *   R_ARM_ABS32 (2)         : *loc += offset  （向量表、literal pool 绝对指针）
 *   R_ARM_TARGET1 (23)      : 等同 ABS32
 *   R_ARM_THM_MOVW_ABS_NC(47)+R_ARM_THM_MOVT_ABS(48)：
 *                             decode imm16 → 加 offset → 若仍在合法范围则 encode 回写
 *   R_ARM_THM_CALL (10)     : BL/BLX PC-relative，目标 & 位置同步偏移，无需修改
 *   R_ARM_RELATIVE (43)     : *loc += offset（位置无关目标）
 *   其余 PC-relative 类型   : 忽略
 *
 * 重要：MOVW/MOVT 对的地址来自 REL 表，可能指向 Flash 或 RAM。
 *        只对结果落入运行时 Flash 范围的进行修改；RAM 地址不随 Flash offset 改变。
 *
 * @param ctx           已 parse 的上下文
 * @param offset        重定向偏移量
 * @param link_base     链接时 Flash 基地址（从 exec section sh_addr 推导）
 * @param rt_base       运行时 Flash 基地址 = link_base + offset
 * @param rt_size       Flash 分区大小
 * @param writeback     写回回调
 * =================================================================== */
static int reloc_path_a(elf_ctx_stream_t *ctx,
                        uint32_t          offset,
                        uint32_t          link_base,
                        uint32_t          rt_base,
                        uint32_t          rt_size,
                        elf_writeback_fn  writeback)
{
    ELF_INFO("--- Path A: REL-driven relocation ---");

    uint32_t total_abs32 = 0, total_movw = 0, total_call = 0,
             total_skip  = 0, total_err  = 0;

    for (uint32_t ri=0; ri<ctx->rel_count; ri++) {
        elf32_shdr *rel_shdr = &ctx->shdrs[ctx->rel_shidx[ri]];

        /* 重置 MOVW lookahead 状态，防止上一个 REL section 遗留的
         * pending MOVW 在本 section 被 MOVT 错误消费 */
        {
            uint32_t *state = (uint32_t *)ctx->work_buf;
            state[2] = 0U;
        }

        /* REL section 针对哪个 target section？ */
        uint32_t target_idx = rel_shdr->sh_info;
        if (target_idx >= ctx->ehdr->e_shnum) { total_skip++; continue; }
        elf32_shdr *tgt = &ctx->shdrs[target_idx];

        uint32_t entsize = rel_shdr->sh_entsize;
        if (entsize < 8U)  entsize = 8U;   /* REL entry = 8 bytes */
        if (entsize > 16U) {                /* RELA = 12, 防止异常值 */
            ELF_WARN("REL[%u] entsize=%u abnormal, skip", ri, entsize);
            total_skip++; continue;
        }
        uint32_t n_entries = rel_shdr->sh_size / entsize;

        ELF_INFO("  REL[%u]: %u entries -> sec[%u] addr=0x%08x",
                 ri, n_entries, target_idx, tgt->sh_addr);

#define REL_CHUNK 64U
        {
        uint8_t  rel_buf[REL_CHUNK * 8U];
        uint32_t buf_start = (uint32_t)-1U;

        for (uint32_t ei=0; ei<n_entries; ei++) {

            uint32_t buf_lo = (ei / REL_CHUNK) * REL_CHUNK;
            if (buf_lo != buf_start) {
                uint32_t n = MIN(REL_CHUNK, n_entries - buf_lo);
                if (ctx->io.read(rel_shdr->sh_offset + buf_lo * entsize,
                                 rel_buf, n * entsize, ctx->io.user) != 0) {
                    ELF_ERR("read REL entries failed"); return ELF_ERR_PARAM;
                }
                buf_start = buf_lo;
            }

            uint8_t *ep = rel_buf + (ei - buf_start) * entsize;
            uint32_t r_offset, r_info;
            memcpy(&r_offset, ep,     4);
            memcpy(&r_info,   ep + 4, 4);
            uint8_t r_type = (uint8_t)(r_info & 0xFFU);

            /* r_offset 须在 target section 范围内 */
            if (r_offset < tgt->sh_addr ||
                r_offset >= tgt->sh_addr + tgt->sh_size) {
                ELF_VERB("entry[%u] r_offset=0x%08x OOB", ei, r_offset);
                total_skip++; continue;
            }
            uint32_t sec_byte = r_offset - tgt->sh_addr;
            uint32_t file_off = tgt->sh_offset + sec_byte;

            switch (r_type) {

            /* ── ABS32 / TARGET1 ───────────────────
             *  仅重定向 Flash 地址；RAM 地址（MSP、.data 指针等）
             *  不随 Flash 偏移变化，必须跳过。
             *  val==0 为 NULL 指针，0-based ELF 中 val>=link_base(0)
             *  会误判为 Flash 地址，需显式跳过。
             *  加 post-check 防止损坏/恶意 ELF 写出非法地址。
             */
            case R_ARM_ABS32:
            case R_ARM_TARGET1: {
                uint32_t val;
                if (ctx->io.read(file_off, &val, 4, ctx->io.user)!=0){
                    ELF_ERR("read ABS32 failed"); total_err++; break;
                }
                if (val == 0U) { total_skip++; break; }  /* NULL 指针 */
                uint32_t v_check = val & ~1U;   /* 去掉 Thumb bit */
                if (v_check >= link_base && v_check < link_base + rt_size) {
                    uint32_t nv = val + offset;
                    uint32_t a = nv & ~1U;
                    if ((a >= rt_base && a < rt_base + rt_size) ||
                        reloc_in_ram_s(a)) {
                        ELF_VERB("ABS32 @0x%08x  0x%08x -> 0x%08x", r_offset, val, nv);
                        if (writeback(file_off, &nv, 4, ctx->io.user)!=0){
                            ELF_ERR("writeback ABS32 failed"); total_err++; break;
                        }
                        total_abs32++;
                    } else {
                        ELF_WARN("ABS32 postchk fail: 0x%08x -> 0x%08x", val, nv);
                        total_skip++;
                    }
                } else {
                    ELF_VERB("ABS32 @0x%08x  0x%08x skip (not Flash)", r_offset, val);
                    total_skip++;
                }
                break;
            }

            /* ── RELATIVE ──────────────────────────
             *  同 ABS32，仅重定向 link Flash 范围内的值
             *  val==0 为 NULL，跳过（同 ABS32）。
             *  加 post-check 防止损坏/恶意 ELF 写出非法地址。
             */
            case R_ARM_RELATIVE: {
                uint32_t val;
                if (ctx->io.read(file_off, &val, 4, ctx->io.user)!=0){
                    ELF_ERR("read RELATIVE failed"); total_err++; break;
                }
                if (val == 0U) { total_skip++; break; }  /* NULL 指针 */
                uint32_t v_check = val & ~1U;
                if (v_check >= link_base && v_check < link_base + rt_size) {
                    uint32_t nv = val + offset;
                    uint32_t a = nv & ~1U;
                    if ((a >= rt_base && a < rt_base + rt_size) ||
                        reloc_in_ram_s(a)) {
                        if (writeback(file_off, &nv, 4, ctx->io.user)!=0){
                            ELF_ERR("writeback RELATIVE failed"); total_err++; break;
                        }
                        total_abs32++;
                    } else {
                        ELF_WARN("RELATIVE postchk fail: 0x%08x -> 0x%08x", val, nv);
                        total_skip++;
                    }
                } else {
                    ELF_VERB("RELATIVE @0x%08x  0x%08x skip (not Flash)", r_offset, val);
                    total_skip++;
                }
                break;
            }

            /* ── THM_MOVW_ABS_NC + THM_MOVT_ABS ─── */
            case R_ARM_THM_MOVW_ABS_NC:
            case R_ARM_THM_MOVT_ABS: {
                uint16_t upper, lower;
                if (ctx->io.read(file_off,   &upper, 2, ctx->io.user)!=0 ||
                    ctx->io.read(file_off+2U, &lower, 2, ctx->io.user)!=0) {
                    ELF_ERR("read MOVW/T failed"); total_err++; break;
                }

                uint16_t imm16 = movw_decode_imm16(upper, lower);

                /* === lookahead 组合 MOVW+MOVT === */
                if (r_type == R_ARM_THM_MOVW_ABS_NC) {
                    uint32_t *state = (uint32_t *)ctx->work_buf;
                    if (state[2] == 0xF00DF00DU) {
                        ELF_WARN("MOVW overwrite: prev MOVW @0x%08x orphaned",
                                 state[0]);
                    }
                    state[0] = file_off;
                    state[1] = (uint32_t)upper | ((uint32_t)lower<<16);
                    state[2] = 0xF00DF00DU;
                    ELF_VERB("MOVW @0x%08x imm16=0x%04x (buffered)", r_offset, imm16);
                    break;
                }

                /* r_type == R_ARM_THM_MOVT_ABS */
                {
                    uint32_t *state = (uint32_t *)ctx->work_buf;
                    uint16_t movw_upper, movw_lower;
                    uint32_t movw_foff = 0;
                    int have_movw = (state[2] == 0xF00DF00DU);

                    if (have_movw) {
                        movw_foff  = state[0];
                        movw_upper = (uint16_t)(state[1] & 0xFFFFU);
                        movw_lower = (uint16_t)(state[1] >> 16);
                        state[2]   = 0U;  /* 消费 */

                        uint16_t w_imm16 = movw_decode_imm16(movw_upper, movw_lower);
                        uint16_t t_imm16 = imm16;

                        uint32_t full_addr = ((uint32_t)t_imm16<<16) | w_imm16;
                        uint32_t new_full  = full_addr + offset;

                        int needs_reloc = (full_addr >= link_base &&
                                           full_addr <  link_base + rt_size);

                        if (needs_reloc) {
                            /* post-check: 确认重定向后地址在运行时 Flash 或 RAM */
                            uint32_t a = new_full & ~1U;
                            if ((a >= rt_base && a < rt_base + rt_size) ||
                                reloc_in_ram_s(a)) {
                                uint16_t new_w = (uint16_t)(new_full & 0xFFFFU);
                                uint16_t new_t = (uint16_t)(new_full >> 16);

                                uint16_t wu = movw_upper, wl = movw_lower;
                                movw_encode_imm16(new_w, &wu, &wl);
                                if (writeback(movw_foff,   &wu, 2, ctx->io.user)!=0 ||
                                    writeback(movw_foff+2U,&wl, 2, ctx->io.user)!=0) {
                                    ELF_ERR("writeback MOVW failed"); total_err++; break;
                                }

                                uint16_t tu=upper, tl=lower;
                                movt_encode_imm16(new_t, &tu, &tl);
                                if (writeback(file_off,   &tu, 2, ctx->io.user)!=0 ||
                                    writeback(file_off+2U,&tl, 2, ctx->io.user)!=0) {
                                    ELF_ERR("writeback MOVT failed"); total_err++; break;
                                }

                                ELF_VERB("MOVW/T @0x%08x/0x%08x  0x%08x -> 0x%08x",
                                         movw_foff, file_off, full_addr, new_full);
                                total_movw++;
                            } else {
                                ELF_WARN("MOVW/T postchk fail: 0x%08x -> 0x%08x",
                                         full_addr, new_full);
                                total_skip++;
                            }
                        } else {
                            ELF_VERB("MOVW/T @0x%08x skip (0x%08x RAM/other)",
                                     movw_foff, full_addr);
                            total_skip++;
                        }
                    } else {
                        /* 孤立的 MOVT，不常见，跳过 */
                        ELF_WARN("orphan MOVT @0x%08x, skip", r_offset);
                        total_skip++;
                    }
                }
                break;
            }

            /* ── THM_CALL / CALL / JUMP24 ────────── */
            case R_ARM_THM_CALL:
            case R_ARM_CALL:
            case R_ARM_JUMP24:
            case R_ARM_THM_JUMP24:
                /* BL/BLX PC-relative：目标与位置同步偏移，相对距离不变，无需修改 */
                total_call++;
                break;

            /* ── V4BX / PREL31 (type=40) ─────────── */
            /* R_ARM_V4BX == R_ARM_PREL31 == 40
             * V4BX: ARMv4 BX 指令修补，无需修改
             * PREL31: .ARM.exidx 31-bit PC-relative 偏移，无需修改
             */
            case R_ARM_V4BX:
                total_skip++;
                break;

            /* ── PC-relative MOVW/ALU PREL ───────── */
            case 54:   /* R_ARM_THM_MOVW_PREL_NC */
            case 102:  /* R_ARM_THM_ALU_PREL_11_0 */
                total_skip++;
                break;

            case R_ARM_NONE:
                break;

            default:
                ELF_VERB("unhandled reloc type=%u @0x%08x", r_type, r_offset);
                total_skip++;
                break;
            }
        } /* for ei */
        } /* rel_buf block */
#undef REL_CHUNK
    }

    ELF_INFO("  ABS32/REL=%u  MOVW/T pairs=%u  CALL(skip)=%u  other_skip=%u  err=%u",
             total_abs32, total_movw, total_call, total_skip, total_err);
    return total_err ? ELF_ERR_PARAM : ELF_OK;
}

/* ===================================================================
 * 路径 B — 值域猜测重定向（无 REL 表时使用）
 *
 * Fix-B1: scan_exec 去掉 sh_addr==0 限制，改为仅检查 is_exec && !is_write
 * Fix-B2: LINK_MIN/LINK_END 以全局 link_base（第一个 exec section 的 sh_addr）为基准
 * Fix-B3: RT_BASE 使用全局 rt_base（= link_base + offset），而非 offset 本身
 * =================================================================== */
static int reloc_path_b(elf_ctx_stream_t *ctx,
                        uint32_t          offset,
                        uint32_t          app_max_size,
                        uint32_t          link_base,
                        uint32_t          rt_base,
                        elf_writeback_fn  writeback)
{
    ELF_INFO("--- Path B: value-range relocation (no REL table) ---");

    uint32_t e_entry_stripped = ctx->ehdr->e_entry & ~1U;

    /* 实际代码上界 */
    uint32_t actual_code_end = 0U;
    for (uint32_t i=0; i<ctx->load_count; i++) {
        elf32_shdr *s = &ctx->shdrs[ctx->load_shidx[i]];
        if ((s->sh_flags & SHF_EXECINSTR) && !(s->sh_flags & SHF_WRITE)) {
            uint32_t end = s->sh_addr + s->sh_size;
            if (end > actual_code_end) actual_code_end = end;
        }
    }
    if (actual_code_end == 0U) actual_code_end = app_max_size;

    uint32_t total_scanned=0, total_fixed=0, total_movw=0;
    uint32_t skip_section=0, skip_bounds=0, skip_align=0;
    uint32_t skip_float=0, skip_thumb=0, skip_litpool=0, skip_postchk=0;
    uint32_t scatter_entries=0;

    for (uint32_t i=0; i<ctx->load_count; i++) {
        elf32_shdr *shdr = &ctx->shdrs[ctx->load_shidx[i]];
        if (shdr->sh_type == SHT_NOBITS) continue;

        uint32_t flags    = shdr->sh_flags;
        int is_exec  = (flags & SHF_EXECINSTR) != 0;
        int is_write = (flags & SHF_WRITE)     != 0;
        int is_alloc = (flags & SHF_ALLOC)     != 0;
        if (!is_alloc) continue;

        const char *name = (ctx->shstrtab && shdr->sh_name)
                           ? ctx->shstrtab + shdr->sh_name : "(?)";

        /* Fix-B1: 去掉 sh_addr==0 限制 */
        int scan_exec  = (is_exec  && !is_write);
        int scan_write = (is_write && !is_exec);

        if (!scan_exec && !scan_write) {
            ELF_INFO("  [skip] \"%s\"", name);
            skip_section += shdr->sh_size / 4U;
            continue;
        }

        uint32_t sec_words = shdr->sh_size / 4U;

        /* Fix-B2: 以全局 link_base 为基准计算 LINK 范围 */
        uint32_t LINK_BASE   = link_base;                        /* 全局链接基地址 */
        uint32_t LINK_MIN    = link_base + 0x100U;               /* 最小有效地址偏移 */
        uint32_t LINK_END    = link_base + app_max_size;         /* 链接分区上界 */
        uint32_t LINK_STRICT = actual_code_end;                  /* Thumb 指针上界 */
        uint32_t RT_BASE     = rt_base;                          /* 运行时基地址 */
        uint32_t RT_SIZE     = app_max_size;

        uint32_t sec_fixed = 0U;

        /* ────────────────────────────────── exec section ── */
        if (scan_exec) {
            uint32_t bmp_bytes = (sec_words + 7U) / 8U;
            if (bmp_bytes >= ctx->work_buf_size) {
                ELF_ERR("work_buf too small for bitmap"); return ELF_ERR_PARAM;
            }
            uint8_t  *bitmap      = ctx->work_buf;
            uint8_t  *chunk_buf   = ctx->work_buf + bmp_bytes;
            uint32_t  chunk_cap   = ctx->work_buf_size - bmp_bytes;
            uint32_t  chunk_words = chunk_cap / 4U;
            uint32_t  chunk_halfcap = chunk_cap / 2U;

            if (chunk_words == 0U) {
                ELF_ERR("work_buf chunk empty"); return ELF_ERR_PARAM;
            }
            memset(bitmap, 0, bmp_bytes);

            ELF_INFO("  exec \"%s\": %u words bmp=%u chunk=%u",
                     name, sec_words, bmp_bytes, chunk_words);

            /* ── Pass-1: build litpool bitmap ── */
            {
                int pending_32bit = 0;
                uint16_t pending_hw = 0;
                uint32_t half_end = sec_words * 2U;
                for (uint32_t hoff=0; hoff<half_end; ) {
                    uint32_t nh = MIN(chunk_halfcap, half_end-hoff);
                    if (ctx->io.read(shdr->sh_offset + hoff*2U,
                                     chunk_buf, nh*2U, ctx->io.user) != 0) {
                        ELF_ERR("Pass-1 read failed"); return ELF_ERR_PARAM;
                    }
                    litpool_bitmap_update((const uint16_t *)chunk_buf,
                                         nh, hoff, sec_words, bitmap,
                                         &pending_32bit, &pending_hw);
                    hoff += nh;
                }
            }

            /* ── Pass-2: reloc + writeback，携带 3-word carry ── */
            {
                uint32_t carry[3] = {0,0,0};
                uint32_t carry_n  = 0;
                uint32_t word_off = 0;
                uint32_t sec_scatter = 0;

                /* e_entry_stripped - sh_addr = 向量表字节长度（无符号安全）*/
                uint32_t vec_end_byte = (e_entry_stripped > LINK_BASE)
                                        ? e_entry_stripped - LINK_BASE
                                        : 0U;

                while (word_off < sec_words || carry_n > 0U) {
                    uint32_t *wbuf   = (uint32_t *)chunk_buf;
                    uint32_t  avail  = chunk_words;

                    for (uint32_t c=0; c<carry_n; c++) wbuf[c]=carry[c];

                    uint32_t new_words = 0U;
                    if (word_off < sec_words) {
                        new_words = MIN(avail-carry_n, sec_words-word_off);
                        if (ctx->io.read(shdr->sh_offset + word_off*4U,
                                         (uint8_t *)(wbuf+carry_n),
                                         new_words*4U, ctx->io.user) != 0) {
                            ELF_ERR("Pass-2 read failed"); return ELF_ERR_PARAM;
                        }
                    }
                    uint32_t total_in = carry_n + new_words;
                    if (total_in == 0U) break;

                    /* abs_base: wbuf[0] 对应的绝对 word 索引。
                     * 不变量：非首次迭代时 word_off >= carry_n（因为
                     * new_words >= 1 才能到达后续迭代），不会下溢。 */
                    uint32_t abs_base = (carry_n>0&&word_off>0) ? word_off-carry_n : 0U;
                    uint32_t wb_start = carry_n;
                    uint32_t sc_start = (carry_n>=3U) ? carry_n-3U : 0U;

                    uint32_t w = sc_start;
                    while (w < total_in) {
                        uint32_t abs_w = abs_base + w;
                        total_scanned++;

                        if (is_scatter_entry_s(wbuf, w, total_in, LINK_END)) {
                            handle_scatter_entry_s(wbuf, w, offset);
                            total_fixed+=2; sec_fixed+=2; scatter_entries++; sec_scatter++;
                            /* carry 区域被修改则补写 */
                            for (uint32_t si=0; si<4U; si++) {
                                if (w+si < carry_n) {
                                    uint32_t co = shdr->sh_offset+(abs_base+w+si)*4U;
                                    if (writeback(co,&wbuf[w+si],4U,ctx->io.user)!=0){
                                        ELF_ERR("carry writeback failed"); return ELF_ERR_PARAM;
                                    }
                                }
                            }
                            w+=4U; continue;
                        }

                        if (abs_w==0U) { skip_litpool++; w++; continue; }

                        int in_vec = (abs_w*4U < vec_end_byte+4U);
                        int in_lp  = (int)((bitmap[abs_w/8U]>>(abs_w%8U))&1U);

                        if (!in_vec && !in_lp) { skip_litpool++; w++; continue; }

                        if (in_vec && (wbuf[w]&1U)==0U) {
                            skip_litpool++; w++; continue;
                        }

                        /* Fix-B4: 跳过 Thumb2 MOVW/MOVT 指令编码
                         * 0-based ELF 中 MOVT 编码如 0x0000F2C4 会落在
                         * [LINK_MIN, LINK_END) 范围内被误判为 Flash 指针。
                         * 这些指令由 Pass-3 MOVW/MOVT 扫描器单独处理。
                         * 检测方法：word 低 16 位（内存中第一个 half-word）
                         * 匹配 MOVW (0xF24x/0xF26x) 或 MOVT (0xF2Cx/0xF2Ex) */
                        {
                            uint16_t hw0 = (uint16_t)(wbuf[w] & 0xFFFFU);
                            if ((hw0 & 0xFBF0U) == 0xF240U ||  /* MOVW */
                                (hw0 & 0xFBF0U) == 0xF2C0U) {  /* MOVT */
                                ELF_VERB("[%3u] skip MOVW/MOVT 0x%08x", abs_w, wbuf[w]);
                                skip_litpool++; w++; continue;
                            }
                        }

                        int fixed = relocate_word_s(
                            &wbuf[w], offset, LINK_MIN, LINK_END, LINK_STRICT,
                            RT_BASE, RT_SIZE, abs_w,
                            &skip_bounds, &skip_align, &skip_float,
                            &skip_thumb, &skip_postchk);
                        if (fixed) {
                            sec_fixed++; total_fixed++;
                            /* carry 区域内的非 scatter word 被 modify 后
                             * 不会进入 wb_start 批量 writeback 范围，
                             * 必须立即单独写回，否则修改会丢失 */
                            if (w < carry_n) {
                                uint32_t co = shdr->sh_offset + (abs_base + w) * 4U;
                                if (writeback(co, &wbuf[w], 4U, ctx->io.user) != 0) {
                                    ELF_ERR("carry non-scatter writeback failed");
                                    return ELF_ERR_PARAM;
                                }
                            }
                        }
                        w++;
                    }

                    if (wb_start < total_in) {
                        uint32_t wb_off = shdr->sh_offset+(abs_base+wb_start)*4U;
                        if (writeback(wb_off, &wbuf[wb_start],
                                      (total_in-wb_start)*4U, ctx->io.user)!=0){
                            ELF_ERR("writeback failed"); return ELF_ERR_PARAM;
                        }
                    }

                    carry_n = MIN(3U, total_in);
                    for (uint32_t c=0; c<carry_n; c++)
                        carry[c]=wbuf[total_in-carry_n+c];
                    word_off+=new_words;
                    if (new_words==0U) break;
                }
                ELF_INFO("    exec \"%s\": fixed=%u scatter=%u", name, sec_fixed, sec_scatter);
            }

            /* ── Pass-3: MOVW/MOVT pair relocation ──
             *
             * Thumb2 MOVW/MOVT 将 32 位地址编码为两个 16 位立即数
             * 嵌入指令中，不在 literal pool 内，Pass-2 word 级扫描
             * 无法检测。此 pass 以 half-word 粒度扫描，
             * 检测 MOVW(0xF240) / MOVT(0xF2C0) 指令对，
             * 按 Rd 寄存器配对，解码组合地址，若在 link Flash 范围
             * 则重定向写回。
             */
            {
                struct {
                    uint16_t upper;     /* MOVW upper halfword (原始) */
                    uint16_t lower;     /* MOVW lower halfword (原始) */
                    uint32_t file_off;  /* MOVW 在文件中的偏移 */
                    uint16_t imm16;     /* MOVW 解码的 imm16 */
                    int      valid;     /* 是否有待配对的 MOVW */
                } mtrack[16];
                memset(mtrack, 0, sizeof(mtrack));

                uint32_t ch3_half = ctx->work_buf_size / 2U;
                uint32_t sec_half = shdr->sh_size / 2U;
                uint32_t pend = 0U;       /* 跨块 32-bit 指令上半字标志 */
                uint16_t pend_hw = 0U;    /* 上半字内容 */
                uint32_t pend_foff = 0U;  /* 上半字文件偏移 */

                for (uint32_t ho = 0U; ho < sec_half; ) {
                    uint32_t nh = MIN(ch3_half, sec_half - ho);
                    if (ctx->io.read(shdr->sh_offset + ho * 2U,
                                     ctx->work_buf, nh * 2U,
                                     ctx->io.user) != 0) {
                        ELF_ERR("Pass-3 read failed"); return ELF_ERR_PARAM;
                    }
                    const uint16_t *h16 = (const uint16_t *)ctx->work_buf;
                    uint32_t j = 0U;

                    /* 处理跨块遗留的 32-bit 指令上半字 */
                    if (pend && nh > 0U) {
                        uint16_t lo = h16[0];
                        if ((pend_hw & 0xFBF0U) == 0xF240U) {
                            uint16_t rd = (lo >> 8) & 0xFU;
                            mtrack[rd].upper    = pend_hw;
                            mtrack[rd].lower    = lo;
                            mtrack[rd].file_off = pend_foff;
                            mtrack[rd].imm16    = movw_decode_imm16(pend_hw, lo);
                            mtrack[rd].valid    = 1;
                            ELF_VERB("MOVW R%u imm16=0x%04x (x-chunk)",
                                     rd, mtrack[rd].imm16);
                        }
                        else if ((pend_hw & 0xFBF0U) == 0xF2C0U) {
                            uint16_t rd = (lo >> 8) & 0xFU;
                            if (rd < 16U && mtrack[rd].valid) {
                                uint16_t ti = movw_decode_imm16(pend_hw, lo);
                                    uint32_t fa = ((uint32_t)ti << 16) | mtrack[rd].imm16;
                                /* MOVW/MOVT 使用 LINK_BASE 而非 LINK_MIN 做范围检测：
                                 * LINK_MIN 加了 0x100 偏移是为了在 word 级扫描中排除
                                 * 小整数常量的误判，但 MOVW/MOVT 只在编译器生成
                                 * 真正地址时才出现，不会与 [0, 0x100) 的小整数混淆，
                                 * 因此直接用 LINK_BASE 即可，也允许重定向低地址如
                                 * 向量表前几项（0x08020000 ~ 0x080200FF）。 */
                                if (fa >= LINK_BASE && fa < LINK_END) {
                                    uint32_t nf = fa + offset;
                                    uint16_t wu = mtrack[rd].upper, wl = mtrack[rd].lower;
                                    movw_encode_imm16((uint16_t)(nf & 0xFFFFU), &wu, &wl);
                                    if (writeback(mtrack[rd].file_off, &wu, 2, ctx->io.user) != 0 ||
                                        writeback(mtrack[rd].file_off + 2U, &wl, 2, ctx->io.user) != 0) {
                                        ELF_ERR("MOVW wb fail"); return ELF_ERR_PARAM;
                                    }
                                    uint16_t tu = pend_hw, tl = lo;
                                    movt_encode_imm16((uint16_t)(nf >> 16), &tu, &tl);
                                    if (writeback(pend_foff, &tu, 2, ctx->io.user) != 0 ||
                                        writeback(pend_foff + 2U, &tl, 2, ctx->io.user) != 0) {
                                        ELF_ERR("MOVT wb fail"); return ELF_ERR_PARAM;
                                    }
                                    ELF_INFO("MOVW/T R%u 0x%08x->0x%08x (x-chunk)",
                                             rd, fa, nf);
                                    sec_fixed++; total_fixed++; total_movw++;
                                }
                                mtrack[rd].valid = 0;
                            }
                        }
                        pend = 0U;
                        j = 1U;
                    }

                    while (j < nh) {
                        uint16_t hw = h16[j];
                        /* 32-bit Thumb2 指令起始？ bits[15:11] >= 0b11101 */
                        if ((hw >> 11U) >= 0x1DU) {
                            if (j + 1U >= nh) {
                                /* 跨块边界，留到下次处理 */
                                pend = 1U;
                                pend_hw = hw;
                                pend_foff = shdr->sh_offset + (ho + j) * 2U;
                                break;
                            }
                            uint16_t up = hw, lo = h16[j + 1U];
                            uint32_t fo = shdr->sh_offset + (ho + j) * 2U;

                            /* MOVW: (upper & 0xFBF0) == 0xF240 */
                            if ((up & 0xFBF0U) == 0xF240U) {
                                uint16_t rd = (lo >> 8) & 0xFU;
                                mtrack[rd].upper    = up;
                                mtrack[rd].lower    = lo;
                                mtrack[rd].file_off = fo;
                                mtrack[rd].imm16    = movw_decode_imm16(up, lo);
                                mtrack[rd].valid    = 1;
                                ELF_VERB("MOVW R%u imm16=0x%04x", rd, mtrack[rd].imm16);
                            }
                            /* MOVT: (upper & 0xFBF0) == 0xF2C0 */
                            else if ((up & 0xFBF0U) == 0xF2C0U) {
                                uint16_t rd = (lo >> 8) & 0xFU;
                                if (rd < 16U && mtrack[rd].valid) {
                                    uint16_t ti = movw_decode_imm16(up, lo);
                                    uint32_t fa = ((uint32_t)ti << 16) | mtrack[rd].imm16;

                                    if (fa >= LINK_BASE && fa < LINK_END) {
                                        uint32_t nf = fa + offset;
                                        /* re-encode MOVW */
                                        uint16_t wu = mtrack[rd].upper, wl = mtrack[rd].lower;
                                        movw_encode_imm16((uint16_t)(nf & 0xFFFFU), &wu, &wl);
                                        if (writeback(mtrack[rd].file_off, &wu, 2, ctx->io.user) != 0 ||
                                            writeback(mtrack[rd].file_off + 2U, &wl, 2, ctx->io.user) != 0) {
                                            ELF_ERR("MOVW wb fail"); return ELF_ERR_PARAM;
                                        }
                                        /* re-encode MOVT */
                                        uint16_t tu = up, tl = lo;
                                        movt_encode_imm16((uint16_t)(nf >> 16), &tu, &tl);
                                        if (writeback(fo, &tu, 2, ctx->io.user) != 0 ||
                                            writeback(fo + 2U, &tl, 2, ctx->io.user) != 0) {
                                            ELF_ERR("MOVT wb fail"); return ELF_ERR_PARAM;
                                        }
                                        ELF_INFO("MOVW/T R%u 0x%08x->0x%08x", rd, fa, nf);
                                        sec_fixed++; total_fixed++; total_movw++;
                                    } else {
                                        ELF_VERB("MOVW/T R%u skip 0x%08x", rd, fa);
                                    }
                                    mtrack[rd].valid = 0;
                                } else {
                                    ELF_VERB("MOVT R%u orphan", rd);
                                }
                            }
                            j += 2U;
                            continue;
                        }
                        /* 16-bit 指令，跳过 */
                        j += 1U;
                    }
                    ho += nh;
                }
                ELF_INFO("    MOVW/MOVT pass \"%s\": movw_fixed=%u", name, total_movw);
            }
        }

        /* ────────────────────────────────── write section ── */
        else {
            uint32_t cw  = ctx->work_buf_size/4U;
            uint32_t *wb = (uint32_t *)ctx->work_buf;

            ELF_INFO("  write \"%s\": %u words LINK=[0x%08x,0x%08x)",
                     name, sec_words, LINK_MIN, LINK_END);

            for (uint32_t woff=0; woff<sec_words; ) {
                uint32_t n = MIN(cw, sec_words-woff);
                uint32_t fo = shdr->sh_offset+woff*4U;
                if (ctx->io.read(fo,(uint8_t*)wb,n*4U,ctx->io.user)!=0){
                    ELF_ERR("write-sec read failed"); return ELF_ERR_PARAM;
                }
                for (uint32_t w=0; w<n; w++) {
                    total_scanned++;
                    uint32_t val=wb[w];
                    /* Fix-B3: 偶数地址高16位检测以 LINK_BASE 为基准 */
                    if ((val&1U)==0U && (val>>16)!=(LINK_BASE>>16)){
                        skip_bounds++; continue;
                    }
                    /* Fix-B4: 跳过 Thumb2 MOVW/MOVT 指令编码（同 exec section） */
                    {
                        uint16_t hw0 = (uint16_t)(val & 0xFFFFU);
                        if ((hw0 & 0xFBF0U) == 0xF240U ||  /* MOVW */
                            (hw0 & 0xFBF0U) == 0xF2C0U) {  /* MOVT */
                            skip_bounds++; continue;
                        }
                    }
                    int fixed=relocate_word_s(
                        &wb[w], offset, LINK_MIN, LINK_END, LINK_STRICT,
                        RT_BASE, RT_SIZE, woff+w,
                        &skip_bounds,&skip_align,&skip_float,
                        &skip_thumb,&skip_postchk);
                    if (fixed){ sec_fixed++; total_fixed++; }
                }
                if (writeback(fo,(const uint8_t*)wb,n*4U,ctx->io.user)!=0){
                    ELF_ERR("write-sec writeback failed"); return ELF_ERR_PARAM;
                }
                woff+=n;
            }
            ELF_INFO("    write \"%s\": fixed=%u", name, sec_fixed);
        }
    }

    ELF_INFO("Path B done: scanned=%u fixed=%u scatter=%u movw=%u",
             total_scanned, total_fixed, scatter_entries, total_movw);
    ELF_INFO("  skip: bounds=%u align=%u float=%u thumb=%u litpool=%u postchk=%u",
             skip_bounds, skip_align, skip_float, skip_thumb, skip_litpool, skip_postchk);
    return ELF_OK;
}

/* ===================================================================
 * elf_relocate_stream() — 主入口
 *
 * 路径选择：
 *   有 REL section → Path A（精确）
 *   无 REL section → Path B（值域猜测）
 * =================================================================== */
int elf_relocate_stream(elf_ctx_stream_t *ctx,
                        uint32_t          offset,
                        uint32_t          app_max_size,
                        elf_writeback_fn  writeback)
{
    if (!ctx||!ctx->ehdr||!ctx->shdrs||!ctx->io.read||!writeback){
        ELF_ERR("invalid params"); return ELF_ERR_PARAM;
    }
    if (!ctx->work_buf||ctx->work_buf_size<ELF_STREAM_WORK_BUF_MIN){
        ELF_ERR("work_buf too small"); return ELF_ERR_PARAM;
    }

    ctx->offset = offset;

    /* 推导链接基地址：取第一个 exec section 的 sh_addr */
    uint32_t link_base = 0U;
    for (uint32_t i=0; i<ctx->load_count; i++) {
        elf32_shdr *s = &ctx->shdrs[ctx->load_shidx[i]];
        if ((s->sh_flags & SHF_EXECINSTR) && !(s->sh_flags & SHF_WRITE)) {
            link_base = s->sh_addr;
            break;
        }
    }
    /* 0-based ELF 修正：虚拟地址空间从 0 开始，section offset 不是基地址。
     * 若 link_base > 0 但 < FLASH_BASE_ADDR（0x08000000），说明是 0-based ELF，
     * 强制 link_base = 0，使 offset = app_start - 0 = app_start，
     * 所有 0-based 地址 + app_start 即映射到正确的 Flash 位置。
     * 注意：Flash 地址（如 0x08020000）虽然 < 0x10000000，但不是 0-based。 */
    if (link_base != 0U && link_base < 0x08000000U) {
        ELF_INFO("0-based ELF: link_base 0x%08x -> 0", link_base);
        link_base = 0U;
    }
    uint32_t rt_base = link_base + offset;

    ELF_INFO("=== elf_relocate_stream ===");
    ELF_INFO("  offset=0x%08x  app_max=0x%08x  link_base=0x%08x  rt_base=0x%08x",
             offset, app_max_size, link_base, rt_base);
    ELF_INFO("  REL sections=%u  -> %s",
             ctx->rel_count, ctx->rel_count>0 ? "Path A" : "Path B");

    int rc;
    if (ctx->rel_count > 0U) {
        rc = reloc_path_a(ctx, offset, link_base, rt_base, app_max_size, writeback);
    } else {
        rc = reloc_path_b(ctx, offset, app_max_size, link_base, rt_base, writeback);
    }

    ELF_INFO("=== elf_relocate_stream done (rc=%d) ===", rc);
    return rc;
}

/* ===================================================================
 * 辅助接口
 * =================================================================== */

uint32_t elf_stream_get_entry(const elf_ctx_stream_t *ctx)
{
    if (!ctx||!ctx->ehdr) return 0U;
    return ctx->ehdr->e_entry + ctx->offset;
}

int elf_stream_get_section(const elf_ctx_stream_t *ctx,
                           uint32_t idx, elf_section_info_t *info)
{
    if (!ctx||!info||idx>=ctx->load_count) return ELF_ERR_PARAM;
    elf32_shdr *s = &ctx->shdrs[ctx->load_shidx[idx]];
    /* sh_addr==0 常见于 SHT_NOBITS/.bss（无固定地址），
     * 加 offset 会产生无意义地址，置零让调用方自行判断 */
    if (s->sh_addr == 0U) {
        info->load_addr = 0U;
    } else {
        info->load_addr = (s->sh_addr < 0x10000000U)
                          ? s->sh_addr + ctx->offset : s->sh_addr;
    }
    info->size   = s->sh_size;
    info->is_bss = (s->sh_type == SHT_NOBITS) ? 1 : 0;
    info->name   = (ctx->shstrtab && s->sh_name) ? ctx->shstrtab+s->sh_name : "";
    info->data   = NULL;
    return ELF_OK;
}



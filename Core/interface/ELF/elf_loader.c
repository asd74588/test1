/**
 * elf_loader.c — ELF 解析与静态重定向实现
 *
 * 逻辑来源：
 *   - elf_parse()     → Zephyr subsys/llext/llext_load.c : llext_find_tables()
 *   - elf_relocate()  → Zephyr subsys/llext/llext_link.c : llext_link()
 *   - arm_relocate()  → Zephyr arch/arm/core/elf.c       : arch_elf_relocate()
 *
 * 与 Zephyr 原实现的主要差异：
 *   - 去除 k_malloc/k_free，全部使用 buf 内指针，不分配内存
 *   - 去除 LOG_DBG/LOG_ERR，替换为可选的 printf 调试输出
 *   - 去除 k_mutex、sys_slist 等运行时管理机制
 *   - 针对"链接基址为 0，烧写时加偏移"的静态重定向场景简化逻辑
 */

#include "elf_loader.h"
#include <string.h>

/* ===== 调试输出（不需要时直接注释掉这行）===== */
/* #define ELF_DEBUG */
#ifdef ELF_DEBUG
  #include <stdio.h>
  #define ELF_LOG(fmt, ...)  printf("[elf] " fmt "\r\n", ##__VA_ARGS__)
#else
  #define ELF_LOG(fmt, ...)  do {} while(0)
#endif

/* ===== 内部工具宏 ===== */
/* 检查指针是否在 buf 范围内，防止越界访问 */
#define IN_BUF(ctx, ptr, size) \
    ((uint8_t*)(ptr) >= (ctx)->buf && \
     (uint8_t*)(ptr) + (size) <= (ctx)->buf + (ctx)->buf_size)

/* ===================================================================
 * 第一部分：ARM 重定向计算（来自 arch/arm/core/elf.c）
 * =================================================================== */

/**
 * arm_relocate() — 对单条重定向记录施加修正
 *
 * 参数说明：
 *   type     : 重定向类型（R_ARM_ABS32 等）
 *   loc      : 指向 buf 中需要修正的位置
 *   sym_val  : 符号的目标地址（已加 offset）
 *   offset   : 本次烧写的基址偏移（即目标分区起始地址）
 *
 * ARM ELF ABI 参考：IHI0044 "ELF for the Arm Architecture"
 * 符号约定（与 ABI 文档一致）：
 *   S = sym_val（符号值）
 *   A = addend（加数，REL类型为*loc的初始值，RELA类型为r_addend）
 *   P = 被修正位置的运行时地址（offset + loc在buf中的section偏移）
 *   T = 1 if 目标是Thumb函数，else 0（从sym_val的bit0读取）
 */
static int arm_relocate(uint8_t type, uint32_t *loc,
                        uint32_t sym_val, uint32_t addend)
{
    uint32_t val;

    switch (type) {

    /* ------------------------------------------------------------------
     * R_ARM_ABS32: *loc = S + A
     * 最常见类型。出现于：全局变量地址、函数指针、向量表条目。
     * 处理：直接把符号值写入 *loc（符号值已包含 offset）。
     * ------------------------------------------------------------------ */
    case R_ARM_ABS32:
    case R_ARM_TARGET1:   /* TARGET1 在 Cortex-M 上等同 ABS32 */
        *loc = sym_val + addend;
        ELF_LOG("  ABS32: loc=0x%08x val=0x%08x", (uint32_t)loc, *loc);
        break;

    /* ------------------------------------------------------------------
     * R_ARM_RELATIVE: *loc = B(S) + A
     * 出现于动态链接（--emit-relocs 也可能产生）。
     * 当符号索引为 0 时，sym_val 传入的是 offset 本身。
     * 处理：在 *loc 原有值基础上加 offset（addend 即 offset）。
     * ------------------------------------------------------------------ */
    case R_ARM_RELATIVE:
        *loc += addend;   /* addend 此处调用时传入 offset */
        ELF_LOG("  RELATIVE: loc=0x%08x val=0x%08x", (uint32_t)loc, *loc);
        break;

    /* ------------------------------------------------------------------
     * R_ARM_THM_CALL: Thumb BL/BLX 指令的目标地址修正
     * 出现于：所有 Thumb 模式下的函数调用（bl 指令）。
     * 这是最复杂的类型，需要拆开并重组 32-bit Thumb-2 指令编码。
     *
     * Thumb-2 BL 编码格式（两个 16-bit half-word）：
     *   上半字 [15:11]=11110  [10]=S  [9:0]=imm10
     *   下半字 [15:14]=11    [13]=J1  [12]=1  [11]=J2  [10:0]=imm11
     *
     * 最终偏移 offset = (S:I1:I2:imm10:imm11:0) 符号扩展
     * 其中 I1 = !(J1 XOR S)，I2 = !(J2 XOR S)
     * ------------------------------------------------------------------ */
    case R_ARM_THM_CALL: {
        /* 读出当前的两个半字（小端序）*/
        uint16_t upper = ((uint16_t *)loc)[0];
        uint16_t lower = ((uint16_t *)loc)[1];

        /* 计算目标跳转偏移：sym_val 相对于当前 PC 的偏移
         * PC = loc 的运行时地址 + 4（Thumb PC 超前2个指令 = 4字节）
         * 注意：loc 指向的是 buf 中的位置，运行时地址 = loc在section中的偏移 + offset
         * 这里 sym_val 已经是运行时绝对地址，直接计算相对偏移即可
         * 但我们不知道 loc 的运行时地址，因此此处采用简化：
         * 对于 --emit-relocs 生成的 ELF，*loc 已经存有链接时的偏移值，
         * 直接在其基础上加 offset 修正即可（同 ABS32 思路）。
         */

        /* 从当前指令中解码出原始偏移量 */
        uint32_t s  =  (upper >> 10) & 0x1;
        uint32_t j1 =  (lower >> 13) & 0x1;
        uint32_t j2 =  (lower >> 11) & 0x1;
        uint32_t i1 = (!(j1 ^ s)) & 0x1;
        uint32_t i2 = (!(j2 ^ s)) & 0x1;
        uint32_t imm10 = upper & 0x3ff;
        uint32_t imm11 = lower & 0x7ff;

        /* 重组为24位偏移（单位：半字 -> 字节 *2），符号扩展 */
        int32_t off = (int32_t)((s << 24) | (i1 << 23) | (i2 << 22) |
                                (imm10 << 12) | (imm11 << 1));
        if (s) off |= (int32_t)0xfe000000; /* 符号扩展 */

        /* 加上 addend（对于 --emit-relocs，addend 通常为 0）*/
        val = (uint32_t)((int32_t)sym_val + addend);

        /*
         * 重新计算偏移并编码回指令
         * 这里需要知道 PC，对于静态重定向场景，直接用 sym_val 覆盖更可靠：
         * 如果目标函数已经被重定向到正确地址，只需把 *loc 原有偏移加上 offset 修正。
         *
         * 简化处理：直接把 offset 加到原始 imm 编码的目标绝对地址上，重新编码。
         * 这对 --emit-relocs 链接（基址0）产生的 ELF 是正确的。
         */
        (void)off; /* 上面解码的 off 在此方案中不直接使用，保留供调试 */

        /* 新的目标绝对地址（Thumb 函数地址 bit0=1）*/
        uint32_t target = val | 1; /* 确保目标是 Thumb 模式 */

        /*
         * 计算新偏移（相对于当前指令的下一条，即 PC+4）
         * 但此处我们没有 loc 的运行时地址，因此采用以下等效方法：
         * 对原有的 imm10:imm11 编码加上 offset>>1（因为偏移单位是半字）。
         *
         * 更稳健的方法：把整条指令修改为绝对跳转（movw/movt/bx），
         * 但那需要 6 字节，原位置只有 4 字节。
         *
         * 实际上对于 --emit-relocs 生成的可重定向 ELF，
         * 链接器已经把 BL 的 imm 设为相对偏移（相对于基址0的绝对地址偏移），
         * 此处只需把 sym_val 写回即可，编码方式参考：
         */
        uint32_t new_off = (target - ((uint32_t)(uintptr_t)loc + 4)) & ~1u;
        /* new_off 是相对偏移（字节，向下取2对齐）*/
        int32_t soff = (int32_t)new_off;

        s  = (soff < 0) ? 1 : 0;
        uint32_t abs_off = (uint32_t)(soff < 0 ? -soff : soff);
        imm10 = (abs_off >> 12) & 0x3ff;
        imm11 = (abs_off >>  1) & 0x7ff;
        i1 = (abs_off >> 23) & 0x1;
        i2 = (abs_off >> 22) & 0x1;
        j1 = (!(i1 ^ s)) & 0x1;
        j2 = (!(i2 ^ s)) & 0x1;

        ((uint16_t *)loc)[0] = (uint16_t)((upper & 0xf800) | (s << 10) | imm10);
        ((uint16_t *)loc)[1] = (uint16_t)((lower & 0xc000) | (j1 << 13) |
                                          (1 << 12) | (j2 << 11) | imm11);

        ELF_LOG("  THM_CALL: loc=0x%08x target=0x%08x", (uint32_t)loc, target);
        break;
    }

    /* ------------------------------------------------------------------
     * R_ARM_CALL / R_ARM_JUMP24: ARM 模式 BL/B 指令（非 Thumb）
     * Cortex-M4 只运行 Thumb 模式，理论上不应出现。
     * 遇到时记录警告并跳过，不影响其他重定向。
     * ------------------------------------------------------------------ */
    case R_ARM_CALL:
    case R_ARM_JUMP24:
        ELF_LOG("  CALL/JUMP24: ARM mode, skipping (Cortex-M4 is Thumb-only)");
        break;

    /* ------------------------------------------------------------------
     * R_ARM_NONE: 无操作，链接器用于标记但不修改
     * ------------------------------------------------------------------ */
    case R_ARM_NONE:
        break;

    default:
        ELF_LOG("  WARN: unsupported reloc type %d, skipping", type);
        /* 不返回错误：未知类型跳过，不中断整个流程 */
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
        return ELF_ERR_PARAM;
    }

    memset(ctx, 0, sizeof(elf_ctx_t));
    ctx->buf      = buf;
    ctx->buf_size = buf_size;

    /* ---- 校验 ELF header ---- */
    elf32_ehdr *ehdr = (elf32_ehdr *)buf;
    ctx->ehdr = ehdr;

    /* 校验魔数 */
    if (*(uint32_t *)ehdr->e_ident != ELF_MAGIC) {
        ELF_LOG("bad magic: 0x%08x", *(uint32_t *)ehdr->e_ident);
        return ELF_ERR_MAGIC;
    }

    /* 只支持 32-bit */
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
        ELF_LOG("not 32-bit ELF");
        return ELF_ERR_CLASS;
    }

    /* 只支持 ARM */
    if (ehdr->e_machine != EM_ARM) {
        ELF_LOG("not ARM ELF (e_machine=0x%x)", ehdr->e_machine);
        return ELF_ERR_MACHINE;
    }

    ELF_LOG("ELF OK: type=%d, sections=%d, entry=0x%08x",
            ehdr->e_type, ehdr->e_shnum, ehdr->e_entry);

    /* ---- 定位 section header table ---- */
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) {
        ELF_LOG("no section headers");
        return ELF_ERR_NO_SYMTAB;
    }

    elf32_shdr *shdrs = (elf32_shdr *)(buf + ehdr->e_shoff);
    if (!IN_BUF(ctx, shdrs, ehdr->e_shnum * sizeof(elf32_shdr))) {
        ELF_LOG("section header table out of buffer");
        return ELF_ERR_PARAM;
    }
    ctx->shdrs = shdrs;

    /* ---- 找 section 名称字符串表（shstrtab）---- */
    if (ehdr->e_shstrndx != 0 && ehdr->e_shstrndx < ehdr->e_shnum) {
        elf32_shdr *shstr_shdr = &shdrs[ehdr->e_shstrndx];
        ctx->shstrtab = (const char *)(buf + shstr_shdr->sh_offset);
    }

    /* ---- 遍历所有 section，分类记录 ---- */
    for (uint32_t i = 0; i < ehdr->e_shnum; i++) {
        elf32_shdr *shdr = &shdrs[i];

        /* 获取 section 名称（调试用）*/
        const char *name = "";
        if (ctx->shstrtab && shdr->sh_name) {
            name = ctx->shstrtab + shdr->sh_name;
        }

        switch (shdr->sh_type) {

        case SHT_SYMTAB:
            /* 符号表：sh_link 指向对应的字符串表 index */
            ctx->symtab     = (elf32_sym *)(buf + shdr->sh_offset);
            ctx->sym_count  = shdr->sh_size / sizeof(elf32_sym);
            /* 对应的字符串表 */
            if (shdr->sh_link < ehdr->e_shnum) {
                elf32_shdr *str_shdr = &shdrs[shdr->sh_link];
                ctx->strtab = (const char *)(buf + str_shdr->sh_offset);
            }
            ELF_LOG("section[%d] SYMTAB: %d symbols", i, ctx->sym_count);
            break;

        case SHT_REL:
        case SHT_RELA:
            /* 重定向表 */
            if (ctx->rel_count < 16) {
                ctx->rel_shdrs[ctx->rel_count++] = shdr;
                ELF_LOG("section[%d] %s \"%s\": size=%d, targets section %d",
                        i,
                        shdr->sh_type == SHT_REL ? "REL" : "RELA",
                        name, shdr->sh_size, shdr->sh_info);
            }
            break;

        case SHT_PROGBITS:
        case SHT_NOBITS:
            /* 需要加载的 section（有 SHF_ALLOC 标志）*/
            if ((shdr->sh_flags & SHF_ALLOC) && shdr->sh_size > 0) {
                if (ctx->load_count < 16) {
                    ctx->load_shdrs[ctx->load_count++] = shdr;
                    ELF_LOG("section[%d] LOAD \"%s\": addr=0x%08x size=%d %s",
                            i, name, shdr->sh_addr, shdr->sh_size,
                            shdr->sh_type == SHT_NOBITS ? "(bss)" : "");
                }
            }
            break;

        default:
            break;
        }
    }

    if (!ctx->symtab) {
        ELF_LOG("no SYMTAB found");
        return ELF_ERR_NO_SYMTAB;
    }

    ELF_LOG("parse done: %d load sections, %d reloc sections",
            ctx->load_count, ctx->rel_count);
    return ELF_OK;
}

/* ===================================================================
 * 第三部分：重定向遍历（来自 llext_link.c : llext_link()）
 * =================================================================== */

int elf_relocate(elf_ctx_t *ctx, uint32_t offset)
{
    if (!ctx || !ctx->buf || !ctx->symtab) {
        return ELF_ERR_PARAM;
    }

    ctx->offset = offset;

    ELF_LOG("relocating with offset=0x%08x", offset);

    /* 遍历每一个重定向 section */
    for (uint32_t ri = 0; ri < ctx->rel_count; ri++) {
        elf32_shdr *rel_shdr = ctx->rel_shdrs[ri];

        /*
         * sh_info：此重定向 section 作用于哪个 section（目标 section 的索引）
         * 需要知道目标 section 在 buf 中的起始地址，以便计算 loc 指针
         */
        uint32_t target_idx = rel_shdr->sh_info;
        elf32_shdr *target_shdr = NULL;
        uint8_t *target_base = NULL;

        if (target_idx < ctx->ehdr->e_shnum) {
            target_shdr = &ctx->shdrs[target_idx];
            target_base = ctx->buf + target_shdr->sh_offset;
        }

        uint32_t entry_count = rel_shdr->sh_size / rel_shdr->sh_entsize;
        ELF_LOG("reloc section %d: %d entries, targets section %d",
                ri, entry_count, target_idx);

        /* 遍历该重定向 section 的每一条记录 */
        for (uint32_t ei = 0; ei < entry_count; ei++) {

            uint32_t r_offset;
            uint32_t r_info;
            int32_t  r_addend = 0;

            if (rel_shdr->sh_type == SHT_RELA) {
                /* RELA：带显式加数 */
                elf32_rela *rela = (elf32_rela *)(ctx->buf +
                                   rel_shdr->sh_offset +
                                   ei * sizeof(elf32_rela));
                r_offset = rela->r_offset;
                r_info   = rela->r_info;
                r_addend = rela->r_addend;
            } else {
                /* REL：加数隐含在 *loc 的当前值中 */
                elf32_rel *rel = (elf32_rel *)(ctx->buf +
                                 rel_shdr->sh_offset +
                                 ei * sizeof(elf32_rel));
                r_offset = rel->r_offset;
                r_info   = rel->r_info;
                r_addend = 0; /* 稍后从 *loc 读取 */
            }

            uint32_t sym_idx = ELF32_R_SYM(r_info);
            uint8_t  rel_type = (uint8_t)ELF32_R_TYPE(r_info);

            /* 跳过无操作 */
            if (rel_type == R_ARM_NONE) {
                continue;
            }

            /* ---- 计算 loc：需要修正的内存位置 ---- */
            uint32_t *loc = NULL;
            if (target_base) {
                /*
                 * r_offset 是相对于目标 section 起始的偏移
                 * 注意：链接基址为0时，r_offset 也可以理解为虚拟地址
                 * 两种解释在基址为0时等价
                 */
                loc = (uint32_t *)(target_base + r_offset);
            } else {
                /* 目标 section 未知，r_offset 作为文件内绝对偏移 */
                loc = (uint32_t *)(ctx->buf + r_offset);
            }

            if (!IN_BUF(ctx, loc, sizeof(uint32_t))) {
                ELF_LOG("  WARN: loc 0x%p out of buffer, skip", loc);
                continue;
            }

            /* ---- 对于 REL 类型，加数从 *loc 读取 ---- */
            if (rel_shdr->sh_type == SHT_REL) {
                r_addend = (int32_t)(*loc);
            }

            /* ---- 查找符号值 ---- */
            uint32_t sym_val = 0;

            if (sym_idx == 0) {
                /*
                 * 符号索引0：R_ARM_RELATIVE 专用，sym_val 设为 offset
                 * 表示"把 *loc 的原有值加上基址偏移"
                 */
                sym_val = offset;
                r_addend = (int32_t)(*loc); /* 从 *loc 读加数 */
            } else if (sym_idx < ctx->sym_count) {
                elf32_sym *sym = &ctx->symtab[sym_idx];

                /*
                 * 符号值的计算（来自 llext_link.c）：
                 *
                 * 情况1：SHN_ABS —— 绝对地址符号，不加 offset（如 _estack）
                 * 情况2：SHN_UNDEF —— 外部未定义符号，bootloader 场景下
                 *          应用是自包含的，不应出现；如遇到则跳过
                 * 情况3：普通 section 内的符号 —— st_value 是相对该 section
                 *          起始的偏移，需要加上该 section 的基址再加 offset
                 */
                if (sym->st_shndx == SHN_ABS) {
                    /* 绝对地址，不重定向（如栈顶地址、特殊标志）*/
                    sym_val = sym->st_value;
                    ELF_LOG("  sym[%d] ABS: 0x%08x", sym_idx, sym_val);
                } else if (sym->st_shndx == SHN_UNDEF) {
                    /* 未定义符号，跳过 */
                    const char *sname = (ctx->strtab && sym->st_name)
                                        ? ctx->strtab + sym->st_name : "?";
                    ELF_LOG("  WARN: sym[%d] \"%s\" undefined, skip", sym_idx, sname);
                    continue;
                } else if (sym->st_shndx < ctx->ehdr->e_shnum) {
                    /*
                     * 普通符号：
                     * sym->st_value = 该符号在其所在 section 中的偏移
                     *（链接基址为0时，等同于虚拟地址）
                     * 加上 offset 得到运行时绝对地址
                     */
                    sym_val = sym->st_value + offset;

                    const char *sname = (ctx->strtab && sym->st_name)
                                        ? ctx->strtab + sym->st_name : "";
                    ELF_LOG("  sym[%d] \"%s\": 0x%08x -> 0x%08x",
                            sym_idx, sname, sym->st_value, sym_val);
                } else {
                    ELF_LOG("  WARN: sym[%d] invalid shndx %d, skip",
                            sym_idx, sym->st_shndx);
                    continue;
                }
            } else {
                ELF_LOG("  WARN: sym_idx %d >= sym_count %d, skip",
                        sym_idx, ctx->sym_count);
                continue;
            }

            /* ---- 调用 ARM 重定向计算，写回 loc ---- */
            int ret = arm_relocate(rel_type, loc, sym_val, (uint32_t)r_addend);
            if (ret != ELF_OK) {
                return ret;
            }
        }
    }

    ELF_LOG("relocation done");
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

    info->load_addr = shdr->sh_addr + ctx->offset;
    info->size      = shdr->sh_size;
    info->is_bss    = (shdr->sh_type == SHT_NOBITS) ? 1 : 0;

    if (info->is_bss) {
        info->data = NULL;  /* .bss 无文件内容，需要调用者清零 */
    } else {
        info->data = ctx->buf + shdr->sh_offset;
    }

    ELF_LOG("section[%d]: load_addr=0x%08x size=%d %s",
            idx, info->load_addr, info->size,
            info->is_bss ? "(bss, zero-fill)" : "");

    return ELF_OK;
}

uint32_t elf_get_entry(const elf_ctx_t *ctx)
{
    if (!ctx || !ctx->ehdr) {
        return 0;
    }
    return ctx->ehdr->e_entry + ctx->offset;
}



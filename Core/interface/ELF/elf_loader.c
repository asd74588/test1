/**
 * elf_loader.c - ELF32 full-buffer parser and in-place relocator
 */

#include "elf_loader.h"
#include "log_config.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define ELF_PREFIX "[elf] "
#if LOG_ELF_LOADER_ENABLE
#define ELF_WARN(fmt, ...) printf(ELF_PREFIX "WARN: " fmt "\r\n", ##__VA_ARGS__)
#define ELF_ERR(fmt, ...)  printf(ELF_PREFIX "ERROR: " fmt "\r\n", ##__VA_ARGS__)
#else
#define ELF_WARN(...) ((void)0)
#define ELF_ERR(...)  ((void)0)
#endif

#if LOG_ELF_LOADER_TRACE_ENABLE
#define ELF_INFO(fmt, ...) printf(ELF_PREFIX fmt "\r\n", ##__VA_ARGS__)
#else
#define ELF_INFO(...) ((void)0)
#endif

#define MIN_U32(a, b) ((a) < (b) ? (a) : (b))

typedef char elf_ehdr_size_check[(sizeof(elf32_ehdr) == 52U) ? 1 : -1];
typedef char elf_shdr_size_check[(sizeof(elf32_shdr) == 40U) ? 1 : -1];
typedef char elf_sym_size_check[(sizeof(elf32_sym) == 16U) ? 1 : -1];

typedef struct
{
    uint32_t file_offset;
    uint32_t symbol;
    uint32_t half_index;
    uint16_t upper;
    uint16_t lower;
    uint8_t  valid;
} movw_pending_t;

static int range_valid(uint32_t total, uint32_t offset, uint32_t length)
{
    return offset <= total && length <= total - offset;
}

static int add_signed_offset(uint32_t value, int32_t offset, uint32_t *result)
{
    int64_t adjusted = (int64_t)(uint64_t)value + (int64_t)offset;

    if (result == NULL || adjusted < 0 || adjusted > (int64_t)UINT32_MAX)
    {
        return 0;
    }

    *result = (uint32_t)adjusted;
    return 1;
}

static int is_power_of_two(uint32_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static uint16_t read_u16(const uint8_t *ptr)
{
    uint16_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

static uint32_t read_u32(const uint8_t *ptr)
{
    uint32_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

static void write_u16(uint8_t *ptr, uint16_t value)
{
    memcpy(ptr, &value, sizeof(value));
}

static void write_u32(uint8_t *ptr, uint32_t value)
{
    memcpy(ptr, &value, sizeof(value));
}

#if LOG_ELF_LOADER_ENABLE
static const char *section_type_name(uint32_t type)
{
    switch (type)
    {
        case SHT_NULL:
            return "NULL";
        case SHT_PROGBITS:
            return "PROGBITS";
        case SHT_SYMTAB:
            return "SYMTAB";
        case SHT_STRTAB:
            return "STRTAB";
        case SHT_RELA:
            return "RELA";
        case SHT_NOBITS:
            return "NOBITS";
        case SHT_REL:
            return "REL";
        case SHT_INIT_ARRAY:
            return "INIT_ARRAY";
        case SHT_FINI_ARRAY:
            return "FINI_ARRAY";
        case SHT_PREINIT_ARRAY:
            return "PREINIT_ARRAY";
        case SHT_ARM_EXIDX:
            return "ARM_EXIDX";
        default:
            return "OTHER";
    }
}
#endif

static int is_load_section(const elf32_shdr *section)
{
    if (section == NULL || (section->sh_flags & SHF_ALLOC) == 0U || section->sh_size == 0U)
    {
        return 0;
    }

    switch (section->sh_type)
    {
        case SHT_PROGBITS:
        case SHT_NOBITS:
        case SHT_INIT_ARRAY:
        case SHT_FINI_ARRAY:
        case SHT_PREINIT_ARRAY:
        case SHT_ARM_EXIDX:
            return 1;
        default:
            return 0;
    }
}

static const char *section_name(const elf_ctx_t *ctx, const elf32_shdr *section)
{
    const char *name;
    uint32_t    remaining;

    if (ctx == NULL || section == NULL || ctx->shstrtab == NULL ||
        section->sh_name >= ctx->shstrtab_size)
    {
        return "";
    }

    name      = ctx->shstrtab + section->sh_name;
    remaining = ctx->shstrtab_size - section->sh_name;
    return memchr(name, '\0', remaining) != NULL ? name : "";
}

static int section_data_valid(const elf_ctx_t *ctx, const elf32_shdr *section)
{
    if (section->sh_type == SHT_NOBITS || section->sh_size == 0U)
    {
        return 1;
    }
    return range_valid(ctx->file_size, section->sh_offset, section->sh_size);
}

static int address_in_region(uint32_t address, uint32_t base, uint32_t size)
{
    address &= ~1U;
    return address >= base && address - base < size;
}

static int relocate_value(
    uint32_t *value, uint32_t link_base, uint32_t runtime_base, uint32_t image_size, int32_t offset)
{
    uint32_t old_value;
    uint32_t new_value;

    if (value == NULL)
    {
        return -1;
    }

    old_value = *value;
    if (old_value == 0U || !address_in_region(old_value, link_base, image_size))
    {
        return 0;
    }

    if ((old_value & 1U) == 0U && (old_value & 3U) != 0U)
    {
        return 0;
    }

    if (!add_signed_offset(old_value, offset, &new_value) ||
        !address_in_region(new_value, runtime_base, image_size))
    {
        return -1;
    }

    *value = new_value;
    return 1;
}

static uint16_t mov_decode_imm16(uint16_t upper, uint16_t lower)
{
    uint16_t imm4 = upper & 0x000FU;
    uint16_t i    = (upper >> 10) & 0x0001U;
    uint16_t imm3 = (lower >> 12) & 0x0007U;
    uint16_t imm8 = lower & 0x00FFU;

    return (uint16_t)((imm4 << 12) | (i << 11) | (imm3 << 8) | imm8);
}

static void mov_encode_imm16(uint16_t imm16, uint16_t *upper, uint16_t *lower)
{
    uint16_t imm4 = (imm16 >> 12) & 0x000FU;
    uint16_t i    = (imm16 >> 11) & 0x0001U;
    uint16_t imm3 = (imm16 >> 8) & 0x0007U;
    uint16_t imm8 = imm16 & 0x00FFU;

    *upper = (uint16_t)((*upper & 0xFBF0U) | (i << 10) | imm4);
    *lower = (uint16_t)((*lower & 0x8F00U) | (imm3 << 12) | imm8);
}

static int is_movw_or_movt_word(uint32_t value)
{
    uint16_t upper = (uint16_t)(value & 0xFFFFU);
    return (upper & 0xFBF0U) == 0xF240U || (upper & 0xFBF0U) == 0xF2C0U;
}

int elf_parse(elf_ctx_t *ctx, uint8_t *buf, uint32_t file_size)
{
    elf32_ehdr *ehdr;
    uint32_t    shdr_bytes;
    uint32_t    i;

    if (ctx == NULL || buf == NULL || file_size < sizeof(elf32_ehdr))
    {
        return ELF_ERR_PARAM;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->buf       = buf;
    ctx->file_size = file_size;
    ctx->link_base = UINT32_MAX;

    ehdr      = (elf32_ehdr *)buf;
    ctx->ehdr = ehdr;

    if (ehdr->e_ident[0] != 0x7FU || ehdr->e_ident[1] != 'E' || ehdr->e_ident[2] != 'L' ||
        ehdr->e_ident[3] != 'F')
    {
        ELF_ERR("bad ELF magic");
        return ELF_ERR_MAGIC;
    }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32)
    {
        ELF_ERR("ELF is not 32-bit");
        return ELF_ERR_CLASS;
    }
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB)
    {
        ELF_ERR("ELF is not little-endian");
        return ELF_ERR_FORMAT;
    }
    if (ehdr->e_ident[EI_VERSION] != EV_CURRENT || ehdr->e_version != EV_CURRENT)
    {
        ELF_ERR("unsupported ELF version");
        return ELF_ERR_FORMAT;
    }
    if (ehdr->e_machine != EM_ARM)
    {
        ELF_ERR("ELF machine is not ARM");
        return ELF_ERR_MACHINE;
    }
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN && ehdr->e_type != ET_REL)
    {
        ELF_ERR("unsupported ELF type: %u", ehdr->e_type);
        return ELF_ERR_FORMAT;
    }
    if (ehdr->e_ehsize != sizeof(elf32_ehdr) || ehdr->e_shentsize != sizeof(elf32_shdr) ||
        ehdr->e_shoff == 0U || ehdr->e_shnum == 0U || (ehdr->e_shoff & 3U) != 0U)
    {
        ELF_ERR("invalid ELF/section header layout");
        return ELF_ERR_FORMAT;
    }

    shdr_bytes = (uint32_t)ehdr->e_shnum * (uint32_t)sizeof(elf32_shdr);
    if (!range_valid(file_size, ehdr->e_shoff, shdr_bytes))
    {
        ELF_ERR("section header table is outside the ELF buffer");
        return ELF_ERR_BOUNDS;
    }

    ctx->shdrs = (elf32_shdr *)(buf + ehdr->e_shoff);

    if (ehdr->e_shstrndx != SHN_UNDEF)
    {
        elf32_shdr *strings;

        if (ehdr->e_shstrndx >= ehdr->e_shnum)
        {
            ELF_ERR("invalid e_shstrndx: %u", ehdr->e_shstrndx);
            return ELF_ERR_FORMAT;
        }
        strings = &ctx->shdrs[ehdr->e_shstrndx];
        if (strings->sh_type != SHT_STRTAB ||
            !range_valid(file_size, strings->sh_offset, strings->sh_size))
        {
            ELF_ERR("invalid section-name string table");
            return ELF_ERR_BOUNDS;
        }
        ctx->shstrtab      = (const char *)(buf + strings->sh_offset);
        ctx->shstrtab_size = strings->sh_size;
        if (ctx->shstrtab_size == 0U || ctx->shstrtab[ctx->shstrtab_size - 1U] != '\0')
        {
            ELF_ERR("section-name string table is not terminated");
            return ELF_ERR_FORMAT;
        }
    }

    for (i = 0U; i < ehdr->e_shnum; ++i)
    {
        elf32_shdr *section = &ctx->shdrs[i];
        const char *name;

        if (section->sh_addralign != 0U && !is_power_of_two(section->sh_addralign))
        {
            ELF_ERR("section[%u] has invalid alignment", i);
            return ELF_ERR_FORMAT;
        }
        if (!section_data_valid(ctx, section))
        {
            ELF_ERR("section[%u] data is outside the ELF buffer", i);
            return ELF_ERR_BOUNDS;
        }
        if (ctx->shstrtab != NULL && section->sh_name >= ctx->shstrtab_size)
        {
            ELF_ERR("section[%u] has invalid name offset", i);
            return ELF_ERR_FORMAT;
        }
        name = section_name(ctx, section);
        if (ctx->shstrtab != NULL && section->sh_name != 0U && name[0] == '\0')
        {
            ELF_ERR("section[%u] name is not terminated", i);
            return ELF_ERR_FORMAT;
        }

        if (section->sh_type == SHT_SYMTAB)
        {
            uint32_t entry_size = section->sh_entsize;

            if (entry_size == 0U)
            {
                entry_size = sizeof(elf32_sym);
            }
            if (entry_size != sizeof(elf32_sym) || section->sh_size % entry_size != 0U)
            {
                ELF_ERR("invalid symbol table section[%u]", i);
                return ELF_ERR_FORMAT;
            }
            if (ctx->symtab == NULL)
            {
                ctx->symtab    = (elf32_sym *)(buf + section->sh_offset);
                ctx->sym_count = section->sh_size / entry_size;

                if (section->sh_link < ehdr->e_shnum)
                {
                    elf32_shdr *linked_strings = &ctx->shdrs[section->sh_link];
                    if (linked_strings->sh_type == SHT_STRTAB &&
                        section_data_valid(ctx, linked_strings))
                    {
                        ctx->strtab      = (const char *)(buf + linked_strings->sh_offset);
                        ctx->strtab_size = linked_strings->sh_size;
                    }
                }
            }
        }

        if (section->sh_type == SHT_REL || section->sh_type == SHT_RELA)
        {
            uint32_t expected_size = section->sh_type == SHT_REL ? (uint32_t)sizeof(elf32_rel)
                                                                 : (uint32_t)sizeof(elf32_rela);
            uint32_t entry_size    = section->sh_entsize;

            if (entry_size == 0U)
            {
                entry_size = expected_size;
            }
            if (entry_size != expected_size || section->sh_size % entry_size != 0U ||
                section->sh_info >= ehdr->e_shnum)
            {
                ELF_ERR("invalid relocation section[%u]", i);
                return ELF_ERR_FORMAT;
            }
            if (ctx->rel_count >= ELF_MAX_REL_SECTIONS)
            {
                ELF_ERR("too many relocation sections");
                return ELF_ERR_CAPACITY;
            }
            ctx->rel_shdrs[ctx->rel_count++] = section;
        }

        if (is_load_section(section))
        {
            if (ctx->load_count >= ELF_MAX_LOAD_SECTIONS)
            {
                ELF_ERR("too many loadable sections");
                return ELF_ERR_CAPACITY;
            }
            ctx->load_shdrs[ctx->load_count++] = section;
            if (section->sh_addr < 0x10000000U && section->sh_addr < ctx->link_base)
            {
                ctx->link_base = section->sh_addr;
            }
        }
    }

    if (ctx->load_count == 0U || ctx->link_base == UINT32_MAX)
    {
        ELF_ERR("ELF has no loadable Flash section");
        return ELF_ERR_FORMAT;
    }

    ELF_INFO("full-buffer parse: file=0x%08x size=%u", (uint32_t)(uintptr_t)buf, file_size);
    ELF_INFO("entry=0x%08x sections=%u load=%u rel=%u link_base=0x%08x",
             ehdr->e_entry,
             ehdr->e_shnum,
             ctx->load_count,
             ctx->rel_count,
             ctx->link_base);
    elf_dump_sections(ctx);
    return ELF_OK;
}

static int relocation_target_offset(const elf32_shdr *target,
                                    uint32_t          r_offset,
                                    uint32_t         *section_offset)
{
    if (r_offset >= target->sh_addr && r_offset - target->sh_addr < target->sh_size)
    {
        *section_offset = r_offset - target->sh_addr;
        return 1;
    }
    if (r_offset < target->sh_size)
    {
        *section_offset = r_offset;
        return 1;
    }
    return 0;
}

static int relocate_word_at(elf_ctx_t *ctx,
                            uint32_t   file_offset,
                            uint32_t   runtime_base,
                            uint32_t   image_size,
                            int32_t    offset)
{
    uint32_t value;
    int      result;

    if (!range_valid(ctx->file_size, file_offset, sizeof(value)))
    {
        return -1;
    }

    value  = read_u32(ctx->buf + file_offset);
    result = relocate_value(&value, ctx->link_base, runtime_base, image_size, offset);
    if (result > 0)
    {
        write_u32(ctx->buf + file_offset, value);
    }
    return result;
}

static int relocate_mov_pair(elf_ctx_t            *ctx,
                             const movw_pending_t *movw,
                             uint32_t              movt_file_offset,
                             uint16_t              movt_upper,
                             uint16_t              movt_lower,
                             uint32_t              runtime_base,
                             uint32_t              image_size,
                             int32_t               offset)
{
    uint32_t full_address;
    uint32_t new_address;
    uint16_t movw_upper;
    uint16_t movw_lower;

    full_address = ((uint32_t)mov_decode_imm16(movt_upper, movt_lower) << 16) |
                   mov_decode_imm16(movw->upper, movw->lower);
    if (!address_in_region(full_address, ctx->link_base, image_size))
    {
        return 0;
    }
    if (!add_signed_offset(full_address, offset, &new_address) ||
        !address_in_region(new_address, runtime_base, image_size))
    {
        return -1;
    }

    movw_upper = movw->upper;
    movw_lower = movw->lower;
    mov_encode_imm16((uint16_t)new_address, &movw_upper, &movw_lower);
    mov_encode_imm16((uint16_t)(new_address >> 16), &movt_upper, &movt_lower);

    write_u16(ctx->buf + movw->file_offset, movw_upper);
    write_u16(ctx->buf + movw->file_offset + 2U, movw_lower);
    write_u16(ctx->buf + movt_file_offset, movt_upper);
    write_u16(ctx->buf + movt_file_offset + 2U, movt_lower);
    return 1;
}

static int reloc_type_is_pc_relative(uint8_t type)
{
    switch (type)
    {
        case R_ARM_REL32:
        case R_ARM_THM_PC8:
        case R_ARM_THM_CALL:
        case R_ARM_CALL:
        case R_ARM_JUMP24:
        case R_ARM_THM_JUMP24:
        case R_ARM_V4BX:
        case R_ARM_PREL31:
        case R_ARM_MOVW_PREL_NC:
        case R_ARM_MOVT_PREL:
        case R_ARM_THM_MOVW_PREL_NC:
        case R_ARM_THM_MOVT_PREL:
        case R_ARM_THM_JUMP19:
        case R_ARM_THM_JUMP6:
        case R_ARM_THM_ALU_PREL_11_0:
        case R_ARM_THM_PC12:
        case 102U: /* Legacy ARMCC PC-relative relocation. */
            return 1;
        default:
            return 0;
    }
}

static int relocate_from_tables(elf_ctx_t *ctx,
                                uint32_t   runtime_base,
                                uint32_t   image_size,
                                int32_t    offset)
{
    uint32_t fixed_words     = 0U;
    uint32_t fixed_mov_pairs = 0U;
    uint32_t skipped         = 0U;
    uint32_t rel_index;

    ELF_INFO("relocation mode: ELF relocation tables");

    for (rel_index = 0U; rel_index < ctx->rel_count; ++rel_index)
    {
        elf32_shdr    *rel_section = ctx->rel_shdrs[rel_index];
        elf32_shdr    *target      = &ctx->shdrs[rel_section->sh_info];
        uint32_t       entry_size  = rel_section->sh_entsize;
        uint32_t       entry_count;
        uint32_t       entry_index;
        movw_pending_t pending[16];

        if ((target->sh_flags & SHF_ALLOC) == 0U)
        {
            continue;
        }
        if (target->sh_type == SHT_NOBITS)
        {
            ELF_WARN("relocation targets NOBITS section \"%s\"; skipped",
                     section_name(ctx, target));
            continue;
        }

        if (entry_size == 0U)
        {
            entry_size = rel_section->sh_type == SHT_REL ? (uint32_t)sizeof(elf32_rel)
                                                         : (uint32_t)sizeof(elf32_rela);
        }

        memset(pending, 0, sizeof(pending));
        entry_count = rel_section->sh_size / entry_size;
        ELF_INFO("  %s: %u entries -> %s",
                 section_name(ctx, rel_section),
                 entry_count,
                 section_name(ctx, target));

        for (entry_index = 0U; entry_index < entry_count; ++entry_index)
        {
            uint8_t *entry    = ctx->buf + rel_section->sh_offset + entry_index * entry_size;
            uint32_t r_offset = read_u32(entry);
            uint32_t r_info   = read_u32(entry + 4U);
            uint32_t symbol   = ELF32_R_SYM(r_info);
            uint8_t  type     = ELF32_R_TYPE(r_info);
            uint32_t section_offset;
            uint32_t file_offset;
            int      result;

            if (!relocation_target_offset(target, r_offset, &section_offset))
            {
                ELF_ERR("relocation offset 0x%08x is outside section \"%s\"",
                        r_offset,
                        section_name(ctx, target));
                return ELF_ERR_RELOC;
            }
            file_offset = target->sh_offset + section_offset;

            switch (type)
            {
                case R_ARM_NONE:
                    break;

                case R_ARM_ABS32:
                case R_ARM_TARGET1:
                case R_ARM_RELATIVE:
                    result = relocate_word_at(ctx, file_offset, runtime_base, image_size, offset);
                    if (result < 0)
                    {
                        ELF_ERR("invalid 32-bit relocation at file+0x%08x", file_offset);
                        return ELF_ERR_RELOC;
                    }
                    if (result > 0)
                    {
                        ++fixed_words;
                    }
                    else
                    {
                        ++skipped;
                    }
                    break;

                case R_ARM_THM_MOVW_ABS_NC:
                case R_ARM_THM_MOVT_ABS:
                {
                    uint16_t upper;
                    uint16_t lower;
                    uint32_t rd;

                    if (!range_valid(ctx->file_size, file_offset, 4U))
                    {
                        return ELF_ERR_BOUNDS;
                    }
                    upper = read_u16(ctx->buf + file_offset);
                    lower = read_u16(ctx->buf + file_offset + 2U);
                    rd    = (lower >> 8) & 0xFU;

                    if (type == R_ARM_THM_MOVW_ABS_NC)
                    {
                        pending[rd].file_offset = file_offset;
                        pending[rd].symbol      = symbol;
                        pending[rd].upper       = upper;
                        pending[rd].lower       = lower;
                        pending[rd].valid       = 1U;
                    }
                    else if (pending[rd].valid != 0U && pending[rd].symbol == symbol)
                    {
                        result            = relocate_mov_pair(ctx,
                                                   &pending[rd],
                                                   file_offset,
                                                   upper,
                                                   lower,
                                                   runtime_base,
                                                   image_size,
                                                   offset);
                        pending[rd].valid = 0U;
                        if (result < 0)
                        {
                            ELF_ERR("invalid MOVW/MOVT relocation at file+0x%08x", file_offset);
                            return ELF_ERR_RELOC;
                        }
                        if (result > 0)
                        {
                            ++fixed_mov_pairs;
                        }
                        else
                        {
                            ++skipped;
                        }
                    }
                    else
                    {
                        ELF_WARN("orphan MOVT relocation at file+0x%08x", file_offset);
                        ++skipped;
                    }
                    break;
                }

                case R_ARM_MOVW_ABS_NC:
                case R_ARM_MOVT_ABS:
                    ELF_ERR("ARM-state MOVW/MOVT relocation is unsupported on Cortex-M");
                    return ELF_ERR_RELOC;

                default:
                    if (reloc_type_is_pc_relative(type))
                    {
                        ++skipped;
                        break;
                    }
                    ELF_ERR("unsupported relocation type %u in section \"%s\"",
                            type,
                            section_name(ctx, target));
                    return ELF_ERR_RELOC;
            }
        }
    }

    ELF_INFO("relocation tables done: words=%u mov-pairs=%u skipped=%u",
             fixed_words,
             fixed_mov_pairs,
             skipped);
    return ELF_OK;
}

static void mark_literal_word(const elf32_shdr *section,
                              uint32_t          address,
                              uint8_t          *bitmap,
                              uint32_t          word_count)
{
    uint32_t byte_offset;
    uint32_t word_index;

    if (address < section->sh_addr || address - section->sh_addr >= section->sh_size)
    {
        return;
    }

    byte_offset = address - section->sh_addr;
    if ((byte_offset & 3U) != 0U)
    {
        return;
    }

    word_index = byte_offset / 4U;
    if (word_index < word_count)
    {
        bitmap[word_index / 8U] |= (uint8_t)(1U << (word_index % 8U));
    }
}

static void build_literal_bitmap(const elf_ctx_t  *ctx,
                                 const elf32_shdr *section,
                                 uint8_t          *bitmap,
                                 uint32_t          word_count)
{
    const uint8_t *data       = ctx->buf + section->sh_offset;
    uint32_t       half_count = section->sh_size / 2U;
    uint32_t       half_index = 0U;

    while (half_index < half_count)
    {
        uint32_t instruction_offset = half_index * 2U;
        uint32_t pc                 = (section->sh_addr + instruction_offset + 4U) & ~3U;
        uint16_t upper              = read_u16(data + instruction_offset);

        if ((upper & 0xF800U) == 0x4800U)
        {
            uint32_t immediate = (uint32_t)(upper & 0x00FFU) * 4U;
            mark_literal_word(section, pc + immediate, bitmap, word_count);
            ++half_index;
            continue;
        }

        if ((upper == 0xF8DFU || upper == 0xF85FU) && half_index + 1U < half_count)
        {
            uint16_t lower = read_u16(data + instruction_offset + 2U);
            uint32_t immediate =
                upper == 0xF8DFU ? (uint32_t)(lower & 0x0FFFU) : (uint32_t)(lower & 0x00FFU);
            if (upper == 0xF8DFU)
            {
                mark_literal_word(section, pc + immediate, bitmap, word_count);
            }
            else if (immediate <= pc)
            {
                mark_literal_word(section, pc - immediate, bitmap, word_count);
            }
            half_index += 2U;
            continue;
        }

        ++half_index;
    }
}

static int is_scatter_entry(const elf_ctx_t  *ctx,
                            const elf32_shdr *section,
                            uint32_t          word_index,
                            uint32_t          word_count,
                            uint32_t          image_size)
{
    const uint8_t *entry;
    uint32_t       source;
    uint32_t       destination;
    uint32_t       length;
    uint32_t       function;

    if (word_index + 4U > word_count)
    {
        return 0;
    }

    entry       = ctx->buf + section->sh_offset + word_index * 4U;
    source      = read_u32(entry);
    destination = read_u32(entry + 4U);
    length      = read_u32(entry + 8U);
    function    = read_u32(entry + 12U);

    return address_in_region(source, ctx->link_base, image_size) &&
           address_in_region(function, ctx->link_base, image_size) && (source & 3U) == 0U &&
           (function & 3U) == 0U && destination >= 0x10000000U && destination < 0x40000000U &&
           length != 0U && length < 0x10000U && function != source;
}

static int relocate_scatter_entry(elf_ctx_t        *ctx,
                                  const elf32_shdr *section,
                                  uint32_t          word_index,
                                  uint32_t          runtime_base,
                                  uint32_t          image_size,
                                  int32_t           offset)
{
    uint32_t file_offset = section->sh_offset + word_index * 4U;
    uint32_t source      = read_u32(ctx->buf + file_offset);
    uint32_t function    = read_u32(ctx->buf + file_offset + 12U);

    if (!add_signed_offset(source, offset, &source) ||
        !add_signed_offset(function, offset, &function) ||
        !address_in_region(source, runtime_base, image_size) ||
        !address_in_region(function, runtime_base, image_size))
    {
        return ELF_ERR_RELOC;
    }

    write_u32(ctx->buf + file_offset, source);
    write_u32(ctx->buf + file_offset + 12U, function);
    return ELF_OK;
}

static int relocate_mov_pairs_by_scan(elf_ctx_t        *ctx,
                                      const elf32_shdr *section,
                                      uint32_t          runtime_base,
                                      uint32_t          image_size,
                                      int32_t           offset,
                                      uint32_t         *fixed_count)
{
    uint8_t       *data       = ctx->buf + section->sh_offset;
    uint32_t       half_count = section->sh_size / 2U;
    uint32_t       half_index = 0U;
    movw_pending_t pending[16];

    memset(pending, 0, sizeof(pending));

    while (half_index < half_count)
    {
        uint16_t upper = read_u16(data + half_index * 2U);

        if ((upper >> 11U) >= 0x1DU && half_index + 1U < half_count)
        {
            uint16_t lower = read_u16(data + half_index * 2U + 2U);
            uint32_t rd    = (lower >> 8) & 0xFU;

            if ((upper & 0xFBF0U) == 0xF240U)
            {
                pending[rd].file_offset = section->sh_offset + half_index * 2U;
                pending[rd].half_index  = half_index;
                pending[rd].upper       = upper;
                pending[rd].lower       = lower;
                pending[rd].valid       = 1U;
            }
            else if ((upper & 0xFBF0U) == 0xF2C0U && pending[rd].valid != 0U &&
                     half_index - pending[rd].half_index <= 256U)
            {
                int result        = relocate_mov_pair(ctx,
                                               &pending[rd],
                                               section->sh_offset + half_index * 2U,
                                               upper,
                                               lower,
                                               runtime_base,
                                               image_size,
                                               offset);
                pending[rd].valid = 0U;
                if (result < 0)
                {
                    return ELF_ERR_RELOC;
                }
                if (result > 0)
                {
                    ++(*fixed_count);
                }
            }
            half_index += 2U;
        }
        else
        {
            ++half_index;
        }
    }

    return ELF_OK;
}

static int relocate_by_scan(elf_ctx_t *ctx,
                            uint32_t   runtime_base,
                            uint32_t   image_size,
                            int32_t    offset,
                            uint8_t   *scratch,
                            uint32_t   scratch_size)
{
    uint32_t section_index;
    uint32_t fixed_words     = 0U;
    uint32_t fixed_scatter   = 0U;
    uint32_t fixed_mov_pairs = 0U;
    uint32_t entry_address   = ctx->ehdr->e_entry & ~1U;

    if (scratch == NULL || scratch_size == 0U)
    {
        ELF_ERR("fallback relocation needs a scratch bitmap");
        return ELF_ERR_PARAM;
    }

    ELF_WARN("ELF has no relocation table; using conservative value scan");

    for (section_index = 0U; section_index < ctx->load_count; ++section_index)
    {
        elf32_shdr *section  = ctx->load_shdrs[section_index];
        uint32_t    flags    = section->sh_flags;
        int         is_exec  = (flags & SHF_EXECINSTR) != 0U;
        int         is_write = (flags & SHF_WRITE) != 0U;
        uint32_t    word_count;
        uint32_t    word_index;

        if (section->sh_type == SHT_NOBITS)
        {
            continue;
        }

        word_count = section->sh_size / 4U;

        if (is_exec && !is_write)
        {
            uint32_t bitmap_size = (word_count + 7U) / 8U;

            if (bitmap_size > scratch_size)
            {
                ELF_ERR("scratch bitmap too small: need %u, have %u", bitmap_size, scratch_size);
                return ELF_ERR_CAPACITY;
            }

            memset(scratch, 0, bitmap_size);
            build_literal_bitmap(ctx, section, scratch, word_count);

            for (word_index = 0U; word_index < word_count;)
            {
                uint32_t file_offset  = section->sh_offset + word_index * 4U;
                uint32_t value        = read_u32(ctx->buf + file_offset);
                uint32_t word_address = section->sh_addr + word_index * 4U;
                int      in_vectors   = section->sh_addr == ctx->link_base && word_index != 0U &&
                                 word_address <= entry_address;
                int in_literal_pool =
                    (scratch[word_index / 8U] & (uint8_t)(1U << (word_index % 8U))) != 0U;
                int result;

                if (is_scatter_entry(ctx, section, word_index, word_count, image_size))
                {
                    result = relocate_scatter_entry(
                        ctx, section, word_index, runtime_base, image_size, offset);
                    if (result != ELF_OK)
                    {
                        return result;
                    }
                    ++fixed_scatter;
                    fixed_words += 2U;
                    word_index += 4U;
                    continue;
                }

                if ((!in_vectors && !in_literal_pool) || (in_vectors && (value & 1U) == 0U) ||
                    is_movw_or_movt_word(value))
                {
                    ++word_index;
                    continue;
                }

                result = relocate_value(&value, ctx->link_base, runtime_base, image_size, offset);
                if (result < 0)
                {
                    return ELF_ERR_RELOC;
                }
                if (result > 0)
                {
                    write_u32(ctx->buf + file_offset, value);
                    ++fixed_words;
                }
                ++word_index;
            }

            if (relocate_mov_pairs_by_scan(
                    ctx, section, runtime_base, image_size, offset, &fixed_mov_pairs) != ELF_OK)
            {
                return ELF_ERR_RELOC;
            }
        }
        else if (is_write && !is_exec)
        {
            for (word_index = 0U; word_index < word_count; ++word_index)
            {
                uint32_t file_offset = section->sh_offset + word_index * 4U;
                uint32_t value       = read_u32(ctx->buf + file_offset);
                int      result;

                if (is_movw_or_movt_word(value))
                {
                    continue;
                }
                result = relocate_value(&value, ctx->link_base, runtime_base, image_size, offset);
                if (result < 0)
                {
                    return ELF_ERR_RELOC;
                }
                if (result > 0)
                {
                    write_u32(ctx->buf + file_offset, value);
                    ++fixed_words;
                }
            }
        }
    }

    ELF_INFO("fallback relocation done: words=%u scatter=%u mov-pairs=%u",
             fixed_words,
             fixed_scatter,
             fixed_mov_pairs);
    return ELF_OK;
}

int elf_relocate(
    elf_ctx_t *ctx, int32_t offset, uint32_t app_max_size, uint8_t *scratch, uint32_t scratch_size)
{
    uint32_t runtime_base;
    int      result;

    if (ctx == NULL || ctx->buf == NULL || ctx->ehdr == NULL || ctx->shdrs == NULL ||
        app_max_size == 0U)
    {
        return ELF_ERR_PARAM;
    }
    if (!add_signed_offset(ctx->link_base, offset, &runtime_base))
    {
        return ELF_ERR_RELOC;
    }

    ctx->offset = offset;
    ELF_INFO("relocate: link=0x%08x runtime=0x%08x offset=%d size=0x%08x",
             ctx->link_base,
             runtime_base,
             offset,
             app_max_size);

    if (offset == 0)
    {
        ELF_INFO("relocation skipped: image already uses the target address");
        return ELF_OK;
    }

    if (ctx->rel_count != 0U)
    {
        result = relocate_from_tables(ctx, runtime_base, app_max_size, offset);
    }
    else
    {
        result = relocate_by_scan(ctx, runtime_base, app_max_size, offset, scratch, scratch_size);
    }
    return result;
}

uint32_t elf_get_entry(const elf_ctx_t *ctx)
{
    uint32_t entry;

    if (ctx == NULL || ctx->ehdr == NULL)
    {
        return 0U;
    }

    entry = ctx->ehdr->e_entry;
    if (entry < 0x10000000U && !add_signed_offset(entry, ctx->offset, &entry))
    {
        return 0U;
    }
    return entry;
}

int elf_get_section(const elf_ctx_t *ctx, uint32_t idx, elf_section_info_t *info)
{
    elf32_shdr *section;

    if (ctx == NULL || info == NULL || idx >= ctx->load_count)
    {
        return ELF_ERR_PARAM;
    }

    section         = ctx->load_shdrs[idx];
    info->load_addr = section->sh_addr;
    if (section->sh_addr < 0x10000000U &&
        !add_signed_offset(section->sh_addr, ctx->offset, &info->load_addr))
    {
        return ELF_ERR_RELOC;
    }
    info->size   = section->sh_size;
    info->is_bss = section->sh_type == SHT_NOBITS ? 1U : 0U;
    info->data   = info->is_bss != 0U ? NULL : ctx->buf + section->sh_offset;
    info->name   = section_name(ctx, section);
    return ELF_OK;
}

void elf_dump_sections(const elf_ctx_t *ctx)
{
#if LOG_ELF_LOADER_ENABLE
    uint32_t i;

    if (ctx == NULL || ctx->ehdr == NULL || ctx->shdrs == NULL)
    {
        return;
    }

    ELF_INFO("sections:");
    for (i = 0U; i < ctx->ehdr->e_shnum; ++i)
    {
        const elf32_shdr *section = &ctx->shdrs[i];
        ELF_INFO("  [%2u] %-20s type=%-11s addr=0x%08x off=0x%06x size=%-6u flags=0x%x",
                 i,
                 section_name(ctx, section),
                 section_type_name(section->sh_type),
                 section->sh_addr,
                 section->sh_offset,
                 section->sh_size,
                 section->sh_flags);
    }
#else
    (void)ctx;
#endif
}

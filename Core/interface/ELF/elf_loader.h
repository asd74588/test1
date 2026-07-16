/**
 * elf_loader.h - ELF32 full-buffer parser and in-place relocator
 *
 * The complete ELF file must already be present in a contiguous memory
 * buffer. Parsing only records pointers into that buffer; section contents
 * are never copied to a second metadata buffer.
 */

#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stdint.h>
#include "elf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define ELF_OK                 0
#define ELF_ERR_MAGIC         -1
#define ELF_ERR_CLASS         -2
#define ELF_ERR_MACHINE       -3
#define ELF_ERR_NO_SYMTAB     -4
#define ELF_ERR_RELOC         -5
#define ELF_ERR_PARAM         -6
#define ELF_ERR_BOUNDS        -7
#define ELF_ERR_FORMAT        -8
#define ELF_ERR_CAPACITY      -9

#define ELF_MAX_LOAD_SECTIONS 32U
#define ELF_MAX_REL_SECTIONS  32U

typedef struct {
    uint32_t    load_addr;
    uint8_t    *data;
    uint32_t    size;
    uint8_t     is_bss;
    const char *name;
} elf_section_info_t;

typedef struct {
    /* Complete ELF image in memory. */
    uint8_t    *buf;
    uint32_t    file_size;

    /* All pointers below point directly into buf. */
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

    uint32_t    link_base;
    int32_t     offset;
} elf_ctx_t;

/** Parse a complete ELF file already stored in buf[0..file_size). */
int elf_parse(elf_ctx_t *ctx, uint8_t *buf, uint32_t file_size);

/**
 * Relocate the ELF image in place.
 *
 * If relocation sections are present they are used directly. For stripped
 * images without relocation sections, scratch is used as a literal-pool
 * bitmap by the conservative fallback scanner.
 */
int elf_relocate(elf_ctx_t *ctx,
                 int32_t    offset,
                 uint32_t   app_max_size,
                 uint8_t   *scratch,
                 uint32_t   scratch_size);

int      elf_get_section(const elf_ctx_t *ctx,
                         uint32_t         idx,
                         elf_section_info_t *info);
uint32_t elf_get_entry(const elf_ctx_t *ctx);
void     elf_dump_sections(const elf_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ELF_LOADER_H */

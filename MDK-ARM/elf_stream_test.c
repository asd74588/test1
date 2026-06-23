/**
 * elf_stream_test.c — PC 端验证 elf_loader_stream
 *
 * 用法 (MinGW/MSVC):
 *   gcc -o elf_stream_test.exe elf_stream_test.c -I..\Core\interface\ELF
 *   elf_stream_test.exe USART.axf 0x08020000
 *
 * 模拟 bootloader 流程：
 *   1. 读取 AXF 到 buffer
 *   2. elf_parse_stream()
 *   3. elf_relocate_stream()
 *   4. 逐 section 检查重定向结果
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* ---- 裁剪：只包含 ELF 结构体定义 ---- */

#define ELF_MAGIC       0x464C457FU
#define ELFCLASS32      1
#define EM_ARM          40

#define EI_CLASS        4
#define EI_DATA         5

#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_REL         9
#define SHT_NOBITS      8

#define SHF_WRITE       0x1U
#define SHF_ALLOC       0x2U
#define SHF_EXECINSTR   0x4U

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf32_ehdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} elf32_shdr;

/* ---- 从 elf_loader.h 搬的错误码和类型 ---- */

#define ELF_OK              0
#define ELF_ERR_MAGIC      -1
#define ELF_ERR_CLASS      -2
#define ELF_ERR_MACHINE    -3
#define ELF_ERR_NO_SYMTAB  -4
#define ELF_ERR_RELOC      -5
#define ELF_ERR_PARAM      -6

typedef struct {
    uint32_t  load_addr;
    uint8_t  *data;
    uint32_t  size;
    uint8_t   is_bss;
    const char *name;
} elf_section_info_t;

/* ---- 从 elf_loader_stream.h 搬的接口 ---- */

enum { R_ARM_THM_MOVW_ABS_NC = 47, R_ARM_THM_MOVT_ABS = 48 };

#define ELF_STREAM_MAX_SHDRS      64U
#define ELF_STREAM_SHSTRTAB_MAX   4096U
#define ELF_STREAM_META_BUF_MIN   \
    ((uint32_t)(sizeof(elf32_ehdr)) + \
     ELF_STREAM_MAX_SHDRS * (uint32_t)(sizeof(elf32_shdr)) + \
     ELF_STREAM_SHSTRTAB_MAX)
#define ELF_STREAM_WORK_BUF_MIN   256U
#define ELF_STREAM_MAX_LOAD       16U
#define ELF_STREAM_MAX_REL        16U

typedef int (*elf_read_fn)(uint32_t file_offset, void *dst, uint32_t len, void *user);
typedef int (*elf_writeback_fn)(uint32_t file_offset, const void *src, uint32_t len, void *user);

typedef struct { elf_read_fn read; void *user; } elf_io_t;

typedef struct {
    elf_io_t    io;
    uint8_t    *meta_buf;
    uint32_t    meta_buf_size;
    uint8_t    *work_buf;
    uint32_t    work_buf_size;
    elf32_ehdr *ehdr;
    elf32_shdr *shdrs;
    const char *shstrtab;
    uint16_t    load_shidx[ELF_STREAM_MAX_LOAD];
    uint32_t    load_count;
    uint16_t    rel_shidx[ELF_STREAM_MAX_REL];
    uint32_t    rel_count;
    uint32_t    sym_count;
    uint32_t    offset;
} elf_ctx_stream_t;

/* ---- 直接 include elf_loader_stream.c 的实现 ---- */
/* (避免修改原始文件，我们把 printf 重定向) */
#undef printf
#define printf(...) fprintf(stdout, ##__VA_ARGS__)

/* ---- 全局 buffer ---- */
static uint8_t *g_elf_buf = NULL;
static uint32_t g_elf_size = 0;

static int buf_read(uint32_t off, void *dst, uint32_t len, void *user)
{
    (void)user;
    if (off + len > g_elf_size) return -1;
    memcpy(dst, g_elf_buf + off, len);
    return 0;
}

static int buf_writeback(uint32_t off, const void *src, uint32_t len, void *user)
{
    (void)user;
    if (off + len > g_elf_size) return -1;
    memcpy(g_elf_buf + off, src, len);
    return 0;
}

/* ---- 直接 include 实现 ---- */
#include "elf_loader_stream.c"

/* ---- main ---- */
int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <axf_file> <target_base_hex>\n", argv[0]);
        fprintf(stderr, "  e.g. %s USART.axf 0x08020000\n", argv[0]);
        return 1;
    }

    const char *axf_path = argv[1];
    uint32_t target_base = (uint32_t)strtoul(argv[2], NULL, 0);

    /* 1. 读取 AXF */
    FILE *f = fopen(axf_path, "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_elf_buf = (uint8_t *)malloc(fsize);
    g_elf_size = (uint32_t)fread(g_elf_buf, 1, fsize, f);
    fclose(f);
    printf("Read %u bytes from %s\n", g_elf_size, axf_path);

    /* 2. 分配 meta_buf / work_buf */
    uint8_t *meta_buf = (uint8_t *)malloc(ELF_STREAM_META_BUF_MIN);
    uint8_t *work_buf = (uint8_t *)malloc(4096);

    elf_ctx_stream_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.io.read       = buf_read;
    ctx.io.user       = NULL;
    ctx.meta_buf      = meta_buf;
    ctx.meta_buf_size = ELF_STREAM_META_BUF_MIN;
    ctx.work_buf      = work_buf;
    ctx.work_buf_size = 4096;

    /* 3. 解析 */
    int rc = elf_parse_stream(&ctx);
    printf("elf_parse_stream => %d\n", rc);
    if (rc != ELF_OK) { free(g_elf_buf); return 1; }

    /* 4. 打印 sections */
    printf("\n--- Loadable Sections ---\n");
    for (uint32_t i = 0; i < ctx.load_count; i++) {
        elf32_shdr *s = &ctx.shdrs[ctx.load_shidx[i]];
        const char *name = (ctx.shstrtab && s->sh_name) ? ctx.shstrtab + s->sh_name : "?";
        printf("  [%u] %-16s addr=0x%08x off=0x%06x size=%-6u flags=0x%x type=%u\n",
               i, name, s->sh_addr, s->sh_offset, s->sh_size, s->sh_flags, s->sh_type);
    }

    printf("\n--- REL Sections ---\n");
    if (ctx.rel_count == 0) {
        printf("  (none - will use Path B)\n");
    }
    for (uint32_t i = 0; i < ctx.rel_count; i++) {
        elf32_shdr *s = &ctx.shdrs[ctx.rel_shidx[i]];
        printf("  REL[%u] target_sec=%u entries=%u\n",
               i, s->sh_info, s->sh_size / 8);
    }

    /* 5. 重定向 */
    uint32_t app_max_size = 0x1E000U;
    rc = elf_relocate_stream(&ctx, target_base, app_max_size, buf_writeback);
    printf("\nelf_relocate_stream(offset=0x%08x) => %d\n", target_base, rc);

    /* 6. 入口地址 */
    uint32_t entry = elf_stream_get_entry(&ctx);
    printf("Entry point: 0x%08x (original e_entry=0x%08x)\n",
           entry, ctx.ehdr->e_entry);

    /* 7. 逐 section 检查重定向结果 */
    printf("\n--- Section Relocation Check ---\n");
    for (uint32_t i = 0; i < ctx.load_count; i++) {
        elf_section_info_t info;
        rc = elf_stream_get_section(&ctx, i, &info);
        if (rc != ELF_OK) continue;

        printf("  %-16s load_addr=0x%08x size=%-6u bss=%d\n",
               info.name ? info.name : "?",
               info.load_addr, info.size, info.is_bss);
    }

    /* 8. 检查向量表（前 16 字节） */
    printf("\n--- Vector Table (first 4 words after reloc) ---\n");
    for (uint32_t i = 0; i < ctx.load_count; i++) {
        elf32_shdr *s = &ctx.shdrs[ctx.load_shidx[i]];
        if (s->sh_type == SHT_NOBITS) continue;
        if (!(s->sh_flags & SHF_ALLOC)) continue;
        if (s->sh_addr > target_base + 0x200) continue; /* 只看最前面的 section */

        uint32_t *words = (uint32_t *)(g_elf_buf + s->sh_offset);
        uint32_t n = (s->sh_size < 16) ? s->sh_size / 4 : 4;
        for (uint32_t w = 0; w < n; w++) {
            printf("  [%u] 0x%08x", w, words[w]);
            if (w == 0) printf("  (MSP)");
            if (w == 1) printf("  (Reset_Handler)");
            printf("\n");
        }
        break;
    }

    /* 9. 输出重定向后的 AXF（可选） */
    char out_path[256];
    snprintf(out_path, sizeof(out_path), "%s.relocated", axf_path);
    f = fopen(out_path, "wb");
    if (f) {
        fwrite(g_elf_buf, 1, g_elf_size, f);
        fclose(f);
        printf("\nRelocated AXF written to: %s\n", out_path);
    }

    free(g_elf_buf);
    free(meta_buf);
    free(work_buf);
    return 0;
}

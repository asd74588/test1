/**
 * elf.h — ELF32 数据结构定义
 *
 * 来源：基于 Zephyr include/zephyr/llext/elf.h 裁剪
 * 裁剪内容：删除 Zephyr 内核头文件依赖，只保留 32-bit 所需结构体和常量
 * 目标平台：STM32L431 (ARM Cortex-M4, 32-bit Little Endian)
 * 许可证：Apache 2.0（与 Zephyr 原文件相同）
 */

#ifndef ELF_H
#define ELF_H

#include <stdint.h>

/* ===== 基础类型 ===== */
typedef uint32_t elf32_addr;   /* 程序地址 */
typedef uint32_t elf32_off;    /* 文件偏移 */
typedef uint16_t elf32_half;   /* 无符号短整数 */
typedef uint32_t elf32_word;   /* 无符号整数 */
typedef int32_t  elf32_sword;  /* 有符号整数 */

/* ===== ELF 魔数 ===== */
#define ELF_MAGIC      0x464c457f   /* Little-endian: 0x7f 'E' 'L' 'F' */
#define EI_NIDENT      16           /* e_ident 数组长度 */

/* e_ident 索引 */
#define EI_MAG0        0   /* 魔数字节0 */
#define EI_MAG1        1   /* 魔数字节1 */
#define EI_MAG2        2   /* 魔数字节2 */
#define EI_MAG3        3   /* 魔数字节3 */
#define EI_CLASS       4   /* 文件类型（32/64 bit）*/
#define EI_DATA        5   /* 字节序 */

/* EI_CLASS 值 */
#define ELFCLASS32     1   /* 32-bit */

/* EI_DATA 值 */
#define ELFDATA2LSB    1   /* Little Endian */

/* ===== ELF 文件头 ===== */
typedef struct {
    uint8_t    e_ident[EI_NIDENT]; /* 魔数 + 类型 + 字节序 + 版本 + padding */
    elf32_half e_type;             /* 文件类型 */
    elf32_half e_machine;          /* 目标架构 */
    elf32_word e_version;          /* ELF 版本（必须为 1）*/
    elf32_addr e_entry;            /* 程序入口虚拟地址 */
    elf32_off  e_phoff;            /* Program header table 偏移 */
    elf32_off  e_shoff;            /* Section header table 偏移 */
    elf32_word e_flags;            /* 处理器相关标志 */
    elf32_half e_ehsize;           /* ELF header 大小（字节）*/
    elf32_half e_phentsize;        /* Program header 条目大小 */
    elf32_half e_phnum;            /* Program header 条目数量 */
    elf32_half e_shentsize;        /* Section header 条目大小 */
    elf32_half e_shnum;            /* Section header 条目数量 */
    elf32_half e_shstrndx;         /* Section 名称字符串表的索引 */
} elf32_ehdr;

/* e_type 值 */
#define ET_NONE        0   /* 未知 */
#define ET_REL         1   /* 可重定向目标文件 */
#define ET_EXEC        2   /* 可执行文件 */
#define ET_DYN         3   /* 共享目标文件 */

/* e_machine 值 */
#define EM_ARM         40  /* ARM 32-bit */

/* ===== Section Header ===== */
typedef struct {
    elf32_word sh_name;      /* section 名称（字符串表中的偏移）*/
    elf32_word sh_type;      /* section 类型 */
    elf32_word sh_flags;     /* section 属性标志 */
    elf32_addr sh_addr;      /* 运行时虚拟地址（链接基址为0时此处也为0）*/
    elf32_off  sh_offset;    /* section 在文件中的偏移 */
    elf32_word sh_size;      /* section 大小（字节）*/
    elf32_word sh_link;      /* 关联 section 索引（语义随 sh_type 变化）*/
    elf32_word sh_info;      /* 附加信息（语义随 sh_type 变化）*/
    elf32_word sh_addralign; /* 地址对齐要求 */
    elf32_word sh_entsize;   /* 固定大小条目的大小（如符号表条目）*/
} elf32_shdr;

/* sh_type 值 */
#define SHT_NULL       0   /* 无效 section */
#define SHT_PROGBITS   1   /* 程序数据（.text .data 等）*/
#define SHT_SYMTAB     2   /* 符号表 */
#define SHT_STRTAB     3   /* 字符串表 */
#define SHT_RELA       4   /* 带加数的重定向表 */
#define SHT_NOBITS     8   /* 无文件内容（.bss）*/
#define SHT_REL        9   /* 不带加数的重定向表 */

/* sh_flags 值 */
#define SHF_WRITE      (1 << 0)  /* section 可写 */
#define SHF_ALLOC      (1 << 1)  /* section 需要加载到内存 */
#define SHF_EXECINSTR  (1 << 2)  /* section 包含可执行代码 */

/* ===== 符号表条目 ===== */
typedef struct {
    elf32_word st_name;   /* 符号名（字符串表中的偏移）*/
    elf32_addr st_value;  /* 符号值（地址或偏移）*/
    elf32_word st_size;   /* 符号关联对象的大小 */
    uint8_t    st_info;   /* 符号类型和绑定属性 */
    uint8_t    st_other;  /* 可见性（通常为0）*/
    elf32_half st_shndx;  /* 所在 section 的索引 */
} elf32_sym;

/* st_shndx 特殊值 */
#define SHN_UNDEF      0       /* 未定义符号 */
#define SHN_ABS        0xfff1  /* 绝对值符号，不受重定向影响 */
#define SHN_COMMON     0xfff2  /* 公共块符号 */

/* st_info 操作宏 */
#define ELF32_ST_BIND(i)   ((i) >> 4)          /* 提取绑定属性 */
#define ELF32_ST_TYPE(i)   ((i) & 0x0f)        /* 提取类型 */

/* 符号绑定值 */
#define STB_LOCAL      0   /* 本地符号 */
#define STB_GLOBAL     1   /* 全局符号 */
#define STB_WEAK       2   /* 弱符号 */

/* 符号类型值 */
#define STT_NOTYPE     0   /* 未指定类型 */
#define STT_OBJECT     1   /* 数据对象（变量）*/
#define STT_FUNC       2   /* 函数 */
#define STT_SECTION    3   /* section 符号 */

/* ===== 重定向表条目（不带加数）===== */
typedef struct {
    elf32_addr r_offset;  /* 需要修正的位置（section内偏移）*/
    elf32_word r_info;    /* 符号索引 + 重定向类型 */
} elf32_rel;

/* ===== 重定向表条目（带加数）===== */
typedef struct {
    elf32_addr  r_offset; /* 需要修正的位置（section内偏移）*/
    elf32_word  r_info;   /* 符号索引 + 重定向类型 */
    elf32_sword r_addend; /* 加数（用于计算最终值）*/
} elf32_rela;

/* r_info 操作宏 */
#define ELF32_R_SYM(i)    ((i) >> 8)       /* 提取符号索引 */
#define ELF32_R_TYPE(i)   ((uint8_t)(i))   /* 提取重定向类型 */

/* ===== ARM 重定向类型 ===== */
/* 完整列表见 ARM ELF ABI 文档，此处只列 Cortex-M4 实际会遇到的 */
#define R_ARM_NONE          0   /* 无操作 */
#define R_ARM_ABS32         2   /* 32-bit 绝对地址：S + A */
#define R_ARM_REL32         3   /* 32-bit PC相对：S + A - P */
#define R_ARM_THM_CALL      10  /* Thumb BL/BLX 指令 */
#define R_ARM_BASE_PREL     25  /* GOT基址相对（PIC用，静态重定向不常见）*/
#define R_ARM_GOT_BREL      26  /* GOT条目偏移（PIC用）*/
#define R_ARM_CALL          28  /* ARM BL/BLX 指令 */
#define R_ARM_JUMP24        29  /* ARM B/BL 指令 */
#define R_ARM_THM_JUMP24    30  /* Thumb2 B.W 指令 */
#define R_ARM_RELATIVE      23  /* B(S) + A，动态链接用 */
#define R_ARM_TARGET1       38  /* 目标平台定义，通常等同 ABS32 */
#define R_ARM_TARGET2       41  /* 目标平台定义，通常等同 REL32 */
#define R_ARM_THM_JUMP11    102 /* Thumb2 短跳转 */
#define R_ARM_THM_JUMP8     103 /* Thumb2 短跳转 */

#endif /* ELF_H */



/**
 * elf.h — ELF 基础类型与常量定义
 *
 * 仅包含本项目用到的字段，参考：
 *   - System V ABI: Tool Interface Standard (TIS) ELF Specification
 *   - IHI0044 "ELF for the Arm Architecture"
 */

#ifndef ELF_H
#define ELF_H

#include <stdint.h>

/* ===== ELF 魔数 ===== */
#define ELF_MAGIC   0x464c457fU   /* 0x7f 'E' 'L' 'F'，小端存储 */

/* ===== e_ident 索引 ===== */
#define EI_CLASS    4   /* 文件类型：32/64-bit */
#define EI_DATA     5   /* 字节序 */
#define EI_VERSION  6   /* ELF header version */

#define ELFCLASS32  1   /* 32-bit ELF */
#define ELFDATA2LSB 1   /* 小端序 */

/* ===== e_type ===== */
#define ET_REL      1   /* relocatable file */
#define ET_EXEC     2   /* 可执行文件 */
#define ET_DYN      3   /* 共享对象（PIC）*/

#define EV_CURRENT  1

/* ===== e_machine ===== */
#define EM_ARM      40  /* ARM 架构 */

/* ===== Section header 类型 (sh_type) ===== */
#define SHT_NULL        0
#define SHT_PROGBITS    1   /* 代码、数据、向量表等 */
#define SHT_SYMTAB      2   /* 符号表 */
#define SHT_STRTAB      3   /* 字符串表 */
#define SHT_RELA        4   /* 带显式加数的重定向表 */
#define SHT_NOBITS      8   /* .bss：文件中不占空间 */
#define SHT_REL         9   /* 不带显式加数的重定向表 */
#define SHT_INIT_ARRAY  14
#define SHT_FINI_ARRAY  15
#define SHT_PREINIT_ARRAY 16
#define SHT_ARM_EXIDX   0x70000001U

/* ===== Section header 标志 (sh_flags) ===== */
#define SHF_WRITE       0x1   /* 可写 */
#define SHF_ALLOC       0x2   /* 需要分配内存（加载时）*/
#define SHF_EXECINSTR   0x4   /* 可执行 */

/* ===== 特殊 section 索引 (st_shndx) ===== */
#define SHN_UNDEF   0       /* 未定义符号 */
#define SHN_ABS     0xfff1  /* 绝对地址符号（不随重定向改变）*/
#define SHN_COMMON  0xfff2  /* 公共符号 */

/* ===== 符号绑定类型 (ELF32_ST_BIND) ===== */
#define STB_LOCAL   0
#define STB_GLOBAL  1
#define STB_WEAK    2

/* ===== 符号类型 (ELF32_ST_TYPE) ===== */
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define STT_FILE    4

/* ===== ARM 重定向类型 ===== */
#define R_ARM_NONE          0
#define R_ARM_ABS32         2
#define R_ARM_REL32         3
#define R_ARM_THM_PC8       11
#define R_ARM_CALL          28
#define R_ARM_JUMP24        29
#define R_ARM_TARGET1       38
#define R_ARM_V4BX          40
#define R_ARM_PREL31        42
#define R_ARM_MOVW_ABS_NC   43
#define R_ARM_MOVT_ABS      44
#define R_ARM_MOVW_PREL_NC  45
#define R_ARM_MOVT_PREL     46
#define R_ARM_THM_MOVW_ABS_NC 47
#define R_ARM_THM_MOVT_ABS  48
#define R_ARM_THM_MOVW_PREL_NC 49
#define R_ARM_THM_MOVT_PREL 50
#define R_ARM_THM_JUMP19    51
#define R_ARM_THM_JUMP6     52
#define R_ARM_THM_ALU_PREL_11_0 53
#define R_ARM_THM_PC12      54
#define R_ARM_RELATIVE      23
#define R_ARM_THM_CALL      10
#define R_ARM_THM_JUMP24    30

/* ===== ELF32 基础结构体 ===== */

/* ELF 文件头（52字节）*/
typedef struct {
    uint8_t  e_ident[16];   /* 魔数 + 文件类型标识 */
    uint16_t e_type;        /* 文件类型 */
    uint16_t e_machine;     /* 目标架构 */
    uint32_t e_version;     /* ELF 版本，固定为 1 */
    uint32_t e_entry;       /* 程序入口虚拟地址 */
    uint32_t e_phoff;       /* Program header table 文件偏移 */
    uint32_t e_shoff;       /* Section header table 文件偏移 */
    uint32_t e_flags;       /* 处理器相关标志 */
    uint16_t e_ehsize;      /* ELF header 大小（字节）*/
    uint16_t e_phentsize;   /* Program header entry 大小 */
    uint16_t e_phnum;       /* Program header entry 数量 */
    uint16_t e_shentsize;   /* Section header entry 大小 */
    uint16_t e_shnum;       /* Section header entry 数量 */
    uint16_t e_shstrndx;    /* Section 名称字符串表的 section 索引 */
} elf32_ehdr;

/* Section header（40字节）*/
typedef struct {
    uint32_t sh_name;       /* Section 名称（shstrtab 中的偏移）*/
    uint32_t sh_type;       /* Section 类型 */
    uint32_t sh_flags;      /* Section 属性标志 */
    uint32_t sh_addr;       /* Section 的虚拟地址（链接基址为0时即偏移）*/
    uint32_t sh_offset;     /* Section 在文件中的偏移 */
    uint32_t sh_size;       /* Section 大小（字节）*/
    uint32_t sh_link;       /* 关联 section 索引（如符号表→字符串表）*/
    uint32_t sh_info;       /* 附加信息（重定向表→目标section索引）*/
    uint32_t sh_addralign;  /* 对齐要求 */
    uint32_t sh_entsize;    /* 固定大小 entry 的字节数（如符号表条目）*/
} elf32_shdr;

/* 符号表条目（16字节）*/
typedef struct {
    uint32_t st_name;       /* 符号名称（strtab 中的偏移）*/
    uint32_t st_value;      /* 符号值（地址或偏移）*/
    uint32_t st_size;       /* 符号大小 */
    uint8_t  st_info;       /* 绑定类型 + 符号类型 */
    uint8_t  st_other;      /* 可见性 */
    uint16_t st_shndx;      /* 所在 section 索引 */
} elf32_sym;

/* REL 重定向条目（8字节，无显式加数）*/
typedef struct {
    uint32_t r_offset;      /* 需要修正的位置（相对目标section起始的偏移）*/
    uint32_t r_info;        /* 符号索引 + 重定向类型 */
} elf32_rel;

/* RELA 重定向条目（12字节，含显式加数）*/
typedef struct {
    uint32_t r_offset;      /* 需要修正的位置 */
    uint32_t r_info;        /* 符号索引 + 重定向类型 */
    int32_t  r_addend;      /* 显式加数 */
} elf32_rela;

/* ===== r_info 拆解宏 ===== */
#define ELF32_R_SYM(info)   ((info) >> 8)
#define ELF32_R_TYPE(info)  ((uint8_t)(info))

/* ===== st_info 拆解宏 ===== */
#define ELF32_ST_BIND(info) ((info) >> 4)
#define ELF32_ST_TYPE(info) ((info) & 0xf)

#endif /* ELF_H */



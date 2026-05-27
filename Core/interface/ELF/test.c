/**
 * bootloader_example.c — STM32L431 Bootloader 使用示例
 *
 * 演示完整流程：
 *   从外挂 Flash 读取 ELF → 解析 → 重定向 → 烧写内部 Flash → 跳转
 *
 * 注意：flash_write() 你已经写好，此处只是占位声明。
 *       ext_flash_read() 需要根据你的实际外挂 Flash 驱动实现。
 */

#include "elf_loader.h"
#include <string.h>

/* ===== 分区地址定义（根据你的实际规划调整）===== */
#define PARTITION_A_ADDR    0x08010000u   /* 分区 A 起始地址 */
#define PARTITION_B_ADDR    0x08028000u   /* 分区 B 起始地址 */
#define PARTITION_SIZE      (96 * 1024u)  /* 每个分区 96KB */

/* 状态区：内部 Flash 末尾，记录当前激活分区 */
#define STATUS_AREA_ADDR    0x0803E000u
#define ACTIVE_PARTITION_A  0xAAAAAAAAu
#define ACTIVE_PARTITION_B  0x55555555u

/* 外挂 Flash 中 ELF 文件的存储地址 */
#define EXT_FLASH_ELF_ADDR  0x00000000u

/* ===== 外部函数声明（由你的驱动实现）===== */

/* 你已经写好的内部 Flash 写入函数 */
extern int flash_write(uint32_t dst_addr, const uint8_t *src, uint32_t len);
extern int flash_erase(uint32_t addr, uint32_t len);

/* 外挂 Flash 读取（根据你的实际接口实现）*/
extern int ext_flash_read(uint32_t src_addr, uint8_t *dst, uint32_t len);

/* ===== 工作 buffer（放在外挂 RAM 或内部 SRAM）===== */
/*
 * 如果外挂的是 PSRAM（可随机读写），直接把 ELF 读进来就行。
 * 如果内部 SRAM 不够，把这个 buffer 放到外挂 RAM 的地址：
 *   static uint8_t *elf_buf = (uint8_t *)0x60000000;  // 外挂 RAM 地址
 *
 * 此处先用内部 SRAM，你根据实际情况调整。
 */
#define ELF_BUF_SIZE    (64 * 1024u)
static uint8_t elf_buf[ELF_BUF_SIZE];

/* ===== 读取当前激活分区 ===== */
static uint32_t get_active_partition(void)
{
    uint32_t flag = *(volatile uint32_t *)STATUS_AREA_ADDR;
    if (flag == ACTIVE_PARTITION_B) {
        return PARTITION_B_ADDR;
    }
    return PARTITION_A_ADDR;  /* 默认分区 A */
}

/* ===== 获取非激活分区（用于 OTA 写入）===== */
static uint32_t get_inactive_partition(void)
{
    uint32_t flag = *(volatile uint32_t *)STATUS_AREA_ADDR;
    if (flag == ACTIVE_PARTITION_B) {
        return PARTITION_A_ADDR;
    }
    return PARTITION_B_ADDR;
}

/* ===== 跳转到应用 ===== */
static void jump_to_app(uint32_t partition_addr)
{
    /* 向量表第 0 项：初始栈指针 */
    uint32_t sp = *(volatile uint32_t *)(partition_addr + 0);
    /* 向量表第 1 项：Reset_Handler 地址 */
    uint32_t pc = *(volatile uint32_t *)(partition_addr + 4);

    /* 告诉 CPU 向量表在哪里 */
    SCB->VTOR = partition_addr;

    /* 关中断，确保跳转过程干净 */
    __disable_irq();

    /* 设置栈指针并跳转（需要裸汇编）*/
    __asm volatile (
        "msr msp, %0    \n"   /* 设置主栈指针 */
        "bx  %1         \n"   /* 跳转（会自动进入 Thumb 模式）*/
        :
        : "r" (sp), "r" (pc)
        : "memory"
    );

    /* 不会执行到这里 */
    while (1);
}

/* ===================================================================
 * 主流程：OTA 更新时调用
 *
 * 从外挂 Flash 读取新固件 ELF，重定向到非激活分区，烧写，切换标志
 * =================================================================== */
int bootloader_update_firmware(void)
{
    int ret;
    uint32_t elf_size;

    /* ---- 步骤1：读取 ELF 文件大小（假设存储在 ELF 头部前4字节或配置区）---- */
    /* 简化：直接读满 buffer，parser 会自行校验边界 */
    elf_size = ELF_BUF_SIZE;

    ret = ext_flash_read(EXT_FLASH_ELF_ADDR, elf_buf, elf_size);
    if (ret != 0) {
        return -1;
    }

    /* ---- 步骤2：解析 ELF ---- */
    elf_ctx_t ctx;
    ret = elf_parse(&ctx, elf_buf, elf_size);
    if (ret != ELF_OK) {
        /* ret 是负数错误码，如 ELF_ERR_MAGIC */
        return ret;
    }

    /* ---- 步骤3：确定目标分区 ---- */
    uint32_t target_partition = get_inactive_partition();

    /* ---- 步骤4：对 buffer 中的内容施加地址偏移（修改 elf_buf 内容）---- */
    ret = elf_relocate(&ctx, target_partition);
    if (ret != ELF_OK) {
        return ret;
    }

    /* ---- 步骤5：按 section 逐个烧写到目标分区 ---- */
    for (uint32_t i = 0; i < ctx.load_count; i++) {
        elf_section_info_t sec;
        elf_get_section(&ctx, i, &sec);

        /* 检查不超出分区范围 */
        if (sec.load_addr < target_partition ||
            sec.load_addr + sec.size > target_partition + PARTITION_SIZE) {
            /* section 超出分区范围，中止 */
            return -2;
        }

        if (sec.is_bss) {
            /* .bss 段：擦除后清零（Flash 擦除后默认是 0xFF，需要写0）*/
            /* 实际上 .bss 不需要烧到 Flash，运行时由启动文件清零 SRAM */
            /* 此处跳过，启动文件的 Reset_Handler 会处理 */
            (void)sec;
        } else {
            /* 先擦除目标区域 */
            flash_erase(sec.load_addr, sec.size);
            /* 烧写数据 */
            flash_write(sec.load_addr, sec.data, sec.size);
        }
    }

    /* ---- 步骤6：切换激活分区标志 ---- */
    uint32_t new_flag = (target_partition == PARTITION_B_ADDR)
                        ? ACTIVE_PARTITION_B
                        : ACTIVE_PARTITION_A;
    flash_erase(STATUS_AREA_ADDR, 2048);  /* 擦除一页 */
    flash_write(STATUS_AREA_ADDR, (uint8_t *)&new_flag, sizeof(new_flag));

    return ELF_OK;
}

/* ===================================================================
 * 主流程：每次上电时调用
 * =================================================================== */
void bootloader_boot(void)
{
    uint32_t active = get_active_partition();

    /* 简单校验：检查向量表魔数（第0项是初始SP，应在 SRAM 范围内）*/
    uint32_t sp = *(volatile uint32_t *)active;
    if (sp < 0x20000000u || sp > 0x20010000u) {
        /* 分区无效，尝试另一个 */
        active = (active == PARTITION_A_ADDR) ? PARTITION_B_ADDR : PARTITION_A_ADDR;
    }

    jump_to_app(active);
}
#ifndef __SHELL_H__
#define __SHELL_H__

/**
 * @file  shell.h
 * @brief 与硬件完全解耦的串口 Shell 框架
 *
 * 用户只需：
 *   1. 实现 putc / getc 两个回调
 *   2. 定义命令表 shell_cmd_t[]
 *   3. 填充 shell_config_t 并调用 shell_init()
 *   4. 主循环里调用 shell_run()
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  可配置项（不想改结构体的参数在这里调）                              */
/* ------------------------------------------------------------------ */
#define SHELL_LINE_MAX      128     /* 单行最大字节数   */
#define SHELL_ARG_MAX       8       /* 最多参数个数     */

/* ------------------------------------------------------------------ */
/*  命令描述                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief 命令回调函数类型
 * @param argc  参数个数（含命令名本身）
 * @param argv  参数列表，argv[0] 为命令名
 */
typedef int (*shell_cmd_func_t)(int argc, char *argv[]);

typedef struct {
    const char        *name;    /* 命令名，如 "ls"            */
    const char        *help;    /* 帮助说明，如 "list files"  */
    shell_cmd_func_t   func;    /* 命令回调                   */
} shell_cmd_t;

/* ------------------------------------------------------------------ */
/*  Shell 配置结构体（用户填充后传给 shell_init）                       */
/* ------------------------------------------------------------------ */
typedef struct {

    /* --- IO 回调（必填）---------------------------------------------- */
    void (*putc)(char c);       /* 输出一个字符，如写 UART TX      */
    char (*getc)(void);         /* 阻塞读一个字符，如从缓冲区取    */

    /* --- 命令表（必填）----------------------------------------------- */
    const shell_cmd_t *commands;    /* 命令数组首地址               */
    uint8_t            cmd_count;   /* 命令个数                     */

    /* --- 可选项（填 0 / NULL 使用默认值）------------------------------ */
    const char *prompt;             /* 提示符，默认 "> "            */

    uint8_t     history_size;       /* 历史记录条数，最大 16，默认 8 */

} shell_config_t;

/* ------------------------------------------------------------------ */
/*  对外接口                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief 初始化 Shell，传入配置结构体
 * @param cfg  用户填充好的配置，生命周期须长于 shell_run()
 */
void shell_init(const shell_config_t *cfg);

/**
 * @brief Shell 主循环，内部 while(1) 永不返回
 *        放入 main() 的 while(1) 或独立任务
 */
void shell_run(void);

#ifdef __cplusplus
}
#endif

#endif /* __SHELL_H__ */



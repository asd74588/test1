/**
 * @file    nr_micro_shell_port.h
 * @brief   nr_micro_shell v2.0.0 STM32 HAL 移植配置
 *
 * 本文件由库的 #include "nr_micro_shell_port.h" 直接引用，
 * 放到与 nr_micro_shell.h 相同的 include path 下即可。
 *
 * 必须修改：将  改为项目实际使用的 UART handle。
 */

#ifndef __NR_MICRO_SHELL_PORT_H__
#define __NR_MICRO_SHELL_PORT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"      /* HAL headers, UART handle 声明 */
#include <stdint.h>

/* ================================================================
 * [必须] 底层单字符输出 — 库内部所有输出最终走此宏
 * 修改  为项目实际的 UART handle 名称
 * ================================================================ */
extern UART_HandleTypeDef huart1;   /* 与 Shell_Init() 中保持一致 */

#define shell_putc(x) \
    do { \
        uint8_t _c = (uint8_t)(x); \
        HAL_UART_Transmit(&huart1, &_c, 1U, 10U); \
    } while (0)

/* ================================================================
 * [可选] 行为配置（不定义则使用库默认值）
 * ================================================================ */

/** 命令行最大长度（字节，含 '\0'），默认 80 */
#define NR_SHELL_MAX_LINE_SZ        128

/** 最大参数数量，默认 8 */
#define NR_SHELL_MAX_PARAM_NUM      8

/** 提示符，默认 "nr@dev" */
#define NR_SHELL_PROMPT             "@dev:"

/** 开启历史命令支持（上下键翻历史） */
#define NR_SHELL_HISTORY_CMD_SUPPORT

#ifdef NR_SHELL_HISTORY_CMD_SUPPORT
#define NR_SHELL_HISTORY_CMD_NUM    5    /* 历史条数 */
#define NR_SHELL_HISTORY_CMD_SZ     64   /* 每条最大长度 */
#endif

#ifdef __cplusplus
}
#endif
#endif /* __NR_MICRO_SHELL_PORT_H__ */





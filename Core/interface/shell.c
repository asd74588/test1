/**
 * @file  shell.c
 * @brief 与硬件完全解耦的串口 Shell 框架
 *
 * 本文件不包含任何硬件头文件，所有 IO 通过 shell_config_t 里的
 * putc / getc 回调完成，可直接移植到任意平台。
 */

#include "shell.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  内部常量                                                            */
/* ------------------------------------------------------------------ */
#define HISTORY_MAX     16      /* history_size 上限 */
#define PROMPT_DEFAULT  "> "

/* ------------------------------------------------------------------ */
/*  运行时状态（全部私有）                                              */
/* ------------------------------------------------------------------ */
static const shell_config_t *s_cfg = NULL;

/* 历史记录 */
static char  s_history[HISTORY_MAX][SHELL_LINE_MAX];
static int   s_history_size  = 0;   /* 实际使用的历史槽位数 */
static int   s_history_count = 0;   /* 已压入的条目总数     */
static int   s_history_index = 0;   /* 上下键浏览游标       */

/* ------------------------------------------------------------------ */
/*  内部 IO 封装                                                        */
/* ------------------------------------------------------------------ */
static inline void io_putc(char c)
{
    s_cfg->putc(c);
}

static inline char io_getc(void)
{
    return s_cfg->getc();
}

static void io_puts(const char *s)
{
    while (*s) io_putc(*s++);
}

static void io_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    io_puts(buf);
}

/* ------------------------------------------------------------------ */
/*  历史记录                                                            */
/* ------------------------------------------------------------------ */
static void history_push(const char *line)
{
    if (line[0] == '\0') return;

    /* 与最新一条相同则不重复压入 */
    if (s_history_count > 0) {
        int last = (s_history_count - 1) % s_history_size;
        if (strcmp(s_history[last], line) == 0) return;
    }

    int idx = s_history_count % s_history_size;
    strncpy(s_history[idx], line, SHELL_LINE_MAX - 1);
    s_history[idx][SHELL_LINE_MAX - 1] = '\0';
    s_history_count++;
    s_history_index = s_history_count;
}

/* offset=1 最新，offset=2 次新，超出范围返回 NULL */
static const char *history_get(int offset)
{
    if (offset < 1 || offset > s_history_count ||
        offset > s_history_size) return NULL;
    int idx = (s_history_count - offset) % s_history_size;
    return s_history[idx];
}

/* ------------------------------------------------------------------ */
/*  行编辑                                                              */
/* ------------------------------------------------------------------ */
static char   s_line[SHELL_LINE_MAX];
static int    s_line_len = 0;

/* 清除终端当前行，重新打印提示符 + 当前行内容 */
static void refresh_line(void)
{
    io_puts("\r\033[K");
    io_puts(s_cfg->prompt ? s_cfg->prompt : PROMPT_DEFAULT);
    io_puts(s_line);
}

/* 读取一整行，支持退格、Ctrl+C、上下键翻历史 */
static void readline(void)
{
    s_line_len      = 0;
    s_line[0]       = '\0';
    s_history_index = s_history_count;

    /* 用于吞掉 \r\n 里多余的 \n */
    static uint8_t last_was_cr = 0;

    while (1) {
        char c = io_getc();

        /* --- \r\n 处理 --- */
        if (c == '\n') {
            if (last_was_cr) { last_was_cr = 0; continue; }
            io_puts("\r\n");
            s_line[s_line_len] = '\0';
            return;
        }
        if (c == '\r') {
            last_was_cr = 1;
            io_puts("\r\n");
            s_line[s_line_len] = '\0';
            return;
        }
        last_was_cr = 0;

        /* --- 退格 --- */
        if (c == '\b' || c == 0x7F) {
            if (s_line_len > 0) {
                s_line_len--;
                s_line[s_line_len] = '\0';
                io_puts("\b \b");
            }
            continue;
        }

        /* --- Ctrl+C：清行 --- */
        if (c == 0x03) {
            s_line_len = 0;
            s_line[0]  = '\0';
            io_puts("^C\r\n");
            io_puts(s_cfg->prompt ? s_cfg->prompt : PROMPT_DEFAULT);
            continue;
        }

        /* --- ESC 序列：方向键 ESC [ A/B --- */
        if (c == '\033') {
            char c2 = io_getc();
            char c3 = io_getc();
            if (c2 == '[') {
                if (c3 == 'A') {            /* 上键：往旧历史 */
                    s_history_index++;
                } else if (c3 == 'B') {     /* 下键：往新历史 */
                    s_history_index--;
                } else {
                    continue;
                }

                /* 限制游标范围 */
                if (s_history_index < 1)
                    s_history_index = 1;
                if (s_history_index > s_history_count)
                    s_history_index = s_history_count + 1;

                /* 替换行内容 */
                const char *h = NULL;
                if (s_history_index <= s_history_count)
                    h = history_get(s_history_index);

                memset(s_line, 0, sizeof(s_line));
                if (h) {
                    strncpy(s_line, h, SHELL_LINE_MAX - 1);
                    s_line_len = strlen(s_line);
                } else {
                    s_line_len = 0;
                }
                refresh_line();
            }
            continue;
        }

        /* --- 普通可打印字符 --- */
        if (c >= 0x20 && c < 0x7F && s_line_len < SHELL_LINE_MAX - 1) {
            s_line[s_line_len++] = c;
            s_line[s_line_len]   = '\0';
            io_putc(c);     /* 回显 */
        }
    }
}

/* ------------------------------------------------------------------ */
/*  命令解析                                                            */
/* ------------------------------------------------------------------ */
static int parse_args(char *line, char *argv[], int max_argc)
{
    int   argc = 0;
    char *p    = line;

    while (*p && argc < max_argc) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

/* ------------------------------------------------------------------ */
/*  内建命令：help                                                      */
/* ------------------------------------------------------------------ */
static void builtin_help(int argc, char *argv[])
{
    (void)argc; (void)argv;
    io_puts("\r\nCommands:\r\n");
    for (int i = 0; i < s_cfg->cmd_count; i++) {
        io_printf("  %-16s  %s\r\n",
                  s_cfg->commands[i].name,
                  s_cfg->commands[i].help ? s_cfg->commands[i].help : "");
    }
    io_printf("  %-16s  show this help\r\n", "help");
    io_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/*  命令分发                                                            */
/* ------------------------------------------------------------------ */
static void dispatch(int argc, char *argv[])
{
    if (argc == 0) return;

    /* 内建 help 优先 */
    if (strcmp(argv[0], "help") == 0) {
        builtin_help(argc, argv);
        return;
    }

    for (int i = 0; i < s_cfg->cmd_count; i++) {
        if (strcmp(argv[0], s_cfg->commands[i].name) == 0) {
            s_cfg->commands[i].func(argc, argv);
            return;
        }
    }

    io_printf("'%s': command not found, type 'help'\r\n", argv[0]);
}

/* ------------------------------------------------------------------ */
/*  对外接口实现                                                        */
/* ------------------------------------------------------------------ */
void shell_init(const shell_config_t *cfg)
{
    s_cfg = cfg;

    /* 历史记录槽位数：限制在 [1, HISTORY_MAX] */
    s_history_size = (cfg->history_size > 0 && cfg->history_size <= HISTORY_MAX)
                     ? cfg->history_size : 8;

    const char *prompt = cfg->prompt ? cfg->prompt : PROMPT_DEFAULT;

    io_puts("\r\n==============================\r\n");
    io_printf("  Shell ready  —  type 'help'\r\n");
    io_puts("==============================\r\n");
    io_puts(prompt);
}

void shell_run(void)
{
    char *argv[SHELL_ARG_MAX];
    const char *prompt = s_cfg->prompt ? s_cfg->prompt : PROMPT_DEFAULT;

    while (1) {
        readline();

        if (s_line_len == 0) {
            io_puts(prompt);
            continue;
        }

        history_push(s_line);

        /* parse_args 原地截断 s_line，必须在 history_push 之后 */
        int argc = parse_args(s_line, argv, SHELL_ARG_MAX);

        dispatch(argc, argv);

        io_puts(prompt);
    }
}


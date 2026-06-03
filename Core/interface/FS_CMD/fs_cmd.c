/**
 * @file  fs_cmd.c
 * @brief 文件系统 Shell 命令实现，基于 LittleFS
 *
 * 函数签名统一为 (int argc, char *argv[])，可直接填入 shell_cmd_t 命令表
 */

#include "fs_cmd.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  内部持有的上下文指针，由 fs_cmd_init() 注入                             */
/* ------------------------------------------------------------------ */
static lfs_ctx_t *s_ctx = NULL;

void fs_cmd_init(lfs_ctx_t *ctx)
{
    s_ctx = ctx;
}

/* ------------------------------------------------------------------ */
/*  内部宏                                                              */
/* ------------------------------------------------------------------ */
#define FS_PRINT(fmt, ...)  printf(fmt, ##__VA_ARGS__)

#define FS_CHECK_READY()                                \
    do {                                                \
        if (s_ctx == NULL) {                            \
            FS_PRINT("fs: not initialized\r\n");        \
            return -1;                                  \
        }                                               \
        if (!s_ctx->mounted) {                          \
            FS_PRINT("fs: not mounted\r\n");            \
            return -1;                                  \
        }                                               \
    } while (0)

#define LFS  (&s_ctx->lfs)

/* ------------------------------------------------------------------ */
/*  fs_cmd_ls  —  列目录                                               */
/*  用法: ls [path]                                                     */
/* ------------------------------------------------------------------ */
int fs_cmd_ls(int argc, char *argv[])
{
    FS_CHECK_READY();

    const char *path = (argc >= 2) ? argv[1] : "/";

    lfs_dir_t       dir;
    struct lfs_info info;

    int err = lfs_dir_open(LFS, &dir, path);
    if (err < 0) {
        FS_PRINT("ls: cannot open '%s' (err=%d)\r\n", path, err);
        return -1;
    }

    FS_PRINT("--- %s ---\r\n", path);

    while (1) {
        int res = lfs_dir_read(LFS, &dir, &info);
        if (res < 0) {
            FS_PRINT("ls: read error (err=%d)\r\n", res);
            lfs_dir_close(LFS, &dir);
            return -1;
        }
        if (res == 0) break;

        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0)
            continue;

        if (info.type == LFS_TYPE_DIR) {
            FS_PRINT("[DIR]  %s\r\n", info.name);
        } else {
            if (info.size < 1024)
                FS_PRINT("[FILE] %-24s %lu B\r\n",
                         info.name, (unsigned long)info.size);
            else
                FS_PRINT("[FILE] %-24s %lu KB\r\n",
                         info.name, (unsigned long)(info.size / 1024));
        }
    }

    lfs_dir_close(LFS, &dir);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  fs_cmd_cat  —  打印文件内容                                        */
/*  用法: cat <file>                                                    */
/* ------------------------------------------------------------------ */
int fs_cmd_cat(int argc, char *argv[])
{
    FS_CHECK_READY();

    if (argc < 2) {
        FS_PRINT("usage: cat <file>\r\n");
        return -1;
    }

    if (s_ctx->file_open) {
        FS_PRINT("cat: a file is already open\r\n");
        return -1;
    }

    int err = lfs_file_open(LFS, &s_ctx->file, argv[1], LFS_O_RDONLY);
    if (err < 0) {
        FS_PRINT("cat: cannot open '%s' (err=%d)\r\n", argv[1], err);
        return -1;
    }
    s_ctx->file_open = 1;

    char        buf[64];
    lfs_ssize_t n;
    int         ret = 0;

    while ((n = lfs_file_read(LFS, &s_ctx->file, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        FS_PRINT("%s", buf);
    }
    FS_PRINT("\r\n");

    if (n < 0) {
        FS_PRINT("cat: read error (err=%d)\r\n", (int)n);
        ret = -1;
    }

    lfs_file_close(LFS, &s_ctx->file);
    s_ctx->file_open = 0;
    return ret;
}

/* ------------------------------------------------------------------ */
/*  fs_cmd_write  —  写入文件（覆盖）                                  */
/*  用法: write <file> <data>                                           */
/* ------------------------------------------------------------------ */
int fs_cmd_write(int argc, char *argv[])
{
    FS_CHECK_READY();

    if (argc < 3) {
        FS_PRINT("usage: write <file> <data>\r\n");
        return -1;
    }

    if (s_ctx->file_open) {
        FS_PRINT("write: a file is already open\r\n");
        return -1;
    }

    int err = lfs_file_open(LFS, &s_ctx->file, argv[1],
                            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err < 0) {
        FS_PRINT("write: cannot open '%s' (err=%d)\r\n", argv[1], err);
        return -1;
    }
    s_ctx->file_open = 1;

    lfs_size_t  len     = (lfs_size_t)strlen(argv[2]);
    lfs_ssize_t written = lfs_file_write(LFS, &s_ctx->file, argv[2], len);

    lfs_file_close(LFS, &s_ctx->file);
    s_ctx->file_open = 0;

    if (written < 0 || (lfs_size_t)written != len) {
        FS_PRINT("write: write error (err=%d)\r\n", (int)written);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  fs_cmd_rm  —  删除文件或空目录                                     */
/*  用法: rm <file>                                                     */
/* ------------------------------------------------------------------ */
int fs_cmd_rm(int argc, char *argv[])
{
    FS_CHECK_READY();

    if (argc < 2) {
        FS_PRINT("usage: rm <file>\r\n");
        return -1;
    }

    int err = lfs_remove(LFS, argv[1]);
    if (err < 0) {
        FS_PRINT("rm: cannot remove '%s' (err=%d)\r\n", argv[1], err);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  fs_cmd_mkdir  —  创建目录                                          */
/*  用法: mkdir <dir>                                                   */
/* ------------------------------------------------------------------ */
int fs_cmd_mkdir(int argc, char *argv[])
{
    FS_CHECK_READY();

    if (argc < 2) {
        FS_PRINT("usage: mkdir <dir>\r\n");
        return -1;
    }

    int err = lfs_mkdir(LFS, argv[1]);
    if (err < 0) {
        FS_PRINT("mkdir: cannot create '%s' (err=%d)\r\n", argv[1], err);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  fs_cmd_free  —  显示存储空间                                       */
/*  用法: free                                                          */
/* ------------------------------------------------------------------ */
int fs_cmd_free(int argc, char *argv[])
{
    (void)argc; (void)argv;
    FS_CHECK_READY();

    lfs_ssize_t used_blocks = lfs_fs_size(LFS);
    if (used_blocks < 0) {
        FS_PRINT("free: cannot get fs size (err=%d)\r\n", (int)used_blocks);
        return -1;
    }

    lfs_size_t block_size  = LFS->cfg->block_size;
    lfs_size_t block_count = LFS->cfg->block_count;
    lfs_size_t total_kb    = (block_size * block_count)                  / 1024;
    lfs_size_t used_kb     = (block_size * (lfs_size_t)used_blocks)      / 1024;
    lfs_size_t free_kb     = total_kb - used_kb;

    FS_PRINT("Total : %4lu KB  (%lu blocks)\r\n",
             (unsigned long)total_kb,  (unsigned long)block_count);
    FS_PRINT("Used  : %4lu KB  (%lu blocks)\r\n",
             (unsigned long)used_kb,   (unsigned long)used_blocks);
    FS_PRINT("Free  : %4lu KB  (%lu blocks)\r\n",
             (unsigned long)free_kb,
             (unsigned long)(block_count - (lfs_size_t)used_blocks));

    return 0;
}
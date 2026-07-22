#ifndef __FS_CMD_H__
#define __FS_CMD_H__

/**
 * @file  fs_cmd.h
 * @brief 文件系统 Shell 命令层
 *
 * 使用方式：
 *   1. main() 里挂载好 lfs 后调用 fs_init(&lfs_ctx) 注入实例
 *   2. 将 fs_cmd_* 直接填入命令表
 *
 *   static const shell_cmd_t my_cmds[] = {
 *       { "ls",    "list directory",     fs_cmd_ls    },
 *       { "cat",   "print file",         fs_cmd_cat   },
 *       { "write", "write to file",      fs_cmd_write },
 *       { "rm",    "remove file",        fs_cmd_rm    },
 *       { "mkdir", "make directory",     fs_cmd_mkdir },
 *       { "free",  "show fs usage",      fs_cmd_free  },
 *   };
 */

#include "lfs.h"
#include "global.h"

/**
 * @brief 注入 lfs_ctx_t 指针，必须在调用任何 fs_cmd_* 之前调用一次
 * @param ctx  已完成 lfs_mount 的 lfs_ctx_t 指针
 */
void fs_cmd_init(lfs_ctx_t *ctx);

int fs_cmd_ls(int argc, char *argv[]);
int fs_cmd_cat(int argc, char *argv[]);
int fs_cmd_write(int argc, char *argv[]);
int fs_cmd_rm(int argc, char *argv[]);
int fs_cmd_mkdir(int argc, char *argv[]);
int fs_cmd_free(int argc, char *argv[]);

#endif /* __FS_CMD_H__ */

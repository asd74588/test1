/**
 * @file  cmds_ymodem.c
 * @brief Ymodem 接收 / 发送命令实现
 *
 * ymodem_receive  — 从串口接收文件，写入 LittleFS
 * ymodem_send     — 从 LittleFS 读文件，通过串口发送
 *
 * 依赖：
 *   Proto_Start_Receive()   xmodem_ymodem.c 提供
 *   lfs / lfs_file_*        LittleFS 原生接口
 *   huart1                  与 Shell 共用同一串口
 */

#include "cmds.h"
#include "xmodem.h"
#include "lfs.h"
#include <string.h>
#include <stdio.h>

extern lfs_ctx_t lfs_ctx; /* LittleFS 实例，由 main.c 挂载 */

/* ================================================================
 * Ymodem 接收（rz）
 * ================================================================ */

/* write_cb：Proto_Start_Receive 每收到一块数据调用一次 */
typedef struct
{
    lfs_file_t file;
    int        opened;
    char       path[64];
} ymodem_rx_ctx_t;

static int ymodem_write_cb(const uint8_t *data, uint32_t len, void *user)
{
    ymodem_rx_ctx_t *ctx     = (ymodem_rx_ctx_t *)user;
    lfs_ssize_t      written = lfs_file_write(&lfs_ctx.lfs, &ctx->file, data, (lfs_size_t)len);
    if (written < 0 || (uint32_t)written != len)
    {
        shell_printf("LFS write error: %d\r\n", (int)written);
        return -1;
    }
    return 0;
}

/**
 * rz [path]
 * 不带 path 时文件名由 Ymodem 协议头提供，存到根目录 /
 *
 * 示例：
 *   rz
 *   rz /fw/app.bin
 */
int ymodem_receive(uint8_t argc, char **argv)
{
    ymodem_rx_ctx_t rx_ctx;
    memset(&rx_ctx, 0, sizeof(rx_ctx));

    YmodemFileInfo file_info;
    memset(&file_info, 0, sizeof(file_info));

    transfer_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.write_cb       = ymodem_write_cb;
    cfg.write_user_ctx = &rx_ctx;

    /* 确定存储路径：命令行参数优先，否则等协议头填充后再定 */
    int path_from_arg = (argc >= 2);
    if (path_from_arg)
    {
        strncpy(rx_ctx.path, argv[1], sizeof(rx_ctx.path) - 1);
    }

    shell_printf("Waiting for Ymodem transfer (send file now)...\r\n");

    /* 握手完成、文件名包解析完毕后才知道文件名，
     * 所以先用临时路径打开，传输完后 rename。
     * 若调用方已给 path 则直接用该路径。           */
    const char *open_path = path_from_arg ? rx_ctx.path : "/tmp_ymodem_rx";

    int lfs_err = lfs_file_open(
        &lfs_ctx.lfs, &rx_ctx.file, open_path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (lfs_err < 0)
    {
        shell_printf("LFS open error: %d  path=%s\r\n", lfs_err, open_path);
        return -1;
    }
    rx_ctx.opened = 1;

    int result = Proto_Start_Receive(&cfg, &file_info);

    lfs_file_close(&lfs_ctx.lfs, &rx_ctx.file);
    rx_ctx.opened = 0;

    if (result < 0)
    {
        lfs_remove(&lfs_ctx.lfs, open_path);
        shell_printf("Ymodem receive failed.\r\n");
        return -1;
    }

    /* 若路径来自协议头，rename 临时文件 */
    if (!path_from_arg && file_info.filename[0] != '\0')
    {
        /* 构造目标路径 */
        char dst[72];
        snprintf(dst, sizeof(dst), "/%s", file_info.filename);

        lfs_rename(&lfs_ctx.lfs, open_path, dst);
        shell_printf("Received: %s  (%d bytes)\r\n", dst, result);
    }
    else
    {
        shell_printf("Received: %s  (%d bytes)\r\n", open_path, result);
    }

    return 0;
}

/* ================================================================
 * Ymodem 发送（sz）
 * ================================================================ */
static int ymodem_read_cb(uint8_t *buf, uint32_t max_len, uint32_t *out_len, void *user)
{
    lfs_file_t *file  = (lfs_file_t *)user;
    lfs_ssize_t nread = lfs_file_read(&lfs_ctx.lfs, file, buf, (lfs_size_t)max_len);
    if (nread < 0)
    {
        shell_printf("LFS read error: %d\r\n", (int)nread);
        return -1;
    }

    *out_len = (uint32_t)nread;
    return 0;
}

/**
 * sz <path>
 * 把 LittleFS 上的文件通过 Ymodem 发送出去
 *
 * 示例：
 *   sz /fw/app.bin
 */
int ymodem_send(uint8_t argc, char **argv)
{
    if (argc < 2)
    {
        shell_printf("Usage: sz <path>\r\n");
        return -1;
    }

    const char *path = argv[1];

    /* ── 打开文件 ─────────────────────────────────────────── */
    lfs_file_t file;
    int        err = lfs_file_open(&lfs_ctx.lfs, &file, path, LFS_O_RDONLY);
    if (err < 0)
    {
        shell_printf("LFS open error: %d  path=%s\r\n", err, path);
        return -1;
    }

    /* 获取文件大小 */
    lfs_soff_t fsize = lfs_file_size(&lfs_ctx.lfs, &file);
    if (fsize < 0)
    {
        lfs_file_close(&lfs_ctx.lfs, &file);
        shell_printf("LFS size error: %d\r\n", (int)fsize);
        return -1;
    }

    /* 提取文件名（去掉目录前缀）*/
    const char *fname = path;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            fname = p + 1;

    if (fsize > 0xFFFFFFFFLL)
    {
        lfs_file_close(&lfs_ctx.lfs, &file);
        shell_printf("File too large for Ymodem: %ld\r\n", (long)fsize);
        return -1;
    }

    shell_printf("Sending: %s  (%ld bytes)\r\n", fname, (long)fsize);
    shell_printf("Start Ymodem receiver on host side...\r\n");

    ymodem_send_cfg_t send_cfg;
    memset(&send_cfg, 0, sizeof(send_cfg));
    send_cfg.filename      = fname;
    send_cfg.filesize      = (uint32_t)fsize;
    send_cfg.read_cb       = ymodem_read_cb;
    send_cfg.read_user_ctx = &file;

    int sent = Proto_Start_Send(&send_cfg);
    lfs_file_close(&lfs_ctx.lfs, &file);

    if (sent < 0)
    {
        shell_printf("Ymodem send failed.\r\n");
        return -1;
    }

    shell_printf("Send complete: %d bytes\r\n", sent);
    return 0;
}

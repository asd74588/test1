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

extern lfs_ctx_t lfs_ctx;   /* LittleFS 实例，由 main.c 挂载 */

/* ================================================================
 * Ymodem 接收（rz）
 * ================================================================ */

/* write_cb：Proto_Start_Receive 每收到一块数据调用一次 */
typedef struct {
    lfs_file_t  file;
    int         opened;
    char        path[64];
} ymodem_rx_ctx_t;

static int ymodem_write_cb(const uint8_t *data, uint32_t len, void *user)
{
    ymodem_rx_ctx_t *ctx = (ymodem_rx_ctx_t *)user;
    lfs_ssize_t written = lfs_file_write(&lfs_ctx.lfs, &ctx->file, data, (lfs_size_t)len);
    if (written < 0 || (uint32_t)written != len) {
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
    if (path_from_arg) {
        strncpy(rx_ctx.path, argv[1], sizeof(rx_ctx.path) - 1);
    }

    shell_printf("Waiting for Ymodem transfer (send file now)...\r\n");

    /* 握手完成、文件名包解析完毕后才知道文件名，
     * 所以先用临时路径打开，传输完后 rename。
     * 若调用方已给 path 则直接用该路径。           */
    const char *open_path = path_from_arg
                            ? rx_ctx.path
                            : "/tmp_ymodem_rx";

    int lfs_err = lfs_file_open(&lfs_ctx.lfs, &rx_ctx.file, open_path,
                                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (lfs_err < 0) {
        shell_printf("LFS open error: %d  path=%s\r\n", lfs_err, open_path);
        return -1;
    }
    rx_ctx.opened = 1;

    int result = Proto_Start_Receive(&cfg, &file_info);

    lfs_file_close(&lfs_ctx.lfs, &rx_ctx.file);
    rx_ctx.opened = 0;

    if (result < 0) {
        lfs_remove(&lfs_ctx.lfs, open_path);
        shell_printf("Ymodem receive failed.\r\n");
        return -1;
    }

    /* 若路径来自协议头，rename 临时文件 */
    if (!path_from_arg && file_info.filename[0] != '\0') {
        /* 构造目标路径 */
        char dst[72];
        snprintf(dst, sizeof(dst), "/%s", file_info.filename);

        lfs_rename(&lfs_ctx.lfs, open_path, dst);
        shell_printf("Received: %s  (%d bytes)\r\n", dst, result);
    } else {
        shell_printf("Received: %s  (%d bytes)\r\n", open_path, result);
    }

    return 0;
}

/* ================================================================
 * Ymodem 发送（sz）
 * ================================================================ */

/* ---- 协议常量（与接收侧保持一致） ---------------------------- */
#define YM_SOH   0x01
#define YM_STX   0x02
#define YM_EOT   0x04
#define YM_ACK   0x06
#define YM_NAK   0x15
#define YM_CAN   0x18
#define YM_C     0x43

#define YM_PKT_128   128
#define YM_PKT_1K    1024
#define YM_RETRIES   10
#define YM_TIMEOUT   3000   /* ms */

extern UART_HandleTypeDef huart1;

static void ym_send_raw(const uint8_t *buf, uint16_t len)
{
    HAL_UART_Transmit(&huart1, buf, len, 5000U);
}

static int ym_recv_byte(uint8_t *b, uint32_t timeout_ms)
{
    return (HAL_UART_Receive(&huart1, b, 1U, timeout_ms) == HAL_OK) ? 0 : -1;
}

static uint16_t ym_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0U;
    for (uint32_t i = 0U; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U)
                                  : (uint16_t)(crc << 1);
    }
    return crc;
}

/**
 * 构造并发送一个 Ymodem 包（128 B 或 1K）
 *
 * @param blk       块号（0 = 文件名包）
 * @param data      数据指针
 * @param data_len  实际有效字节（< pkt_sz 时用 0x1A 填充）
 * @param pkt_sz    YM_PKT_128 或 YM_PKT_1K
 * @return 0=ACK, -1=失败/取消
 */
static int ym_send_packet(uint8_t blk, const uint8_t *data,
                          uint32_t data_len, uint32_t pkt_sz)
{
    /* 组包：SOH/STX + blk + ~blk + data[pkt_sz] + CRC_H + CRC_L */
    static uint8_t pkt[3 + YM_PKT_1K + 2];
    memset(pkt, 0, sizeof(pkt));

    pkt[0] = (pkt_sz == YM_PKT_1K) ? YM_STX : YM_SOH;
    pkt[1] = blk;
    pkt[2] = (uint8_t)(~blk);

    if (data && data_len > 0U) {
        uint32_t copy = (data_len < pkt_sz) ? data_len : pkt_sz;
        memcpy(&pkt[3], data, copy);
        /* 末尾填充 0x1A（Ctrl-Z）*/
        if (copy < pkt_sz)
            memset(&pkt[3 + copy], 0x1A, pkt_sz - copy);
    }

    uint16_t crc = ym_crc16(&pkt[3], pkt_sz);
    pkt[3 + pkt_sz]     = (uint8_t)(crc >> 8);
    pkt[3 + pkt_sz + 1] = (uint8_t)(crc & 0xFFU);

    uint16_t total_len = (uint16_t)(3U + pkt_sz + 2U);

    for (int retry = 0; retry < YM_RETRIES; retry++) {
        ym_send_raw(pkt, total_len);

        uint8_t resp;
        if (ym_recv_byte(&resp, YM_TIMEOUT) < 0) continue;

        if (resp == YM_ACK)  return 0;
        if (resp == YM_CAN)  return -1;
        /* NAK：重发 */
    }
    return -1;
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
    if (argc < 2) {
        shell_printf("Usage: sz <path>\r\n");
        return -1;
    }

    const char *path = argv[1];

    /* ── 打开文件 ─────────────────────────────────────────── */
    lfs_file_t file;
    int err = lfs_file_open(&lfs_ctx.lfs, &file, path, LFS_O_RDONLY);
    if (err < 0) {
        shell_printf("LFS open error: %d  path=%s\r\n", err, path);
        return -1;
    }

    /* 获取文件大小 */
    lfs_soff_t fsize = lfs_file_size(&lfs_ctx.lfs, &file);
    if (fsize < 0) {
        lfs_file_close(&lfs_ctx.lfs, &file);
        shell_printf("LFS size error: %d\r\n", (int)fsize);
        return -1;
    }

    /* 提取文件名（去掉目录前缀）*/
    const char *fname = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') fname = p + 1;

    shell_printf("Sending: %s  (%ld bytes)\r\n", fname, (long)fsize);
    shell_printf("Start Ymodem receiver on host side...\r\n");

    /* ── 等待接收方首个 'C' ───────────────────────────────── */
    uint8_t c;
    int got_c = 0;
    for (int i = 0; i < YM_RETRIES; i++) {
        if (ym_recv_byte(&c, YM_TIMEOUT) == 0 && c == YM_C) {
            got_c = 1; break;
        }
    }
    if (!got_c) {
        lfs_file_close(&lfs_ctx.lfs, &file);
        shell_printf("Timeout: no 'C' from receiver.\r\n");
        return -1;
    }

    /* ── 发送文件名包（块号 0）────────────────────────────── */
    uint8_t hdr[YM_PKT_128];
    memset(hdr, 0, sizeof(hdr));
    /* 格式：filename\0size_decimal\0 */
    int hlen = snprintf((char *)hdr, sizeof(hdr),
                        "%s", fname);
    snprintf((char *)hdr + hlen + 1,
             sizeof(hdr) - (uint32_t)hlen - 1U,
             "%ld", (long)fsize);

    if (ym_send_packet(0, hdr, sizeof(hdr), YM_PKT_128) < 0) {
        lfs_file_close(&lfs_ctx.lfs, &file);
        shell_printf("Header packet rejected.\r\n");
        return -1;
    }

    /* 接收方在 ACK 文件名包后再发一个 'C' 才开始数据 */
    got_c = 0;
    for (int i = 0; i < YM_RETRIES; i++) {
        if (ym_recv_byte(&c, YM_TIMEOUT) == 0 && c == YM_C) {
            got_c = 1; break;
        }
    }
    if (!got_c) {
        lfs_file_close(&lfs_ctx.lfs, &file);
        shell_printf("No 'C' after header ACK.\r\n");
        return -1;
    }

    /* ── 发送数据包（块号从 1 开始）──────────────────────── */
    static uint8_t data_buf[YM_PKT_1K];
    uint8_t blk       = 1U;
    int     total_sent = 0;
    int     ret        = 0;

    while (1) {
        lfs_ssize_t nread = lfs_file_read(&lfs_ctx.lfs, &file,
                                          data_buf, YM_PKT_1K);
        if (nread < 0) {
            shell_printf("LFS read error: %d\r\n", (int)nread);
            ret = -1;
            break;
        }
        if (nread == 0) break;   /* 文件读完 */

        uint32_t pkt_sz = ((uint32_t)nread > YM_PKT_128)
                          ? YM_PKT_1K : YM_PKT_128;

        if (ym_send_packet(blk, data_buf, (uint32_t)nread, pkt_sz) < 0) {
            shell_printf("Packet %u rejected / cancelled.\r\n", blk);
            ret = -1;
            break;
        }
        total_sent += (int)nread;
        blk++;
    }

    lfs_file_close(&lfs_ctx.lfs, &file);

    if (ret < 0) return -1;

    /* ── EOT 握手 ─────────────────────────────────────────── */
    for (int i = 0; i < YM_RETRIES; i++) {
        uint8_t eot = YM_EOT;
        ym_send_raw(&eot, 1U);
        if (ym_recv_byte(&c, YM_TIMEOUT) == 0 && c == YM_ACK) break;
    }

    /* ── 发送空文件名包（Ymodem 结束标志）────────────────── */
    /* 接收方先发 'C' */
    for (int i = 0; i < YM_RETRIES; i++) {
        if (ym_recv_byte(&c, YM_TIMEOUT) == 0 && c == YM_C) break;
    }
    uint8_t empty[YM_PKT_128];
    memset(empty, 0, sizeof(empty));
    ym_send_packet(0, empty, sizeof(empty), YM_PKT_128);

    shell_printf("Send complete: %d bytes\r\n", total_sent);
    return 0;
}



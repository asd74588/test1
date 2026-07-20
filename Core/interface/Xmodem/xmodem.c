/**
 * @file  xmodem_ymodem.c
 * @brief 统一 Xmodem / Xmodem-1K / Ymodem 接收实现
 *
 * 分层设计：
 *   L1 UART 原语      uart_flush / uart_send_byte / uart_recv
 *   L2 包收发         recv_packet_body / send_cancel
 *   L3 包校验         crc16_ccitt / pkt_crc_check / validate_packet
 *   L4 协议语义       detect_protocol / ymodem_process_header_pkt
 *                     flush_prev_buf / handle_eot / wait_next_frame
 *   L5 公共入口       Proto_Start_Receive
 */

#include "xmodem.h"
#include "usart.h"
#include "log_config.h"
#include <string.h>
#include <stdio.h>

#if LOG_XMODEM_ENABLE
#define dbg_printf(format,args...) printf(format, ##args)
#else
#define dbg_printf(format,args...) do{}while(0)
#endif

/* ============================================================
 *  常量
 * ============================================================ */
#define PKT_DATA_128      128
#define PKT_DATA_1K       1024
#define PKT_MAX_LEN       (3 + PKT_DATA_1K + 2)   /* 1029 */

#define HANDSHAKE_RETRIES 3
#define PACKET_RETRIES    10
#define TIMEOUT_HANDSHAKE 5000
#define TIMEOUT_PACKET    2000
#define TIMEOUT_BODY      2000
#define TIMEOUT_EOT_RETRY 1000
#define SEND_RETRIES      10
#define SEND_TIMEOUT      3000

/* ============================================================
 *  传输上下文
 *  所有运行时状态集中在一个结构，函数间通过指针传递，
 *  消除隐式全局耦合，也方便将来支持多路并发传输。
 * ============================================================ */
typedef struct {
    ProtoType   protocol;
    uint8_t     expected;       /* 期望下一个包的块号 */
    int         is_ymodem;

    uint8_t     pkt[PKT_MAX_LEN];   /* 当前包缓冲 */
    int         data_len;           /* 当前包数据段长度 */

    uint8_t     prev_buf[PKT_DATA_1K];  /* 前一包数据（"看前一包"策略） */
    int         prev_len;
    int         has_prev;

    int         total_recv;         /* 累计有效字节数 */
    YmodemFileInfo *file_info;      /* 外部传入，Ymodem 文件信息输出 */


    transfer_cfg_t *transfer_cfg;       /* 外部传入，数据存储回调配置 */
} TransferCtx;

/* ============================================================
 *  L1：UART 原语
 * ============================================================ */
static void uart_flush(void)
{
    UART_FlushBuffers(&huart1);
}

static void uart_send_byte(uint8_t b)
{
    HAL_UART_Transmit(&huart1, &b, 1, 0xFFFF);
}

static HAL_StatusTypeDef uart_recv(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    return HAL_UART_Receive(&huart1, buf, len, timeout_ms);
}

static void uart_send_raw(const uint8_t *buf, uint16_t len)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)buf, len, 5000U);
}

/* ============================================================
 *  L2：包收发
 * ============================================================ */

/**
 * @brief 发送取消序列（2 × CAN）
 */
static void send_cancel(void)
{
    uart_send_byte(PROTO_CAN);
    uart_send_byte(PROTO_CAN);
}

/**
 * @brief 接收包体（pkt[0] 已填充 SOH/STX）
 *        继续收 blk + ~blk + data + CRC_H + CRC_L
 * @param pkt       缓冲区首地址
 * @param data_len  [out] 数据段长度
 * @return 1=成功, 0=超时
 */
static int recv_packet_body(uint8_t *pkt, int *data_len)
{
    *data_len = (pkt[0] == PROTO_STX) ? PKT_DATA_1K : PKT_DATA_128;
    int body_len = 2 + *data_len + 2;   /* blk + ~blk + data + crc */

    if (uart_recv(&pkt[1], (uint16_t)body_len, TIMEOUT_BODY) != HAL_OK) {
        uart_flush();
        return 0;
    }
    return 1;
}

/* ============================================================
 *  L3：包校验
 * ============================================================ */

/**
 * @brief CRC16-CCITT（多项式 0x1021）
 */
static uint16_t crc16_ccitt(const uint8_t *data, int len)
{
    uint16_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}

/**
 * @brief 对完整包（pkt[0]=SOH/STX）做 CRC 校验
 * @return 1=通过, 0=失败
 */
static int pkt_crc_check(const uint8_t *pkt, int data_len)
{
    uint16_t calc = crc16_ccitt(&pkt[3], data_len);
    uint16_t recv = ((uint16_t)pkt[3 + data_len] << 8) | pkt[3 + data_len + 1];
    return (calc == recv) ? 1 : 0;
}

/**
 * @brief 构造并发送一个Ymodem包（128B或1K）
 *
 * @param blk       块号（0 = 文件名包）
 * @param data      数据指针
 * @param data_len  实际有效字节，短包用0x1A填充
 * @param pkt_sz    PKT_DATA_128 或 PKT_DATA_1K
 * @return 0=ACK, -1=失败/取消
 */
static int send_ymodem_packet(uint8_t blk, const uint8_t *data,
                              uint32_t data_len, uint32_t pkt_sz)
{
    static uint8_t pkt[PKT_MAX_LEN];

    if (pkt_sz != PKT_DATA_128 && pkt_sz != PKT_DATA_1K) {
        return -1;
    }

    memset(pkt, 0, sizeof(pkt));
    pkt[0] = (pkt_sz == PKT_DATA_1K) ? PROTO_STX : PROTO_SOH;
    pkt[1] = blk;
    pkt[2] = (uint8_t)(~blk);

    if (data != NULL && data_len > 0U) {
        uint32_t copy = (data_len < pkt_sz) ? data_len : pkt_sz;
        memcpy(&pkt[3], data, copy);
        if (copy < pkt_sz) {
            memset(&pkt[3 + copy], 0x1A, pkt_sz - copy);
        }
    }

    uint16_t crc = crc16_ccitt(&pkt[3], (int)pkt_sz);
    pkt[3 + pkt_sz]     = (uint8_t)(crc >> 8);
    pkt[3 + pkt_sz + 1] = (uint8_t)(crc & 0xFFU);

    uint16_t total_len = (uint16_t)(3U + pkt_sz + 2U);

    for (int retry = 0; retry < SEND_RETRIES; retry++) {
        uart_send_raw(pkt, total_len);

        uint8_t resp = 0U;
        if (uart_recv(&resp, 1U, SEND_TIMEOUT) != HAL_OK) {
            continue;
        }

        if (resp == PROTO_ACK) {
            return 0;
        }
        if (resp == PROTO_CAN) {
            return -1;
        }
    }

    return -1;
}

/**
 * @brief 校验当前包的块号完整性、顺序、CRC
 *
 * @return  1  = 校验通过，可写缓冲
 *          0  = 重复包（ACK 丢失重传），已补发 ACK，调用方跳过写缓冲
 *         -1  = 校验失败，已发 NAK
 */
static int validate_packet(const TransferCtx *ctx)
{
    const uint8_t *pkt = ctx->pkt;
    uint8_t blk     = pkt[1];
    uint8_t blk_inv = pkt[2];

    /* 块号补码完整性 */
    if ((uint8_t)(blk + blk_inv) != 0xFF) {
        uart_send_byte(PROTO_NAK);
        return -1;
    }

    /* 重复包：ACK 丢失导致发送方重传上一包 */
    if (blk == (uint8_t)(ctx->expected - 1)) {
        uart_send_byte(PROTO_ACK);
        return 0;
    }

    /* 乱序 */
    if (blk != ctx->expected) {
        uart_send_byte(PROTO_NAK);
        return -1;
    }

    /* CRC */
    // if (!pkt_crc_check(pkt, ctx->data_len)) {
    //     uart_send_byte(PROTO_NAK);
    //     return -1;
    // }

   if (!pkt_crc_check(pkt, ctx->data_len)) {
        dbg_printf("CRC fail at blk %d\r\n", blk);  // 加这行
        uart_send_byte(PROTO_NAK);
        return -1;
    }
    return 1;
}

/* ============================================================
 *  L4：协议语义
 * ============================================================ */

/**
 * @brief 握手：发 'C'，等待首包头字节（SOH 或 STX）
 * @param first_byte [out]
 * @return 1=成功, -1=失败
 */
static int proto_handshake(uint8_t *first_byte)
{
    int retries = HANDSHAKE_RETRIES;
    while (retries--) {
        uart_flush();
        uart_send_byte(PROTO_C);
        if (uart_recv(first_byte, 1, TIMEOUT_HANDSHAKE) == HAL_OK &&
            (*first_byte == PROTO_SOH || *first_byte == PROTO_STX))
            return 1;
    }
    return -1;
}

/**
 * @brief 根据首包内容检测协议类型，填充 ctx->protocol / is_ymodem
 *        调用前 pkt[0..2] 必须已填充（头字节 + 块号 + 反块号）
 */
static void detect_protocol(TransferCtx *ctx)
{
    if (ctx->pkt[1] == 0x00 && (uint8_t)(ctx->pkt[1] + ctx->pkt[2]) == 0xFF) {
        /* SOH/STX + 块号 0 -> Ymodem 文件名包 */
        ctx->protocol  = PROTO_YMODEM;
        ctx->is_ymodem = 1;
    } else if (ctx->pkt[0] == PROTO_SOH) {
        ctx->protocol  = PROTO_XMODEM;
        ctx->is_ymodem = 0;
    } else {
        ctx->protocol  = PROTO_XMODEM_1K;
        ctx->is_ymodem = 0;
    }

    dbg_printf("Protocol detected: %s\r\n",
           ctx->protocol == PROTO_XMODEM    ? "Xmodem (128B)"    :
           ctx->protocol == PROTO_XMODEM_1K ? "Xmodem-1K (1024B)" : "Ymodem");
}

/**
 * @brief 解析 Ymodem 文件名包的数据段
 *        格式: "filename\0size_decimal\0..."
 */
static void parse_ymodem_header(const uint8_t *data, YmodemFileInfo *info)
{
    if (!info) return;
    memset(info, 0, sizeof(*info));

    int n = 0;
    while (n < 63 && data[n] != '\0')
        info->filename[n] = (char)data[n++];
    info->filename[n] = '\0';

    if (n > 0 && data[n] == '\0') {
        const uint8_t *p = &data[n + 1];
        uint32_t size = 0;
        while (*p >= '0' && *p <= '9')
            size = size * 10 + (*p++ - '0');
        info->filesize = size;
    }
}

/**
 * @brief 处理 Ymodem 文件名包：校验 → 解析 → ACK+'C' → 等待数据首包
 *
 * 成功后 ctx->pkt 已填充第一个数据包，ctx->data_len 已更新，
 * ctx->expected 重置为 1。
 *
 * @return  1 = 成功继续
 *          0 = 空文件名包（无更多文件），调用方应返回 0
 *         -1 = 错误，调用方应返回 -1
 */
static int ymodem_process_header_pkt(TransferCtx *ctx)
{
    if (!pkt_crc_check(ctx->pkt, ctx->data_len)) {
        uart_send_byte(PROTO_NAK);
        dbg_printf("Ymodem header CRC error.\r\n");
        return -1;
    }

    /* 全零数据段 = 无更多文件（Ymodem 多文件结束标志） */
    int all_zero = 1;
    for (int i = 3; i < 3 + ctx->data_len; i++) {
        if (ctx->pkt[i] != 0) { all_zero = 0; break; }
    }
    if (all_zero) {
        uart_send_byte(PROTO_ACK);
        dbg_printf("Ymodem: no more files.\r\n");
        return 0;
    }

    parse_ymodem_header(&ctx->pkt[3], ctx->file_info);
    if (ctx->file_info)
        dbg_printf("Ymodem file: \"%s\", size: %lu bytes\r\n",
               ctx->file_info->filename,
               (unsigned long)ctx->file_info->filesize);

    /* ACK 文件名包，再发 'C' 请求数据 */
    uart_send_byte(PROTO_ACK);
    uart_send_byte(PROTO_C);

    /* 等待第一个数据包 */
    if (uart_recv(&ctx->pkt[0], 1, TIMEOUT_HANDSHAKE) != HAL_OK ||
        (ctx->pkt[0] != PROTO_SOH && ctx->pkt[0] != PROTO_STX)) {
        dbg_printf("Ymodem: timeout waiting for first data packet.\r\n");
        return -1;
    }
    if (!recv_packet_body(ctx->pkt, &ctx->data_len)) {
        uart_send_byte(PROTO_NAK);
        return -1;
    }

    ctx->expected = 1;
    return 1;
}

/**
 * @brief 写回调包装：失败时自动发送 CAN
 * @return 0=成功, -1=写失败
 */
static int write_to_storage(transfer_cfg_t *transfer_cfg, const uint8_t *data, size_t len)
{
    dbg_printf("write_to_storage\r\n");
    if (!transfer_cfg->write_cb) return 0;

    dbg_printf("write_to_storage\r\n");
    if (transfer_cfg->write_cb((const void *)data, len, transfer_cfg->write_user_ctx) != 0) {
        send_cancel();
        dbg_printf("Flash write error, transfer cancelled.\r\n");
        return -1;
    }
    return 0;
}

/**
 * @brief 将前一包缓冲写入 Flash（中间包，非最后包）
 *        调用后更新 ctx->total_recv
 * @return 0=成功, -1=写失败
 */
static int flush_prev_buf(TransferCtx *ctx)
{
    dbg_printf("flush_prev_buf\r\n");
    if (!ctx->has_prev) return 0;

    dbg_printf("flush_prev_buf\r\n");
    if (write_to_storage(ctx->transfer_cfg, ctx->prev_buf, (size_t)ctx->prev_len) < 0)
        return -1;
    ctx->total_recv += ctx->prev_len;
    return 0;
}

/**
 * @brief 处理最后一包（收到 EOT 时调用）
 *
 * Ymodem 有明确文件长度，只写有效字节。Xmodem 没有长度字段，
 * 因此保留完整末包；上层若有自己的文件头，可据此恢复以 0x1A
 * 结尾的有效数据，再按逻辑长度裁剪。
 * @return 0=成功, -1=写失败
 */
static int flush_last_buf(TransferCtx *ctx)
{
    if (!ctx->has_prev) return 0;

    int valid = ctx->prev_len;
    int write_len = ctx->prev_len;

    if (ctx->file_info && ctx->file_info->filesize > 0) {
        uint32_t total = (uint32_t)ctx->total_recv;
        uint32_t remaining;

        if (ctx->file_info->filesize <= total) {
            return -1;
        }

        remaining = ctx->file_info->filesize - total;
        if (remaining > (uint32_t)ctx->prev_len) {
            return -1;
        }

        valid = (int)remaining;
        write_len = valid;
    } else {
        /* 返回推断的有效长度，但原样保存末包供上层按包头精确裁剪。 */
        while (valid > 0 && (uint8_t)ctx->prev_buf[valid - 1] == 0x1A)
            valid--;
    }

    if (write_len > 0 &&
        write_to_storage(ctx->transfer_cfg, ctx->prev_buf, (size_t)write_len) < 0)
        return -1;
    ctx->total_recv += valid;
    return 0;
}

/**
 * @brief 处理 EOT：ACK → 写最后一包 → 等待可能的 EOT 重传
 *                  → Ymodem 额外接收尾包（全零文件名包）
 * @return 传输总字节数（>=0），失败返回 -1
 */
static int handle_eot(TransferCtx *ctx)
{
    uart_send_byte(PROTO_ACK);

    if (flush_last_buf(ctx) < 0)
        return -1;

    /* 等待可能的 EOT 重传并再次确认 */
    uint8_t tmp;
    if (uart_recv(&tmp, 1, TIMEOUT_EOT_RETRY) == HAL_OK && tmp == PROTO_EOT)
        uart_send_byte(PROTO_ACK);

    /* Ymodem：接收结束尾包（全零文件名包） */
    if (ctx->is_ymodem) {
        uart_send_byte(PROTO_C);

        int tail_dlen  = 0;
        int ymodem_ok  = 0;
        if (uart_recv(&ctx->pkt[0], 1, TIMEOUT_HANDSHAKE) == HAL_OK &&
            (ctx->pkt[0] == PROTO_SOH || ctx->pkt[0] == PROTO_STX) &&
            recv_packet_body(ctx->pkt, &tail_dlen) &&
            pkt_crc_check(ctx->pkt, tail_dlen))
        {
            uart_send_byte(PROTO_ACK);
            ymodem_ok = 1;
        }
        if (!ymodem_ok)
            dbg_printf("Warning: Ymodem tail packet not received.\r\n");
    }

    dbg_printf("Transfer complete. Protocol=%s, Written=%d bytes.\r\n",
           ctx->protocol == PROTO_XMODEM    ? "Xmodem"    :
           ctx->protocol == PROTO_XMODEM_1K ? "Xmodem-1K" : "Ymodem",
           ctx->total_recv);

    return ctx->total_recv;
}

/**
 * @brief 等待下一帧（首字节），处理 EOT / CAN / 超时重试
 *
 * @return  WAIT_FRAME_DATA  (1)  收到完整 SOH/STX 包，可继续处理
 *          WAIT_FRAME_EOT   (2)  收到 EOT，调用方调用 handle_eot()
 *          WAIT_FRAME_ERROR (-1) 超时重试耗尽 / 对端取消
 */
#define WAIT_FRAME_DATA   1
#define WAIT_FRAME_EOT    2
#define WAIT_FRAME_ERROR  (-1)

static int wait_next_frame(TransferCtx *ctx)
{
    memset(ctx->pkt, 0, sizeof(ctx->pkt));
    int retries = PACKET_RETRIES;

    while (retries--) {
        if (uart_recv(&ctx->pkt[0], 1, TIMEOUT_PACKET) != HAL_OK) {
            uart_flush();
            uart_send_byte(PROTO_NAK);
            continue;
        }

        switch (ctx->pkt[0]) {

        case PROTO_EOT:
            return WAIT_FRAME_EOT;

        case PROTO_CAN: {
            uint8_t second;
            if (uart_recv(&second, 1, 1000) == HAL_OK && second == PROTO_CAN) {
                send_cancel();
                dbg_printf("Transfer cancelled by sender.\r\n");
                return WAIT_FRAME_ERROR;
            }
            /* 单个 CAN 视为线路噪声，继续重试 */
            continue;
        }

        case PROTO_SOH:
        case PROTO_STX:
            if (recv_packet_body(ctx->pkt, &ctx->data_len))
                return WAIT_FRAME_DATA;
            uart_send_byte(PROTO_NAK);
            break;

        default:
            /* 未知字节，噪声，忽略 */
            break;
        }
    }

    send_cancel();
    dbg_printf("Too many retries, transfer aborted.\r\n");
    return WAIT_FRAME_ERROR;
}

/* ============================================================
 *  L5：公共入口
 * ============================================================ */
int Proto_Start_Receive(transfer_cfg_t *transfer_cfg, YmodemFileInfo *file_info)
{

    TransferCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.expected  = 1;
    ctx.file_info = file_info;

    ctx.transfer_cfg = transfer_cfg;

    /* ── 阶段 1：握手 ──────────────────────────────────────── */
    uart_flush();
    if (proto_handshake(&ctx.pkt[0]) < 0) {
        dbg_printf("Handshake failed.\r\n");
        return -1;
    }

    /* ── 阶段 2：接收首包包体 ──────────────────────────────── */
    if (!recv_packet_body(ctx.pkt, &ctx.data_len)) {
        uart_send_byte(PROTO_NAK);
        dbg_printf("Timeout on first packet body.\r\n");
        return -1;
    }

    /* ── 阶段 3：协议检测 ──────────────────────────────────── */
    detect_protocol(&ctx);

    /* ── 阶段 4：Ymodem 特有——处理文件名首包 ───────────────── */
    if (ctx.is_ymodem) {
        int ret = ymodem_process_header_pkt(&ctx);
        if (ret <= 0) return ret;   /* 0=无更多文件, -1=错误 */
    }

    /* ── 阶段 5：主循环接收数据包 ──────────────────────────── */
    while (1) {
        int vret = validate_packet(&ctx);

        if (vret == 1) {
            /* 校验通过：写前一包，缓冲当前包 */
            if (flush_prev_buf(&ctx) < 0)
                return -1;
            memcpy(ctx.prev_buf, &ctx.pkt[3], (size_t)ctx.data_len);
            ctx.prev_len = ctx.data_len;
            ctx.has_prev = 1;
            uart_send_byte(PROTO_ACK);
            ctx.expected++;
        }
        /* vret == 0: 重复包，validate_packet 已补发 ACK，直接等下一帧 */
        /* vret ==-1: 校验失败，validate_packet 已发 NAK，直接等下一帧 */

        /* 等待下一帧 */
        int fret = wait_next_frame(&ctx);
        if (fret == WAIT_FRAME_EOT)
            return handle_eot(&ctx);
        if (fret == WAIT_FRAME_ERROR)
            return -1;
        /* fret == WAIT_FRAME_DATA：ctx.pkt 已填充，继续循环 */
    }
}

int Proto_Start_Send(const ymodem_send_cfg_t *send_cfg)
{
    if (send_cfg == NULL || send_cfg->filename == NULL ||
        send_cfg->read_cb == NULL) {
        return -1;
    }

    uint8_t c = 0U;
    int got_c = 0;

    /* 等待接收方发'C'，进入CRC模式 */
    for (int i = 0; i < SEND_RETRIES; i++) {
        if (uart_recv(&c, 1U, SEND_TIMEOUT) == HAL_OK && c == PROTO_C) {
            got_c = 1;
            break;
        }
    }
    if (!got_c) {
        dbg_printf("Ymodem send timeout: no 'C' from receiver.\r\n");
        return -1;
    }

    /* 发送文件名包：filename\0size_decimal\0 */
    uint8_t hdr[PKT_DATA_128];
    memset(hdr, 0, sizeof(hdr));

    int hlen = snprintf((char *)hdr, sizeof(hdr), "%s", send_cfg->filename);
    if (hlen < 0 || hlen >= (int)sizeof(hdr)) {
        return -1;
    }
    snprintf((char *)hdr + hlen + 1,
             sizeof(hdr) - (uint32_t)hlen - 1U,
             "%lu", (unsigned long)send_cfg->filesize);

    if (send_ymodem_packet(0U, hdr, sizeof(hdr), PKT_DATA_128) < 0) {
        dbg_printf("Ymodem send: header rejected.\r\n");
        return -1;
    }

    /* 接收方ACK文件名包后，会再发一个'C'请求数据 */
    got_c = 0;
    for (int i = 0; i < SEND_RETRIES; i++) {
        if (uart_recv(&c, 1U, SEND_TIMEOUT) == HAL_OK && c == PROTO_C) {
            got_c = 1;
            break;
        }
    }
    if (!got_c) {
        dbg_printf("Ymodem send: no 'C' after header ACK.\r\n");
        return -1;
    }

    static uint8_t data_buf[PKT_DATA_1K];
    uint8_t  blk = 1U;
    uint32_t total_sent = 0U;

    while (1) {
        uint32_t nread = 0U;
        if (send_cfg->read_cb(data_buf, PKT_DATA_1K, &nread,
                              send_cfg->read_user_ctx) != 0) {
            send_cancel();
            dbg_printf("Ymodem send: read callback failed.\r\n");
            return -1;
        }

        if (nread == 0U) {
            break;
        }

        uint32_t pkt_sz = (nread > PKT_DATA_128) ? PKT_DATA_1K : PKT_DATA_128;
        if (send_ymodem_packet(blk, data_buf, nread, pkt_sz) < 0) {
            dbg_printf("Ymodem send: packet %u rejected/cancelled.\r\n", blk);
            return -1;
        }

        total_sent += nread;
        blk++;
    }

    /* EOT握手 */
    got_c = 0;
    int eot_acked = 0;
    for (int i = 0; i < SEND_RETRIES; i++) {
        uint8_t eot = PROTO_EOT;
        uart_send_byte(eot);

        if (uart_recv(&c, 1U, SEND_TIMEOUT) == HAL_OK) {
            if (c == PROTO_ACK) {
                eot_acked = 1;
                break;
            }
            if (c == PROTO_NAK) {
                continue;
            }
            if (c == PROTO_CAN) {
                return -1;
            }
        }
    }
    if (!eot_acked) {
        dbg_printf("Ymodem send: EOT not acknowledged.\r\n");
        return -1;
    }

    /* 结束批量传输：等待'C'后发送空文件名包 */
    got_c = 0;
    for (int i = 0; i < SEND_RETRIES; i++) {
        if (uart_recv(&c, 1U, SEND_TIMEOUT) == HAL_OK && c == PROTO_C) {
            got_c = 1;
            break;
        }
    }
    if (!got_c) {
        dbg_printf("Ymodem send: no 'C' before tail packet.\r\n");
        return -1;
    }

    uint8_t empty[PKT_DATA_128];
    memset(empty, 0, sizeof(empty));
    if (send_ymodem_packet(0U, empty, sizeof(empty), PKT_DATA_128) < 0) {
        dbg_printf("Ymodem send: tail packet rejected.\r\n");
        return -1;
    }

    return (int)total_sent;
}


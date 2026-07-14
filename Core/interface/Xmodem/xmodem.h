#ifndef XMODEM_YMODEM_H
#define XMODEM_YMODEM_H

#include <stdint.h>
#include <stddef.h>
#include "uart_bootloader.h"
/* ============================================================
 *  协议控制字节
 * ============================================================ */
#define PROTO_SOH   0x01    /* Xmodem 128字节包头 */
#define PROTO_STX   0x02    /* Xmodem-1K / Ymodem 1024字节包头 */
#define PROTO_EOT   0x04
#define PROTO_ACK   0x06
#define PROTO_NAK   0x15
#define PROTO_CAN   0x18
#define PROTO_C     0x43    /* 'C'  启动CRC模式 */

/* ============================================================
 *  协议类型
 * ============================================================ */
typedef enum {
    PROTO_XMODEM     = 0,   /* 标准 Xmodem,  128字节/包 */
    PROTO_XMODEM_1K  = 1,   /* Xmodem-1K,   1024字节/包 */
    PROTO_YMODEM     = 2,   /* Ymodem,      1024字节/包 + 文件名首包 */
} ProtoType;

/* ============================================================
 *  Ymodem 文件信息（握手成功后填充）
 * ============================================================ */
typedef struct {
    char     filename[64];  /* 文件名 */
    uint32_t filesize;      /* 文件大小，0 = 未知 */
} YmodemFileInfo;

/* ============================================================
 *  回调：数据存储回调
 *  参数: buf      数据缓冲区
 *        len      有效字节数（最后一包可能 < 块大小）
 *        user_ctx  用户上下文指针（Proto_Start_Receive 的 storage_cfg 参数）
 *  返回: 0=成功, 非0=失败（失败将导致传输取消）
 * ============================================================ */
typedef int (*storage_callback_t)(const uint8_t *buf, uint32_t len, void *user_ctx);
typedef int (*receive_callback_t)(void *user_ctx, void *file_info_out);
typedef int (*read_callback_t)(uint8_t *buf, uint32_t max_len,
                               uint32_t *out_len, void *user_ctx);

/* ============================================================
 *  数据存储的回调接口定义
 *  由上层业务实现，Xmodem协议层调用
 * ============================================================ */
typedef struct {
    storage_callback_t  write_cb;
    void                *write_user_ctx;  // 存储回调的私有数据，不关心类型

    receive_callback_t receive_cb;  
    void               *recv_user_ctx; // 接收回调的私有数据不关心类型   
} transfer_cfg_t;

/* ============================================================
 *  Ymodem发送配置
 *  由上层业务提供文件名、文件大小和读数据回调，协议层负责组包发送
 * ============================================================ */
typedef struct {
    const char      *filename;
    uint32_t         filesize;
    read_callback_t  read_cb;
    void            *read_user_ctx;
} ymodem_send_cfg_t;


/* ============================================================
 *  公共 API
 * ============================================================ */

/**
 * @brief 启动接收传输（自动检测协议）
 *
 * @param transfer_cfg   传输配置指针，包含写回调和用户上下文
 * @param file_info   若为 Ymodem，握手后填充文件名/大小；其他协议忽略，可传 NULL
 * @return            实际写入字节数，失败返回 -1
 */
int Proto_Start_Receive(transfer_cfg_t *transfer_cfg, YmodemFileInfo *file_info);

/**
 * @brief 启动Ymodem发送
 *
 * @param send_cfg  发送配置指针，包含文件名、大小和读回调
 * @return          实际发送字节数，失败返回 -1
 */
int Proto_Start_Send(const ymodem_send_cfg_t *send_cfg);

#endif /* XMODEM_YMODEM_H */


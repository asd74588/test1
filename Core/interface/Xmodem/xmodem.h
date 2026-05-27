#ifndef XMODEM_YMODEM_H
#define XMODEM_YMODEM_H

#include <stdint.h>
#include <stddef.h>

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
 *  回调：写 NorFlash
 *  参数: buf      数据缓冲区
 *        len      有效字节数（最后一包可能 < 块大小）
 *  返回: 0=成功, 非0=失败（失败将导致传输取消）
 * ============================================================ */
typedef int (*Write_Flash_Callback)(const void *buf, size_t len);

/* ============================================================
 *  公共 API
 * ============================================================ */

/**
 * @brief 注册 Flash 写回调
 */
void Proto_Register_Write_Callback(Write_Flash_Callback cb);

/**
 * @brief 启动接收传输（自动检测协议）
 *
 * @param start_addr  Flash 写入起始地址（保留，供回调内部使用）
 * @param file_info   若为 Ymodem，握手后填充文件名/大小；其他协议忽略，可传 NULL
 * @return            实际写入字节数，失败返回 -1
 */
int Proto_Start_Receive(uint32_t start_addr, YmodemFileInfo *file_info);

#endif /* XMODEM_YMODEM_H */


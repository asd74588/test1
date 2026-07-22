#ifndef __SHA_H__
#define __SHA_H__

#include "stdint.h"
#include "main.h"
#include "global.h"

typedef enum
{
    FIRMWARE_VERIFY_OK            = 0,
    FIRMWARE_VERIFY_ERR_PARAM     = -1,
    FIRMWARE_VERIFY_ERR_OPEN      = -2,
    FIRMWARE_VERIFY_ERR_HEADER    = -3,
    FIRMWARE_VERIFY_ERR_MAGIC     = -4,
    FIRMWARE_VERIFY_ERR_SEEK      = -5,
    FIRMWARE_VERIFY_ERR_READ      = -6,
    FIRMWARE_VERIFY_ERR_SHA256    = -7,
    FIRMWARE_VERIFY_ERR_SIGNATURE = -8,
    FIRMWARE_VERIFY_ERR_SIZE      = -9,
    FIRMWARE_VERIFY_ERR_ALGORITHM = -10,
} firmware_verify_status_t;

firmware_verify_status_t verify_firmware(const char *path);
firmware_verify_status_t verify_lfs_file_sha256(lfs_ctx_t  *fs,
                                                const char *path,
                                                const char *expected_hex,
                                                uint32_t    expected_size,
                                                char       *calc_hex,
                                                uint16_t    calc_hex_size);
#endif

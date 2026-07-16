#ifndef __SHA_H__
#define __SHA_H__

#include "stdint.h"
#include "main.h"

typedef enum {
    FIRMWARE_VERIFY_OK             = 0,
    FIRMWARE_VERIFY_ERR_PARAM      = -1,
    FIRMWARE_VERIFY_ERR_OPEN       = -2,
    FIRMWARE_VERIFY_ERR_HEADER     = -3,
    FIRMWARE_VERIFY_ERR_MAGIC      = -4,
    FIRMWARE_VERIFY_ERR_SEEK       = -5,
    FIRMWARE_VERIFY_ERR_READ       = -6,
    FIRMWARE_VERIFY_ERR_SHA256     = -7,
    FIRMWARE_VERIFY_ERR_SIGNATURE  = -8,
} firmware_verify_status_t;

firmware_verify_status_t verify_firmware(const char *path);
#endif 



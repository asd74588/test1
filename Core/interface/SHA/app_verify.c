#include "cmox_crypto.h"
#include "hash/cmox_hash.h"
#include "global.h"
#include "fw_pubkey.h"
#include "app_verify.h"
#include "log_config.h"

#include "cmox_ecc.h"
#include "cmox_ecc_types.h"
#include "cmox_ecdsa.h"
#include <stdio.h>
#include <string.h>
extern lfs_ctx_t                    lfs_ctx;
extern const struct lfs_file_config lfs_file_cfg;

#define BL_PREFIX "[boot] "
#if LOG_APP_VERIFY_ENABLE
#define BL_INFO(fmt, ...) printf(BL_PREFIX fmt "\r\n", ##__VA_ARGS__)
#define BL_ERR(fmt, ...)  printf(BL_PREFIX "ERROR: " fmt "\r\n", ##__VA_ARGS__)
#define BL_WARN(fmt, ...) printf(BL_PREFIX "WARN: " fmt "\r\n", ##__VA_ARGS__)
#else
#define BL_INFO(...) ((void)0)
#define BL_ERR(...)  ((void)0)
#define BL_WARN(...) ((void)0)
#endif

#if LOG_APP_VERIFY_TRACE_ENABLE
#define VERIFY_TRACE(...) printf(__VA_ARGS__)
#else
#define VERIFY_TRACE(...) ((void)0)
#endif

static int hex_char_to_value(char ch)
{
    if (ch >= '0' && ch <= '9')
    {
        return ch - '0';
    }

    if (ch >= 'a' && ch <= 'f')
    {
        return ch - 'a' + 10;
    }

    if (ch >= 'A' && ch <= 'F')
    {
        return ch - 'A' + 10;
    }

    return -1;
}

static int sha256_hex_to_bytes(const char *hex, uint8_t *out)
{
    uint16_t i;

    if (hex == NULL || out == NULL || strlen(hex) != 64U)
    {
        return -1;
    }

    for (i = 0U; i < 32U; i++)
    {
        int high = hex_char_to_value(hex[i * 2U]);
        int low  = hex_char_to_value(hex[i * 2U + 1U]);

        if (high < 0 || low < 0)
        {
            return -2;
        }

        out[i] = (uint8_t)((high << 4) | low);
    }

    return 0;
}

static void sha256_bytes_to_hex(const uint8_t *hash, char *hex, uint16_t hex_size)
{
    static const char digits[] = "0123456789abcdef";
    uint16_t          i;

    if (hash == NULL || hex == NULL || hex_size < 65U)
    {
        return;
    }

    for (i = 0U; i < 32U; i++)
    {
        hex[i * 2U]      = digits[(hash[i] >> 4) & 0x0FU];
        hex[i * 2U + 1U] = digits[hash[i] & 0x0FU];
    }
    hex[64] = '\0';
}

firmware_verify_status_t verify_lfs_file_sha256(lfs_ctx_t  *fs,
                                                const char *path,
                                                const char *expected_hex,
                                                uint32_t    expected_size,
                                                char       *calc_hex,
                                                uint16_t    calc_hex_size)
{
    static lfs_file_t           file;
    static uint8_t              expected_hash[32];
    static uint8_t              calc_hash[32];
    static uint8_t              buf[512];
    lfs_soff_t                  file_size;
    size_t                      hash_len = 0U;
    static cmox_sha256_handle_t ctx;
    uint32_t                    total = 0U;
    uint32_t                    remaining;
    int                         err;

    if (fs == NULL || path == NULL || expected_hex == NULL || fs->mounted == 0U)
    {
        return FIRMWARE_VERIFY_ERR_PARAM;
    }

    if (sha256_hex_to_bytes(expected_hex, expected_hash) != 0)
    {
        return FIRMWARE_VERIFY_ERR_SHA256;
    }

    memset(&file, 0, sizeof(file));
    err = lfs_file_opencfg(&fs->lfs, &file, path, LFS_O_RDONLY, &lfs_file_cfg);
    if (err < 0)
    {
        return FIRMWARE_VERIFY_ERR_OPEN;
    }

    file_size = lfs_file_size(&fs->lfs, &file);
    if (file_size < 0)
    {
        lfs_file_close(&fs->lfs, &file);
        return FIRMWARE_VERIFY_ERR_READ;
    }

    if (expected_size == 0U || file_size != (lfs_soff_t)expected_size)
    {
        lfs_file_close(&fs->lfs, &file);
        return FIRMWARE_VERIFY_ERR_SIZE;
    }

    memset(&ctx, 0, sizeof(ctx));
    cmox_sha256_construct(&ctx);
    cmox_hash_init((cmox_hash_handle_t *)&ctx);
    cmox_hash_setTagLen((cmox_hash_handle_t *)&ctx, 32);

    remaining = expected_size;
    while (remaining > 0U)
    {
        lfs_size_t request =
            (remaining < (uint32_t)sizeof(buf)) ? (lfs_size_t)remaining : (lfs_size_t)sizeof(buf);
        int rd = lfs_file_read(&fs->lfs, &file, buf, request);
        if (rd < 0)
        {
            cmox_hash_cleanup((cmox_hash_handle_t *)&ctx);
            lfs_file_close(&fs->lfs, &file);
            return FIRMWARE_VERIFY_ERR_READ;
        }

        if (rd == 0)
        {
            cmox_hash_cleanup((cmox_hash_handle_t *)&ctx);
            lfs_file_close(&fs->lfs, &file);
            return FIRMWARE_VERIFY_ERR_READ;
        }

        cmox_hash_append((cmox_hash_handle_t *)&ctx, buf, (size_t)rd);
        total += (uint32_t)rd;
        remaining -= (uint32_t)rd;
    }

    cmox_hash_generateTag((cmox_hash_handle_t *)&ctx, calc_hash, &hash_len);
    cmox_hash_cleanup((cmox_hash_handle_t *)&ctx);
    lfs_file_close(&fs->lfs, &file);

    if (calc_hex != NULL && calc_hex_size >= 65U)
    {
        sha256_bytes_to_hex(calc_hash, calc_hex, calc_hex_size);
    }

    if (total != expected_size || hash_len != 32U)
    {
        return FIRMWARE_VERIFY_ERR_READ;
    }

    if (memcmp(calc_hash, expected_hash, sizeof(calc_hash)) != 0)
    {
        return FIRMWARE_VERIFY_ERR_SHA256;
    }

    return FIRMWARE_VERIFY_OK;
}

static int verify_ecdsa(const Firmware_Header_t *hdr)
{
    static uint8_t           pubkey[64];
    static cmox_ecc_handle_t ecc_ctx;
    static uint32_t          ecc_buf_words[4096U / sizeof(uint32_t)];
    uint8_t                 *ecc_buf = (uint8_t *)ecc_buf_words;
    memcpy(&pubkey[0], FW_PUBKEY_X, 32);
    memcpy(&pubkey[32], FW_PUBKEY_Y, 32);

    uint32_t          fault_check = 0;
    cmox_ecc_retval_t ret;

    memset(&ecc_ctx, 0, sizeof(ecc_ctx));
    cmox_ecc_construct(&ecc_ctx, CMOX_MATH_FUNCS_SMALL, ecc_buf, sizeof(ecc_buf_words));

    ret = cmox_ecdsa_verify(&ecc_ctx,
                            CMOX_ECC_SECP256R1_LOWMEM,
                            pubkey,
                            64,
                            hdr->sha256,
                            32,
                            hdr->signature,
                            64,
                            &fault_check);

    if (ret != CMOX_ECC_AUTH_SUCCESS || fault_check != CMOX_ECC_AUTH_SUCCESS)
    {
        BL_ERR("ECDSA verify failed: ret=0x%X fault=0x%X", ret, fault_check);
        memset(ecc_buf_words, 0, sizeof(ecc_buf_words));
        return -1;
    }

    memset(ecc_buf_words, 0, sizeof(ecc_buf_words));
    BL_INFO("ECDSA OK");
    return 0;
}

firmware_verify_status_t verify_firmware(const char *path)
{
    static Firmware_Header_t    hdr;
    static uint8_t              buf[512];
    static uint8_t              calc_hash[32];
    static cmox_sha256_handle_t ctx;
    lfs_soff_t                  file_size;
    uint32_t                    package_size;
    int                         err;

    if (path == NULL)
    {
        return FIRMWARE_VERIFY_ERR_PARAM;
    }

    /* 1. 打开文件读Header */
    err = lfs_file_opencfg(&lfs_ctx.lfs, &lfs_ctx.file, path, LFS_O_RDONLY, &lfs_file_cfg);
    if (err < 0)
    {
        BL_ERR("open failed: %d", err);
        return FIRMWARE_VERIFY_ERR_OPEN;
    }

    if (lfs_file_read(&lfs_ctx.lfs, &lfs_ctx.file, &hdr, sizeof(hdr)) != sizeof(hdr))
    {
        BL_ERR("header read failed");
        err = FIRMWARE_VERIFY_ERR_HEADER;
        goto fail;
    }

    /* 2. 校验Magic */
    if (hdr.magic != 0xAABBCCDD)
    {
        BL_ERR("bad magic: 0x%08X", hdr.magic);
        err = FIRMWARE_VERIFY_ERR_MAGIC;
        goto fail;
    }

    file_size = lfs_file_size(&lfs_ctx.lfs, &lfs_ctx.file);
    if (file_size < 0)
    {
        BL_ERR("file size read failed: %d", (int)file_size);
        err = FIRMWARE_VERIFY_ERR_READ;
        goto fail;
    }

    if (hdr.size == 0U || hdr.size > ((uint32_t)LFS_FILE_MAX - (uint32_t)sizeof(Firmware_Header_t)))
    {
        BL_ERR("invalid payload size: %lu", (unsigned long)hdr.size);
        err = FIRMWARE_VERIFY_ERR_SIZE;
        goto fail;
    }

    package_size = (uint32_t)sizeof(Firmware_Header_t) + hdr.size;
    if (file_size != (lfs_soff_t)package_size)
    {
        BL_ERR("package size mismatch: header=%lu actual=%d",
               (unsigned long)package_size,
               (int)file_size);
        err = FIRMWARE_VERIFY_ERR_SIZE;
        goto fail;
    }

    VERIFY_TRACE(BL_PREFIX "Version: %lu\r\n", hdr.version);
    VERIFY_TRACE(BL_PREFIX "Size   : %lu\r\n", hdr.size);

    /* 3. 校验SHA256 */
    {
        int      rd;
        uint32_t total     = 0U;
        uint32_t remaining = hdr.size;
        size_t   hash_len  = 0U;

        if (lfs_file_seek(&lfs_ctx.lfs, &lfs_ctx.file, sizeof(Firmware_Header_t), LFS_SEEK_SET) < 0)
        {
            BL_ERR("payload seek failed");
            err = FIRMWARE_VERIFY_ERR_SEEK;
            goto fail;
        }

        memset(&ctx, 0, sizeof(ctx));
        cmox_sha256_construct(&ctx);
        cmox_hash_init((cmox_hash_handle_t *)&ctx);
        cmox_hash_setTagLen((cmox_hash_handle_t *)&ctx, 32);

        while (remaining > 0U)
        {
            lfs_size_t request = (remaining < (uint32_t)sizeof(buf)) ? (lfs_size_t)remaining
                                                                     : (lfs_size_t)sizeof(buf);
            rd                 = lfs_file_read(&lfs_ctx.lfs, &lfs_ctx.file, buf, request);
            if (rd <= 0)
            {
                BL_ERR("payload read stopped at %lu / %lu bytes (err=%d)",
                       (unsigned long)total,
                       (unsigned long)hdr.size,
                       rd);
                cmox_hash_cleanup((cmox_hash_handle_t *)&ctx);
                err = FIRMWARE_VERIFY_ERR_READ;
                goto fail;
            }

            cmox_hash_append((cmox_hash_handle_t *)&ctx, buf, (size_t)rd);
            total += (uint32_t)rd;
            remaining -= (uint32_t)rd;
        }
        cmox_hash_generateTag((cmox_hash_handle_t *)&ctx, calc_hash, &hash_len);
        cmox_hash_cleanup((cmox_hash_handle_t *)&ctx);

        VERIFY_TRACE(BL_PREFIX "Hashed %lu bytes\r\n", (unsigned long)total);

        if (memcmp(calc_hash, hdr.sha256, 32) != 0)
        {
            BL_ERR("SHA256 mismatch");
            err = FIRMWARE_VERIFY_ERR_SHA256;
            goto fail;
        }
        BL_INFO("SHA256 OK");
    }

    BL_INFO("SHA256 OK");
    /* 4. 校验ECDSA签名 */
    if (verify_ecdsa(&hdr) != 0)
    {
        BL_ERR("Signature verification failed");
        err = FIRMWARE_VERIFY_ERR_SIGNATURE;
        goto fail;
    }

    lfs_file_close(&lfs_ctx.lfs, &lfs_ctx.file);
    return FIRMWARE_VERIFY_OK;

fail:
    lfs_file_close(&lfs_ctx.lfs, &lfs_ctx.file);
    return (firmware_verify_status_t)err;
}

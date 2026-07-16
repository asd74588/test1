#include "cmox_crypto.h"
#include "hash/cmox_hash.h"
#include "global.h"
#include "fw_pubkey.h"
#include "app_verify.h"

#include "cmox_ecc.h"
#include "cmox_ecc_types.h"
#include "cmox_ecdsa.h"
extern lfs_ctx_t lfs_ctx;

#define BL_PREFIX  "[boot] "
#define BL_INFO(fmt, ...)  printf(BL_PREFIX fmt "\r\n", ##__VA_ARGS__)
#define BL_ERR(fmt, ...)   printf(BL_PREFIX "ERROR: " fmt "\r\n", ##__VA_ARGS__)
#define BL_WARN(fmt, ...)  printf(BL_PREFIX "WARN: "  fmt "\r\n", ##__VA_ARGS__)


int test_ota_sha256()
{
    uint8_t test_hash[32];
    size_t hash_len;
    cmox_sha256_handle_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    cmox_sha256_construct(&ctx);
    cmox_hash_init((cmox_hash_handle_t *)&ctx);
    cmox_hash_setTagLen((cmox_hash_handle_t *)&ctx, 32);
    // 不 append 任何数据
    cmox_hash_generateTag((cmox_hash_handle_t *)&ctx, test_hash, &hash_len);
    cmox_hash_cleanup((cmox_hash_handle_t *)&ctx);

    dbg_printf("Empty SHA256:\r\n");
    for(int i = 0; i < 32; i++) dbg_printf("%02X", test_hash[i]);
    dbg_printf("\r\n");

    return 0;
}


static int verify_ecdsa(const Firmware_Header_t *hdr)
{
    BL_INFO("SHA256 OK");
    uint8_t pubkey[64];
    memcpy(&pubkey[0],  FW_PUBKEY_X, 32);
    memcpy(&pubkey[32], FW_PUBKEY_Y, 32);

    cmox_ecc_handle_t ecc_ctx;
    static uint8_t ecc_buf[4096];  // 放入 SRAM2
    uint32_t fault_check = 0;

    BL_INFO("SHA256 OK");


    cmox_ecc_construct(&ecc_ctx, CMOX_MATH_FUNCS_FAST, ecc_buf, sizeof(ecc_buf));


    BL_INFO("SHA256 OK");
    cmox_ecc_retval_t ret = cmox_ecdsa_verify(
        &ecc_ctx,
        CMOX_ECC_SECP256R1_HIGHMEM,
        pubkey,    64,
        hdr->sha256, 32,
        hdr->signature, 64,
        &fault_check
    );


    BL_INFO("SHA256 OK");
    cmox_ecc_cleanup(&ecc_ctx);

    dbg_printf("ret=0x%X fault=0x%X\r\n", ret, fault_check);

    if (ret != CMOX_ECC_AUTH_SUCCESS || fault_check != CMOX_ECC_AUTH_SUCCESS) {
        BL_ERR("ECDSA verify failed: ret=0x%X fault=0x%X", ret, fault_check);
        return -1;
    }

    BL_INFO("ECDSA OK");
    return 0;
}

firmware_verify_status_t verify_firmware(const char *path)
{
    Firmware_Header_t hdr;
    int err;

    if (path == NULL) {
        return FIRMWARE_VERIFY_ERR_PARAM;
    }

    /* 1. 打开文件读Header */
    err = lfs_file_open(&lfs_ctx.lfs, &lfs_ctx.file, path, LFS_O_RDONLY);
    if (err < 0) {
        BL_ERR("open failed: %d", err);
        return FIRMWARE_VERIFY_ERR_OPEN;
    }

    if (lfs_file_read(&lfs_ctx.lfs, &lfs_ctx.file, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        BL_ERR("header read failed");
        err = FIRMWARE_VERIFY_ERR_HEADER;
        goto fail;
    }

    /* 2. 校验Magic */
    if (hdr.magic != 0xAABBCCDD) {
        BL_ERR("bad magic: 0x%08X", hdr.magic);
        err = FIRMWARE_VERIFY_ERR_MAGIC;
        goto fail;
    }
    BL_INFO("Version: %lu", hdr.version);
    BL_INFO("Size   : %lu", hdr.size);

    /* 3. 校验SHA256 */
    {
        uint8_t buf[512];
        int rd, total = 0;
        uint8_t calc_hash[32];
        size_t hash_len;
        cmox_sha256_handle_t ctx;

        if (lfs_file_seek(&lfs_ctx.lfs, &lfs_ctx.file,
                          sizeof(Firmware_Header_t), LFS_SEEK_SET) < 0) {
            BL_ERR("payload seek failed");
            err = FIRMWARE_VERIFY_ERR_SEEK;
            goto fail;
        }

        cmox_sha256_construct(&ctx);
        cmox_hash_init((cmox_hash_handle_t *)&ctx);
        cmox_hash_setTagLen((cmox_hash_handle_t *)&ctx, 32);

        while ((rd = lfs_file_read(&lfs_ctx.lfs, &lfs_ctx.file, buf, sizeof(buf))) > 0) {
            total += rd;
            cmox_hash_append((cmox_hash_handle_t *)&ctx, buf, (size_t)rd);
        }
        if (rd < 0) {
            BL_ERR("payload read failed: %d", rd);
            cmox_hash_cleanup((cmox_hash_handle_t *)&ctx);
            err = FIRMWARE_VERIFY_ERR_READ;
            goto fail;
        }
        cmox_hash_generateTag((cmox_hash_handle_t *)&ctx, calc_hash, &hash_len);
        cmox_hash_cleanup((cmox_hash_handle_t *)&ctx);

        BL_INFO("Hashed %d bytes", total);

        if (memcmp(calc_hash, hdr.sha256, 32) != 0) {
            BL_ERR("SHA256 mismatch");
            err = FIRMWARE_VERIFY_ERR_SHA256;
            goto fail;
        }
        BL_INFO("SHA256 OK");
    }

    BL_INFO("SHA256 OK");
    /* 4. 校验ECDSA签名 */
    if (verify_ecdsa(&hdr) != 0) {
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



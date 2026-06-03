#include "data_storage.h"


int lfs_storage_callback(const uint8_t *buf, uint32_t len, void *user_ctx)
{
    dbg_printf("[LFS] storage callback enter, len=%lu\r\n", len);
    lfs_ctx_t *ctx = (lfs_ctx_t *)user_ctx;
    // 用ctx->lfs、ctx->handle_a 等做实际写入

    int ret = lfs_file_write(&ctx->lfs, &ctx->file, buf, len);

    if (ret < 0) {
        dbg_printf("[LFS][FAIL] lfs_file_write len=%lu ret=%d\r\n", len, ret);
        return -1;
    }

    if ((uint32_t)ret != len) {
        dbg_printf("[LFS][FAIL] partial write len=%lu ret=%d\r\n", len, ret);
        return -1;
    }

    dbg_printf("[LFS][PASS] lfs_file_write len=%lu\r\n", len);
    return 0;

}

// int flash_storage_callback(const uint8_t *buf, uint32_t len, void *user_ctx)
// {
//     lfs_ctx_t *ctx = (lfs_ctx_t *)user_ctx;
//     // 直接调用你的Flash写函数，传入buf和len
//     return Write_Buffer_To_NorFlash(ctx->flash_handle, buf, len);
// }



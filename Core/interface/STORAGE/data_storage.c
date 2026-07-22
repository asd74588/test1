#include "data_storage.h"
#include "log_config.h"

#include <stdio.h>

#if LOG_DATA_STORAGE_ENABLE
#define storage_log(...) printf(__VA_ARGS__)
#else
#define storage_log(...) ((void)0)
#endif

#if LOG_DATA_STORAGE_TRACE_ENABLE
#define storage_trace(...) printf(__VA_ARGS__)
#else
#define storage_trace(...) ((void)0)
#endif

int lfs_storage_callback(const uint8_t *buf, uint32_t len, void *user_ctx)
{
    storage_trace("[LFS] storage callback enter, len=%lu\r\n", len);
    lfs_ctx_t *ctx = (lfs_ctx_t *)user_ctx;
    // 用ctx->lfs、ctx->handle_a 等做实际写入

    int ret = lfs_file_write(&ctx->lfs, &ctx->file, buf, len);

    if (ret < 0)
    {
        storage_log("[LFS][FAIL] lfs_file_write len=%lu ret=%d\r\n", len, ret);
        return -1;
    }

    if ((uint32_t)ret != len)
    {
        storage_log("[LFS][FAIL] partial write len=%lu ret=%d\r\n", len, ret);
        return -1;
    }

    storage_trace("[LFS][PASS] lfs_file_write len=%lu\r\n", len);
    return 0;
}

// int flash_storage_callback(const uint8_t *buf, uint32_t len, void *user_ctx)
// {
//     lfs_ctx_t *ctx = (lfs_ctx_t *)user_ctx;
//     // 直接调用你的Flash写函数，传入buf和len
//     return Write_Buffer_To_NorFlash(ctx->flash_handle, buf, len);
// }

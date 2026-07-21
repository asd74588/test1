#ifndef THINGSBOARD_OTA_H
#define THINGSBOARD_OTA_H

#include <stdint.h>

#include "global.h"
#include "ota_state_machine.h"

#define THINGSBOARD_OTA_DEFAULT_PATH       "a.elf"
#define THINGSBOARD_OTA_DEFAULT_CHUNK_SIZE 512U

void thingsboard_ota_context_init(ota_ctx_t *ctx,
                                  transfer_cfg_t *transfer_cfg,
                                  lfs_ctx_t *fs);

#endif /* THINGSBOARD_OTA_H */

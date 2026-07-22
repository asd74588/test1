#ifndef __DATA_STORAGE_H
#define __DATA_STORAGE_H

#include "global.h"
#include "ota_state_machine.h"

int lfs_storage_callback(const uint8_t *buf, uint32_t len, void *user_ctx);

#endif

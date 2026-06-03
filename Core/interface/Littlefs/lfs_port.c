#include "lfs.h"
#include "w25qxx.h" // 你的SPI Flash驱动头文件
#include "spi.h"    // 你的SPI操作函数头文件
#include "lfs_config.h"


static uint8_t lfs_read_buffer[LFS_CACHE_SIZE];
static uint8_t lfs_prog_buffer[LFS_CACHE_SIZE];
static uint8_t lfs_lookahead_buffer[LFS_LOOKAHEAD_SIZE];
static uint8_t lfs_file_cache[LFS_FILE_CACHE_SIZE];

extern spinor_info_t          spinor;

// 参考结构，你需要根据实际情况实现函数体
int my_flash_read(const struct lfs_config *c, lfs_block_t block, 
                  lfs_off_t off, void *buffer, lfs_size_t size) {
    uint32_t address = block * c->block_size + off;
    return spinor_read(&spinor, address, (uint8_t*)buffer, size);
}

int my_flash_prog(const struct lfs_config *c, lfs_block_t block, 
                  lfs_off_t off, const void *buffer, lfs_size_t size) {
    uint32_t address = block * c->block_size + off;
    return spinor_write(&spinor, address, (uint8_t*)buffer, size);
}

int my_flash_erase(const struct lfs_config *c, lfs_block_t block) {
    uint32_t address = block * c->block_size;
    return spinor_erase_sector(&spinor, address, c->block_size);
}

int my_flash_sync(const struct lfs_config *c) {
    // 确保所有数据都已写入硬件，对于SPI Flash，通常直接返回0即可

    return 0;
}

const struct lfs_file_config lfs_file_cfg = {
    .buffer = lfs_file_cache,
};


// 定义配置结构体，把回调函数的地址赋给对应的函数指针成员
const struct lfs_config my_lfs_config = {
    // 🎯 关键：将你的函数地址赋值给结构体的函数指针成员
    .read  = my_flash_read,   // 函数名就是函数的地址
    .prog  = my_flash_prog,
    .erase = my_flash_erase,
    .sync  = my_flash_sync,
    
    // 硬件配置参数
    .read_size   = LFS_READ_SIZE,
    .prog_size   = LFS_PROG_SIZE,
    .block_size  = LFS_BLOCK_SIZE,
    .block_count = LFS_BLOCK_COUNT,
    .cache_size  = LFS_CACHE_SIZE,
    .lookahead_size = LFS_LOOKAHEAD_SIZE,
    .block_cycles = LFS_BLOCK_CYCLES,


    .read_buffer = lfs_read_buffer,
    .prog_buffer = lfs_prog_buffer,
    .lookahead_buffer = lfs_lookahead_buffer,
};


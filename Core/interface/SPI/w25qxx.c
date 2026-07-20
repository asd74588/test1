

#include <stdio.h>
#include <string.h>
#include "main.h"
#include "w25qxx.h"
#include "spi.h"
#include "log_config.h"

#if LOG_W25QXX_ENABLE
#define spinor_print(format,args...) printf(format, ##args)
#else
#define spinor_print(format,args...) do{} while(0)
#endif

__attribute__((unused)) static void dump_buf(const char *prompt, uint8_t *buf, uint32_t size)
{
    int      i;

    if(!buf)
        return ;

    if(prompt)
        spinor_print("%s\r\n", prompt);

    for(i=0; i<size; i++)
        spinor_print("%02x ", buf[i]);

    spinor_print("\r\n");
}

#ifdef USE_FREERTOS
#include "cmsis_os.h"
#define mdelay(ms) osDelay(ms)
#else
#define mdelay(ms) HAL_Delay(ms)
#endif

/*+-----------------------+
 *|   SPI API Functions   |
 *+-----------------------+*/

#define W25Q_SPI               &hspi1
#define W25Q_CS_PORT            GPIOA
#define W25Q_CS_PIN             GPIO_PIN_4

#define SPI_DUMMY_BYTE          0xA5

spinor_info_t          spinor;


void spinor_gpio_init(struct spi_info *spi)
{
    GPIO_InitTypeDef    GPIO_InitStruct = {0};

    /* Initial W25Q Norflash SPI CS pin */
    HAL_GPIO_WritePin(spi->cs_gpio, spi->cs_pin, GPIO_PIN_SET);
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Pin = spi->cs_pin;
    HAL_GPIO_Init(spi->cs_gpio, &GPIO_InitStruct);
}

void spi_cs_enable(struct spi_info *spi)
{
    HAL_GPIO_WritePin(spi->cs_gpio, spi->cs_pin, GPIO_PIN_RESET);
}

void spi_cs_disable(struct spi_info *spi)
{
    HAL_GPIO_WritePin(spi->cs_gpio, spi->cs_pin, GPIO_PIN_SET);
}

uint8_t spi_xfer(struct spi_info *spi, uint8_t data)
{
    uint8_t       rxbyte;
    HAL_SPI_TransmitReceive(spi->hspi, &data, &rxbyte, 1, 100);
    return rxbyte;
}

void spi_send(struct spi_info *spi, uint8_t *data, uint32_t bytes)
{
    HAL_SPI_Transmit(spi->hspi, data, bytes, 100);
}

void spi_recv(struct spi_info *spi, uint8_t *buf, uint32_t size)
{
    HAL_SPI_Receive(spi->hspi, buf, size, 100);
}

#define SPI_INFO(_hspi, _cs_gpio, _cs_pin) {\
        .hspi       = _hspi,            \
        .cs_gpio    = _cs_gpio,         \
        .cs_pin     = _cs_pin,          \
        .select     = spi_cs_enable,    \
        .deselect   = spi_cs_disable,   \
        .xfer       = spi_xfer,         \
        .send       = spi_send,         \
        .recv       = spi_recv,         \
    }

static struct spi_info spinor_spi = SPI_INFO(W25Q_SPI, W25Q_CS_PORT, W25Q_CS_PIN);

/*+-----------------------+
 *|  W25Q SPI Norflash ID |
 *+-----------------------+*/

#define W25Q_PAGSIZE            256     /* 1Page=256B */
#define W25Q_SECSIZE            4096    /* 1Sector=16Pages=4KB */
#define W25Q_BLKSIZE            65536   /* 1Block=16Sector=64KB */

#define ARRAY_SIZE(x)           (sizeof(x)/sizeof(x[0]))

/* JEDEC ID the 3rd bytes is the storage capacity */
#define CAPCITY_ID(id)          ((((id) & 0xFFu) == 0x20u) ? (1UL << 26) : (1UL << ((id) & 0xFFu)))
#define NOR_INFO(_name, _jedec_id) \
        .name       = _name,                                \
        .jedec_id   = _jedec_id,                            \
        .block_size = W25Q_BLKSIZE,                         \
        .sector_size= W25Q_SECSIZE,                         \
        .page_size  = W25Q_PAGSIZE,                         \
        .capacity   = CAPCITY_ID(_jedec_id),                \
        .n_blocks   = CAPCITY_ID(_jedec_id)/W25Q_BLKSIZE,   \
        .n_sectors  = CAPCITY_ID(_jedec_id)/W25Q_SECSIZE,   \
        .n_pages    = CAPCITY_ID(_jedec_id)/W25Q_PAGSIZE,   \

static struct flash_info spinor_ids[] = {
        { NOR_INFO("W25Q512", 0xef4020) },
        { NOR_INFO("W25Q256", 0xef4019) },
        { NOR_INFO("W25Q128", 0xef4018) },
        { NOR_INFO("W25Q64",  0xef4017) },
        { NOR_INFO("W25Q32",  0xef4016) },
        { NOR_INFO("W25Q16",  0xef4015) },
        { NOR_INFO("W25Q80",  0xef4014) },
        { NOR_INFO("W25Q40",  0xef4013) },
        { NOR_INFO("W25Q20",  0xef4012) },
        { NOR_INFO("W25Q10",  0xef4011) },
};

/*+-------------------------------+
 *|   SPI Norflash HighLevel API  |
 *+-------------------------------+*/

/* SPI Norflash API test function */
void spinor_test(void)
{
    spinor_info_t          spinor;
    int                    i;
    uint8_t                buf[W25Q_PAGSIZE*2];

    if( spinor_init(&spinor) < 0 )
        return ;

    //spinor_erase_chip(&spinor);
    //spinor_erase_block(&spinor, 1, W25Q_BLKSIZE);
    spinor_erase_sector(&spinor, 1, W25Q_SECSIZE);

    memset(buf, 0, sizeof(buf));
    spinor_read(&spinor, 0, buf, sizeof(buf));
    dump_buf("<<<Read data after erase:\n", buf, sizeof(buf));

    /* Read/Write data test on address not page align */
    for(i=0; i<sizeof(buf); i++)
        buf[i] = i;
    spinor_write(&spinor, 8, buf, W25Q_PAGSIZE*2);

    memset(buf, 0, sizeof(buf));
    spinor_read(&spinor, 8, buf, sizeof(buf));
    dump_buf("<<<Read data after write:\n", buf, sizeof(buf));

    return ;
}

/* Initial SPI and detect the flash chip. */
int spinor_init(struct spinor_info *spinor)
{
    spinor->spi = &spinor_spi;

    spinor_gpio_init(spinor->spi);

    if( !spinor_detect_by_jedec(spinor) )
        return -1;

    spinor_print("Norflash %s detected, capacity %lu KB, %lu blocks, %lu sectors, %lu pages.\r\n",
            spinor->flash->name, spinor->flash->capacity>>10,
            spinor->flash->n_blocks, spinor->flash->n_sectors, spinor->flash->n_pages);

    return 0;
}

/* Description:  Erase whole flash chip.
 * Reference  :  P60, 8.2.32 Chip Erase (C7h / 60h)
 */
int spinor_erase_chip(struct spinor_info *spinor)
{
    struct spi_info *spi = spinor->spi;

    while (spinor->lock == 1)
        mdelay(1);

    spinor->lock = 1;

#if LOG_W25QXX_ENABLE
    uint32_t StartTime = HAL_GetTick();
    spinor_print("Norflash EraseChip Begin...\r\n");
#endif

    spinor_write_enable(spi);

    spi->select(spi);
    spi->xfer(spi, SPINOR_OP_CHIP_ERASE);
    spi->deselect(spi);
    spinor_WaitForWriteEnd(spi);

#if LOG_W25QXX_ENABLE
    spinor_print("Norflash EraseChip done after %ld ms!\r\n", HAL_GetTick() - StartTime);
#endif

    mdelay(10);
    spinor->lock = 0;

    return 0;
}

/* Description:  Erase blocks by 64KiB,
 * Reference  :  P59, 8.2.31 64KB Block Erase with 4-Byte Address (DCh)
 *  @address is the erase start physical address, which can be not block alignment such as 0x10001.
 *  @size is the erase size, which can be larger than a block such as 4097, and it will erase 2 blocks;
 */
int spinor_erase_block(struct spinor_info *spinor, uint32_t address, uint32_t size)
{
    struct spi_info    *spi = spinor->spi;
    struct flash_info  *flash = spinor->flash;
    uint32_t            block, first, last;
    uint32_t            addr;

    while (spinor->lock == 1)
        mdelay(1);

    spinor->lock = 1;

    /* find first and last erase block */
    first = address / flash->block_size;
    last  = (address+size-1) / flash->block_size;

#if LOG_W25QXX_ENABLE
    spinor_print("Norflash Erase %ld Bytes Block@0x%lx Begin...\r\n", size, address);
    uint32_t StartTime = HAL_GetTick();
#endif

    /* start erase all the blocks */
    for( block=first; block<=last; block++)
    {
        addr = block * flash->sector_size;
#if LOG_W25QXX_ENABLE
        spinor_print("Norflash Erase Block@%lx ...\r\n", addr);
#endif
        spinor_WaitForWriteEnd(spi);
        spinor_write_enable(spi);

        spi->select(spi);
        if (spinor->flash->n_blocks >= 512 ) /* larger than W25Q256 */
        {
            spi->xfer(spi, SPINOR_OP_BE_4K_4B);
            spi->xfer(spi, (addr & 0xFF000000) >> 24);
        }
        else
        {
            spi->xfer(spi, SPINOR_OP_BE_4K);
        }

        spi->xfer(spi, (addr & 0xFF0000) >> 16);
        spi->xfer(spi, (addr & 0xFF00) >> 8);
        spi->xfer(spi, addr & 0xFF);
        spi->deselect(spi);

        spinor_WaitForWriteEnd(spi);
    }

#if LOG_W25QXX_ENABLE
    spinor_print("Norflash EraseBlock@0x%lx done after %ld ms\r\n", address, HAL_GetTick() - StartTime);
    mdelay(100);
#endif

    mdelay(1);
    spinor->lock = 0;

    return 0;
}

/* Description:  Erase sectors by 4KiB
 * Reference  :  P56, 8.2.28 Sector Erase with 4-Byte Address (21h)
 *  @address is the erase start physical address, which can be not sector alignment such as 0x1001.
 *  @size is the erase size, which can be larger than a sector such as 4097, and it will erase 2 sectors;
 */
int spinor_erase_sector(struct spinor_info *spinor, uint32_t address, uint32_t size)
{
    struct spi_info    *spi = spinor->spi;
    struct flash_info  *flash = spinor->flash;
    uint32_t            sector, first, last;
    uint32_t            addr;

    while (spinor->lock == 1)
        mdelay(1);

    spinor->lock = 1;

    /* find first and last erase sector */
    first = address / flash->sector_size;
    last  = (address+size-1) / flash->sector_size;

#if LOG_W25QXX_ENABLE
    uint32_t StartTime = HAL_GetTick();
    spinor_print("Norflash Erase %ld Bytes Sector@0x%lx Begin...\r\n", size, address);
#endif

    /* start erase all the sectors */
    for( sector=first; sector<=last; sector++)
    {
        addr = sector * flash->sector_size;
#if LOG_W25QXX_ENABLE
        spinor_print("Norflash Erase Sector@%lx ...\r\n", addr);
#endif

        spinor_WaitForWriteEnd(spi);
        spinor_write_enable(spi);

        spi->select(spi);
        if (spinor->flash->n_blocks >= 512 ) /* larger than W25Q256 */
        {
            spi->xfer(spi, SPINOR_OP_SE_4B);
            spi->xfer(spi, (addr & 0xFF000000) >> 24);
        }
        else
        {
            spi->xfer(spi, SPINOR_OP_SE);
        }
        spi->xfer(spi, (addr & 0xFF0000) >> 16);
        spi->xfer(spi, (addr & 0xFF00) >> 8);
        spi->xfer(spi, addr & 0xFF);
        spi->deselect(spi);

        spinor_WaitForWriteEnd(spi);
    }

#if LOG_W25QXX_ENABLE
    spinor_print("Norflash EraseSector@0x%lx done after %ld ms\r\n", address, HAL_GetTick() - StartTime);
#endif

    mdelay(1);
    spinor->lock = 0;

    return 0;
}

/* P32: 10.2.14 Page Program (02h) */
int spinor_write(struct spinor_info *spinor, uint32_t address, uint8_t *data, uint32_t bytes)
{
    struct spi_info    *spi = spinor->spi;
    struct flash_info  *flash = spinor->flash;
    uint32_t            page, first, last;
    uint32_t            addr, ofset, len;

    while (spinor->lock == 1)
        mdelay(1);

    spinor->lock = 1;

    /* find first and last write page */
    first = address / flash->page_size;
    last  = (address+bytes-1) / flash->page_size;

#if LOG_W25QXX_ENABLE
    uint32_t StartTime = HAL_GetTick();
    spinor_print("Norflash Write %ld Bytes to addr@0x%lx Begin...\r\n", bytes, address);
#endif

    /* address in page and offset in buffer */
    addr = address;
    ofset = 0;

    /* start write all the pages */
    for( page=first; page<=last; page++)
    {
        len = flash->page_size - (addr%flash->page_size);
        len = len > bytes ? bytes : len;
#if LOG_W25QXX_ENABLE
        spinor_print("Norflash write addr@0x%lx, %lu bytes\r\n", addr, len);
#endif

        spinor_WaitForWriteEnd(spi);
        spinor_write_enable(spi);

        spi->select(spi);
        if (spinor->flash->n_blocks >= 512 )
        {
            spi->xfer(spi, SPINOR_OP_PP_4B);
            spi->xfer(spi, (addr & 0xFF000000) >> 24);
        }
        else
        {
            spi->xfer(spi, SPINOR_OP_PP);
        }
        spi->xfer(spi, (addr & 0xFF0000) >> 16);
        spi->xfer(spi, (addr & 0xFF00) >> 8);
        spi->xfer(spi, addr & 0xFF);

        /* send data */
        spi->send(spi, data+ofset, len);

        spi->deselect(spi);
        spinor_WaitForWriteEnd(spi);

        addr  += len;
        ofset += len;
        bytes  -= len;
    }

#if LOG_W25QXX_ENABLE
    spinor_print("Norflash WriteByte@0x%lx done after %ld ms\r\n", address, HAL_GetTick() - StartTime);
#endif

    mdelay(1);
    spinor->lock = 0;

    return 0;
}

/* Description:  The Fast Read instruction can read the entire memory chip.
 * Reference  :  P41, 8.2.13 Fast Read with 4-Byte Address (0Ch)
 *  @address is the read start physical address, which can be not page alignment such as 0x101.
 *  @size is the read size, which can be larger than a page such as 257, and it will read 2 pages;
 */
int spinor_read(struct spinor_info *spinor, uint32_t address, uint8_t *buf, uint32_t bytes)
{
    struct spi_info *spi = spinor->spi;

    while (spinor->lock == 1)
        mdelay(1);

    spinor->lock = 1;

#if LOG_W25QXX_ENABLE
    uint32_t StartTime = HAL_GetTick();
    spinor_print("Norflash Read %ld Bytes from addr@0x%lx Begin...\r\n", bytes, address);
#endif

    spi->select(spi);

    /* send instruction and address */
    if (spinor->flash->n_blocks >= 512 )
    {
        spi->xfer(spi, SPINOR_OP_READ_FAST_4B);
        spi->xfer(spi, (address & 0xFF000000) >> 24);
    }
    else
    {
        spi->xfer(spi, SPINOR_OP_READ_FAST);
    }
    spi->xfer(spi, (address & 0xFF0000) >> 16);
    spi->xfer(spi, (address & 0xFF00) >> 8);
    spi->xfer(spi, address & 0xFF);

    /* read data */
    spi->xfer(spi, SPI_DUMMY_BYTE);
    spi->recv(spi, buf, bytes);

    spi->deselect(spi);

#if LOG_W25QXX_ENABLE
    spinor_print("Norflash ReadBytes@0x%lx done after %ld ms\r\n", address, HAL_GetTick() - StartTime);
#endif
    spinor->lock = 0;

    return 0;
}

/*+-------------------------------+
 *|   SPI Norflash LowLevel API   |
 *+-------------------------------+*/

/* Detect the norflash by JEDEC ID */
int spinor_detect_by_jedec(struct spinor_info *spinor)
{
    uint32_t            jedec_id;
    int                 i, found = 0;

    jedec_id = spinor_read_jedecid(spinor->spi);

    for(i=0; i<ARRAY_SIZE(spinor_ids); i++)
    {
        if(spinor_ids[i].jedec_id == jedec_id)
        {
            found = 1;
            spinor->flash = &spinor_ids[i];
            break;
        }
    }

    spinor_print("Detect JEDEC ID[0x%lx], Norflash %s found\r\n", jedec_id, found?spinor->flash->name:"not");
    return found;
}

/* Description:  Read the chipset UNIQUE ID.
 * Reference  :  P68, 8.2.40 Read Unique ID Number (4Bh)
 */
int spinor_read_uniqid(struct spi_info *spi, uint8_t *uniq_id)
{
    uint8_t              i;
    uint8_t              id;

    if( !uniq_id )
        return -1;

    spi->select(spi);
    spi->xfer(spi, SPINOR_OP_RDUID);

    /* Skip 4 bytes dummy bytes */
    for (i=0; i<4; i++)
        spi->xfer(spi, SPI_DUMMY_BYTE);

    for (i=0; i<8; i++)
    {
        id = spi->xfer(spi, SPI_DUMMY_BYTE);
        uniq_id[i] = id;
    }

    spi->deselect(spi);

    return 0;
}

/* Description:  Read the chipset JEDEC ID.
 * Reference  :  P69, 8.2.41 Read JEDEC ID (9Fh)
 */
uint32_t spinor_read_jedecid(struct spi_info *spi)
{
    uint32_t            jedec_id = 0x0;
    uint8_t             id[3];

    spi->select(spi);
    spi->xfer(spi, SPINOR_OP_RDID);
    id[0] = spi->xfer(spi, SPI_DUMMY_BYTE); /* Vendor ID */
    id[1] = spi->xfer(spi, SPI_DUMMY_BYTE); /* Memory Type */
    id[2] = spi->xfer(spi, SPI_DUMMY_BYTE); /* Storage Capacity */
    spi->deselect(spi);

    jedec_id = (id[0] << 16) | (id[1] << 8) | id[2];
    return jedec_id;
}

/* Description:  Write Enable
 * Reference  :  P31, 8.2.1 Write Enable (06h)
 */
void spinor_write_enable(struct spi_info *spi)
{
    spi->select(spi);
    spi->xfer(spi, SPINOR_OP_WREN);
    spi->deselect(spi);

    mdelay(1);
}

/* Description:  Write Disable
 * Reference  :  P32, 8.2.3 Write Disable (04h)
 */
void spinor_write_disable(struct spi_info *spi)
{
    spi->select(spi);
    spi->xfer(spi, SPINOR_OP_WRDI);
    spi->deselect(spi);

    mdelay(1);
}

/* Description:  Read Status Register
 * Reference  :  P32, 8.2.4 Read Status Register-1 (05h), Status Register-2 (35h) & Status Register-3 (15h)
 */
uint8_t spinor_read_status_reg(struct spi_info *spi, uint8_t reg)
{
    uint8_t value = 0;
    uint8_t cmd[REG_STATUS_MAX] = { SPINOR_OP_RDSR1 , SPINOR_OP_RDSR2, SPINOR_OP_RDSR3 }; /* Status Register 1~3 */

    if( reg>= REG_STATUS_MAX )
        return 0xFF;

    spi->select(spi);
    spi->xfer(spi, cmd[reg]);
    value = spi->xfer(spi, SPI_DUMMY_BYTE);
    spi->deselect(spi);

    return value;
}

/* Description:  Write Status Register
 * Reference  :  P33, 8.2.5 Write Status Register-1 (01h), Status Register-2 (31h) & Status Register-3 (11h)
 */
void spinor_write_status_reg(struct spi_info *spi, uint8_t reg, uint8_t value)
{
    uint8_t cmd[REG_STATUS_MAX] = { SPINOR_OP_WRSR1 , SPINOR_OP_WRSR2, SPINOR_OP_WRSR3 }; /* Status Register 1~3 */

    if( reg>= REG_STATUS_MAX )
        return ;

    spi->select(spi);
    spi->xfer(spi, cmd[reg]);
    value = spi->xfer(spi, value);
    spi->deselect(spi);
}

/* Description:  Wait flash program/erase finished by read Status Register for BUSY bit
 * Reference  :  P15, 7.1 Status Registers
 */
void spinor_WaitForWriteEnd(struct spi_info *spi)
{
    uint8_t value = 0;
    mdelay(1);

    spi->select(spi);
    spi->xfer(spi, SPINOR_OP_RDSR1);

    do
    {
        value = spi->xfer(spi, SPI_DUMMY_BYTE);
        mdelay(1);
    } while ((value & 0x01) == 0x01);

    spi->deselect(spi);
}


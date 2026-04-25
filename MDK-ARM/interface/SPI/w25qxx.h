#ifndef __W25QXX_H
#define __W25QXX_H

#include "main.h"


#include "global.h"
#include <stdint.h>

/* Flash opcodes. Refer to <<W25Q256JV.pdf>> P26 Table 8.1.2 Instruction Set Table */
#define SPINOR_OP_RDID          0x9f    /* Read JEDEC ID */
#define SPINOR_OP_RDUID         0x4b    /* Read unique ID */
#define SPINOR_OP_WRSR1         0x01    /* Write status register-1 */
#define SPINOR_OP_WRSR2         0x31    /* Write status register-2 */
#define SPINOR_OP_WRSR3         0x11    /* Write status register-3 */
#define SPINOR_OP_BP            0x02    /* Byte program */
#define SPINOR_OP_READ          0x03    /* Read data bytes (low frequency) */
#define SPINOR_OP_WRDI          0x04    /* Write disable */
#define SPINOR_OP_RDSR1         0x05    /* Read status register-1 */
#define SPINOR_OP_RDSR2         0x35    /* Read status register-2 */
#define SPINOR_OP_RDSR3         0x15    /* Read status register-3 */
#define SPINOR_OP_WREN          0x06    /* Write enable */
#define SPINOR_OP_READ_FAST     0x0b    /* Read data bytes (high frequency) */
#define SPINOR_OP_READ_FAST_4B  0x0c    /* Read data bytes (high frequency) */

#define SPINOR_OP_CHIP_ERASE    0xc7    /* Erase whole flash chip */
#define SPINOR_OP_BE_4K_4B      0xdc    /* Block erase (64KiB) with 4-Byte Address */
#define SPINOR_OP_BE_4K         0xd8    /* Block erase (64KiB) */
#define SPINOR_OP_SE_4B         0x21    /* Sector erase (4KiB) with 4-Byte Address */
#define SPINOR_OP_SE            0x20    /* Sector erase (4KiB)  */
#define SPINOR_OP_PP_4B         0x12    /* Page Program (up to 256 bytes) with 4-Byte Address */
#define SPINOR_OP_PP            0x02    /* Page program (up to 256 bytes) */
#define SPINOR_OP_SRSTEN        0x66    /* Software Reset Enable */
#define SPINOR_OP_SRST          0x99    /* Software Reset */

typedef struct spi_info
{
    SPI_HandleTypeDef    *hspi;     /* SPI port */
    GPIO_TypeDef         *cs_gpio;  /* CS GPIO group */
    uint16_t              cs_pin;   /* CS GPIO pin */

    void    (*select)(struct spi_info *spi);    /* CS enable function  */
    void    (*deselect)(struct spi_info *spi);  /* CS disable function */
    uint8_t (*xfer)(struct spi_info *spi, uint8_t data); /* Transmit and Receive one byte */
    void    (*send)(struct spi_info *spi, uint8_t *data, uint32_t bytes); /* Transmit $bytes data */
    void    (*recv)(struct spi_info *spi, uint8_t *buf, uint32_t size);   /* Receive $size data */
} spi_info_t;

typedef struct flash_info
{
    char            *name;          /* Chip name */
    uint32_t         jedec_id;      /* JEDEC ID, 3 bytes */
    uint32_t         capacity;      /* Chip   size in bytes */
    uint32_t         block_size;    /* Block  size in bytes */
    uint32_t         sector_size;   /* Sector size in bytes */
    uint32_t         page_size;     /* Page   size in bytes */
    uint32_t         n_blocks;      /* Number of blocks */
    uint32_t         n_sectors;     /* Number of sectors */
    uint32_t         n_pages;       /* Number of pages */
} flash_info_t;


typedef struct spinor_info
{
    spi_info_t      *spi;
    flash_info_t    *flash;
    uint8_t          lock;
} spinor_info_t;


/* Status registers */
enum
{
    REG_STATUS1,
    REG_STATUS2,
    REG_STATUS3,
    REG_STATUS_MAX,
};

/*+-------------------------------+
 *|   SPI Norflash HighLevel API  |
 *+-------------------------------+*/

/* SPI Norflash API test function */
extern void spinor_test(void);

/* Initial SPI and detect the flash chip */
extern int spinor_init(struct spinor_info *spinor);

/* Description:  Erase whole flash chip.
 * Reference  :  P60, 8.2.32 Chip Erase (C7h / 60h)
 */
extern int spinor_erase_chip(struct spinor_info *spinor);

/* Description:  Erase blocks by 64KiB,
 * Reference  :  P59, 8.2.31 64KB Block Erase with 4-Byte Address (DCh)
 *  @address is the erase start physical address, which can be not block alignment such as 0x10001.
 *  @size is the erase size, which can be larger than a block such as 4097, and it will erase 2 blocks;
 */
extern int spinor_erase_block(struct spinor_info *spinor, uint32_t address, uint32_t size);

/* Description:  Erase sectors by 4KiB
 * Reference  :  P56, 8.2.28 Sector Erase with 4-Byte Address (21h)
 *  @address is the erase start physical address, which can be not sector alignment such as 0x1001.
 *  @size is the erase size, which can be larger than a sector such as 4097, and it will erase 2 sectors;
 */
extern int spinor_erase_sector(struct spinor_info *spinor, uint32_t address, uint32_t size);

/* Description:  Page random write by 256B
 *  @addr is the write start physical address, which can be not page alignment such as 0x101.
 *  @size is the write size, which can be larger than a page such as 257, and it will write 2 pages;
 */
extern int spinor_write(struct spinor_info *spinor, uint32_t address, uint8_t *data, uint32_t bytes);

/* Description:  The Fast Read instruction can read the entire memory chip.
 * Reference  :  P41, 8.2.13 Fast Read with 4-Byte Address (0Ch)
 *  @address is the read start physical address, which can be not page alignment such as 0x101.
 *  @size is the read size, which can be larger than a page such as 257, and it will read 2 pages;
 */
extern int spinor_read(struct spinor_info *spinor, uint32_t address, uint8_t *buf, uint32_t bytes);

/*+-------------------------------+
 *|   SPI Norflash LowLevel API   |
 *+-------------------------------+*/

/* Detect the norflash by JEDEC ID */
int spinor_detect_by_jedec(struct spinor_info *spinor);

/* Description:  Read the chipset UNIQUE ID.
 * Reference  :  P68, 8.2.40 Read Unique ID Number (4Bh)
 */
int spinor_read_uniqid(struct spi_info *spi, uint8_t *uniq_id);

/* Description:  Read the chipset JEDEC ID.
 * Reference  :  P69, 8.2.41 Read JEDEC ID (9Fh)
 */
uint32_t spinor_read_jedecid(struct spi_info *spi);

/* Description:  Write Enable
 * Reference  :  P31, 8.2.1 Write Enable (06h)
 */
void spinor_write_enable(struct spi_info *spi);

/* Description:  Write Disable
 * Reference  :  P32, 8.2.3 Write Disable (04h)
 */
void spinor_write_disable(struct spi_info *spi);

/* Description:  Read Status Register
 * Reference  :  P32, 8.2.4 Read Status Register-1 (05h), Status Register-2 (35h) & Status Register-3 (15h)
 */
uint8_t spinor_read_status_reg(struct spi_info *spi, uint8_t reg);

/* Description:  Write Status Register
 * Reference  :  P33, 8.2.5 Write Status Register-1 (01h), Status Register-2 (31h) & Status Register-3 (11h)
 */
void spinor_write_status_reg(struct spi_info *spi, uint8_t reg, uint8_t value);

/* Description:  Wait flash program/erase finished by read Status Register for BUSY bit
 * Reference  :  P15, 7.1 Status Registers
 */
void spinor_WaitForWriteEnd(struct spi_info *spi);


#endif


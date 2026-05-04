#include "eeprom_emul.h"
#include <string.h>

/* ======================== 内部类型 ======================== */

/* 数据条目：8 字节，与 Flash 双字编程对齐 */
typedef struct {
    uint16_t virt_addr;   /* 虚拟地址 / 变量 ID     */
    uint16_t reserved;    /* 保留，擦除态 0xFFFF     */
    uint32_t data;        /* 变量数据                */
} ee_entry_t;

/* 页状态枚举 */
typedef enum {
    EE_PAGE_ERASED  = 0,  /* status1=ERASED, status2=ERASED */
    EE_PAGE_RECEIVE = 1,  /* status1=RECEIVE, status2=ERASED */
    EE_PAGE_ACTIVE  = 2,  /* status1=RECEIVE, status2=ACTIVE */
    EE_PAGE_ERROR   = 3   /* 非法组合                       */
} ee_page_state_t;

/* ======================== 内部函数声明 ======================== */

static ee_page_state_t EE_GetPageState(uint32_t page_addr);
static int8_t          EE_FindActivePage(void);
static int16_t         EE_FindFreeEntry(uint32_t page_addr);
static ee_entry_t      EE_ReadEntry(uint32_t page_addr, uint16_t index);
static HAL_StatusTypeDef EE_WriteEntry(uint32_t page_addr, uint16_t index,
                                       uint16_t virt_addr, uint32_t data);
static HAL_StatusTypeDef EE_WritePageStatus(uint32_t addr, uint64_t value);
static HAL_StatusTypeDef EE_ErasePage(uint32_t page_addr);
static HAL_StatusTypeDef EE_PageTransfer(uint32_t active_page,
                                         uint32_t receive_page,
                                         uint16_t new_virt_addr,
                                         uint32_t new_data);

/* ======================== 内部函数实现 ======================== */

/**
 * @brief  获取指定页的状态
 */
static ee_page_state_t EE_GetPageState(uint32_t page_addr)
{
    uint64_t s1 = *(__IO uint64_t*)(page_addr);
    uint64_t s2 = *(__IO uint64_t*)(page_addr + 8U);

    if (s1 == EE_STATUS_ERASED && s2 == EE_STATUS_ERASED)
        return EE_PAGE_ERASED;
    if (s1 == EE_STATUS_RECEIVE && s2 == EE_STATUS_ERASED)
        return EE_PAGE_RECEIVE;
    if (s1 == EE_STATUS_RECEIVE && s2 == EE_STATUS_ACTIVE)
        return EE_PAGE_ACTIVE;

    return EE_PAGE_ERROR;
}

/**
 * @brief  查找当前活跃页
 * @retval 0 = Page0, 1 = Page1, -1 = 未找到
 */
static int8_t EE_FindActivePage(void)
{
    ee_page_state_t s0 = EE_GetPageState(EE_PAGE0_BASE);
    ee_page_state_t s1 = EE_GetPageState(EE_PAGE1_BASE);

    /* 正常：一页 ACTIVE + 一页 ERASED */
    if (s0 == EE_PAGE_ACTIVE && s1 == EE_PAGE_ERASED) return 0;
    if (s0 == EE_PAGE_ERASED && s1 == EE_PAGE_ACTIVE) return 1;

    /* 页转移中断：一页 ACTIVE + 一页 RECEIVE */
    if (s0 == EE_PAGE_ACTIVE && s1 == EE_PAGE_RECEIVE) return 0;
    if (s0 == EE_PAGE_RECEIVE && s1 == EE_PAGE_ACTIVE) return 1;

    return -1;
}

/**
 * @brief  擦除一页 Flash
 */
static HAL_StatusTypeDef EE_ErasePage(uint32_t page_addr)
{
    FLASH_EraseInitTypeDef erase_init;
    uint32_t page_error = 0U;

    erase_init.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase_init.Banks       = EE_FLASH_BANK;
    erase_init.Page        = (page_addr - EE_FLASH_BASE) / EE_FLASH_PAGE_SIZE;
    erase_init.NbPages     = 1U;

    return HAL_FLASHEx_Erase(&erase_init, &page_error);
}

/**
 * @brief  写入一个双字到 Flash（调用前须 Unlock）
 */
static HAL_StatusTypeDef EE_WritePageStatus(uint32_t addr, uint64_t value)
{
    return HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, value);
}

/**
 * @brief  读取指定页、指定索引的条目
 */
static ee_entry_t EE_ReadEntry(uint32_t page_addr, uint16_t index)
{
    ee_entry_t entry;
    uint32_t addr = page_addr + EE_HEADER_SIZE
                    + ((uint32_t)index * EE_ENTRY_SIZE);
    uint64_t raw = *(__IO uint64_t*)addr;
    memcpy(&entry, &raw, sizeof(ee_entry_t));
    return entry;
}

/**
 * @brief  在指定页的指定索引处写入条目（调用前须 Unlock）
 */
static HAL_StatusTypeDef EE_WriteEntry(uint32_t page_addr, uint16_t index,
                                       uint16_t virt_addr, uint32_t data)
{
    uint32_t addr = page_addr + EE_HEADER_SIZE
                    + ((uint32_t)index * EE_ENTRY_SIZE);
    ee_entry_t entry;
    uint64_t raw;

    entry.virt_addr = virt_addr;
    entry.reserved  = 0xFFFFU;
    entry.data      = data;
    memcpy(&raw, &entry, sizeof(uint64_t));

    return HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, raw);
}

/**
 * @brief  在页中查找第一个空闲条目索引
 * @retval >=0 空闲索引，-1 页已满
 */
static int16_t EE_FindFreeEntry(uint32_t page_addr)
{
    for (uint16_t i = 0U; i < (uint16_t)EE_MAX_ENTRIES; i++)
    {
        ee_entry_t entry = EE_ReadEntry(page_addr, i);
        if (entry.virt_addr == EE_ADDR_INVALID)
            return (int16_t)i;
    }
    return -1;
}

/**
 * @brief  页转移：将活跃页最新有效数据搬移到接收页
 *
 * 流程：
 *   1. 擦除接收页
 *   2. 标记接收页为 RECEIVE
 *   3. 从活跃页末尾向前扫描，只搬移每个变量的最新值
 *   4. 写入触发转移的新变量
 *   5. 先擦除旧活跃页（保障掉电安全）
 *   6. 标记接收页为 ACTIVE
 *
 * @param  active_page   活跃页地址
 * @param  receive_page  接收页地址
 * @param  new_virt_addr 触发转移的新变量地址（EE_ADDR_INVALID = 无新写入）
 * @param  new_data      新变量数据
 * @note   调用前须 HAL_FLASH_Unlock()，调用后须 HAL_FLASH_Lock()
 */
static HAL_StatusTypeDef EE_PageTransfer(uint32_t active_page,
                                         uint32_t receive_page,
                                         uint16_t new_virt_addr,
                                         uint32_t new_data)
{
    HAL_StatusTypeDef status;
    uint16_t write_idx = 0U;

    /* ---- 1. 擦除接收页 ---- */
    status = EE_ErasePage(receive_page);
    if (status != HAL_OK) return status;

    /* ---- 2. 标记接收页为 RECEIVE ---- */
    status = EE_WritePageStatus(receive_page, EE_STATUS_RECEIVE);
    if (status != HAL_OK) return status;

    /* ---- 3. 从活跃页末尾向前扫描，搬移最新值 ---- */
    for (int16_t i = (int16_t)EE_MAX_ENTRIES - 1; i >= 0; i--)
    {
        ee_entry_t entry = EE_ReadEntry(active_page, (uint16_t)i);
        if (entry.virt_addr == EE_ADDR_INVALID)
            continue;

        /* 检查该地址是否已写入接收页（向后扫描先遇到的是最新值） */
        uint8_t already_moved = 0U;
        for (uint16_t j = 0U; j < write_idx; j++)
        {
            ee_entry_t moved = EE_ReadEntry(receive_page, j);
            if (moved.virt_addr == entry.virt_addr)
            {
                already_moved = 1U;
                break;
            }
        }
        if (already_moved)
            continue;

        status = EE_WriteEntry(receive_page, write_idx,
                               entry.virt_addr, entry.data);
        if (status != HAL_OK) return status;
        write_idx++;
    }

    /* ---- 4. 写入触发转移的新变量 ---- */
    if (new_virt_addr != EE_ADDR_INVALID)
    {
        status = EE_WriteEntry(receive_page, write_idx,
                               new_virt_addr, new_data);
        if (status != HAL_OK) return status;
        write_idx++;
    }

    /* ---- 5. 先擦除旧活跃页（掉电后可恢复） ---- */
    status = EE_ErasePage(active_page);
    if (status != HAL_OK) return status;

    /* ---- 6. 标记接收页为 ACTIVE ---- */
    status = EE_WritePageStatus(receive_page + 8U, EE_STATUS_ACTIVE);
    return status;
}

/* ======================== 外部接口 ======================== */

HAL_StatusTypeDef EE_Init(void)
{
    ee_page_state_t s0 = EE_GetPageState(EE_PAGE0_BASE);
    ee_page_state_t s1 = EE_GetPageState(EE_PAGE1_BASE);
    HAL_StatusTypeDef status;

    /* ---- 情况 1：两页均擦除 → 首次使用 ---- */
    if (s0 == EE_PAGE_ERASED && s1 == EE_PAGE_ERASED)
    {
        HAL_FLASH_Unlock();
        status = EE_WritePageStatus(EE_PAGE0_BASE, EE_STATUS_RECEIVE);
        if (status != HAL_OK) { HAL_FLASH_Lock(); return status; }
        status = EE_WritePageStatus(EE_PAGE0_BASE + 8U, EE_STATUS_ACTIVE);
        HAL_FLASH_Lock();
        return status;
    }

    /* ---- 情况 2：正常状态 → 一页 ACTIVE + 一页 ERASED ---- */
    if ((s0 == EE_PAGE_ACTIVE && s1 == EE_PAGE_ERASED) ||
        (s0 == EE_PAGE_ERASED && s1 == EE_PAGE_ACTIVE))
    {
        return HAL_OK;
    }

    /* ---- 情况 3：页转移中断 → ACTIVE + RECEIVE ---- */
    if (s0 == EE_PAGE_ACTIVE && s1 == EE_PAGE_RECEIVE)
    {
        HAL_FLASH_Unlock();
        status = EE_PageTransfer(EE_PAGE0_BASE, EE_PAGE1_BASE,
                                 EE_ADDR_INVALID, 0U);
        HAL_FLASH_Lock();
        return status;
    }
    if (s0 == EE_PAGE_RECEIVE && s1 == EE_PAGE_ACTIVE)
    {
        HAL_FLASH_Unlock();
        status = EE_PageTransfer(EE_PAGE1_BASE, EE_PAGE0_BASE,
                                 EE_ADDR_INVALID, 0U);
        HAL_FLASH_Lock();
        return status;
    }

    /* ---- 情况 4：掉电发生在擦除旧页之后 → ERASED + RECEIVE ---- */
    if (s0 == EE_PAGE_ERASED && s1 == EE_PAGE_RECEIVE)
    {
        HAL_FLASH_Unlock();
        status = EE_WritePageStatus(EE_PAGE1_BASE + 8U, EE_STATUS_ACTIVE);
        HAL_FLASH_Lock();
        return status;
    }
    if (s0 == EE_PAGE_RECEIVE && s1 == EE_PAGE_ERASED)
    {
        HAL_FLASH_Unlock();
        status = EE_WritePageStatus(EE_PAGE0_BASE + 8U, EE_STATUS_ACTIVE);
        HAL_FLASH_Lock();
        return status;
    }

    /* ---- 情况 5：异常状态 → 格式化 ---- */
    return EE_Format();
}

HAL_StatusTypeDef EE_Format(void)
{
    HAL_StatusTypeDef status;

    HAL_FLASH_Unlock();

    status = EE_ErasePage(EE_PAGE0_BASE);
    if (status != HAL_OK) { HAL_FLASH_Lock(); return status; }

    status = EE_ErasePage(EE_PAGE1_BASE);
    if (status != HAL_OK) { HAL_FLASH_Lock(); return status; }

    /* 将 Page0 标记为 ACTIVE */
    status = EE_WritePageStatus(EE_PAGE0_BASE, EE_STATUS_RECEIVE);
    if (status != HAL_OK) { HAL_FLASH_Lock(); return status; }

    status = EE_WritePageStatus(EE_PAGE0_BASE + 8U, EE_STATUS_ACTIVE);
    HAL_FLASH_Lock();
    return status;
}

HAL_StatusTypeDef EE_Read(uint16_t virt_addr, uint32_t *p_data)
{
    if (p_data == NULL || virt_addr == EE_ADDR_INVALID)
        return HAL_ERROR;

    int8_t active = EE_FindActivePage();
    if (active < 0)
        return HAL_ERROR;

    uint32_t page_addr = (active == 0) ? EE_PAGE0_BASE : EE_PAGE1_BASE;

    /* 从页末尾向前扫描，找到该变量的最新值 */
    for (int16_t i = (int16_t)EE_MAX_ENTRIES - 1; i >= 0; i--)
    {
        ee_entry_t entry = EE_ReadEntry(page_addr, (uint16_t)i);
        if (entry.virt_addr == virt_addr)
        {
            *p_data = entry.data;
            return HAL_OK;
        }
    }

    return HAL_ERROR;
}

HAL_StatusTypeDef EE_Write(uint16_t virt_addr, uint32_t data)
{
    if (virt_addr == EE_ADDR_INVALID)
        return HAL_ERROR;

    int8_t active = EE_FindActivePage();
    if (active < 0)
        return HAL_ERROR;

    uint32_t active_addr  = (active == 0) ? EE_PAGE0_BASE : EE_PAGE1_BASE;
    uint32_t receive_addr = (active == 0) ? EE_PAGE1_BASE : EE_PAGE0_BASE;

    /* 查找空闲条目 */
    int16_t free_idx = EE_FindFreeEntry(active_addr);

    if (free_idx >= 0)
    {
        /* 活跃页有空位，直接追加写入 */
        HAL_FLASH_Unlock();
        HAL_StatusTypeDef status = EE_WriteEntry(active_addr, (uint16_t)free_idx,
                                                  virt_addr, data);
        HAL_FLASH_Lock();
        return status;
    }

    /* 活跃页已满，执行页转移 */
    HAL_FLASH_Unlock();
    HAL_StatusTypeDef status = EE_PageTransfer(active_addr, receive_addr,
                                                virt_addr, data);
    HAL_FLASH_Lock();
    return status;
}

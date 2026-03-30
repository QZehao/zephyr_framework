/**
 * @file sys_memory.c
 * @brief 系统内存管理实现文件
 *
 * 提供内存池管理、分配跟踪和统计功能。
 * 实现基于 bump allocator（顺序分配器），带每池互斥锁和可选跟踪。
 *
 * @copyright Copyright (c) 2026
 * @par License
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sys_memory.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <limits.h>

LOG_MODULE_REGISTER(sys_memory, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部定义
 * ============================================================================= */

/** 默认池大小（如未配置） */
#ifndef CONFIG_SYS_MEMORY_POOL_SIZE
#define CONFIG_SYS_MEMORY_POOL_SIZE 8192
#endif

#define DEFAULT_POOL_SIZE     CONFIG_SYS_MEMORY_POOL_SIZE   ///< 默认池大小（字节）
#define MAX_ALLOCATIONS       256                           ///< 最大跟踪分配数
#define MEMORY_MAGIC          0x4D454D30U                   ///< 魔数 - 有效分配标识（"MEM0"）
#define MEMORY_FREED_MAGIC    0x46524545U                   ///< 魔数 - 已释放块标识（"FREE"）
#define MEMORY_ALIGN_BYTES    4U                            ///< 内存对齐字节数

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

/**
 * @brief 内存分配头部结构
 *
 * 位于每个分配块之前，用于跟踪和验证。
 * 用户指针指向此结构之后的内存。
 */
typedef struct {
    uint32_t magic;             ///< 魔数，用于验证分配有效性
    uint32_t pool_type;         ///< 所属内存池类型
    size_t requested_size;      ///< 用户请求的大小
    size_t payload_size;        ///< 对齐后的实际负载大小
} mem_alloc_header_t;

/**
 * @brief 内存池控制结构
 *
 * 管理单个内存池的所有信息和状态。
 */
typedef struct {
    uint8_t *buffer;            ///< 池内存缓冲区起始地址
    size_t total_size;          ///< 池总大小（字节）
    size_t used_size;           ///< 当前已使用大小（字节）
    size_t max_used;            ///< 历史最大使用量（字节）
    uint32_t alloc_count;       ///< 累计分配次数
    uint32_t free_count;        ///< 累计释放次数
    uint32_t fail_count;        ///< 分配失败次数
    sys_mem_pool_type_t type;   ///< 池类型
    struct k_mutex lock;        ///< 池互斥锁（线程安全）
    bool initialized;           ///< 池是否已初始化
} mem_pool_t;

/**
 * @brief 内存跟踪器结构
 *
 * 记录所有活跃分配，用于泄漏检测。
 */
typedef struct {
    sys_mem_alloc_info_t allocations[MAX_ALLOCATIONS];  ///< 分配记录数组
    uint32_t count;             ///< 当前记录数
    uint32_t max_records;       ///< 最大记录数
    bool tracking_enabled;      ///< 跟踪是否启用
    struct k_mutex lock;        ///< 跟踪器互斥锁
} mem_tracker_t;

/**
 * @brief 内存系统全局控制块
 *
 * 包含所有内存池、跟踪器和配置信息。
 */
typedef struct {
    mem_pool_t pools[SYS_MEM_POOL_COUNT];   ///< 内存池数组
    mem_tracker_t tracker;                  ///< 分配跟踪器
    sys_mem_config_t config;                ///< 系统配置
} sys_mem_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

/** 全局内存系统控制块 */
static sys_mem_cb_t g_sys_mem;

/** 静态分配的内存池缓冲区（每个池 DEFAULT_POOL_SIZE 字节） */
static uint8_t g_mem_buffer[SYS_MEM_POOL_COUNT][DEFAULT_POOL_SIZE];

/* =============================================================================
 * 内部辅助函数
 * ============================================================================= */

/**
 * @brief 根据类型获取内存池指针
 *
 * @param type 内存池类型
 * @return 成功返回池指针，失败返回 NULL
 */
static mem_pool_t *get_pool(sys_mem_pool_type_t type)
{
    if (type >= SYS_MEM_POOL_COUNT) {
        return NULL;
    }
    return &g_sys_mem.pools[type];
}

/**
 * @brief 向上对齐大小到指定边界
 *
 * 将给定大小对齐到 align 的整数倍。
 *
 * @param size 原始大小
 * @param align 对齐边界（必须为 2 的幂）
 * @param aligned 输出：对齐后的大小
 * @return 成功返回 true，失败返回 false
 *
 * @note 失败条件：aligned 为 NULL、align 为 0、溢出
 */
static bool align_up_size(size_t size, size_t align, size_t *aligned)
{
    if (aligned == NULL || align == 0U) {
        return false;
    }

    /* 检查溢出 */
    if (size > (SIZE_MAX - (align - 1U))) {
        return false;
    }

    /* 向上对齐：(size + align - 1) & ~(align - 1) */
    *aligned = (size + align - 1U) & ~(align - 1U);
    return true;
}

/**
 * @brief 从用户指针获取分配头部
 *
 * 用户指针 = 头部之后，所以头部地址 = 用户指针 - 头部大小
 *
 * @param ptr 用户指针
 * @return 分配头部指针，NULL 如果 ptr 为 NULL
 */
static mem_alloc_header_t *get_alloc_header(void *ptr)
{
    if (ptr == NULL) {
        return NULL;
    }

    return (mem_alloc_header_t *)((uint8_t *)ptr - sizeof(mem_alloc_header_t));
}

/**
 * @brief 检查指针是否在池内
 *
 * @param pool 内存池指针
 * @param ptr 要检查的指针
 * @return true 指针在池内，false 不在
 */
static bool ptr_in_pool(const mem_pool_t *pool, const void *ptr)
{
    if (pool == NULL || ptr == NULL || !pool->initialized) {
        return false;
    }

    uintptr_t start = (uintptr_t)pool->buffer;
    uintptr_t end = start + pool->total_size;
    uintptr_t addr = (uintptr_t)ptr;

    return (addr >= start && addr < end);
}

/**
 * @brief 查找包含指定指针的内存池
 *
 * 遍历所有池，找到包含给定指针的池。
 *
 * @param ptr 要查找的指针
 * @return 包含指针的池，未找到返回 NULL
 */
static mem_pool_t *find_pool_containing_ptr(const void *ptr)
{
    for (int i = 0; i < SYS_MEM_POOL_COUNT; i++) {
        mem_pool_t *pool = &g_sys_mem.pools[i];
        if (ptr_in_pool(pool, ptr)) {
            return pool;
        }
    }

    return NULL;
}

/**
 * @brief 从指针获取池和分配头部（带验证）
 *
 * 验证指针的有效性，包括魔数检查和池类型匹配。
 *
 * @param ptr 用户指针
 * @param header_out 输出：分配头部指针（可选）
 * @return 成功返回池指针，失败返回 NULL
 */
static mem_pool_t *get_pool_from_ptr(void *ptr, mem_alloc_header_t **header_out)
{
    /* 查找包含此指针的池 */
    mem_pool_t *pool = find_pool_containing_ptr(ptr);
    if (pool == NULL) {
        return NULL;
    }

    /* 获取分配头部 */
    mem_alloc_header_t *header = get_alloc_header(ptr);
    if (header == NULL || !ptr_in_pool(pool, header)) {
        return NULL;
    }

    /* 验证魔数和池类型 */
    if ((header->magic != MEMORY_MAGIC && header->magic != MEMORY_FREED_MAGIC) ||
        header->pool_type >= SYS_MEM_POOL_COUNT ||
        header->pool_type != (uint32_t)pool->type) {
        return NULL;
    }

    if (header_out != NULL) {
        *header_out = header;
    }

    return pool;
}

/**
 * @brief 添加分配记录到跟踪器
 *
 * @param ptr 分配的指针
 * @param size 分配大小
 *
 * @note 如跟踪器已满，新分配不会被记录
 * @note 线程安全（使用互斥锁）
 */
static void tracker_add(void *ptr, size_t size)
{
    if (!g_sys_mem.tracker.tracking_enabled || ptr == NULL) {
        return;
    }

    k_mutex_lock(&g_sys_mem.tracker.lock, K_FOREVER);

    if (g_sys_mem.tracker.count < g_sys_mem.tracker.max_records) {
        sys_mem_alloc_info_t *info = &g_sys_mem.tracker.allocations[g_sys_mem.tracker.count];
        info->ptr = ptr;
        info->size = size;
        info->timestamp = k_uptime_get_32();
        info->module = NULL;
        info->line = 0U;
        g_sys_mem.tracker.count++;
    }

    k_mutex_unlock(&g_sys_mem.tracker.lock);
}

/**
 * @brief 从跟踪器移除分配记录
 *
 * @param ptr 要移除的指针
 *
 * @note 使用 memmove 保持数组连续
 * @note 线程安全
 */
static void tracker_remove(void *ptr)
{
    if (!g_sys_mem.tracker.tracking_enabled || ptr == NULL) {
        return;
    }

    k_mutex_lock(&g_sys_mem.tracker.lock, K_FOREVER);

    for (uint32_t i = 0; i < g_sys_mem.tracker.count; i++) {
        if (g_sys_mem.tracker.allocations[i].ptr == ptr) {
            /* 找到记录，移动后续元素覆盖 */
            memmove(&g_sys_mem.tracker.allocations[i],
                    &g_sys_mem.tracker.allocations[i + 1],
                    (g_sys_mem.tracker.count - i - 1U) * sizeof(sys_mem_alloc_info_t));
            g_sys_mem.tracker.count--;
            break;
        }
    }

    k_mutex_unlock(&g_sys_mem.tracker.lock);
}

/**
 * @brief 从池分配内存（内部实现）
 *
 * 使用 bump allocator 策略：从池末尾顺序分配。
 *
 * @param pool 内存池指针
 * @param size 请求大小
 * @param zero 是否清零
 * @return 成功返回用户指针，失败返回 NULL
 *
 * @note 包含头部开销
 * @note 线程安全
 */
static void *pool_alloc(mem_pool_t *pool, size_t size, bool zero)
{
    if (pool == NULL || !pool->initialized) {
        return NULL;
    }

    /* 计算对齐后的大小 */
    size_t payload_size;
    if (!align_up_size(size, MEMORY_ALIGN_BYTES, &payload_size)) {
        k_mutex_lock(&pool->lock, K_FOREVER);
        pool->fail_count++;
        k_mutex_unlock(&pool->lock);
        return NULL;
    }

    /* 检查总大小是否溢出 */
    if (payload_size > (SIZE_MAX - sizeof(mem_alloc_header_t))) {
        k_mutex_lock(&pool->lock, K_FOREVER);
        pool->fail_count++;
        k_mutex_unlock(&pool->lock);
        return NULL;
    }

    size_t total_alloc_size = sizeof(mem_alloc_header_t) + payload_size;

    k_mutex_lock(&pool->lock, K_FOREVER);

    /* 检查池空间是否足够 */
    if (pool->used_size > pool->total_size ||
        total_alloc_size > (pool->total_size - pool->used_size)) {
        pool->fail_count++;
        k_mutex_unlock(&pool->lock);
        return NULL;
    }

    /* 分配内存：返回当前 used_size 位置 */
    uint8_t *raw_ptr = pool->buffer + pool->used_size;
    mem_alloc_header_t *header = (mem_alloc_header_t *)raw_ptr;
    void *user_ptr = raw_ptr + sizeof(mem_alloc_header_t);

    /* 初始化头部 */
    header->magic = MEMORY_MAGIC;
    header->pool_type = (uint32_t)pool->type;
    header->requested_size = size;
    header->payload_size = payload_size;

    /* 如需要，清零用户内存 */
    if (zero) {
        memset(user_ptr, 0, payload_size);
    }

    /* 更新池状态 */
    pool->used_size += total_alloc_size;
    pool->alloc_count++;

    /* 更新峰值使用量 */
    if (pool->used_size > pool->max_used) {
        pool->max_used = pool->used_size;
    }

    k_mutex_unlock(&pool->lock);

    /* 添加到跟踪器 */
    tracker_add(user_ptr, size);
    return user_ptr;
}

/**
 * @brief 释放内存回池（内部实现）
 *
 * 验证指针有效性，检测重复释放和损坏。
 *
 * @param expected_pool 期望的池（可为 NULL）
 * @param ptr 要释放的指针
 *
 * @note 实际池由指针的头部确定，不依赖 expected_pool
 * @note 线程安全
 */
static void pool_free(mem_pool_t *expected_pool, void *ptr)
{
    mem_alloc_header_t *header;
    /* 从指针获取实际池和头部 */
    mem_pool_t *actual_pool = get_pool_from_ptr(ptr, &header);

    if (actual_pool == NULL || header == NULL) {
        LOG_WRN("Ignoring invalid memory free: ptr=%p", ptr);
        return;
    }

    /* 检查池类型是否匹配（仅警告，不影响释放） */
    if (expected_pool != NULL && expected_pool != actual_pool) {
        LOG_WRN("Free pool mismatch: expected=%d actual=%d ptr=%p",
                expected_pool->type, actual_pool->type, ptr);
    }

    k_mutex_lock(&actual_pool->lock, K_FOREVER);

    /* 检测重复释放 */
    if (header->magic == MEMORY_FREED_MAGIC) {
        k_mutex_unlock(&actual_pool->lock);
        LOG_WRN("Double free detected: ptr=%p", ptr);
        return;
    }

    /* 检测头部损坏 */
    if (header->magic != MEMORY_MAGIC) {
        k_mutex_unlock(&actual_pool->lock);
        LOG_WRN("Corrupted allocation header: ptr=%p", ptr);
        return;
    }

    /* 标记为已释放 */
    header->magic = MEMORY_FREED_MAGIC;

    /* 更新计数器 */
    if (actual_pool->free_count < actual_pool->alloc_count) {
        actual_pool->free_count++;
    }

    k_mutex_unlock(&actual_pool->lock);

    /* 从跟踪器移除 */
    tracker_remove(ptr);
}

/**
 * @brief 获取分配的实际大小
 *
 * @param ptr 用户指针
 * @return 分配大小，0 表示无效指针
 */
static size_t get_allocation_size(void *ptr)
{
    mem_alloc_header_t *header;
    mem_pool_t *pool = get_pool_from_ptr(ptr, &header);

    if (pool == NULL || header == NULL) {
        return 0U;
    }

    return header->requested_size;
}

/* =============================================================================
 * 核心 API 实现
 * ============================================================================= */

int sys_mem_init(const sys_mem_config_t *config)
{
    LOG_INF("Initializing memory system...");

    /* 清零全局控制块 */
    memset(&g_sys_mem, 0, sizeof(g_sys_mem));

    /* 设置配置（使用传入或默认值） */
    if (config != NULL) {
        g_sys_mem.config = *config;
    } else {
        /* 默认配置 */
        g_sys_mem.config.pool_sizes[SYS_MEM_POOL_GENERAL] = DEFAULT_POOL_SIZE;
        g_sys_mem.config.pool_sizes[SYS_MEM_POOL_EVENT] = DEFAULT_POOL_SIZE / 2;
        g_sys_mem.config.pool_sizes[SYS_MEM_POOL_MODULE] = DEFAULT_POOL_SIZE / 2;
        g_sys_mem.config.pool_sizes[SYS_MEM_POOL_DMA] = 0;  /* 默认不启用 */
        g_sys_mem.config.enable_tracking = true;
        g_sys_mem.config.enable_defrag = false;
        g_sys_mem.config.max_allocations = MAX_ALLOCATIONS;
    }

    /* 初始化所有内存池 */
    for (int i = 0; i < SYS_MEM_POOL_COUNT; i++) {
        mem_pool_t *pool = &g_sys_mem.pools[i];
        size_t configured_size = g_sys_mem.config.pool_sizes[i];

        /* 如配置大小超过缓冲区，限制到最大值 */
        if (configured_size > DEFAULT_POOL_SIZE) {
            LOG_WRN("Pool %d size %u exceeds buffer %u, clamped",
                    i, (uint32_t)configured_size, (uint32_t)DEFAULT_POOL_SIZE);
            configured_size = DEFAULT_POOL_SIZE;
            g_sys_mem.config.pool_sizes[i] = DEFAULT_POOL_SIZE;
        }

        /* 初始化池结构 */
        pool->buffer = g_mem_buffer[i];
        pool->total_size = configured_size;
        pool->used_size = 0;
        pool->max_used = 0;
        pool->alloc_count = 0;
        pool->free_count = 0;
        pool->fail_count = 0;
        pool->type = (sys_mem_pool_type_t)i;
        pool->initialized = (pool->total_size > 0U);
        k_mutex_init(&pool->lock);

        if (pool->initialized) {
            LOG_DBG("Pool %d initialized: %u bytes", i, (uint32_t)pool->total_size);
        }
    }

    /* 初始化跟踪器 */
    g_sys_mem.tracker.count = 0;
    g_sys_mem.tracker.max_records = g_sys_mem.config.max_allocations;
    /* 限制最大记录数 */
    if (g_sys_mem.tracker.max_records == 0U || g_sys_mem.tracker.max_records > MAX_ALLOCATIONS) {
        g_sys_mem.tracker.max_records = MAX_ALLOCATIONS;
    }
    g_sys_mem.tracker.tracking_enabled = g_sys_mem.config.enable_tracking;
    k_mutex_init(&g_sys_mem.tracker.lock);

    LOG_INF("Memory system initialized");
    return 0;
}

void *sys_mem_alloc(sys_mem_pool_type_t type, size_t size)
{
    if (size == 0) {
        return NULL;
    }

    /* 获取指定池 */
    mem_pool_t *pool = get_pool(type);
    /* 如池不可用，回退到通用池 */
    if (pool == NULL || !pool->initialized) {
        pool = get_pool(SYS_MEM_POOL_GENERAL);
    }

    return pool_alloc(pool, size, false);
}

void *sys_mem_calloc(sys_mem_pool_type_t type, size_t size)
{
    if (size == 0) {
        return NULL;
    }

    mem_pool_t *pool = get_pool(type);
    if (pool == NULL || !pool->initialized) {
        pool = get_pool(SYS_MEM_POOL_GENERAL);
    }

    return pool_alloc(pool, size, true);
}

void sys_mem_free(sys_mem_pool_type_t type, void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    mem_pool_t *pool = get_pool(type);
    /* 如池未初始化，设为 NULL（pool_free 会自动处理） */
    if (pool != NULL && !pool->initialized) {
        pool = NULL;
    }

    pool_free(pool, ptr);
}

void *sys_mem_realloc(sys_mem_pool_type_t type, void *ptr, size_t size)
{
    /* ptr 为 NULL：等同于分配 */
    if (ptr == NULL) {
        return sys_mem_alloc(type, size);
    }

    /* size 为 0：等同于释放 */
    if (size == 0) {
        sys_mem_free(type, ptr);
        return NULL;
    }

    /* 获取原分配大小 */
    size_t old_size = get_allocation_size(ptr);
    if (old_size == 0U) {
        LOG_WRN("realloc on invalid pointer: %p", ptr);
        return NULL;
    }

    /* 分配新内存 */
    void *new_ptr = sys_mem_alloc(type, size);
    if (new_ptr != NULL) {
        /* 复制数据（取较小大小） */
        size_t copy_size = (old_size < size) ? old_size : size;
        memcpy(new_ptr, ptr, copy_size);
        /* 释放原内存 */
        sys_mem_free(type, ptr);
    }

    return new_ptr;
}

/* =============================================================================
 * 统计与调试 API 实现
 * ============================================================================= */

void sys_mem_get_stats(sys_mem_pool_type_t type, sys_mem_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    mem_pool_t *pool = get_pool(type);
    if (pool == NULL || !pool->initialized) {
        memset(stats, 0, sizeof(sys_mem_stats_t));
        return;
    }

    k_mutex_lock(&pool->lock, K_FOREVER);

    stats->total_size = pool->total_size;
    stats->used_size = pool->used_size;
    stats->free_size = pool->total_size - pool->used_size;
    stats->max_used = pool->max_used;
    stats->alloc_count = pool->alloc_count;
    stats->free_count = pool->free_count;
    stats->fail_count = pool->fail_count;

    /* 计算碎片化程度（简化版：峰值使用率百分比） */
    if (pool->total_size > 0) {
        stats->fragmentation = (pool->max_used * 100) / pool->total_size;
    }

    k_mutex_unlock(&pool->lock);
}

void sys_mem_reset_stats(sys_mem_pool_type_t type)
{
    mem_pool_t *pool = get_pool(type);
    if (pool == NULL || !pool->initialized) {
        return;
    }

    k_mutex_lock(&pool->lock, K_FOREVER);
    /* 重置统计计数器 */
    pool->max_used = pool->used_size;
    pool->alloc_count = 0;
    pool->free_count = 0;
    pool->fail_count = 0;
    k_mutex_unlock(&pool->lock);
}

uint32_t sys_mem_get_active_allocations(sys_mem_pool_type_t type)
{
    mem_pool_t *pool = get_pool(type);
    if (pool == NULL || !pool->initialized) {
        return 0;
    }

    uint32_t active;

    k_mutex_lock(&pool->lock, K_FOREVER);
    /* 活跃数 = 分配数 - 释放数 */
    active = (pool->alloc_count >= pool->free_count) ?
             (pool->alloc_count - pool->free_count) : 0U;
    k_mutex_unlock(&pool->lock);

    return active;
}

void sys_mem_dump_allocations(sys_mem_pool_type_t type)
{
    mem_pool_t *pool = get_pool(type);
    if (pool == NULL) {
        return;
    }

    /* 获取池统计快照 */
    size_t total_size;
    size_t used_size;
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t fail_count;

    k_mutex_lock(&pool->lock, K_FOREVER);
    total_size = pool->total_size;
    used_size = pool->used_size;
    alloc_count = pool->alloc_count;
    free_count = pool->free_count;
    fail_count = pool->fail_count;
    k_mutex_unlock(&pool->lock);

    /* 打印池信息 */
    printk("\n=== Memory Pool %d Dump ===\n", type);
    printk("Total: %u, Used: %u, Free: %u\n",
           (uint32_t)total_size,
           (uint32_t)used_size,
           (uint32_t)(total_size - used_size));
    printk("Allocations: %u, Frees: %u, Fails: %u\n",
           alloc_count, free_count, fail_count);

    /* 打印跟踪器中的活跃分配 */
    if (g_sys_mem.tracker.tracking_enabled) {
        k_mutex_lock(&g_sys_mem.tracker.lock, K_FOREVER);

        if (g_sys_mem.tracker.count > 0U) {
            printk("\nActive Allocations:\n");
            for (uint32_t i = 0; i < g_sys_mem.tracker.count; i++) {
                sys_mem_alloc_info_t *info = &g_sys_mem.tracker.allocations[i];
                mem_alloc_header_t *header = get_alloc_header(info->ptr);

                /* 仅打印属于此池的活跃分配 */
                if (header->magic == MEMORY_MAGIC && header->pool_type == (uint32_t)type) {
                    printk("  [%u] ptr=%p, size=%u, time=%u\n",
                           i, info->ptr, (uint32_t)info->size, info->timestamp);
                }
            }
        }

        k_mutex_unlock(&g_sys_mem.tracker.lock);
    }

    printk("=== End Dump ===\n\n");
}

uint32_t sys_mem_check_leaks(sys_mem_pool_type_t type)
{
    if (!g_sys_mem.tracker.tracking_enabled) {
        return 0U;
    }

    uint32_t leaks = 0U;

    k_mutex_lock(&g_sys_mem.tracker.lock, K_FOREVER);

    /* 如类型为无效值，检查所有池 */
    if (type >= SYS_MEM_POOL_COUNT) {
        leaks = g_sys_mem.tracker.count;
    } else {
        /* 仅检查指定池 */
        for (uint32_t i = 0; i < g_sys_mem.tracker.count; i++) {
            mem_alloc_header_t *header = get_alloc_header(g_sys_mem.tracker.allocations[i].ptr);
            if (header->magic == MEMORY_MAGIC && header->pool_type == (uint32_t)type) {
                leaks++;
            }
        }
    }

    k_mutex_unlock(&g_sys_mem.tracker.lock);

    return leaks;
}

size_t sys_mem_defrag(sys_mem_pool_type_t type)
{
    /* 简单实现：仅当所有分配都释放时才重置池 */
    /* 生产代码应实现更完善的碎片整理算法 */
    mem_pool_t *pool = get_pool(type);
    if (pool == NULL || !pool->initialized) {
        return 0;
    }

    /* 检查是否启用碎片整理 */
    if (!g_sys_mem.config.enable_defrag) {
        return 0;
    }

    k_mutex_lock(&pool->lock, K_FOREVER);

    size_t reclaimed = 0;
    /* 仅当所有分配都已释放时才能回收 */
    if (pool->alloc_count == pool->free_count) {
        reclaimed = pool->used_size;
        pool->used_size = 0;
    }

    k_mutex_unlock(&pool->lock);
    return reclaimed;
}

/* =============================================================================
 * 堆信息 API 实现
 * ============================================================================= */

size_t sys_mem_get_heap_size(void)
{
    size_t total = 0;
    for (int i = 0; i < SYS_MEM_POOL_COUNT; i++) {
        mem_pool_t *pool = &g_sys_mem.pools[i];
        if (!pool->initialized) {
            continue;
        }

        k_mutex_lock(&pool->lock, K_FOREVER);
        total += pool->total_size;
        k_mutex_unlock(&pool->lock);
    }
    return total;
}

size_t sys_mem_get_free_size(void)
{
    size_t free_size = 0;
    for (int i = 0; i < SYS_MEM_POOL_COUNT; i++) {
        mem_pool_t *pool = &g_sys_mem.pools[i];
        if (!pool->initialized) {
            continue;
        }

        k_mutex_lock(&pool->lock, K_FOREVER);
        free_size += pool->total_size - pool->used_size;
        k_mutex_unlock(&pool->lock);
    }
    return free_size;
}

size_t sys_mem_get_min_free_size(void)
{
    size_t min_free = SIZE_MAX;
    for (int i = 0; i < SYS_MEM_POOL_COUNT; i++) {
        mem_pool_t *pool = &g_sys_mem.pools[i];
        if (!pool->initialized) {
            continue;
        }

        k_mutex_lock(&pool->lock, K_FOREVER);
        /* 最小可用 = 总大小 - 峰值使用 */
        size_t free_size = pool->total_size - pool->max_used;
        k_mutex_unlock(&pool->lock);

        if (free_size < min_free) {
            min_free = free_size;
        }
    }
    return (min_free == SIZE_MAX) ? 0 : min_free;
}

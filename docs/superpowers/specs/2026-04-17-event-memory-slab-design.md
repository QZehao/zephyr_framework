# 事件系统内存管理优化设计

## 概述

为事件系统引入基于 Slab 的内存管理机制，解决 `k_malloc` 的内存碎片和分配时间不确定问题，满足硬实时场景需求。

---

## 设计目标

| 目标 | 说明 |
|------|------|
| 实时安全 | 分配/释放时间 O(1) 确定 |
| 零外部碎片 | 使用固定大小块分配 |
| 优先级隔离 | 高优先级事件有独立内存配额 |
| 可裁剪 | 适配从极小到极大内存设备 |
| 兼容性 | 保持现有 API 兼容 |

---

## 架构设计

### 内存池架构

```
┌─────────────────────────────────────────────────────┐
│                  事件内存架构                        │
├─────────────────────────────────────────────────────┤
│  CRITICAL 池 (独立预留)                              │
│  └─ slab_event[STRUCT_SIZE] × N                     │
├─────────────────────────────────────────────────────┤
│  HIGH 池 (独立预留)                                  │
│  └─ slab_event[STRUCT_SIZE] × M                     │
├─────────────────────────────────────────────────────┤
│  NORMAL/LOW 池 (共享)                               │
│  └─ slab_event[STRUCT_SIZE] × K                     │
├─────────────────────────────────────────────────────┤
│  大数据 Slab (共享，可裁剪)                          │
│  ├─ slab_256B                                       │
│  ├─ slab_1KB                                        │
│  └─ slab_4KB                                        │
└─────────────────────────────────────────────────────┘

注：STRUCT_SIZE 可配置为 32B/64B/128B
```

---

## 详细设计

### 1. event_t 结构体重构

#### 结构体定义

```c
/**
 * @brief 内联数据大小（可配置）
 *   - 32B 结构体: 内联 16B
 *   - 64B 结构体: 内联 48B
 *   - 128B 结构体: 内联 112B
 */
#ifndef CONFIG_EVENT_INLINE_DATA_SIZE
#define CONFIG_EVENT_INLINE_DATA_SIZE   48
#endif

#ifndef CONFIG_EVENT_STRUCT_SIZE
#define CONFIG_EVENT_STRUCT_SIZE        64
#endif

/**
 * @brief 事件数据结构
 *
 * 内存布局（64B 示例）：
 * ┌────────────────────────────────┐
 * │ type(1) priority(1) flags(1) ? │  4B
 * │ timestamp                      │  4B
 * │ source_id                      │  4B
 * │ data_len                       │  4B
 * ├────────────────────────────────┤  16B 头部
 * │ inline_data[48] 或 ptr(8)      │ 48B
 * └────────────────────────────────┘  64B 总计
 */
typedef struct {
    uint8_t          type;           /**< 事件类型 */
    uint8_t          priority;       /**< 事件优先级 */
    uint8_t          flags;          /**< 标志位 */
    uint8_t          reserved;       /**< 预留扩展 */
    uint32_t         timestamp;      /**< 事件创建时间戳 */
    uint32_t         source_id;      /**< 源模块/组件 ID */
    uint32_t         data_len;       /**< 事件数据长度 */
    union {
        uint8_t  inline_data[CONFIG_EVENT_INLINE_DATA_SIZE];
        void*    ptr;
    } data;
} event_t;

/* 标志位定义 */
#define EVENT_FLAG_DATA_INLINE   0x01  /**< 数据内联存储 */
#define EVENT_FLAG_DATA_DYNAMIC  0x02  /**< 数据动态分配 */
#define EVENT_FLAG_FROM_SLAB     0x04  /**< event_t 来自 slab 池 */
```

#### 编译时验证

```c
BUILD_ASSERT(CONFIG_EVENT_STRUCT_SIZE == 32 ||
             CONFIG_EVENT_STRUCT_SIZE == 64 ||
             CONFIG_EVENT_STRUCT_SIZE == 128,
             "EVENT_STRUCT_SIZE must be 32, 64, or 128");

BUILD_ASSERT(sizeof(event_t) == CONFIG_EVENT_STRUCT_SIZE,
             "event_t size mismatch with configuration");
```

---

### 2. Slab 内存池配置

#### Kconfig 配置项

```ini
# =============================================================================
# Event Structure Size Configuration
# =============================================================================

choice
    prompt "Event structure size"
    default EVENT_STRUCT_SIZE_64

config EVENT_STRUCT_SIZE_32
    bool "32 bytes (compact)"
config EVENT_STRUCT_SIZE_64
    bool "64 bytes (balanced)"
config EVENT_STRUCT_SIZE_128
    bool "128 bytes (high capacity)"

endchoice

# =============================================================================
# Event Slab Configuration
# =============================================================================

config EVENT_SLAB_ENABLE
    bool "Enable slab-based memory management"
    default y

if EVENT_SLAB_ENABLE

config EVENT_SLAB_CRITICAL_COUNT
    int "CRITICAL priority pool size"
    default 8
    range 0 64

config EVENT_SLAB_HIGH_COUNT
    int "HIGH priority pool size"
    default 16
    range 0 64

config EVENT_SLAB_NORMAL_COUNT
    int "NORMAL/LOW priority pool size"
    default 32
    range 4 128

config EVENT_SLAB_LARGE_ENABLE
    bool "Enable large data slab pools"
    default y

if EVENT_SLAB_LARGE_ENABLE

config EVENT_SLAB_LARGE_256_COUNT
    int "256-byte block count"
    default 8
    range 0 32

config EVENT_SLAB_LARGE_1K_COUNT
    int "1KB block count"
    default 4
    range 0 16

config EVENT_SLAB_LARGE_4K_COUNT
    int "4KB block count"
    default 2
    range 0 8

endif # EVENT_SLAB_LARGE_ENABLE

endif # EVENT_SLAB_ENABLE

# =============================================================================
# Extended Features
# =============================================================================

config EVENT_SLAB_EXHAUSTED_CB
    bool "Enable slab exhausted callback"
    default n

config EVENT_DEBUG_MEM
    bool "Enable memory debugging support"
    default n

config EVENT_RUNTIME_STATUS
    bool "Enable runtime pool status query"
    default y

config EVENT_SLAB_STATS_DETAILED
    bool "Enable detailed slab statistics"
    default n
    depends on EVENT_RUNTIME_STATUS
```

#### Slab 池定义

```c
#if CONFIG_EVENT_SLAB_ENABLE

/* CRITICAL 优先级池 */
#if CONFIG_EVENT_SLAB_CRITICAL_COUNT > 0
K_MEM_SLAB_DEFINE(event_slab_critical, EVENT_STRUCT_SIZE,
                  CONFIG_EVENT_SLAB_CRITICAL_COUNT, 4);
#define EVENT_SLAB_CRITICAL_AVAILABLE 1
#else
#define EVENT_SLAB_CRITICAL_AVAILABLE 0
#endif

/* HIGH 优先级池 */
#if CONFIG_EVENT_SLAB_HIGH_COUNT > 0
K_MEM_SLAB_DEFINE(event_slab_high, EVENT_STRUCT_SIZE,
                  CONFIG_EVENT_SLAB_HIGH_COUNT, 4);
#define EVENT_SLAB_HIGH_AVAILABLE 1
#else
#define EVENT_SLAB_HIGH_AVAILABLE 0
#endif

/* NORMAL/LOW 优先级池（必须存在） */
K_MEM_SLAB_DEFINE(event_slab_normal, EVENT_STRUCT_SIZE,
                  CONFIG_EVENT_SLAB_NORMAL_COUNT, 4);

/* 大数据 slab 池 */
#if CONFIG_EVENT_SLAB_LARGE_ENABLE
#define EVENT_SLAB_LARGE_AVAILABLE 1

#if CONFIG_EVENT_SLAB_LARGE_256_COUNT > 0
K_MEM_SLAB_DEFINE(event_slab_data_256, 256,
                  CONFIG_EVENT_SLAB_LARGE_256_COUNT, 4);
#define EVENT_SLAB_256_AVAILABLE 1
#endif

#if CONFIG_EVENT_SLAB_LARGE_1K_COUNT > 0
K_MEM_SLAB_DEFINE(event_slab_data_1k, 1024,
                  CONFIG_EVENT_SLAB_LARGE_1K_COUNT, 4);
#define EVENT_SLAB_1K_AVAILABLE 1
#endif

#if CONFIG_EVENT_SLAB_LARGE_4K_COUNT > 0
K_MEM_SLAB_DEFINE(event_slab_data_4k, 4096,
                  CONFIG_EVENT_SLAB_LARGE_4K_COUNT, 4);
#define EVENT_SLAB_4K_AVAILABLE 1
#endif

#else
#define EVENT_SLAB_LARGE_AVAILABLE 0
#endif

#else
#define EVENT_SLAB_CRITICAL_AVAILABLE 0
#define EVENT_SLAB_HIGH_AVAILABLE 0
#define EVENT_SLAB_LARGE_AVAILABLE 0
#endif
```

---

### 3. 内存分配 API

#### API 概览

| API | 内存来源 | 实时安全 | 使用场景 |
|-----|----------|----------|----------|
| `event_create_rt` | 仅 Slab | ✅ | 硬实时线程 |
| `event_create_with_data_rt` | 内联/Slab | ✅ | 硬实时线程 |
| `event_create` | Slab → k_malloc | ❌ | 初始化/非关键 |
| `event_create_with_data` | 内联/Slab → k_malloc | ❌ | 非关键路径 |
| `event_free` | 自动判断 | ✅ | 所有场景 |

#### 实时安全 API

```c
/**
 * @brief 创建事件（实时安全）
 *
 * @note 完全从 slab 池分配，分配时间 O(1) 确定
 * @note Slab 耗尽时返回 NULL，不回退 k_malloc
 */
event_t* event_create_rt(event_type_t type, event_priority_t priority);

/**
 * @brief 创建带数据的事件（实时安全）
 *
 * @note 数据存储策略：
 *   - data_len ≤ INLINE_DATA_SIZE: 内联存储
 *   - data_len > INLINE_DATA_SIZE: 从 slab 池分配
 *   - 无可用 slab 或 slab 满: 返回 NULL
 */
event_t* event_create_with_data_rt(event_type_t type, event_priority_t priority,
                                    const void* data, size_t data_len);
```

#### 灵活模式 API

```c
/**
 * @brief 创建事件（灵活模式）
 *
 * @note 优先从 slab 分配，slab 耗尽时回退 k_malloc
 * @note 回退路径非实时安全
 */
event_t* event_create(event_type_t type, event_priority_t priority);

/**
 * @brief 创建带数据的事件（灵活模式）
 *
 * @note 优先使用内联/slab，不可用时回退 k_malloc
 */
event_t* event_create_with_data(event_type_t type, event_priority_t priority,
                                 const void* data, size_t data_len);
```

#### 释放 API

```c
/**
 * @brief 释放事件
 *
 * @note 自动判断内存来源（内联/slab/k_malloc）并正确释放
 */
void event_free(event_t* event);
```

#### 发布 API

```c
/**
 * @brief 发布事件并复制数据（实时安全）
 */
event_status_t event_publish_copy_rt(event_type_t type, event_priority_t priority,
                                      const void* data, size_t data_len);

/**
 * @brief 发布事件并复制数据（灵活模式）
 */
event_status_t event_publish_copy(event_type_t type, event_priority_t priority,
                                   const void* data, size_t data_len);
```

---

### 4. 扩展功能

#### Slab 耗尽回调（CONFIG_EVENT_SLAB_EXHAUSTED_CB）

```c
typedef void (*event_slab_exhausted_cb_t)(event_priority_t priority, size_t data_size);

void event_register_slab_exhausted_cb(event_slab_exhausted_cb_t cb);
```

#### 内存调试支持（CONFIG_EVENT_DEBUG_MEM）

```c
/**
 * @brief 检查是否有内存泄漏
 * @return 未释放的事件数量
 */
uint32_t event_check_leaks(void);

/**
 * @brief 打印所有未释放事件的详细信息
 */
void event_dump_leaks(void);
```

#### 运行时状态查询（CONFIG_EVENT_RUNTIME_STATUS）

```c
/**
 * @brief 检查指定优先级的 slab 是否可用
 */
bool event_slab_available(event_priority_t priority);

/**
 * @brief 获取指定优先级 slab 的剩余块数
 */
uint32_t event_slab_remaining(event_priority_t priority);

/**
 * @brief Slab 统计信息
 */
typedef struct {
    uint32_t critical_used;
    uint32_t critical_total;
    uint32_t high_used;
    uint32_t high_total;
    uint32_t normal_used;
    uint32_t normal_total;
    uint32_t data_256_used;
    uint32_t data_1k_used;
    uint32_t data_4k_used;
    uint32_t fallback_count;
} event_slab_stats_t;

void event_get_slab_stats(event_slab_stats_t* stats);
```

#### ISR 专用 API

```c
/**
 * @brief 从 ISR 创建事件（实时安全）
 */
static inline event_t* event_create_from_isr(event_type_t type,
                                              event_priority_t priority,
                                              const void* data, size_t data_len) {
    return event_create_with_data_rt(type, priority, data, data_len);
}
```

---

### 5. 配置预设

| 设备类型 | RAM | 结构体大小 | CRITICAL | HIGH | NORMAL | 大数据 |
|----------|-----|------------|----------|------|--------|--------|
| 极小 | <8KB | 32B | 2 | 4 | 8 | 禁用 |
| 小 | 8-16KB | 32B | 4 | 8 | 16 | 256B×2 |
| 中 | 16-64KB | 64B | 4 | 8 | 16 | 256B×4, 1K×2 |
| 大 | 64-256KB | 64B | 8 | 16 | 32 | 全部启用 |
| 极大 | >256KB | 128B | 16 | 32 | 64 | 全部启用 |

---

### 6. 迁移指南

| 场景 | 旧 API | 新 API |
|------|--------|--------|
| 中断上下文 | `event_publish_from_isr` | 不变 |
| 硬实时线程 | `event_create` | `event_create_rt` |
| 初始化阶段 | `event_create` | 不变 |
| 非关键路径 | `event_create` | 不变 |

---

## 编译时验证

```c
/* 基础约束 */
BUILD_ASSERT(CONFIG_EVENT_SLAB_NORMAL_COUNT >= 4,
             "NORMAL slab count must be at least 4");

BUILD_ASSERT(CONFIG_EVENT_INLINE_DATA_SIZE >= 4,
             "INLINE_DATA_SIZE must be at least 4 bytes");

BUILD_ASSERT(CONFIG_EVENT_INLINE_DATA_SIZE <= 128,
             "INLINE_DATA_SIZE must not exceed 128 bytes");

/* 优先级池约束 */
#if CONFIG_EVENT_SLAB_ENABLE
BUILD_ASSERT(CONFIG_EVENT_SLAB_CRITICAL_COUNT + 
             CONFIG_EVENT_SLAB_HIGH_COUNT + 
             CONFIG_EVENT_SLAB_NORMAL_COUNT <= 256,
             "Total event slab count exceeds 256");
#endif
```

---

## 内存占用估算

### 默认配置（64B 结构体）

| 池类型 | 块大小 | 数量 | 小计 |
|--------|--------|------|------|
| CRITICAL event | 64B | 8 | 512B |
| HIGH event | 64B | 16 | 1KB |
| NORMAL event | 64B | 32 | 2KB |
| data_256 | 256B | 8 | 2KB |
| data_1k | 1KB | 4 | 4KB |
| data_4k | 4KB | 2 | 8KB |
| **总计** | | | **17.5KB** |

### 最小配置（32B 结构体）

| 池类型 | 块大小 | 数量 | 小计 |
|--------|--------|------|------|
| CRITICAL event | 32B | 2 | 64B |
| HIGH event | 32B | 4 | 128B |
| NORMAL event | 32B | 8 | 256B |
| **总计** | | | **448B** |

---

## 线程安全

| 操作 | 线程安全 | 说明 |
|------|----------|------|
| `k_mem_slab_alloc` | ✅ | 内部有锁保护 |
| `k_mem_slab_free` | ✅ | 内部有锁保护 |
| 统计信息读取 | ⚠️ | 需要 mutex 保护 |
| ISR 调用 | ✅ | 使用 K_NO_WAIT |

---

## 实现文件

| 文件 | 说明 |
|------|------|
| `src/core/event_system.h` | 结构体定义、API 声明 |
| `src/core/event_system.c` | 核心实现 |
| `Kconfig` | 配置项定义 |

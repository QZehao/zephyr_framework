/**
 * @file event_dispatcher_autoinit.c
 * @brief 事件分发器自动初始化
 *
 * 为事件分发器提供 SYS_INIT 自动初始化机制。
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-04-10
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-10       1.0            zeh            初始版本
 *
 */

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include "app_config.h"
#include "event_dispatcher.h"

LOG_MODULE_REGISTER(event_dispatcher_autoinit, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * SYS_INIT 自动初始化
 * ============================================================================= */

static int event_dispatcher_auto_init(void) {
    dispatcher_config_t dispatcher_config = {.stack_size = CONFIG_EVENT_DISPATCHER_STACK_SIZE,
                                             .priority = CONFIG_EVENT_DISPATCHER_PRIORITY,
                                             .thread_name = "event_disp",
                                             .enable_stats = APP_CONFIG_ENABLE_STATS,
                                             .max_events_per_cycle = 100};
    if (event_dispatcher_init(&dispatcher_config) != EVENT_OK) {
        LOG_ERR("event_dispatcher_init failed");
        return -EIO;
    }

    if (event_dispatcher_start() != EVENT_OK) {
        LOG_ERR("event_dispatcher_start failed");
        return -EIO;
    }

    LOG_INF("Event dispatcher initialized and started");
    return 0;
}

SYS_INIT(event_dispatcher_auto_init, POST_KERNEL, APP_INIT_PRIO_DISPATCHER);

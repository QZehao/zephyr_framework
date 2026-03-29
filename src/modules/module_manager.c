/**
 * @file module_manager.c
 * @brief Module Manager Implementation
 * 
 * Dynamic module registration, lifecycle management, and communication.
 * 
 * @copyright Copyright (c) 2026
 * @license SPDX-License-Identifier: Apache-2.0
 */

#include "module_manager.h"
#include "event_system.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(module_manager, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * Internal Data Structures
 * ============================================================================= */

typedef struct {
    module_info_t modules[CONFIG_MAX_MODULES];
    uint32_t module_count;
    uint32_t next_module_id;
    module_mgr_stats_t stats;
    module_mgr_callback_t callback;
    void *callback_user_data;
    struct k_mutex lock;
    bool initialized;
    bool running;
} module_manager_cb_t;

/* =============================================================================
 * Static Variables
 * ============================================================================= */

static module_manager_cb_t g_module_mgr;

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static void module_event_handler(const event_t *event, void *user_data);
static void notify_callback(uint32_t module_id, module_mgr_event_t event);

/* =============================================================================
 * Core API Implementation
 * ============================================================================= */

int module_manager_init(void)
{
    LOG_INF("Initializing module manager...");

    memset(&g_module_mgr, 0, sizeof(g_module_mgr));
    k_mutex_init(&g_module_mgr.lock);

    /* Initialize module slots */
    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        g_module_mgr.modules[i].status = MODULE_STATUS_UNINITIALIZED;
        g_module_mgr.modules[i].id = 0;
    }

    g_module_mgr.next_module_id = 1;
    g_module_mgr.initialized = true;

    LOG_INF("Module manager initialized");
    return 0;
}

int module_manager_start(void)
{
    if (!g_module_mgr.initialized) {
        return -1;
    }

    k_mutex_lock(&g_module_mgr.lock, K_FOREVER);
    g_module_mgr.running = true;
    k_mutex_unlock(&g_module_mgr.lock);

    LOG_INF("Module manager started");
    return 0;
}

int module_manager_stop(void)
{
    if (!g_module_mgr.initialized) {
        return -1;
    }

    /* Stop all modules first */
    module_manager_stop_all();

    k_mutex_lock(&g_module_mgr.lock, K_FOREVER);
    g_module_mgr.running = false;
    k_mutex_unlock(&g_module_mgr.lock);

    LOG_INF("Module manager stopped");
    return 0;
}

int module_manager_shutdown(void)
{
    LOG_INF("Shutting down module manager...");

    module_manager_stop();

    /* Shutdown all modules */
    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        module_info_t *info = &g_module_mgr.modules[i];
        if (info->status != MODULE_STATUS_UNINITIALIZED &&
            info->interface != NULL &&
            info->interface->shutdown != NULL) {
            info->interface->shutdown();
            info->status = MODULE_STATUS_UNINITIALIZED;
        }
    }

    g_module_mgr.initialized = false;
    LOG_INF("Module manager shutdown complete");
    return 0;
}

/* =============================================================================
 * Module Registration API
 * ============================================================================= */

int module_manager_register(const module_interface_t *interface,
                            void *config,
                            uint32_t *module_id)
{
    if (!g_module_mgr.initialized || interface == NULL) {
        return -1;
    }

    k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

    if (g_module_mgr.module_count >= CONFIG_MAX_MODULES) {
        k_mutex_unlock(&g_module_mgr.lock);
        LOG_ERR("Maximum module count reached");
        return -1;
    }

    /* Find free slot */
    module_info_t *info = NULL;
    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        if (g_module_mgr.modules[i].status == MODULE_STATUS_UNINITIALIZED) {
            info = &g_module_mgr.modules[i];
            break;
        }
    }

    if (info == NULL) {
        k_mutex_unlock(&g_module_mgr.lock);
        return -1;
    }

    /* Initialize module info */
    info->interface = interface;
    info->config = config;
    info->internal_data = NULL;
    info->status = MODULE_STATUS_INITIALIZING;
    info->id = g_module_mgr.next_module_id++;
    info->event_subscriber_id = 0;

    if (module_id != NULL) {
        *module_id = info->id;
    }

    g_module_mgr.module_count++;
    g_module_mgr.stats.total_modules++;

    k_mutex_unlock(&g_module_mgr.lock);

    /* Call module init */
    if (interface->init != NULL) {
        int ret = interface->init(config);
        if (ret != 0) {
            LOG_ERR("Module '%s' init failed: %d", interface->name, ret);
            info->status = MODULE_STATUS_ERROR;
            g_module_mgr.stats.error_modules++;
            return ret;
        }
    }

    info->status = MODULE_STATUS_INITIALIZED;
    LOG_INF("Module registered: %s (id=%d)", interface->name, info->id);

    notify_callback(info->id, MODULE_MGR_EVENT_REGISTERED);
    return 0;
}

int module_manager_unregister(uint32_t module_id)
{
    if (!g_module_mgr.initialized || module_id == 0) {
        return -1;
    }

    k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

    module_info_t *info = NULL;
    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        if (g_module_mgr.modules[i].id == module_id) {
            info = &g_module_mgr.modules[i];
            break;
        }
    }

    if (info == NULL) {
        k_mutex_unlock(&g_module_mgr.lock);
        return -1;
    }

    /* Stop module if running */
    if (info->status == MODULE_STATUS_RUNNING) {
        k_mutex_unlock(&g_module_mgr.lock);
        module_manager_stop_module(module_id);
        k_mutex_lock(&g_module_mgr.lock, K_FOREVER);
    }

    /* Call shutdown */
    if (info->interface != NULL && info->interface->shutdown != NULL) {
        info->interface->shutdown();
    }

    /* Clear module info */
    info->interface = NULL;
    info->config = NULL;
    info->internal_data = NULL;
    info->status = MODULE_STATUS_UNINITIALIZED;
    info->id = 0;

    g_module_mgr.module_count--;

    k_mutex_unlock(&g_module_mgr.lock);

    LOG_INF("Module unregistered: id=%d", module_id);
    notify_callback(module_id, MODULE_MGR_EVENT_UNREGISTERED);
    return 0;
}

module_info_t *module_manager_get_module(uint32_t module_id)
{
    if (!g_module_mgr.initialized || module_id == 0) {
        return NULL;
    }

    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        if (g_module_mgr.modules[i].id == module_id) {
            return &g_module_mgr.modules[i];
        }
    }

    return NULL;
}

uint32_t module_manager_get_id_by_name(const char *name)
{
    if (!g_module_mgr.initialized || name == NULL) {
        return 0;
    }

    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        if (g_module_mgr.modules[i].interface != NULL &&
            g_module_mgr.modules[i].interface->name != NULL &&
            strcmp(g_module_mgr.modules[i].interface->name, name) == 0) {
            return g_module_mgr.modules[i].id;
        }
    }

    return 0;
}

void module_manager_foreach(void (*callback)(module_info_t *, void *),
                            void *user_data)
{
    if (!g_module_mgr.initialized || callback == NULL) {
        return;
    }

    k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        if (g_module_mgr.modules[i].status != MODULE_STATUS_UNINITIALIZED) {
            callback(&g_module_mgr.modules[i], user_data);
        }
    }

    k_mutex_unlock(&g_module_mgr.lock);
}

/* =============================================================================
 * Module Lifecycle API
 * ============================================================================= */

int module_manager_start_module(uint32_t module_id)
{
    module_info_t *info = module_manager_get_module(module_id);
    if (info == NULL) {
        return -1;
    }

    if (info->status != MODULE_STATUS_INITIALIZED &&
        info->status != MODULE_STATUS_STOPPED) {
        return -1;
    }

    if (info->interface->start != NULL) {
        int ret = info->interface->start();
        if (ret != 0) {
            LOG_ERR("Module '%s' start failed: %d", info->interface->name, ret);
            info->status = MODULE_STATUS_ERROR;
            return ret;
        }
    }

    info->status = MODULE_STATUS_RUNNING;
    g_module_mgr.stats.active_modules++;

    LOG_INF("Module started: %s", info->interface->name);
    notify_callback(module_id, MODULE_MGR_EVENT_STARTED);
    return 0;
}

int module_manager_stop_module(uint32_t module_id)
{
    module_info_t *info = module_manager_get_module(module_id);
    if (info == NULL) {
        return -1;
    }

    if (info->status != MODULE_STATUS_RUNNING) {
        return 0;
    }

    if (info->interface->stop != NULL) {
        info->interface->stop();
    }

    info->status = MODULE_STATUS_STOPPED;
    if (g_module_mgr.stats.active_modules > 0) {
        g_module_mgr.stats.active_modules--;
    }

    LOG_INF("Module stopped: %s", info->interface->name);
    notify_callback(module_id, MODULE_MGR_EVENT_STOPPED);
    return 0;
}

int module_manager_start_all(void)
{
    int started = 0;

    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        if (g_module_mgr.modules[i].status == MODULE_STATUS_INITIALIZED ||
            g_module_mgr.modules[i].status == MODULE_STATUS_STOPPED) {
            if (module_manager_start_module(g_module_mgr.modules[i].id) == 0) {
                started++;
            }
        }
    }

    return started;
}

int module_manager_stop_all(void)
{
    int stopped = 0;

    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        if (g_module_mgr.modules[i].status == MODULE_STATUS_RUNNING) {
            if (module_manager_stop_module(g_module_mgr.modules[i].id) == 0) {
                stopped++;
            }
        }
    }

    return stopped;
}

int module_manager_suspend_module(uint32_t module_id)
{
    module_info_t *info = module_manager_get_module(module_id);
    if (info == NULL) {
        return -1;
    }

    if (info->status != MODULE_STATUS_RUNNING) {
        return -1;
    }

    info->status = MODULE_STATUS_SUSPENDED;
    LOG_INF("Module suspended: %s", info->interface->name);
    return 0;
}

int module_manager_resume_module(uint32_t module_id)
{
    module_info_t *info = module_manager_get_module(module_id);
    if (info == NULL) {
        return -1;
    }

    if (info->status != MODULE_STATUS_SUSPENDED) {
        return -1;
    }

    info->status = MODULE_STATUS_RUNNING;
    LOG_INF("Module resumed: %s", info->interface->name);
    return 0;
}

/* =============================================================================
 * Event Handling API
 * ============================================================================= */

int module_manager_subscribe(uint32_t module_id, event_type_t event_type)
{
    module_info_t *info = module_manager_get_module(module_id);
    if (info == NULL || info->interface == NULL) {
        return -1;
    }

    if (info->interface->on_event == NULL) {
        return -1;
    }

    uint32_t subscriber_id;
    event_status_t status = event_subscribe(event_type,
                                            module_event_handler,
                                            info,
                                            &subscriber_id);

    if (status == EVENT_OK) {
        info->event_subscriber_id = subscriber_id;
        return 0;
    }

    return -1;
}

int module_manager_unsubscribe(uint32_t module_id, event_type_t event_type)
{
    module_info_t *info = module_manager_get_module(module_id);
    if (info == NULL) {
        return -1;
    }

    if (info->event_subscriber_id != 0) {
        event_unsubscribe(event_type, info->event_subscriber_id);
        info->event_subscriber_id = 0;
    }

    return 0;
}

int module_manager_send_to_module(uint32_t module_id, const event_t *event)
{
    module_info_t *info = module_manager_get_module(module_id);
    if (info == NULL || info->interface == NULL) {
        return -1;
    }

    if (info->status != MODULE_STATUS_RUNNING) {
        return -1;
    }

    if (info->interface->on_event != NULL) {
        info->interface->on_event(event, info->internal_data);
        g_module_mgr.stats.events_processed++;
        return 0;
    }

    return -1;
}

int module_manager_broadcast(const event_t *event)
{
    if (event == NULL) {
        return 0;
    }

    int count = 0;

    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        module_info_t *info = &g_module_mgr.modules[i];
        if (info->status == MODULE_STATUS_RUNNING &&
            info->interface != NULL &&
            info->interface->on_event != NULL) {
            info->interface->on_event(event, info->internal_data);
            count++;
        }
    }

    g_module_mgr.stats.events_processed += count;
    return count;
}

/* =============================================================================
 * Statistics & Debug API
 * ============================================================================= */

void module_manager_get_stats(module_mgr_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    k_mutex_lock(&g_module_mgr.lock, K_FOREVER);
    *stats = g_module_mgr.stats;
    k_mutex_unlock(&g_module_mgr.lock);
}

void module_manager_reset_stats(void)
{
    k_mutex_lock(&g_module_mgr.lock, K_FOREVER);
    memset(&g_module_mgr.stats, 0, sizeof(g_module_mgr.stats));
    k_mutex_unlock(&g_module_mgr.lock);
}

void module_manager_dump_info(void)
{
    printk("\n=== Module Manager Info ===\n");
    printk("Total modules: %d / %d\n", g_module_mgr.module_count, CONFIG_MAX_MODULES);
    printk("Active: %d, Errors: %d\n\n",
           g_module_mgr.stats.active_modules,
           g_module_mgr.stats.error_modules);

    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        module_info_t *info = &g_module_mgr.modules[i];
        if (info->status != MODULE_STATUS_UNINITIALIZED) {
            const char *status_str;
            switch (info->status) {
                case MODULE_STATUS_INITIALIZED: status_str = "INIT"; break;
                case MODULE_STATUS_RUNNING: status_str = "RUNNING"; break;
                case MODULE_STATUS_STOPPED: status_str = "STOPPED"; break;
                case MODULE_STATUS_ERROR: status_str = "ERROR"; break;
                case MODULE_STATUS_SUSPENDED: status_str = "SUSPENDED"; break;
                default: status_str = "UNKNOWN"; break;
            }

            printk("  [%d] %s - %s (v%d.%d.%d)\n",
                   info->id,
                   info->interface != NULL ? info->interface->name : "N/A",
                   status_str,
                   info->interface != NULL ? MODULE_VERSION_MAJOR(info->interface->version) : 0,
                   info->interface != NULL ? MODULE_VERSION_MINOR(info->interface->version) : 0,
                   info->interface != NULL ? MODULE_VERSION_PATCH(info->interface->version) : 0);
        }
    }

    printk("\n");
}

void module_manager_set_callback(module_mgr_callback_t callback, void *user_data)
{
    g_module_mgr.callback = callback;
    g_module_mgr.callback_user_data = user_data;
}

/* =============================================================================
 * Internal Functions
 * ============================================================================= */

static void module_event_handler(const event_t *event, void *user_data)
{
    if (event == NULL || user_data == NULL) {
        return;
    }

    module_info_t *info = (module_info_t *)user_data;

    if (info->status != MODULE_STATUS_RUNNING) {
        return;
    }

    if (info->interface != NULL && info->interface->on_event != NULL) {
        info->interface->on_event(event, info->internal_data);
    }
}

static void notify_callback(uint32_t module_id, module_mgr_event_t event)
{
    if (g_module_mgr.callback != NULL) {
        g_module_mgr.callback(module_id, event, g_module_mgr.callback_user_data);
    }
}

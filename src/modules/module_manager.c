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
#include <stdint.h>
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

typedef struct {
	uint32_t id;
	module_priority_t priority;
} start_order_entry_t;

/* =============================================================================
 * Static Variables
 * ============================================================================= */

static module_manager_cb_t g_module_mgr;

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static module_info_t *find_module_by_id_locked(uint32_t module_id);
static void clear_module_slot_unlocked(module_info_t *info);
static void module_event_clear_all_unlocked(module_info_t *info);
static void module_event_handler(const event_t *event, void *user_data);
static void notify_callback_unlocked(uint32_t module_id, module_mgr_event_t evt);

/* =============================================================================
 * Internal Helpers (lock must be held unless noted)
 * ============================================================================= */

static module_info_t *find_module_by_id_locked(uint32_t module_id)
{
	if (module_id == 0U) {
		return NULL;
	}

	for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
		if (g_module_mgr.modules[i].id == module_id) {
			return &g_module_mgr.modules[i];
		}
	}

	return NULL;
}

static void clear_module_slot_unlocked(module_info_t *info)
{
	if (info == NULL) {
		return;
	}

	info->interface = NULL;
	info->config = NULL;
	info->internal_data = NULL;
	info->status = MODULE_STATUS_UNINITIALIZED;
	info->id = 0U;
	info->event_subscription_count = 0U;
	(void)memset(info->event_subscriptions, 0, sizeof(info->event_subscriptions));
}

static void module_event_clear_all_unlocked(module_info_t *info)
{
	if (info == NULL) {
		return;
	}

	for (uint8_t i = 0; i < info->event_subscription_count; i++) {
		(void)event_unsubscribe(info->event_subscriptions[i].type,
				       info->event_subscriptions[i].subscriber_id);
	}

	info->event_subscription_count = 0U;
	(void)memset(info->event_subscriptions, 0, sizeof(info->event_subscriptions));
}

static void notify_callback_unlocked(uint32_t module_id, module_mgr_event_t evt)
{
	module_mgr_callback_t cb;
	void *ud;

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);
	cb = g_module_mgr.callback;
	ud = g_module_mgr.callback_user_data;
	k_mutex_unlock(&g_module_mgr.lock);

	if (cb != NULL) {
		cb(module_id, evt, ud);
	}
}

static int find_event_sub_index(const module_info_t *info, event_type_t type)
{
	for (uint8_t i = 0; i < info->event_subscription_count; i++) {
		if (info->event_subscriptions[i].type == type) {
			return (int)i;
		}
	}
	return -1;
}

static void sort_start_entries(start_order_entry_t *entries, int n)
{
	for (int i = 0; i < n - 1; i++) {
		for (int j = i + 1; j < n; j++) {
			if ((int)entries[j].priority < (int)entries[i].priority) {
				start_order_entry_t t = entries[i];

				entries[i] = entries[j];
				entries[j] = t;
			}
		}
	}
}

/* =============================================================================
 * Core API Implementation
 * ============================================================================= */

int module_manager_init(void)
{
	LOG_INF("Initializing module manager...");

	(void)memset(&g_module_mgr, 0, sizeof(g_module_mgr));
	k_mutex_init(&g_module_mgr.lock);

	for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
		g_module_mgr.modules[i].status = MODULE_STATUS_UNINITIALIZED;
		g_module_mgr.modules[i].id = 0U;
	}

	g_module_mgr.next_module_id = 1U;
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

	(void)module_manager_stop_all();

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);
	g_module_mgr.running = false;
	k_mutex_unlock(&g_module_mgr.lock);

	LOG_INF("Module manager stopped");
	return 0;
}

int module_manager_shutdown(void)
{
	int (*shutdown_fn[CONFIG_MAX_MODULES])(void);
	bool need_shutdown[CONFIG_MAX_MODULES];

	LOG_INF("Shutting down module manager...");

	(void)module_manager_stop();

	(void)memset(need_shutdown, 0, sizeof(need_shutdown));
	(void)memset(shutdown_fn, 0, sizeof(shutdown_fn));

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
		module_info_t *info = &g_module_mgr.modules[i];

		module_event_clear_all_unlocked(info);

		if (info->status != MODULE_STATUS_UNINITIALIZED && info->interface != NULL &&
		    info->interface->shutdown != NULL) {
			shutdown_fn[i] = info->interface->shutdown;
			need_shutdown[i] = true;
		}
	}

	k_mutex_unlock(&g_module_mgr.lock);

	for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
		if (need_shutdown[i] && shutdown_fn[i] != NULL) {
			shutdown_fn[i]();
		}
	}

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
		clear_module_slot_unlocked(&g_module_mgr.modules[i]);
	}

	g_module_mgr.module_count = 0U;
	(void)memset(&g_module_mgr.stats, 0, sizeof(g_module_mgr.stats));
	g_module_mgr.initialized = false;

	k_mutex_unlock(&g_module_mgr.lock);

	LOG_INF("Module manager shutdown complete");
	return 0;
}

/* =============================================================================
 * Module Registration API
 * ============================================================================= */

int module_manager_register(const module_interface_t *interface, void *config,
			    uint32_t *module_id)
{
	if (!g_module_mgr.initialized || interface == NULL) {
		return -1;
	}

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	module_info_t *info = NULL;

	for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
		if (g_module_mgr.modules[i].status == MODULE_STATUS_UNINITIALIZED) {
			info = &g_module_mgr.modules[i];
			break;
		}
	}

	if (info == NULL) {
		k_mutex_unlock(&g_module_mgr.lock);
		LOG_ERR("Maximum module count reached");
		return -1;
	}

	info->interface = interface;
	info->config = config;
	info->internal_data = NULL;
	info->status = MODULE_STATUS_INITIALIZING;
	info->id = g_module_mgr.next_module_id++;
	info->event_subscription_count = 0U;
	(void)memset(info->event_subscriptions, 0, sizeof(info->event_subscriptions));

	if (module_id != NULL) {
		*module_id = info->id;
	}

	if (interface->init != NULL) {
		const uint32_t t0 = k_uptime_get_32();
		const int ret = interface->init(config);
		const uint32_t elapsed = k_uptime_get_32() - t0;

		if (ret != 0) {
			LOG_ERR("Module '%s' init failed: %d", interface->name, ret);
			g_module_mgr.next_module_id--;
			clear_module_slot_unlocked(info);
			g_module_mgr.stats.error_modules++;
			k_mutex_unlock(&g_module_mgr.lock);
			return ret;
		}

		if (CONFIG_MODULE_INIT_TIMEOUT_MS > 0 &&
		    elapsed > (uint32_t)CONFIG_MODULE_INIT_TIMEOUT_MS) {
			LOG_ERR("Module '%s' init exceeded timeout (%u ms)", interface->name,
				(unsigned int)elapsed);
			if (interface->shutdown != NULL) {
				interface->shutdown();
			}
			g_module_mgr.next_module_id--;
			clear_module_slot_unlocked(info);
			g_module_mgr.stats.error_modules++;
			k_mutex_unlock(&g_module_mgr.lock);
			return -1;
		}
	}

	info->status = MODULE_STATUS_INITIALIZED;
	g_module_mgr.module_count++;
	g_module_mgr.stats.total_modules++;

	k_mutex_unlock(&g_module_mgr.lock);

	LOG_INF("Module registered: %s (id=%d)", interface->name, info->id);
	notify_callback_unlocked(info->id, MODULE_MGR_EVENT_REGISTERED);
	return 0;
}

int module_manager_unregister(uint32_t module_id)
{
	if (!g_module_mgr.initialized || module_id == 0U) {
		return -1;
	}

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	module_info_t *info = find_module_by_id_locked(module_id);

	if (info == NULL) {
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	if (info->status == MODULE_STATUS_RUNNING) {
		int (*stop_fn)(void) = NULL;

		if (info->interface != NULL) {
			stop_fn = info->interface->stop;
		}
		k_mutex_unlock(&g_module_mgr.lock);
		if (stop_fn != NULL) {
			stop_fn();
		}
		k_mutex_lock(&g_module_mgr.lock, K_FOREVER);
		info = find_module_by_id_locked(module_id);
		if (info == NULL) {
			k_mutex_unlock(&g_module_mgr.lock);
			return -1;
		}
		if (info->status == MODULE_STATUS_RUNNING) {
			info->status = MODULE_STATUS_STOPPED;
			if (g_module_mgr.stats.active_modules > 0U) {
				g_module_mgr.stats.active_modules--;
			}
		}
	}

	module_event_clear_all_unlocked(info);

	if (info->interface != NULL && info->interface->shutdown != NULL) {
		int (*sd)(void) = info->interface->shutdown;

		k_mutex_unlock(&g_module_mgr.lock);
		sd();
		k_mutex_lock(&g_module_mgr.lock, K_FOREVER);
		info = find_module_by_id_locked(module_id);
		if (info == NULL) {
			k_mutex_unlock(&g_module_mgr.lock);
			return -1;
		}
	}

	if (g_module_mgr.module_count > 0U) {
		g_module_mgr.module_count--;
	}
	if (g_module_mgr.stats.total_modules > 0U) {
		g_module_mgr.stats.total_modules--;
	}

	clear_module_slot_unlocked(info);

	k_mutex_unlock(&g_module_mgr.lock);

	LOG_INF("Module unregistered: id=%d", module_id);
	notify_callback_unlocked(module_id, MODULE_MGR_EVENT_UNREGISTERED);
	return 0;
}

int module_manager_get_module_info(uint32_t module_id, module_info_t *out)
{
	if (!g_module_mgr.initialized || module_id == 0U || out == NULL) {
		return -1;
	}

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	module_info_t *info = find_module_by_id_locked(module_id);

	if (info == NULL) {
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	*out = *info;
	k_mutex_unlock(&g_module_mgr.lock);
	return 0;
}

uint32_t module_manager_get_id_by_name(const char *name)
{
	if (!g_module_mgr.initialized || name == NULL) {
		return 0U;
	}

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
		if (g_module_mgr.modules[i].interface != NULL &&
		    g_module_mgr.modules[i].interface->name != NULL &&
		    strcmp(g_module_mgr.modules[i].interface->name, name) == 0) {
			const uint32_t id = g_module_mgr.modules[i].id;

			k_mutex_unlock(&g_module_mgr.lock);
			return id;
		}
	}

	k_mutex_unlock(&g_module_mgr.lock);
	return 0U;
}

void module_manager_foreach(void (*callback)(module_info_t *, void *), void *user_data)
{
	if (!g_module_mgr.initialized || callback == NULL) {
		return;
	}

	module_info_t snapshot[CONFIG_MAX_MODULES];

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	int n = 0;

	for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
		if (g_module_mgr.modules[i].status != MODULE_STATUS_UNINITIALIZED) {
			snapshot[n++] = g_module_mgr.modules[i];
		}
	}

	k_mutex_unlock(&g_module_mgr.lock);

	for (int i = 0; i < n; i++) {
		callback(&snapshot[i], user_data);
	}
}

/* =============================================================================
 * Module Lifecycle API
 * ============================================================================= */

int module_manager_start_module(uint32_t module_id)
{
	int (*start_fn)(void);
	const char *name;
	int ret = 0;

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	module_info_t *info = find_module_by_id_locked(module_id);

	if (info == NULL || info->interface == NULL) {
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	if (info->status != MODULE_STATUS_INITIALIZED &&
	    info->status != MODULE_STATUS_STOPPED) {
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	start_fn = info->interface->start;
	name = info->interface->name;

	k_mutex_unlock(&g_module_mgr.lock);

	if (start_fn != NULL) {
		ret = start_fn();
	}

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	info = find_module_by_id_locked(module_id);
	if (info == NULL || info->interface == NULL) {
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	if (ret != 0) {
		LOG_ERR("Module '%s' start failed: %d", name != NULL ? name : "?", ret);
		info->status = MODULE_STATUS_ERROR;
		g_module_mgr.stats.error_modules++;
		k_mutex_unlock(&g_module_mgr.lock);
		notify_callback_unlocked(module_id, MODULE_MGR_EVENT_ERROR);
		return ret;
	}

	info->status = MODULE_STATUS_RUNNING;
	g_module_mgr.stats.active_modules++;

	k_mutex_unlock(&g_module_mgr.lock);

	LOG_INF("Module started: %s", name != NULL ? name : "?");
	notify_callback_unlocked(module_id, MODULE_MGR_EVENT_STARTED);
	return 0;
}

int module_manager_stop_module(uint32_t module_id)
{
	int (*stop_fn)(void);
	const char *name;

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	module_info_t *info = find_module_by_id_locked(module_id);

	if (info == NULL || info->interface == NULL) {
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	if (info->status != MODULE_STATUS_RUNNING) {
		k_mutex_unlock(&g_module_mgr.lock);
		return 0;
	}

	stop_fn = info->interface->stop;
	name = info->interface->name;

	k_mutex_unlock(&g_module_mgr.lock);

	if (stop_fn != NULL) {
		stop_fn();
	}

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	info = find_module_by_id_locked(module_id);
	if (info != NULL && info->status == MODULE_STATUS_RUNNING) {
		info->status = MODULE_STATUS_STOPPED;
		if (g_module_mgr.stats.active_modules > 0U) {
			g_module_mgr.stats.active_modules--;
		}
	}

	k_mutex_unlock(&g_module_mgr.lock);

	LOG_INF("Module stopped: %s", name != NULL ? name : "?");
	notify_callback_unlocked(module_id, MODULE_MGR_EVENT_STOPPED);
	return 0;
}

int module_manager_start_all(void)
{
	start_order_entry_t entries[CONFIG_MAX_MODULES];
	int n = 0;

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
		module_info_t *m = &g_module_mgr.modules[i];

		if ((m->status == MODULE_STATUS_INITIALIZED ||
		     m->status == MODULE_STATUS_STOPPED) &&
		    m->interface != NULL) {
			entries[n].id = m->id;
			entries[n].priority = m->interface->priority;
			n++;
		}
	}

	k_mutex_unlock(&g_module_mgr.lock);

	sort_start_entries(entries, n);

	int started = 0;

	for (int i = 0; i < n; i++) {
		if (module_manager_start_module(entries[i].id) == 0) {
			started++;
		}
	}

	return started;
}

int module_manager_stop_all(void)
{
	uint32_t ids[CONFIG_MAX_MODULES];
	int n = 0;

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
		if (g_module_mgr.modules[i].status == MODULE_STATUS_RUNNING) {
			ids[n++] = g_module_mgr.modules[i].id;
		}
	}

	k_mutex_unlock(&g_module_mgr.lock);

	int stopped = 0;

	for (int i = 0; i < n; i++) {
		if (module_manager_stop_module(ids[i]) == 0) {
			stopped++;
		}
	}

	return stopped;
}

int module_manager_suspend_module(uint32_t module_id)
{
	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	module_info_t *info = find_module_by_id_locked(module_id);

	if (info == NULL || info->interface == NULL) {
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	if (info->status != MODULE_STATUS_RUNNING) {
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	info->status = MODULE_STATUS_SUSPENDED;
	const char *name = info->interface->name;

	k_mutex_unlock(&g_module_mgr.lock);

	LOG_INF("Module suspended: %s", name != NULL ? name : "?");
	return 0;
}

int module_manager_resume_module(uint32_t module_id)
{
	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	module_info_t *info = find_module_by_id_locked(module_id);

	if (info == NULL || info->interface == NULL) {
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	if (info->status != MODULE_STATUS_SUSPENDED) {
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	info->status = MODULE_STATUS_RUNNING;
	const char *name = info->interface->name;

	k_mutex_unlock(&g_module_mgr.lock);

	LOG_INF("Module resumed: %s", name != NULL ? name : "?");
	return 0;
}

/* =============================================================================
 * Event Handling API
 * ============================================================================= */

int module_manager_subscribe(uint32_t module_id, event_type_t event_type)
{
	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	module_info_t *info = find_module_by_id_locked(module_id);

	if (info == NULL || info->interface == NULL ||
	    info->interface->on_event == NULL) {
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	if (find_event_sub_index(info, event_type) >= 0) {
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	if (info->event_subscription_count >= CONFIG_MODULE_MAX_EVENT_SUBSCRIPTIONS) {
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	k_mutex_unlock(&g_module_mgr.lock);

	uint32_t subscriber_id = 0U;
	const event_status_t status =
		event_subscribe(event_type, module_event_handler,
				(void *)(uintptr_t)module_id, &subscriber_id);

	if (status != EVENT_OK) {
		return -1;
	}

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	info = find_module_by_id_locked(module_id);
	if (info == NULL) {
		(void)event_unsubscribe(event_type, subscriber_id);
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	if (info->event_subscription_count >= CONFIG_MODULE_MAX_EVENT_SUBSCRIPTIONS ||
	    find_event_sub_index(info, event_type) >= 0) {
		(void)event_unsubscribe(event_type, subscriber_id);
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	const uint8_t idx = info->event_subscription_count;

	info->event_subscriptions[idx].type = event_type;
	info->event_subscriptions[idx].subscriber_id = subscriber_id;
	info->event_subscription_count++;

	k_mutex_unlock(&g_module_mgr.lock);
	return 0;
}

int module_manager_unsubscribe(uint32_t module_id, event_type_t event_type)
{
	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	module_info_t *info = find_module_by_id_locked(module_id);

	if (info == NULL) {
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	const int idx = find_event_sub_index(info, event_type);

	if (idx < 0) {
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	const uint32_t sub_id = info->event_subscriptions[idx].subscriber_id;

	(void)event_unsubscribe(event_type, sub_id);

	const uint8_t last = (uint8_t)(info->event_subscription_count - 1U);

	if ((uint8_t)idx != last) {
		info->event_subscriptions[idx] = info->event_subscriptions[last];
	}

	(void)memset(&info->event_subscriptions[last], 0, sizeof(info->event_subscriptions[last]));
	info->event_subscription_count = last;

	k_mutex_unlock(&g_module_mgr.lock);
	return 0;
}

int module_manager_send_to_module(uint32_t module_id, const event_t *event)
{
	module_event_handler_t on_ev;
	void *idata;

	if (event == NULL) {
		k_mutex_lock(&g_module_mgr.lock, K_FOREVER);
		g_module_mgr.stats.events_dropped++;
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	module_info_t *info = find_module_by_id_locked(module_id);

	if (info == NULL || info->interface == NULL) {
		g_module_mgr.stats.events_dropped++;
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	if (info->status != MODULE_STATUS_RUNNING) {
		g_module_mgr.stats.events_dropped++;
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	on_ev = info->interface->on_event;
	idata = info->internal_data;

	if (on_ev == NULL) {
		g_module_mgr.stats.events_dropped++;
		k_mutex_unlock(&g_module_mgr.lock);
		return -1;
	}

	g_module_mgr.stats.events_processed++;
	k_mutex_unlock(&g_module_mgr.lock);

	on_ev(event, idata);
	return 0;
}

int module_manager_broadcast(const event_t *event)
{
	module_event_handler_t handlers[CONFIG_MAX_MODULES];
	void *datas[CONFIG_MAX_MODULES];
	int n = 0;

	if (event == NULL) {
		return -1;
	}

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
		module_info_t *info = &g_module_mgr.modules[i];

		if (info->status == MODULE_STATUS_RUNNING && info->interface != NULL &&
		    info->interface->on_event != NULL) {
			handlers[n] = info->interface->on_event;
			datas[n] = info->internal_data;
			n++;
		}
	}

	g_module_mgr.stats.events_processed += (uint32_t)n;

	k_mutex_unlock(&g_module_mgr.lock);

	for (int i = 0; i < n; i++) {
		handlers[i](event, datas[i]);
	}

	return n;
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
	(void)memset(&g_module_mgr.stats, 0, sizeof(g_module_mgr.stats));
	k_mutex_unlock(&g_module_mgr.lock);
}

void module_manager_dump_info(void)
{
	module_info_t snap[CONFIG_MAX_MODULES];
	uint32_t mod_count;
	uint32_t active;
	uint32_t errors;
	int n = 0;

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	mod_count = g_module_mgr.module_count;
	active = g_module_mgr.stats.active_modules;
	errors = g_module_mgr.stats.error_modules;

	for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
		if (g_module_mgr.modules[i].status != MODULE_STATUS_UNINITIALIZED) {
			snap[n++] = g_module_mgr.modules[i];
		}
	}

	k_mutex_unlock(&g_module_mgr.lock);

	printk("\n=== Module Manager Info ===\n");
	printk("Total modules: %u / %d\n", (unsigned int)mod_count, CONFIG_MAX_MODULES);
	printk("Active: %u, Errors: %u\n\n", (unsigned int)active, (unsigned int)errors);

	for (int i = 0; i < n; i++) {
		module_info_t *info = &snap[i];
		const char *status_str;

		switch (info->status) {
		case MODULE_STATUS_INITIALIZING:
			status_str = "INITING";
			break;
		case MODULE_STATUS_INITIALIZED:
			status_str = "INIT";
			break;
		case MODULE_STATUS_RUNNING:
			status_str = "RUNNING";
			break;
		case MODULE_STATUS_STOPPED:
			status_str = "STOPPED";
			break;
		case MODULE_STATUS_ERROR:
			status_str = "ERROR";
			break;
		case MODULE_STATUS_SUSPENDED:
			status_str = "SUSPENDED";
			break;
		default:
			status_str = "UNKNOWN";
			break;
		}

		printk("  [%u] %s - %s (v%u.%u.%u)\n", (unsigned int)info->id,
		       info->interface != NULL && info->interface->name != NULL ? info->interface->name
									      : "N/A",
		       status_str,
		       info->interface != NULL ? MODULE_VERSION_MAJOR(info->interface->version) : 0,
		       info->interface != NULL ? MODULE_VERSION_MINOR(info->interface->version) : 0,
		       info->interface != NULL ? MODULE_VERSION_PATCH(info->interface->version) : 0);
	}

	printk("\n");
}

void module_manager_set_callback(module_mgr_callback_t callback, void *user_data)
{
	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);
	g_module_mgr.callback = callback;
	g_module_mgr.callback_user_data = user_data;
	k_mutex_unlock(&g_module_mgr.lock);
}

/* =============================================================================
 * Internal Functions
 * ============================================================================= */

static void module_event_handler(const event_t *event, void *user_data)
{
	if (event == NULL || user_data == NULL) {
		return;
	}

	const uint32_t module_id = (uint32_t)(uintptr_t)user_data;

	k_mutex_lock(&g_module_mgr.lock, K_FOREVER);

	module_info_t *info = find_module_by_id_locked(module_id);

	if (info == NULL || info->interface == NULL ||
	    info->status != MODULE_STATUS_RUNNING) {
		k_mutex_unlock(&g_module_mgr.lock);
		return;
	}

	module_event_handler_t handler = info->interface->on_event;
	void *idata = info->internal_data;

	g_module_mgr.stats.events_processed++;
	k_mutex_unlock(&g_module_mgr.lock);

	if (handler != NULL) {
		handler(event, idata);
	}
}

/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ipc_service_event.h
 * @brief Optional bridge: Thread IPC Service → Event System
 *
 * Enable with CONFIG_THREAD_IPC_SERVICE_EVENT_BRIDGE=y (requires EVENT_SYSTEM).
 */

#ifndef IPC_SERVICE_EVENT_H_
#define IPC_SERVICE_EVENT_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_EVENT_BRIDGE)

#include "event_system.h"
#include "ipc_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 随事件分发的固定载荷（可复制进事件队列）
 *
 * 扩展大数据请另发事件类型或自行在 service_func 内调用 event_publish_copy，
 * 并自行保证 data 指针生命周期。
 */
typedef struct {
	uint32_t source_id;	   /**< 调用方约定的服务/模块 ID */
	ipc_request_id_t request_id; /**< 对应 ipc_call_* 的请求 ID */
	int32_t result;		   /**< service_func 返回值 */
} thread_ipc_event_result_t;

/**
 * @brief 注册本桥接使用的事件类型（幂等，可多次调用）
 *
 * @return EVENT_OK 或 event_register_type 错误码
 */
event_status_t thread_ipc_event_register_types(void);

/**
 * @brief 将单次 IPC 处理结果发布到事件总线（内部 event_publish_copy）
 *
 * 典型用法：在 ipc_service_func_t 末尾、return 前调用，供其他模块订阅。
 *
 * @param source_id 来源 ID（如模块 ID 或固定魔数）
 * @param request_id 当前请求 ID
 * @param result 将写入 payload.result
 * @param priority 事件优先级
 */
event_status_t thread_ipc_event_publish_result(uint32_t source_id,
					       ipc_request_id_t request_id,
					       int result,
					       event_priority_t priority);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_THREAD_IPC_SERVICE_EVENT_BRIDGE */

#endif /* IPC_SERVICE_EVENT_H_ */

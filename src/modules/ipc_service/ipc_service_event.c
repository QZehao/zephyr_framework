/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ipc_service_event.c
 * @brief Thread IPC 与事件系统桥接：注册事件类型、发布处理结果副本
 */

#include "ipc_service_event.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(thread_ipc_evt, CONFIG_THREAD_IPC_SERVICE_LOG_LEVEL);

event_status_t thread_ipc_event_register_types(void)
{
	/* 与订阅方约定的类型名，用于调试与事件过滤 */
	return event_register_type(EVENT_TYPE_THREAD_IPC_RESPONSE,
				   "thread_ipc_response");
}

/** 将固定大小的结果结构体复制进事件队列（非零拷贝，适合小载荷） */
event_status_t thread_ipc_event_publish_result(uint32_t source_id,
					       ipc_request_id_t request_id,
					       int result,
					       event_priority_t priority)
{
	thread_ipc_event_result_t payload = {
		.source_id = source_id,
		.request_id = request_id,
		.result = (int32_t)result,
	};

	return event_publish_copy(EVENT_TYPE_THREAD_IPC_RESPONSE, priority, &payload,
				  sizeof(payload));
}

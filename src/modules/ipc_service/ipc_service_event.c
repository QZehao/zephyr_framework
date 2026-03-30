/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ipc_service_event.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(thread_ipc_evt, CONFIG_THREAD_IPC_SERVICE_LOG_LEVEL);

event_status_t thread_ipc_event_register_types(void)
{
	return event_register_type(EVENT_TYPE_THREAD_IPC_RESPONSE,
				   "thread_ipc_response");
}

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

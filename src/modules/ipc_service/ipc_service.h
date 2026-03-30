/*
 * Copyright (c) 2024 Your Name
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ipc_service.h
 * @brief 基于专用线程的 IPC 服务（同步 / 异步回调 / future）
 *
 * 需 CONFIG_THREAD_IPC_SERVICE=y。队列与栈内嵌于 ipc_service_t，不使用 k_malloc。
 * 注意：与 Zephyr 子系统名 IPC_SERVICE（核间 IPC）无关。
 */

#ifndef ZEPHYR_IPC_SERVICE_H_
#define ZEPHYR_IPC_SERVICE_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <stdint.h>
#include <stdbool.h>

#if !IS_ENABLED(CONFIG_THREAD_IPC_SERVICE)
#error "ipc_service.h requires CONFIG_THREAD_IPC_SERVICE=y (see THREAD_IPC_SERVICE in Kconfig)"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t ipc_request_id_t;

typedef int (*ipc_service_func_t)(ipc_request_id_t request_id,
				  const void *data,
				  size_t data_size,
				  void **out_data,
				  size_t *out_data_size);

typedef void (*ipc_async_callback_t)(ipc_request_id_t request_id,
				     int result,
				     const void *data,
				     size_t data_size,
				     void *user_data);

typedef struct ipc_future {
	ipc_request_id_t request_id;
	struct k_sem semaphore;
	int result;
	const void *data;
	size_t data_size;
	bool completed;
	struct ipc_future *next;
} ipc_future_t;

typedef struct ipc_pending_request {
	ipc_request_id_t request_id;
	struct k_thread *caller_thread;
	ipc_async_callback_t callback;
	void *callback_user_data;
	ipc_future_t *future;
	struct k_sem response_sem;
	int result;
	const void *response_data;
	size_t response_data_size;
	bool in_use;
} ipc_pending_request_t;

typedef struct ipc_request_msg {
	ipc_request_id_t request_id;
	const void *data;
	size_t data_size;
	ipc_async_callback_t callback;
	void *callback_user_data;
	struct k_thread *caller_thread;
} ipc_request_msg_t;

typedef struct ipc_response_msg {
	ipc_request_id_t request_id;
	int result;
	const void *data;
	size_t data_size;
	struct k_thread *caller_thread;
} ipc_response_msg_t;

/**
 * @brief IPC service instance (queues + stacks embedded; no heap)
 */
typedef struct ipc_service {
	const char *name;
	struct k_thread thread;
	struct k_thread response_thread;
	struct k_msgq request_queue;
	struct k_msgq response_queue;

	uint8_t request_queue_buf[CONFIG_THREAD_IPC_SERVICE_REQUEST_QUEUE_SIZE *
				  sizeof(ipc_request_msg_t)];
	uint8_t response_queue_buf[CONFIG_THREAD_IPC_SERVICE_RESPONSE_QUEUE_SIZE *
				   sizeof(ipc_response_msg_t)];

	K_KERNEL_STACK_MEMBER(worker_stack, CONFIG_THREAD_IPC_SERVICE_STACK_SIZE);
	K_KERNEL_STACK_MEMBER(dispatcher_stack, CONFIG_THREAD_IPC_SERVICE_STACK_SIZE);

	int priority;

	ipc_service_func_t service_func;

	ipc_pending_request_t pending_requests[CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS];
	struct k_mutex pending_lock;
	ipc_future_t futures[CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS];
	ipc_future_t *free_futures;

	bool running;
	volatile bool shutdown;
} ipc_service_t;

int ipc_service_init(ipc_service_t *service,
		     const char *name,
		     ipc_service_func_t service_func,
		     size_t stack_size,
		     int priority,
		     size_t request_queue_size,
		     size_t response_queue_size);

int ipc_service_start(ipc_service_t *service);

int ipc_service_stop(ipc_service_t *service);

int ipc_call_sync(ipc_service_t *service,
		  const void *data,
		  size_t data_size,
		  void **out_data,
		  size_t *out_data_size,
		  k_timeout_t timeout);

int ipc_call_async(ipc_service_t *service,
		   const void *data,
		   size_t data_size,
		   ipc_async_callback_t callback,
		   void *user_data,
		   ipc_request_id_t *out_request_id);

int ipc_call_future(ipc_service_t *service,
		    const void *data,
		    size_t data_size,
		    ipc_future_t **out_future);

int ipc_future_wait(ipc_future_t *future,
		    int *out_result,
		    const void **out_data,
		    size_t *out_data_size,
		    k_timeout_t timeout);

bool ipc_future_is_ready(ipc_future_t *future);

int ipc_future_release(ipc_service_t *service, ipc_future_t *future);

size_t ipc_service_get_pending_count(ipc_service_t *service);

int ipc_service_cancel(ipc_service_t *service, ipc_request_id_t request_id);

ipc_request_id_t ipc_generate_request_id(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_IPC_SERVICE_H_ */

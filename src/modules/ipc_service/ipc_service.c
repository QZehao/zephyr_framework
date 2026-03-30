/*
 * Copyright (c) 2024 Your Name
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ipc_service.c
 * @brief 基于专用线程 + 双消息队列的 IPC：worker 执行 service_func，dispatcher 投递响应
 *
 * 设计要点：无堆分配；pending 表与 future 空闲链表受 pending_lock 保护；停止时向两队列投递哑包以唤醒阻塞的 k_msgq_get。
 */

#include "ipc_service.h"

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(thread_ipc_svc, CONFIG_THREAD_IPC_SERVICE_LOG_LEVEL);

/** 全局单调递增请求 ID（跳过 0，避免与未初始化混淆） */
static atomic_t s_request_id_counter;

ipc_request_id_t ipc_generate_request_id(void)
{
	ipc_request_id_t id;

	do {
		id = (ipc_request_id_t)atomic_inc(&s_request_id_counter);
	} while (id == 0U);
	return id;
}

static ipc_pending_request_t *find_pending_entry(ipc_service_t *service,
						ipc_request_id_t request_id)
{
	for (int i = 0; i < CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS; i++) {
		if (service->pending_requests[i].in_use &&
		    service->pending_requests[i].request_id == request_id) {
			return &service->pending_requests[i];
		}
	}
	return NULL;
}

static ipc_pending_request_t *allocate_pending_entry(ipc_service_t *service)
{
	for (int i = 0; i < CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS; i++) {
		if (!service->pending_requests[i].in_use) {
			return &service->pending_requests[i];
		}
	}
	return NULL;
}

static void init_pending_entry(ipc_service_t *service,
			       ipc_pending_request_t *entry,
			       ipc_request_id_t request_id,
			       struct k_thread *caller_thread,
			       ipc_async_callback_t callback,
			       void *callback_user_data,
			       ipc_future_t *future)
{
	ARG_UNUSED(service);
	entry->request_id = request_id;
	entry->caller_thread = caller_thread;
	entry->callback = callback;
	entry->callback_user_data = callback_user_data;
	entry->future = future;
	entry->result = 0;
	entry->response_data = NULL;
	entry->response_data_size = 0;
	entry->in_use = true;
	k_sem_init(&entry->response_sem, 0, 1);
}

static void release_pending_entry(ipc_pending_request_t *entry)
{
	entry->in_use = false;
	entry->callback = NULL;
	entry->future = NULL;
}

static ipc_future_t *allocate_future(ipc_service_t *service)
{
	if (service->free_futures == NULL) {
		return NULL;
	}

	ipc_future_t *future = service->free_futures;

	service->free_futures = future->next;
	future->next = NULL;

	return future;
}

static void release_future(ipc_service_t *service, ipc_future_t *future)
{
	future->completed = false;
	future->result = 0;
	future->data = NULL;
	future->data_size = 0;
	future->next = service->free_futures;
	service->free_futures = future;
}

static bool future_belongs_to_service(const ipc_service_t *service,
				      const ipc_future_t *future)
{
	for (int i = 0; i < CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS; i++) {
		if (future == &service->futures[i]) {
			return true;
		}
	}
	return false;
}

/** Worker：从请求队列取消息，调用用户 service_func，将结果写入响应队列 */
static void service_thread_func(void *p1, void *p2, void *p3)
{
	ipc_service_t *service = (ipc_service_t *)p1;
	ipc_request_msg_t request_msg;
	ipc_response_msg_t response_msg;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("IPC service '%s' worker started", service->name);

	for (;;) {
		int ret = k_msgq_get(&service->request_queue, &request_msg, K_FOREVER);

		if (ret != 0) {
			continue;
		}

		/* shutdown 后先收到哑包再退出，避免在已置位时仍调用 service_func */
		if (service->shutdown) {
			break;
		}

		void *out_data = NULL;
		size_t out_data_size = 0;

		LOG_DBG("Processing request %u", request_msg.request_id);

		int result = service->service_func(request_msg.request_id,
						   request_msg.data,
						   request_msg.data_size,
						   &out_data,
						   &out_data_size);

		response_msg.request_id = request_msg.request_id;
		response_msg.result = result;
		response_msg.data = out_data;
		response_msg.data_size = out_data_size;
		response_msg.caller_thread = request_msg.caller_thread;

		ret = k_msgq_put(&service->response_queue, &response_msg, K_FOREVER);
		if (ret != 0) {
			LOG_ERR("Failed to send response for request %u",
				request_msg.request_id);
		}
	}

	LOG_INF("IPC service '%s' worker stopped", service->name);
}

/**
 * Dispatcher：根据 request_id 查找 pending，同步路径 k_sem_give、异步路径直接回调（持锁策略见内联注释）
 */
static void response_dispatcher_thread(void *p1, void *p2, void *p3)
{
	ipc_service_t *service = (ipc_service_t *)p1;
	ipc_response_msg_t response_msg;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("IPC service '%s' dispatcher started", service->name);

	for (;;) {
		int ret = k_msgq_get(&service->response_queue, &response_msg, K_FOREVER);

		if (ret != 0) {
			continue;
		}

		if (service->shutdown) {
			break;
		}

		k_mutex_lock(&service->pending_lock, K_FOREVER);

		ipc_pending_request_t *entry =
			find_pending_entry(service, response_msg.request_id);

		if (entry != NULL) {
			entry->result = response_msg.result;
			entry->response_data = response_msg.data;
			entry->response_data_size = response_msg.data_size;

			if (entry->callback != NULL) {
				/* 先复制回调参数并释放槽位，再在锁外调用用户回调，避免死锁 */
				ipc_async_callback_t cb = entry->callback;
				void *ud = entry->callback_user_data;
				ipc_request_id_t rid = entry->request_id;
				int res = entry->result;
				const void *rdata = entry->response_data;
				size_t rsize = entry->response_data_size;

				release_pending_entry(entry);
				k_mutex_unlock(&service->pending_lock);

				cb(rid, res, rdata, rsize, ud);
				continue;
			}

			if (entry->future != NULL) {
				entry->future->result = entry->result;
				entry->future->data = entry->response_data;
				entry->future->data_size = entry->response_data_size;
				entry->future->completed = true;
				k_sem_give(&entry->future->semaphore);
				entry->future = NULL;
				release_pending_entry(entry);
			} else {
				k_sem_give(&entry->response_sem);
			}
		} else {
			LOG_WRN("No pending entry for response %u",
				response_msg.request_id);
		}

		k_mutex_unlock(&service->pending_lock);
	}

	LOG_INF("IPC service '%s' dispatcher stopped", service->name);
}

int ipc_service_init(ipc_service_t *service,
		     const char *name,
		     ipc_service_func_t service_func,
		     size_t stack_size,
		     int priority,
		     size_t request_queue_size,
		     size_t response_queue_size)
{
	if (service == NULL || name == NULL || service_func == NULL) {
		return -EINVAL;
	}

	if (stack_size != (size_t)CONFIG_THREAD_IPC_SERVICE_STACK_SIZE ||
	    request_queue_size != (size_t)CONFIG_THREAD_IPC_SERVICE_REQUEST_QUEUE_SIZE ||
	    response_queue_size != (size_t)CONFIG_THREAD_IPC_SERVICE_RESPONSE_QUEUE_SIZE) {
		return -EINVAL;
	}

	memset(service, 0, sizeof(ipc_service_t));

	service->name = name;
	service->service_func = service_func;
	service->priority = priority;
	service->running = false;
	service->shutdown = false;

	k_msgq_init(&service->request_queue, (char *)service->request_queue_buf,
		    sizeof(ipc_request_msg_t),
		    CONFIG_THREAD_IPC_SERVICE_REQUEST_QUEUE_SIZE);

	k_msgq_init(&service->response_queue, (char *)service->response_queue_buf,
		    sizeof(ipc_response_msg_t),
		    CONFIG_THREAD_IPC_SERVICE_RESPONSE_QUEUE_SIZE);

	k_mutex_init(&service->pending_lock);

	for (int i = 0; i < CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS; i++) {
		service->pending_requests[i].in_use = false;
	}

	service->free_futures = NULL;
	for (int i = 0; i < CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS; i++) {
		service->futures[i].request_id = 0;
		service->futures[i].completed = false;
		service->futures[i].next = service->free_futures;
		k_sem_init(&service->futures[i].semaphore, 0, 1);
		service->free_futures = &service->futures[i];
	}

	LOG_INF("IPC service '%s' initialized", name);

	return 0;
}

int ipc_service_start(ipc_service_t *service)
{
	if (service == NULL || !service->service_func) {
		return -EINVAL;
	}

	if (service->running) {
		return -EALREADY;
	}

	service->shutdown = false;

	k_thread_create(&service->thread, service->worker_stack,
			K_KERNEL_STACK_SIZEOF(service->worker_stack),
			service_thread_func, service, NULL, NULL, service->priority, 0,
			K_NO_WAIT);
#if IS_ENABLED(CONFIG_THREAD_NAME)
	k_thread_name_set(&service->thread, service->name);
#endif

	k_thread_create(&service->response_thread, service->dispatcher_stack,
			K_KERNEL_STACK_SIZEOF(service->dispatcher_stack),
			response_dispatcher_thread, service, NULL, NULL,
			service->priority, 0, K_NO_WAIT);
#if IS_ENABLED(CONFIG_THREAD_NAME)
	k_thread_name_set(&service->response_thread, "ipc_disp");
#endif

	service->running = true;

	LOG_INF("IPC service '%s' started", service->name);

	return 0;
}

int ipc_service_stop(ipc_service_t *service)
{
	if (service == NULL) {
		return -EINVAL;
	}

	if (!service->running) {
		return 0;
	}

	service->shutdown = true;

	/* 非阻塞投递哑消息，唤醒可能阻塞在 k_msgq_get 的 worker/dispatcher */
	ipc_request_msg_t dummy_request;

	memset(&dummy_request, 0, sizeof(dummy_request));
	(void)k_msgq_put(&service->request_queue, &dummy_request, K_NO_WAIT);

	ipc_response_msg_t dummy_response;

	memset(&dummy_response, 0, sizeof(dummy_response));
	(void)k_msgq_put(&service->response_queue, &dummy_response, K_NO_WAIT);

	k_thread_join(&service->thread, K_FOREVER);
	k_thread_join(&service->response_thread, K_FOREVER);

	service->running = false;

	LOG_INF("IPC service '%s' stopped", service->name);

	return 0;
}

int ipc_call_sync(ipc_service_t *service,
		  const void *data,
		  size_t data_size,
		  void **out_data,
		  size_t *out_data_size,
		  k_timeout_t timeout)
{
	if (service == NULL || !service->running) {
		return -EINVAL;
	}

	if (data == NULL || out_data == NULL || out_data_size == NULL) {
		return -EINVAL;
	}

	ipc_request_id_t request_id = ipc_generate_request_id();

	k_mutex_lock(&service->pending_lock, K_FOREVER);

	ipc_pending_request_t *entry = allocate_pending_entry(service);

	if (entry == NULL) {
		k_mutex_unlock(&service->pending_lock);
		return -ENOMEM;
	}

	init_pending_entry(service, entry, request_id, k_current_get(), NULL, NULL,
			   NULL);

	k_mutex_unlock(&service->pending_lock);

	ipc_request_msg_t request_msg = {
		.request_id = request_id,
		.data = data,
		.data_size = data_size,
		.callback = NULL,
		.callback_user_data = NULL,
		.caller_thread = k_current_get(),
	};

	int ret = k_msgq_put(&service->request_queue, &request_msg, K_FOREVER);

	if (ret != 0) {
		k_mutex_lock(&service->pending_lock, K_FOREVER);
		release_pending_entry(entry);
		k_mutex_unlock(&service->pending_lock);
		return ret;
	}

	ret = k_sem_take(&entry->response_sem, timeout);
	if (ret != 0) {
		k_mutex_lock(&service->pending_lock, K_FOREVER);
		release_pending_entry(entry);
		k_mutex_unlock(&service->pending_lock);
		return ret;
	}

	*out_data = (void *)entry->response_data;
	*out_data_size = entry->response_data_size;
	int result = entry->result;

	k_mutex_lock(&service->pending_lock, K_FOREVER);
	release_pending_entry(entry);
	k_mutex_unlock(&service->pending_lock);

	return result;
}

int ipc_call_async(ipc_service_t *service,
		   const void *data,
		   size_t data_size,
		   ipc_async_callback_t callback,
		   void *user_data,
		   ipc_request_id_t *out_request_id)
{
	if (service == NULL || !service->running) {
		return -EINVAL;
	}

	if (data == NULL || callback == NULL) {
		return -EINVAL;
	}

	ipc_request_id_t request_id = ipc_generate_request_id();

	if (out_request_id != NULL) {
		*out_request_id = request_id;
	}

	k_mutex_lock(&service->pending_lock, K_FOREVER);

	ipc_pending_request_t *entry = allocate_pending_entry(service);

	if (entry == NULL) {
		k_mutex_unlock(&service->pending_lock);
		return -ENOMEM;
	}

	init_pending_entry(service, entry, request_id, k_current_get(), callback,
			   user_data, NULL);

	k_mutex_unlock(&service->pending_lock);

	ipc_request_msg_t request_msg = {
		.request_id = request_id,
		.data = data,
		.data_size = data_size,
		.callback = callback,
		.callback_user_data = user_data,
		.caller_thread = k_current_get(),
	};

	int ret = k_msgq_put(&service->request_queue, &request_msg, K_FOREVER);

	if (ret != 0) {
		k_mutex_lock(&service->pending_lock, K_FOREVER);
		release_pending_entry(entry);
		k_mutex_unlock(&service->pending_lock);
		return ret;
	}

	return 0;
}

int ipc_call_future(ipc_service_t *service,
		    const void *data,
		    size_t data_size,
		    ipc_future_t **out_future)
{
	if (service == NULL || !service->running) {
		return -EINVAL;
	}

	if (data == NULL || out_future == NULL) {
		return -EINVAL;
	}

	ipc_request_id_t request_id = ipc_generate_request_id();

	k_mutex_lock(&service->pending_lock, K_FOREVER);

	ipc_future_t *future = allocate_future(service);

	if (future == NULL) {
		k_mutex_unlock(&service->pending_lock);
		return -ENOMEM;
	}

	future->request_id = request_id;
	future->completed = false;
	future->result = 0;
	future->data = NULL;
	future->data_size = 0;
	k_sem_reset(&future->semaphore);

	ipc_pending_request_t *entry = allocate_pending_entry(service);

	if (entry == NULL) {
		release_future(service, future);
		k_mutex_unlock(&service->pending_lock);
		return -ENOMEM;
	}

	init_pending_entry(service, entry, request_id, k_current_get(), NULL, NULL,
			   future);

	k_mutex_unlock(&service->pending_lock);

	ipc_request_msg_t request_msg = {
		.request_id = request_id,
		.data = data,
		.data_size = data_size,
		.callback = NULL,
		.callback_user_data = NULL,
		.caller_thread = k_current_get(),
	};

	int ret = k_msgq_put(&service->request_queue, &request_msg, K_FOREVER);

	if (ret != 0) {
		k_mutex_lock(&service->pending_lock, K_FOREVER);
		release_pending_entry(entry);
		release_future(service, future);
		k_mutex_unlock(&service->pending_lock);
		return ret;
	}

	*out_future = future;

	return 0;
}

int ipc_future_wait(ipc_future_t *future,
		    int *out_result,
		    const void **out_data,
		    size_t *out_data_size,
		    k_timeout_t timeout)
{
	if (future == NULL) {
		return -EINVAL;
	}

	int ret = k_sem_take(&future->semaphore, timeout);

	if (ret != 0) {
		return ret;
	}

	if (out_result != NULL) {
		*out_result = future->result;
	}

	if (out_data != NULL) {
		*out_data = future->data;
	}

	if (out_data_size != NULL) {
		*out_data_size = future->data_size;
	}

	return 0;
}

bool ipc_future_is_ready(ipc_future_t *future)
{
	if (future == NULL) {
		return false;
	}

	return future->completed;
}

int ipc_future_release(ipc_service_t *service, ipc_future_t *future)
{
	if (service == NULL || future == NULL) {
		return -EINVAL;
	}

	if (!future_belongs_to_service(service, future)) {
		return -EINVAL;
	}

	k_mutex_lock(&service->pending_lock, K_FOREVER);
	release_future(service, future);
	k_mutex_unlock(&service->pending_lock);

	return 0;
}

size_t ipc_service_get_pending_count(ipc_service_t *service)
{
	if (service == NULL) {
		return 0;
	}

	size_t count = 0;

	k_mutex_lock(&service->pending_lock, K_FOREVER);

	for (int i = 0; i < CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS; i++) {
		if (service->pending_requests[i].in_use) {
			count++;
		}
	}

	k_mutex_unlock(&service->pending_lock);

	return count;
}

int ipc_service_cancel(ipc_service_t *service, ipc_request_id_t request_id)
{
	if (service == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&service->pending_lock, K_FOREVER);

	ipc_pending_request_t *entry = find_pending_entry(service, request_id);
	int ret = -ENOENT;

	if (entry != NULL) {
		if (entry->future != NULL) {
			entry->future->result = -ECANCELED;
			entry->future->data = NULL;
			entry->future->data_size = 0;
			entry->future->completed = true;
			k_sem_give(&entry->future->semaphore);
			entry->future = NULL;
			release_pending_entry(entry);
			ret = 0;
		} else if (entry->callback != NULL) {
			release_pending_entry(entry);
			ret = 0;
		} else {
			entry->result = -ECANCELED;
			entry->response_data = NULL;
			entry->response_data_size = 0;
			k_sem_give(&entry->response_sem);
			ret = 0;
		}
	}

	k_mutex_unlock(&service->pending_lock);

	return ret;
}

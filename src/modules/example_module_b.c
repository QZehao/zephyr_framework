/**
 * @file example_module_b.c
 * @brief Example Module B Implementation
 * 
 * Example business module - simulates a communication/actuator module.
 * This module subscribes to sensor data events and processes them.
 * 
 * @copyright Copyright (c) 2026
 * @license SPDX-License-Identifier: Apache-2.0
 */

#include "example_module_b.h"
#include "event_system.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(example_module_b, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * Internal Definitions
 * ============================================================================= */

#define EXAMPLE_MODULE_B_TX_SIZE  512
#define EXAMPLE_MODULE_B_RX_SIZE  512
#define EXAMPLE_MODULE_B_THREAD_PRIORITY  6
#define EXAMPLE_MODULE_B_THREAD_STACK_SIZE  1024

/* Event types */
#define EVENT_TYPE_SENSOR_DATA    10  /* From Module A */
#define EVENT_TYPE_COMM_DATA      20
#define EVENT_TYPE_ACTUATOR_CMD   21

/* Control commands */
#define CMD_SEND_DATA     1
#define CMD_GET_STATS     2
#define CMD_CLEAR_BUFFERS 3

/* =============================================================================
 * Internal Data Structures
 * ============================================================================= */

typedef struct {
    example_module_b_config_t config;
    module_status_t status;
    struct k_thread thread;
    K_THREAD_STACK_MEMBER(stack, EXAMPLE_MODULE_B_THREAD_STACK_SIZE);
    uint8_t tx_buffer[EXAMPLE_MODULE_B_TX_SIZE];
    uint8_t rx_buffer[EXAMPLE_MODULE_B_RX_SIZE];
    uint32_t tx_count;
    uint32_t rx_count;
    uint32_t subscriber_id;
} example_module_b_cb_t;

/* =============================================================================
 * Static Variables
 * ============================================================================= */

static example_module_b_cb_t g_module_b;

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static void module_b_thread_func(void *p1, void *p2, void *p3);
static void on_sensor_data(const event_t *event, void *user_data);

/* =============================================================================
 * Module Interface Implementation
 * ============================================================================= */

int example_module_b_init(void *config)
{
    LOG_INF("Initializing Example Module B...");

    memset(&g_module_b, 0, sizeof(g_module_b));

    /* Set default or provided config */
    if (config != NULL) {
        g_module_b.config = *(example_module_b_config_t *)config;
    } else {
        g_module_b.config.tx_buffer_size = EXAMPLE_MODULE_B_TX_SIZE;
        g_module_b.config.rx_buffer_size = EXAMPLE_MODULE_B_RX_SIZE;
        g_module_b.config.timeout_ms = 1000;
    }

    g_module_b.status = MODULE_STATUS_INITIALIZED;

    k_mutex_init(&g_module_b.tx_lock);
    k_mutex_init(&g_module_b.rx_lock);

    /* Register event types */
    event_register_type(EVENT_TYPE_COMM_DATA, "comm_data");
    event_register_type(EVENT_TYPE_ACTUATOR_CMD, "actuator_cmd");

    LOG_INF("Example Module B initialized");
    return 0;
}

int example_module_b_start(void)
{
    if (g_module_b.status != MODULE_STATUS_INITIALIZED &&
        g_module_b.status != MODULE_STATUS_STOPPED) {
        return -1;
    }

    g_module_b.status = MODULE_STATUS_RUNNING;

    /* Create processing thread */
    k_thread_create(&g_module_b.thread,
                    g_module_b.stack,
                    K_THREAD_STACK_SIZEOF(g_module_b.stack),
                    module_b_thread_func,
                    NULL, NULL, NULL,
                    EXAMPLE_MODULE_B_THREAD_PRIORITY,
                    0,
                    K_FOREVER);

    k_thread_name_set(&g_module_b.thread, "mod_b_comm");
    k_thread_start(&g_module_b.thread);

    /* Subscribe to sensor data events from Module A */
    event_subscribe(EVENT_TYPE_SENSOR_DATA,
                    on_sensor_data,
                    &g_module_b,
                    &g_module_b.subscriber_id);

    LOG_INF("Example Module B started, subscribed to sensor events");
    return 0;
}

int example_module_b_stop(void)
{
    if (g_module_b.status != MODULE_STATUS_RUNNING) {
        return 0;
    }

    g_module_b.status = MODULE_STATUS_STOPPED;
    k_thread_abort(&g_module_b.thread);

    /* Unsubscribe from events */
    if (g_module_b.subscriber_id != 0) {
        event_unsubscribe(EVENT_TYPE_SENSOR_DATA, g_module_b.subscriber_id);
        g_module_b.subscriber_id = 0;
    }

    LOG_INF("Example Module B stopped");
    return 0;
}

int example_module_b_shutdown(void)
{
    example_module_b_stop();
    g_module_b.status = MODULE_STATUS_UNINITIALIZED;
    LOG_INF("Example Module B shutdown");
    return 0;
}

void example_module_b_on_event(const event_t *event, void *user_data)
{
    if (event == NULL) {
        return;
    }

    LOG_DBG("Module B received event: type=%d, len=%d", event->type, event->data_len);

    switch (event->type) {
        case EVENT_TYPE_ACTUATOR_CMD:
            /* Handle actuator command */
            if (event->data != NULL) {
                k_mutex_lock(&g_module_b.rx_lock, K_FOREVER);
                
                /* Copy to RX buffer */
                size_t copy_len = MIN(event->data_len, 
                                      g_module_b.config.rx_buffer_size - g_module_b.rx_head);
                memcpy(&g_module_b.rx_buffer[g_module_b.rx_head], event->data, copy_len);
                g_module_b.rx_head += copy_len;
                g_module_b.rx_count += copy_len;
                
                k_mutex_unlock(&g_module_b.rx_lock);
                
                LOG_DBG("Actuator command processed: %d bytes", copy_len);
            }
            break;

        default:
            LOG_DBG("Unhandled event type: %d", event->type);
            break;
    }
}

module_status_t example_module_b_get_status(void)
{
    return g_module_b.status;
}

int example_module_b_control(int cmd, void *arg)
{
    switch (cmd) {
        case CMD_SEND_DATA:
            if (arg == NULL) return -1;
            /* Handled by send function */
            return 0;

        case CMD_GET_STATS:
            if (arg == NULL) return -1;
            {
                uint32_t *stats = (uint32_t *)arg;
                stats[0] = g_module_b.tx_count;
                stats[1] = g_module_b.rx_count;
                stats[2] = g_module_b.error_count;
            }
            return 0;

        case CMD_CLEAR_BUFFERS:
            k_mutex_lock(&g_module_b.tx_lock, K_FOREVER);
            k_mutex_lock(&g_module_b.rx_lock, K_FOREVER);
            g_module_b.tx_head = 0;
            g_module_b.rx_head = 0;
            k_mutex_unlock(&g_module_b.rx_lock);
            k_mutex_unlock(&g_module_b.tx_lock);
            return 0;

        default:
            return -1;
    }
}

/* =============================================================================
 * Module-specific API
 * ============================================================================= */

int example_module_b_send(const void *data, size_t len)
{
    if (data == NULL || len == 0) {
        return -1;
    }

    if (g_module_b.status != MODULE_STATUS_RUNNING) {
        return -1;
    }

    k_mutex_lock(&g_module_b.tx_lock, K_FOREVER);

    size_t available = g_module_b.config.tx_buffer_size - g_module_b.tx_head;
    size_t to_copy = MIN(len, available);

    if (to_copy == 0) {
        k_mutex_unlock(&g_module_b.tx_lock);
        g_module_b.error_count++;
        return -1;
    }

    memcpy(&g_module_b.tx_buffer[g_module_b.tx_head], data, to_copy);
    g_module_b.tx_head += to_copy;
    g_module_b.tx_count += to_copy;

    k_mutex_unlock(&g_module_b.tx_lock);

    /* Publish comm data event */
    event_t event = {
        .type = EVENT_TYPE_COMM_DATA,
        .priority = EVENT_PRIORITY_NORMAL,
        .timestamp = k_uptime_get_32(),
        .source_id = 2,  /* Module B ID */
        .data_len = to_copy,
        .data = (void *)data,
        .is_dynamic = false
    };

    event_publish(&event);

    return (int)to_copy;
}

int example_module_b_receive(void *data, size_t len)
{
    if (data == NULL || len == 0) {
        return -1;
    }

    k_mutex_lock(&g_module_b.rx_lock, K_FOREVER);

    size_t available = g_module_b.rx_head;
    size_t to_copy = MIN(len, available);

    if (to_copy > 0) {
        memcpy(data, g_module_b.rx_buffer, to_copy);
        
        /* Shift remaining data */
        if (g_module_b.rx_head > to_copy) {
            memmove(g_module_b.rx_buffer, 
                   &g_module_b.rx_buffer[to_copy],
                   g_module_b.rx_head - to_copy);
        }
        g_module_b.rx_head -= to_copy;
    }

    k_mutex_unlock(&g_module_b.rx_lock);
    return (int)to_copy;
}

void example_module_b_get_stats(uint32_t *tx_count, uint32_t *rx_count, uint32_t *errors)
{
    if (tx_count != NULL) *tx_count = g_module_b.tx_count;
    if (rx_count != NULL) *rx_count = g_module_b.rx_count;
    if (errors != NULL) *errors = g_module_b.error_count;
}

/* =============================================================================
 * Internal Functions
 * ============================================================================= */

static void module_b_thread_func(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("Module B thread started");

    while (g_module_b.status == MODULE_STATUS_RUNNING) {
        /* Simulate communication processing */
        k_msleep(50);
    }

    LOG_INF("Module B thread stopped");
}

static void on_sensor_data(const event_t *event, void *user_data)
{
    if (event == NULL || user_data == NULL) {
        return;
    }

    LOG_DBG("Received sensor data");

    /* Process sensor data and potentially send response */
    if (event->data != NULL && event->data_len >= sizeof(int32_t)) {
        int32_t sensor_value = *(int32_t *)event->data;

        /* Example: Send acknowledgment */
        uint32_t ack = sensor_value * 2;  /* Simple transformation */
        example_module_b_send(&ack, sizeof(ack));

        LOG_DBG("Processed sensor value %d", sensor_value);
    }
}

/* =============================================================================
 * Module Interface Declaration
 * ============================================================================= */

DECLARE_MODULE_INTERFACE(example_module_b);

const module_interface_t *example_module_b_get_interface(void)
{
    return &example_module_b_interface;
}

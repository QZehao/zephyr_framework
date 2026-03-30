/**
 * @file test_sys_watchdog.c
 * @brief sys_watchdog（软件看门狗路径）单元测试
 *
 * @copyright Copyright (c) 2026
 * @license SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>
#include "sys_watchdog.h"

LOG_MODULE_REGISTER(test_sys_watchdog);

ZTEST(sys_watchdog, test_init_start_feed_stop)
{
	zassert_equal(sys_wdt_init(NULL), 0, "init 失败");
	zassert_equal(sys_wdt_start(), 0, "start 失败");
	zassert_equal(sys_wdt_feed(), 0, "feed 失败");
	zassert_equal(sys_wdt_stop(), 0, "stop 失败");
}

ZTEST(sys_watchdog, test_pause_resume)
{
	zassert_equal(sys_wdt_init(NULL), 0, NULL);
	zassert_equal(sys_wdt_start(), 0, NULL);
	zassert_equal(sys_wdt_pause(), 0, NULL);
	zassert_equal(sys_wdt_resume(), 0, NULL);
	zassert_equal(sys_wdt_stop(), 0, NULL);
}

ZTEST_SUITE(sys_watchdog, NULL, NULL, NULL, NULL, NULL);

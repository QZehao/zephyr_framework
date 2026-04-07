#
# Copyright (c) 2026
#
# SPDX-License-Identifier: GPL-3.0
#

zephyr_library()

zephyr_library_sources(ipc_service.c)

zephyr_library_sources_ifdef(CONFIG_THREAD_IPC_SERVICE_EXAMPLE ipc_service_example.c)

zephyr_library_sources_ifdef(CONFIG_THREAD_IPC_SERVICE_EVENT_BRIDGE ipc_service_event.c)

zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR})

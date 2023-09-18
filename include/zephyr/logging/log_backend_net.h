/*
 * Copyright (c) 2023 David Corbeil
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_LOG_BACKEND_NET_H_
#define ZEPHYR_LOG_BACKEND_NET_H_

#include <stdbool.h>

/**
 * @brief Allows user to set a server IP address at runtime
 *
 * @details This function allows the user to set an IP address at runtime before the syslog network
 *          backend is initialized since the string address gets parsed in log_backend_api::init().
 *          For this to work, CONFIG_LOG_BACKEND_NET_SERVER_RUNTIME needs to be enabled and is
 *          mutually exclusive with CONFIG_LOG_BACKEND_NET_SERVER_STATIC.
 *
 * @param addr String that contains the IP address.
 *
 * @return True if parsing could be done, false otherwise.
 */

#ifdef CONFIG_LOG_BACKEND_NET_SERVER_RUNTIME
bool log_backend_net_set_addr(const char *addr);
#endif

#endif /* ZEPHYR_LOG_BACKEND_NET_H_ */

/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef H_BLE_HS_LOG_
#define H_BLE_HS_LOG_

#include "tuya_ble_os_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

struct os_mbuf;

#ifndef BLE_HS_VERBOSE_LOG
#define BLE_HS_VERBOSE_LOG 0
#endif

#undef BLE_HS_LOG_DEBUG
#undef BLE_HS_LOG_INFO
#undef BLE_HS_LOG_WARN
#undef BLE_HS_LOG_ERROR

#if defined(BLE_HS_VERBOSE_LOG) && (BLE_HS_VERBOSE_LOG == 1)
#define BLE_HS_LOG_DEBUG(fmt, ...)              PR_DEBUG(fmt, ##__VA_ARGS__)
#define BLE_HS_LOG_INFO(fmt, ...)               PR_INFO(fmt, ##__VA_ARGS__)
#define BLE_HS_LOG_NOTICE(fmt, ...)             PR_NOTICE(fmt, ##__VA_ARGS__)
#else
#define BLE_HS_LOG_DEBUG(fmt, ...)              do { } while (0)
#define BLE_HS_LOG_INFO(fmt, ...)               do { } while (0)
#define BLE_HS_LOG_NOTICE(fmt, ...)             do { } while (0)
#endif

#define BLE_HS_LOG_WARN(fmt, ...)               PR_WARN(fmt, ##__VA_ARGS__)
#define BLE_HS_LOG_ERROR(fmt, ...)              PR_ERR(fmt, ##__VA_ARGS__)
#define BLE_HS_LOG_ERR(fmt, ...)                PR_ERR(fmt, ##__VA_ARGS__)

#define BLE_HS_LOG(lvl, fmt, ...) \
    BLE_HS_LOG_ ## lvl(fmt, ##__VA_ARGS__)

#define BLE_HS_LOG_ADDR(addr)


void ble_hs_log_mbuf(const struct os_mbuf *om);
void ble_hs_log_flat_buf(const void *data, int len);

#ifdef __cplusplus
}
#endif

#endif

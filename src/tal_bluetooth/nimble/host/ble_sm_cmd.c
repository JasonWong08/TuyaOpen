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

#include <string.h>
#include <errno.h>
//#include "ble.h"
#include "ble_hs_definition.h"
#include "ble_sm.h"
#include "ble_hs_priv.h"
#include "ble_hs_conn_priv.h"
#include "ble_sm_priv.h"
#include "ble_hs_log.h"

#if TY_HS_BLE_CONNECT
void *
ble_sm_cmd_get(uint8_t opcode, size_t len, struct os_mbuf **txom)
{
    struct ble_sm_hdr *hdr;

    *txom = ble_hs_mbuf_l2cap_pkt();
    if (*txom == NULL) {
        return NULL;
    }

    if (os_mbuf_extend(*txom, sizeof(*hdr) + len) == NULL) {
        os_mbuf_free_chain(*txom);
        return NULL;
    }

    hdr = (struct ble_sm_hdr *)(*txom)->om_data;

    hdr->opcode = opcode;

    return hdr->data;
}

/* this function consumes tx os_mbuf */
int
ble_sm_tx(uint16_t conn_handle, struct os_mbuf *txom)
{
    struct ble_l2cap_chan *chan;
    struct ble_hs_conn *conn;
    int rc;

    BLE_HS_DBG_ASSERT(ble_hs_locked_by_cur_task());

    STATS_INC(ble_l2cap_stats, sm_tx);

    rc = ble_hs_misc_conn_chan_find_reqd(conn_handle, BLE_L2CAP_CID_SM,
                                         &conn, &chan);
    if (rc != 0) {
        struct ble_hs_conn *lazy_conn = ble_hs_conn_find(conn_handle);
        if (lazy_conn != NULL) {
            struct ble_l2cap_chan *lazy_chan = ble_sm_create_chan(conn_handle);
            if (lazy_chan != NULL) {
                int lazy_rc = ble_hs_conn_chan_insert(lazy_conn, lazy_chan);
                BLE_HS_LOG(INFO,
                           "SM channel missing, create lazily handle=0x%04x find_rc=%d insert_rc=%d\n",
                           conn_handle, rc, lazy_rc);
                if (lazy_rc == 0 || lazy_rc == BLE_HS_EALREADY) {
                    conn = lazy_conn;
                    chan = ble_hs_conn_chan_find_by_scid(lazy_conn, BLE_L2CAP_CID_SM);
                    rc = (chan != NULL) ? 0 : BLE_HS_ENOTCONN;
                } else {
                    chan = NULL;
                    rc = lazy_rc;
                }
            } else {
                rc = BLE_HS_ENOMEM;
                BLE_HS_LOG(ERR, "SM channel missing, lazy create failed handle=0x%04x\n",
                           conn_handle);
            }
        }
    }
    if (rc == 0) {
        rc = ble_l2cap_tx(conn, chan, txom);
    } else {
        BLE_HS_LOG(ERR, "SM tx failed to find channel handle=0x%04x rc=%d\n",
                   conn_handle, rc);
    }

    return rc;
}

#endif

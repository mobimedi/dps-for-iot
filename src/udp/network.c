/*
 *******************************************************************
 *
 * Copyright 2016 Intel Corporation All rights reserved.
 *
 *-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 */

#include <assert.h>
#include <string.h>
#include <malloc.h>
#include <dps/dbg.h>
#include <dps/dps.h>
#include <dps/private/network.h>
#include "../node.h"

/*
 * Debug control for this module
 */
DPS_DEBUG_CONTROL(DPS_DEBUG_ON);


#define MAX_READ_LEN   4096

struct _DPS_NetContext {
    uv_udp_t rxSocket;
    DPS_Node* node;
    DPS_OnReceive receiveCB;
    char buffer[MAX_READ_LEN];
};

static void AllocBuffer(uv_handle_t* handle, size_t suggestedSize, uv_buf_t* buf)
{
    DPS_NetContext* netCtx = (DPS_NetContext*)handle->data;

    DPS_DBGTRACE();
    buf->len = MAX_READ_LEN;
    buf->base = netCtx->buffer;
}

static void RxHandleClosed(uv_handle_t* handle)
{
    DPS_DBGPRINT("Closed Rx handle %p\n", handle);
    free(handle->data);
}

static void OnData(uv_udp_t* socket, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags)
{
    DPS_NetEndpoint ep;
    DPS_NetContext* netCtx = (DPS_NetContext*)socket->data;

    DPS_DBGTRACE();
    if (nread < 0) {
        DPS_ERRPRINT("OnData error %s\n", uv_err_name((int)nread));
        return;
    }
    if (!nread) {
        return;
    }
    if (!buf) {
        DPS_ERRPRINT("OnData no buffer\n");
        return;
    }
    if (!addr) {
        DPS_ERRPRINT("OnData no address\n");
        return;
    }
    ep.cn = NULL;
    DPS_SetAddress(&ep.addr, addr);
    netCtx->receiveCB(netCtx->node, &ep, DPS_OK, (uint8_t*)buf->base, nread);
}

DPS_NetContext* DPS_NetStart(DPS_Node* node, uint16_t port, DPS_OnReceive cb)
{
    int ret;
    DPS_NetContext* netCtx;
    struct sockaddr_storage addr;

    netCtx = calloc(1, sizeof(*netCtx));
    if (!netCtx) {
        return NULL;
    }
    ret = uv_udp_init(DPS_GetLoop(node), &netCtx->rxSocket);
    if (ret) {
        DPS_ERRPRINT("uv_tcp_init error=%s\n", uv_err_name(ret));
        free(netCtx);
        return NULL;
    }
    netCtx->node = node;
    netCtx->receiveCB = cb;
    ret = uv_ip6_addr("::", port, (struct sockaddr_in6*)&addr);
    if (ret) {
        goto ErrorExit;
    }
    netCtx->rxSocket.data = netCtx;
    ret = uv_udp_bind(&netCtx->rxSocket, (const struct sockaddr*)&addr, 0);
    if (ret) {
        goto ErrorExit;
    }
    ret = uv_udp_recv_start(&netCtx->rxSocket, AllocBuffer, OnData);
    if (ret) {
        goto ErrorExit;
    }
    return netCtx;

ErrorExit:

    DPS_ERRPRINT("Failed to start net netCtx: error=%s\n", uv_err_name(ret));
    uv_close((uv_handle_t*)&netCtx->rxSocket, RxHandleClosed);
    return NULL;
}

uint16_t DPS_NetGetListenerPort(DPS_NetContext* netCtx)
{
    struct sockaddr_in6 addr;
    int len = sizeof(addr);

    if (!netCtx) {
        return 0;
    }
    if (uv_udp_getsockname(&netCtx->rxSocket, (struct sockaddr*)&addr, &len)) {
        return 0;
    }
    DPS_DBGPRINT("Listener port = %d\n", ntohs(addr.sin6_port));
    return ntohs(addr.sin6_port);
}

void DPS_NetStop(DPS_NetContext* netCtx)
{
    if (netCtx) {
        uv_udp_recv_stop(&netCtx->rxSocket);
        uv_close((uv_handle_t*)&netCtx->rxSocket, RxHandleClosed);
    }
}

#define MAX_BUFS 3

typedef struct {
    DPS_Node* node;
    void* appCtx;
    DPS_NetEndpoint peerEp;
    uv_udp_send_t sendReq;
    DPS_NetSendComplete onSendComplete;
    size_t numBufs;
    uv_buf_t bufs[1];
} NetSender;

static void OnSendComplete(uv_udp_send_t* req, int status)
{
    NetSender* sender = (NetSender*)req->data;
    DPS_Status dpsRet = DPS_OK;

    if (status) {
        DPS_ERRPRINT("OnSendComplete status=%s\n", uv_err_name(status));
        dpsRet = DPS_ERR_NETWORK;
    }
    sender->onSendComplete(sender->node, sender->appCtx, &sender->peerEp, sender->bufs, sender->numBufs, dpsRet);
    free(sender);
}

DPS_Status DPS_NetSend(DPS_Node* node, void* appCtx, DPS_NetEndpoint* ep, uv_buf_t* bufs, size_t numBufs, DPS_NetSendComplete sendCompleteCB)
{
    int ret;
    NetSender* sender;

#ifndef NDEBUG
    {
        size_t i;
        size_t len = 0;
        for (i = 0; i < numBufs; ++i) {
            len += bufs[i].len;
        }
        DPS_DBGPRINT("DPS_NetSend total %zu bytes to %s\n", len, DPS_NodeAddrToString(&ep->addr));
    }
#endif

    sender = malloc(sizeof(NetSender) + (numBufs - 1) * sizeof(uv_buf_t));
    if (!sender) {
        return DPS_ERR_RESOURCES;
    }

    sender->sendReq.data = sender;
    sender->onSendComplete = sendCompleteCB;
    sender->appCtx = appCtx;
    sender->peerEp = *ep;
    sender->node = node;
    memcpy_s(sender->bufs, numBufs * sizeof(uv_buf_t), bufs, numBufs * sizeof(uv_buf_t));
    sender->numBufs = numBufs;

    struct sockaddr_storage inaddr;
    memcpy_s(&inaddr, sizeof(inaddr), &ep->addr.inaddr, sizeof(ep->addr.inaddr));
    DPS_MapAddrToV6((struct sockaddr *)&inaddr);

    ret = uv_udp_send(&sender->sendReq, &node->netCtx->rxSocket, sender->bufs, (uint32_t)numBufs, (const struct sockaddr *)&inaddr, OnSendComplete);
    if (ret) {
        DPS_ERRPRINT("DPS_NetSend status=%s\n", uv_err_name(ret));
        free(sender);
        return DPS_ERR_NETWORK;
    }
    return DPS_OK;
}

void DPS_NetConnectionAddRef(DPS_NetConnection* cn)
{
    /* No-op for udp */
}

void DPS_NetConnectionDecRef(DPS_NetConnection* cn)
{
    /* No-op for udp */
}

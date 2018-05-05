/*
 * tunnel.h - Define tunnel's buffers and callbacks
 *
 * Copyright (C) 2013 - 2016, Max Lv <max.c.lv@gmail.com>
 *
 * This file is part of the shadowsocks-libev.
 *
 * shadowsocks-libev is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * shadowsocks-libev is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with shadowsocks-libev; see the file COPYING. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef _TUNNEL_H
#define _TUNNEL_H

#include <ev.h>
#include "encrypt.h"
#include "obfs/obfs.h"
#include "jconf.h"

#include "common.h"

typedef struct listen_ctx {
    ev_io io;
    ss_addr_t tunnel_addr;
    char *iface;
    int remote_num;
    int method;
    int timeout;
    int fd;
    int mptcp;
    struct sockaddr **remote_addr;

    // SSR
    char *protocol_name;
    char *protocol_param;
    char *obfs_name;
    char *obfs_param;
    void **list_protocol_global;
    void **list_obfs_global;
} listen_ctx_t;

typedef struct server_ctx {
    ev_io io;
    int connected;
    struct server *server;
} server_ctx_t;

typedef struct server {
    int fd;
    buffer_t *buf;
    ssize_t buf_capacity;
    struct enc_ctx *e_ctx;
    struct enc_ctx *d_ctx;
    struct server_ctx *recv_ctx;
    struct server_ctx *send_ctx;
    struct remote *remote;
    ss_addr_t destaddr;

    // SSR
    obfs *protocol;
    obfs *obfs;
    obfs_class *protocol_plugin;
    obfs_class *obfs_plugin;
} server_t;

typedef struct remote_ctx {
    ev_io io;
    ev_timer watcher;
    int connected;
    struct remote *remote;
} remote_ctx_t;

typedef struct remote {
    int fd;
    buffer_t *buf;
    ssize_t buf_capacity;
    struct remote_ctx *recv_ctx;
    struct remote_ctx *send_ctx;
    struct server *server;
    uint32_t counter;

    // SSR
    int remote_index;
} remote_t;

#endif // _TUNNEL_H

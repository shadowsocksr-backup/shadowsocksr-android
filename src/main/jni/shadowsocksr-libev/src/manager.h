/*
 * server.h - Define shadowsocks server's buffers and callbacks
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

#ifndef _MANAGER_H
#define _MANAGER_H

#include <ev.h>
#include <time.h>
#include <libcork/ds.h>

#include "jconf.h"

#include "common.h"

struct manager_ctx {
    ev_io io;
    int fd;
    int fast_open;
    int verbose;
    int mode;
    char *password;
    char *timeout;
    char *method;
    char *iface;
    char *acl;
    char *user;
    char *manager_address;
    char **hosts;
    int host_num;
    char **nameservers;
    int nameserver_num;
    int mtu;
#ifdef HAVE_SETRLIMIT
    int nofile;
#endif
};

struct server {
    char port[8];
    char password[128];
    uint64_t traffic;
};

#endif // _MANAGER_H

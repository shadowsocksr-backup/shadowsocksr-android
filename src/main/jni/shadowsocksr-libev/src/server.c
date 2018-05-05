/*
 * server.c - Provide shadowsocks service
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <math.h>

#ifndef __MINGW32__
#include <netdb.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/un.h>
#endif

#include <libcork/core.h>
#include <udns.h>

#ifdef __MINGW32__
#include "win32.h"
#endif

#if defined(HAVE_SYS_IOCTL_H) && defined(HAVE_NET_IF_H) && defined(__linux__)
#include <net/if.h>
#include <sys/ioctl.h>
#define SET_INTERFACE
#endif

#include "netutils.h"
#include "utils.h"
#include "acl.h"
#include "server.h"

#ifndef EAGAIN
#define EAGAIN EWOULDBLOCK
#endif

#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

#ifndef BUF_SIZE
#define BUF_SIZE 2048
#endif

#ifndef SSMAXCONN
#define SSMAXCONN 1024
#endif

#ifndef UPDATE_INTERVAL
#define UPDATE_INTERVAL 30
#endif

static void signal_cb(EV_P_ ev_signal *w, int revents);
static void accept_cb(EV_P_ ev_io *w, int revents);
static void server_send_cb(EV_P_ ev_io *w, int revents);
static void server_recv_cb(EV_P_ ev_io *w, int revents);
static void remote_recv_cb(EV_P_ ev_io *w, int revents);
static void remote_send_cb(EV_P_ ev_io *w, int revents);
static void server_timeout_cb(EV_P_ ev_timer *watcher, int revents);
static void block_list_clear_cb(EV_P_ ev_timer *watcher, int revents);

static remote_t *new_remote(int fd);
static server_t *new_server(int fd, listen_ctx_t *listener);
static remote_t *connect_to_remote(EV_P_ struct addrinfo *res,
                                   server_t *server);

static void free_remote(remote_t *remote);
static void close_and_free_remote(EV_P_ remote_t *remote);
static void free_server(server_t *server);
static void close_and_free_server(EV_P_ server_t *server);
static void server_resolve_cb(struct sockaddr *addr, void *data);
static void query_free_cb(void *data);

static int is_header_complete(const buffer_t *buf);

int verbose = 0;

static int acl       = 0;
static int mode      = TCP_ONLY;
static int ipv6first = 0;

static int fast_open = 0;
#ifdef HAVE_SETRLIMIT
static int nofile = 0;
#endif
static int remote_conn = 0;
static int server_conn = 0;

static char *bind_address    = NULL;
static char *server_port     = NULL;
static char *manager_address = NULL;
uint64_t tx                  = 0;
uint64_t rx                  = 0;
ev_timer stat_update_watcher;
ev_timer block_list_watcher;

static struct cork_dllist connections;

static void
stat_update_cb(EV_P_ ev_timer *watcher, int revents)
{
#ifndef __MINGW32__
    struct sockaddr_un svaddr, claddr;
#endif

    int sfd = -1;
    size_t msgLen;
    char resp[BUF_SIZE];

    if (verbose) {
        LOGI("update traffic stat: tx: %" PRIu64 " rx: %" PRIu64 "", tx, rx);
    }

    snprintf(resp, BUF_SIZE, "stat: {\"%s\":%" PRIu64 "}", server_port, tx + rx);
    msgLen = strlen(resp) + 1;

    ss_addr_t ip_addr = { .host = NULL, .port = NULL };
    parse_addr(manager_address, &ip_addr);

    if (ip_addr.host == NULL || ip_addr.port == NULL) {
#ifndef __MINGW32__
        sfd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (sfd == -1) {
            ERROR("stat_socket");
            return;
        }

        memset(&claddr, 0, sizeof(struct sockaddr_un));
        claddr.sun_family = AF_UNIX;
        snprintf(claddr.sun_path, sizeof(claddr.sun_path), "/tmp/shadowsocks.%s", server_port);

        unlink(claddr.sun_path);

        if (bind(sfd, (struct sockaddr *)&claddr, sizeof(struct sockaddr_un)) == -1) {
            ERROR("stat_bind");
            close(sfd);
            return;
        }

        memset(&svaddr, 0, sizeof(struct sockaddr_un));
        svaddr.sun_family = AF_UNIX;
        strncpy(svaddr.sun_path, manager_address, sizeof(svaddr.sun_path) - 1);

        if (sendto(sfd, resp, strlen(resp) + 1, 0, (struct sockaddr *)&svaddr,
                   sizeof(struct sockaddr_un)) != msgLen) {
            ERROR("stat_sendto");
            close(sfd);
            return;
        }

        unlink(claddr.sun_path);
#else
        ERROR("unsupported platform");
        return;
#endif
    } else {
        struct sockaddr_storage storage;
        memset(&storage, 0, sizeof(struct sockaddr_storage));
        if (get_sockaddr(ip_addr.host, ip_addr.port, &storage, 0, ipv6first) == -1) {
            ERROR("failed to parse the manager addr");
            return;
        }

        sfd = socket(storage.ss_family, SOCK_DGRAM, 0);

        if (sfd == -1) {
            ERROR("stat_socket");
            return;
        }

        size_t addr_len = get_sockaddr_len((struct sockaddr *)&storage);
        if (sendto(sfd, resp, strlen(resp) + 1, 0, (struct sockaddr *)&storage,
                   addr_len) != msgLen) {
            ERROR("stat_sendto");
            close(sfd);
            return;
        }
    }

    close(sfd);
}

static void
free_connections(struct ev_loop *loop)
{
    struct cork_dllist_item *curr, *next;
    cork_dllist_foreach_void(&connections, curr, next) {
        server_t *server = cork_container_of(curr, server_t, entries);
        remote_t *remote = server->remote;
        close_and_free_server(loop, server);
        close_and_free_remote(loop, remote);
    }
}

static int
is_header_complete(const buffer_t *buf)
{
    size_t header_len = 0;
    size_t buf_len    = buf->len;

    char atyp = buf->array[header_len];

    // 1 byte for atyp
    header_len++;

    if ((atyp & ADDRTYPE_MASK) == 1) {
        // IP V4
        header_len += sizeof(struct in_addr);
    } else if ((atyp & ADDRTYPE_MASK) == 3) {
        // Domain name
        // domain len + len of domain
        if (buf_len < header_len + 1)
            return 0;
        uint8_t name_len = *(uint8_t *)(buf->array + header_len);
        header_len += name_len + 1;
    } else if ((atyp & ADDRTYPE_MASK) == 4) {
        // IP V6
        header_len += sizeof(struct in6_addr);
    } else {
        return -1;
    }

    // len of port
    header_len += 2;

    return buf_len >= header_len ? 1 : 0;
}

static char *
get_peer_name(int fd)
{
    static char peer_name[INET6_ADDRSTRLEN] = { 0 };
    struct sockaddr_storage addr;
    socklen_t len = sizeof(struct sockaddr_storage);
    memset(&addr, 0, len);
    memset(peer_name, 0, INET6_ADDRSTRLEN);
    int err = getpeername(fd, (struct sockaddr *)&addr, &len);
    if (err == 0) {
        if (addr.ss_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)&addr;
            dns_ntop(AF_INET, &s->sin_addr, peer_name, INET_ADDRSTRLEN);
        } else if (addr.ss_family == AF_INET6) {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
            dns_ntop(AF_INET6, &s->sin6_addr, peer_name, INET6_ADDRSTRLEN);
        }
    } else {
        return NULL;
    }
    return peer_name;
}

#ifdef __linux__
static void
set_linger(int fd)
{
    struct linger so_linger;
    memset(&so_linger, 0, sizeof(struct linger));
    so_linger.l_onoff  = 1;
    so_linger.l_linger = 0;
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &so_linger, sizeof so_linger);
}
#endif

static void
reset_addr(int fd)
{
    char *peer_name;
    peer_name = get_peer_name(fd);
    if (peer_name != NULL) {
        remove_from_block_list(peer_name);
    }
}

static void
report_addr(int fd, int err_level)
{
#ifdef __linux__
    set_linger(fd);
#endif

    char *peer_name;
    peer_name = get_peer_name(fd);
    if (peer_name != NULL) {
        LOGE("failed to handshake with %s", peer_name);
        update_block_list(peer_name, err_level);
    }
}

int
setfastopen(int fd)
{
    int s = 0;
#ifdef TCP_FASTOPEN
    if (fast_open) {
#ifdef __APPLE__
        int opt = 1;
#else
        int opt = 5;
#endif
        s = setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, &opt, sizeof(opt));

        if (s == -1) {
            if (errno == EPROTONOSUPPORT || errno == ENOPROTOOPT) {
                LOGE("fast open is not supported on this platform");
                fast_open = 0;
            } else {
                ERROR("setsockopt");
            }
        }
    }
#endif
    return s;
}

#ifndef __MINGW32__
int
setnonblocking(int fd)
{
    int flags;
    if (-1 == (flags = fcntl(fd, F_GETFL, 0))) {
        flags = 0;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

#endif

int
create_and_bind(const char *host, const char *port, int mptcp)
{
    struct addrinfo hints;
    struct addrinfo *result, *rp, *ipv4v6bindall;
    int s, listen_sock;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family   = AF_UNSPEC;               /* Return IPv4 and IPv6 choices */
    hints.ai_socktype = SOCK_STREAM;             /* We want a TCP socket */
    hints.ai_flags    = AI_PASSIVE | AI_ADDRCONFIG; /* For wildcard IP address */
    hints.ai_protocol = IPPROTO_TCP;

    for (int i = 1; i < 8; i++) {
        s = getaddrinfo(host, port, &hints, &result);
        if (s == 0) {
            break;
        } else {
            sleep(pow(2, i));
            LOGE("failed to resolve server name, wait %.0f seconds", pow(2, i));
        }
    }

    if (s != 0) {
        LOGE("getaddrinfo: %s", gai_strerror(s));
        return -1;
    }

    rp = result;

    /*
     * On Linux, with net.ipv6.bindv6only = 0 (the default), getaddrinfo(NULL) with
     * AI_PASSIVE returns 0.0.0.0 and :: (in this order). AI_PASSIVE was meant to
     * return a list of addresses to listen on, but it is impossible to listen on
     * 0.0.0.0 and :: at the same time, if :: implies dualstack mode.
     */
    if (!host) {
        ipv4v6bindall = result;

        /* Loop over all address infos found until a IPV6 address is found. */
        while (ipv4v6bindall) {
            if (ipv4v6bindall->ai_family == AF_INET6) {
                rp = ipv4v6bindall; /* Take first IPV6 address available */
                break;
            }
            ipv4v6bindall = ipv4v6bindall->ai_next; /* Get next address info, if any */
        }
    }

    for (/*rp = result*/; rp != NULL; rp = rp->ai_next) {
        listen_sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listen_sock == -1) {
            continue;
        }

        if (rp->ai_family == AF_INET6) {
            int ipv6only = host ? 1 : 0;
            setsockopt(listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6only, sizeof(ipv6only));
        }

        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_NOSIGPIPE
        setsockopt(listen_sock, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif
        int err = set_reuseport(listen_sock);
        if (err == 0) {
            LOGI("tcp port reuse enabled");
        }

        if (mptcp == 1) {
            int err = setsockopt(listen_sock, SOL_TCP, MPTCP_ENABLED, &opt, sizeof(opt));
            if (err == -1) {
                ERROR("failed to enable multipath TCP");
            }
        }

        s = bind(listen_sock, rp->ai_addr, rp->ai_addrlen);
        if (s == 0) {
            /* We managed to bind successfully! */
            break;
        } else {
            ERROR("bind");
        }

        close(listen_sock);
    }

    if (rp == NULL) {
        LOGE("Could not bind");
        return -1;
    }

    freeaddrinfo(result);

    return listen_sock;
}

static remote_t *
connect_to_remote(EV_P_ struct addrinfo *res,
                  server_t *server)
{
    int sockfd;
#ifdef SET_INTERFACE
    const char *iface = server->listen_ctx->iface;
#endif

    if (acl) {
        char ipstr[INET6_ADDRSTRLEN];
        memset(ipstr, 0, INET6_ADDRSTRLEN);

        if (res->ai_addr->sa_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)res->ai_addr;
            dns_ntop(AF_INET, &s->sin_addr, ipstr, INET_ADDRSTRLEN);
        } else if (res->ai_addr->sa_family == AF_INET6) {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)res->ai_addr;
            dns_ntop(AF_INET6, &s->sin6_addr, ipstr, INET6_ADDRSTRLEN);
        }

        if (outbound_block_match_host(ipstr) == 1) {
            if (verbose)
                LOGI("outbound blocked %s", ipstr);
            return NULL;
        }
    }

    // initialize remote socks
    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd == -1) {
        ERROR("socket");
        close(sockfd);
        return NULL;
    }

    int opt = 1;
    setsockopt(sockfd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt));
#ifdef SO_NOSIGPIPE
    setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // setup remote socks

    if (setnonblocking(sockfd) == -1)
        ERROR("setnonblocking");

    if (bind_address != NULL)
        if (bind_to_address(sockfd, bind_address) == -1) {
            ERROR("bind_to_address");
            close(sockfd);
            return NULL;
        }

#ifdef SET_INTERFACE
    if (iface) {
        if (setinterface(sockfd, iface) == -1) {
            ERROR("setinterface");
            close(sockfd);
            return NULL;
        }
    }
#endif

    remote_t *remote = new_remote(sockfd);

#ifdef TCP_FASTOPEN
    if (fast_open) {
#ifdef __APPLE__
        ((struct sockaddr_in *)(res->ai_addr))->sin_len = sizeof(struct sockaddr_in);
        sa_endpoints_t endpoints;
        memset((char *)&endpoints, 0, sizeof(endpoints));
        endpoints.sae_dstaddr    = res->ai_addr;
        endpoints.sae_dstaddrlen = res->ai_addrlen;

        struct iovec iov;
        iov.iov_base = server->buf->array + server->buf->idx;
        iov.iov_len  = server->buf->len;
        size_t len;
        int s = connectx(sockfd, &endpoints, SAE_ASSOCID_ANY, CONNECT_DATA_IDEMPOTENT,
                         &iov, 1, &len, NULL);
        if (s == 0) {
            s = len;
        }
#else
        ssize_t s = sendto(sockfd, server->buf->array + server->buf->idx,
                           server->buf->len, MSG_FASTOPEN, res->ai_addr,
                           res->ai_addrlen);
#endif
        if (s == -1) {
            if (errno == CONNECT_IN_PROGRESS || errno == EAGAIN
                || errno == EWOULDBLOCK) {
                // The remote server doesn't support tfo or it's the first connection to the server.
                // It will automatically fall back to conventional TCP.
            } else if (errno == EOPNOTSUPP || errno == EPROTONOSUPPORT ||
                       errno == ENOPROTOOPT) {
                // Disable fast open as it's not supported
                fast_open = 0;
                LOGE("fast open is not supported on this platform");
            } else {
                ERROR("sendto");
            }
        } else if (s <= server->buf->len) {
            server->buf->idx += s;
            server->buf->len -= s;
        } else {
            server->buf->idx = 0;
            server->buf->len = 0;
        }
    }
#endif

    if (!fast_open) {
        int r = connect(sockfd, res->ai_addr, res->ai_addrlen);

        if (r == -1 && errno != CONNECT_IN_PROGRESS) {
            ERROR("connect");
            close_and_free_remote(EV_A_ remote);
            return NULL;
        }
    }

    return remote;
}

static void
server_recv_cb(EV_P_ ev_io *w, int revents)
{
    server_ctx_t *server_recv_ctx = (server_ctx_t *)w;
    server_t *server              = server_recv_ctx->server;
    remote_t *remote              = NULL;

    int len       = server->buf->len;
    buffer_t *buf = server->buf;

    if (server->stage > STAGE_PARSE) {
        remote = server->remote;
        buf    = remote->buf;
        len    = 0;

        ev_timer_again(EV_A_ & server->recv_ctx->watcher);
    }

    if (len > BUF_SIZE) {
        ERROR("out of recv buffer");
        close_and_free_remote(EV_A_ remote);
        close_and_free_server(EV_A_ server);
        return;
    }

    ssize_t r = recv(server->fd, buf->array + len, BUF_SIZE - len, 0);

    if (r == 0) {
        // connection closed
        if (verbose) {
            LOGI("server_recv close the connection");
        }
        close_and_free_remote(EV_A_ remote);
        close_and_free_server(EV_A_ server);
        return;
    } else if (r == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // no data
            // continue to wait for recv
            return;
        } else {
            ERROR("server recv");
            close_and_free_remote(EV_A_ remote);
            close_and_free_server(EV_A_ server);
            return;
        }
    }

    tx += r;

    if (server->stage == STAGE_ERROR) {
        server->buf->len = 0;
        server->buf->idx = 0;
        return;
    }

    // handle incomplete header part 1
    if (server->stage == STAGE_INIT) {
        buf->len += r;
        if (buf->len <= enc_get_iv_len(&cipher_env) + 1) {
            // wait for more
            return;
        }
    } else {
        buf->len = r;
    }

    int err = ss_decrypt(&cipher_env, buf, server->d_ctx, BUF_SIZE);

    if (err) {
        report_addr(server->fd, MALICIOUS);
        close_and_free_remote(EV_A_ remote);
        close_and_free_server(EV_A_ server);
        return;
    }

    // handle incomplete header part 2
    if (server->stage == STAGE_INIT) {
        int ret = is_header_complete(server->buf);
        if (ret == 1) {
            bfree(server->header_buf);
            ss_free(server->header_buf);
            server->stage = STAGE_PARSE;
        } else if (ret == -1) {
            server->stage = STAGE_ERROR;
            report_addr(server->fd, MALFORMED);
            server->buf->len = 0;
            server->buf->idx = 0;
            return;
        } else {
            server->stage = STAGE_HANDSHAKE;
        }
    }

    if (server->stage == STAGE_HANDSHAKE) {
        size_t header_len = server->header_buf->len;
        brealloc(server->header_buf, server->buf->len + header_len, BUF_SIZE);
        memcpy(server->header_buf->array + header_len,
               server->buf->array, server->buf->len);
        server->header_buf->len = server->buf->len + header_len;

        int ret = is_header_complete(server->buf);

        if (ret == 1) {
            brealloc(server->buf, server->header_buf->len, BUF_SIZE);
            memcpy(server->buf->array, server->header_buf->array, server->header_buf->len);
            server->buf->len = server->header_buf->len;
            bfree(server->header_buf);
            ss_free(server->header_buf);
            server->stage = STAGE_PARSE;
        } else {
            if (ret == -1)
                server->stage = STAGE_ERROR;
            server->buf->len = 0;
            server->buf->idx = 0;
            return;
        }
    }

    // handshake and transmit data
    if (server->stage == STAGE_STREAM) {
        int s = send(remote->fd, remote->buf->array, remote->buf->len, 0);
        if (s == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // no data, wait for send
                remote->buf->idx = 0;
                ev_io_stop(EV_A_ & server_recv_ctx->io);
                ev_io_start(EV_A_ & remote->send_ctx->io);
            } else {
                ERROR("server_recv_send");
                close_and_free_remote(EV_A_ remote);
                close_and_free_server(EV_A_ server);
            }
        } else if (s < remote->buf->len) {
            remote->buf->len -= s;
            remote->buf->idx  = s;
            ev_io_stop(EV_A_ & server_recv_ctx->io);
            ev_io_start(EV_A_ & remote->send_ctx->io);
        }
        return;
    } else if (server->stage == STAGE_PARSE) {
        /*
         * Shadowsocks TCP Relay Header:
         *
         *    +------+----------+----------+
         *    | ATYP | DST.ADDR | DST.PORT |
         *    +------+----------+----------+
         *    |  1   | Variable |    2     |
         *    +------+----------+----------+
         */

        /*
         * TCP Relay's payload
         *
         *    +-------------+------+
         *    |    DATA     |      ...
         *    +-------------+------+
         *    |  Variable   |      ...
         *    +-------------+------+
         */

        int offset     = 0;
        int need_query = 0;
        char atyp      = server->buf->array[offset++];
        char host[257] = { 0 };
        uint16_t port  = 0;
        struct addrinfo info;
        struct sockaddr_storage storage;
        memset(&info, 0, sizeof(struct addrinfo));
        memset(&storage, 0, sizeof(struct sockaddr_storage));

        // get remote addr and port
        if ((atyp & ADDRTYPE_MASK) == 1) {
            // IP V4
            struct sockaddr_in *addr = (struct sockaddr_in *)&storage;
            size_t in_addr_len       = sizeof(struct in_addr);
            addr->sin_family = AF_INET;
            if (server->buf->len >= in_addr_len + 3) {
                addr->sin_addr = *(struct in_addr *)(server->buf->array + offset);
                dns_ntop(AF_INET, (const void *)(server->buf->array + offset),
                         host, INET_ADDRSTRLEN);
                offset += in_addr_len;
            } else {
                LOGE("invalid header with addr type %d", atyp);
                report_addr(server->fd, MALFORMED);
                close_and_free_server(EV_A_ server);
                return;
            }
            addr->sin_port   = *(uint16_t *)(server->buf->array + offset);
            info.ai_family   = AF_INET;
            info.ai_socktype = SOCK_STREAM;
            info.ai_protocol = IPPROTO_TCP;
            info.ai_addrlen  = sizeof(struct sockaddr_in);
            info.ai_addr     = (struct sockaddr *)addr;
        } else if ((atyp & ADDRTYPE_MASK) == 3) {
            // Domain name
            uint8_t name_len = *(uint8_t *)(server->buf->array + offset);
            if (name_len + 4 <= server->buf->len) {
                memcpy(host, server->buf->array + offset + 1, name_len);
                offset += name_len + 1;
            } else {
                LOGE("invalid name length: %d", name_len);
                report_addr(server->fd, MALFORMED);
                close_and_free_server(EV_A_ server);
                return;
            }
            if (acl && outbound_block_match_host(host) == 1) {
                if (verbose)
                    LOGI("outbound blocked %s", host);
                close_and_free_server(EV_A_ server);
                return;
            }
            struct cork_ip ip;
            if (cork_ip_init(&ip, host) != -1) {
                info.ai_socktype = SOCK_STREAM;
                info.ai_protocol = IPPROTO_TCP;
                if (ip.version == 4) {
                    struct sockaddr_in *addr = (struct sockaddr_in *)&storage;
                    dns_pton(AF_INET, host, &(addr->sin_addr));
                    addr->sin_port   = *(uint16_t *)(server->buf->array + offset);
                    addr->sin_family = AF_INET;
                    info.ai_family   = AF_INET;
                    info.ai_addrlen  = sizeof(struct sockaddr_in);
                    info.ai_addr     = (struct sockaddr *)addr;
                } else if (ip.version == 6) {
                    struct sockaddr_in6 *addr = (struct sockaddr_in6 *)&storage;
                    dns_pton(AF_INET6, host, &(addr->sin6_addr));
                    addr->sin6_port   = *(uint16_t *)(server->buf->array + offset);
                    addr->sin6_family = AF_INET6;
                    info.ai_family    = AF_INET6;
                    info.ai_addrlen   = sizeof(struct sockaddr_in6);
                    info.ai_addr      = (struct sockaddr *)addr;
                }
            } else {
                if (!validate_hostname(host, name_len)) {
                    LOGE("invalid host name");
                    report_addr(server->fd, MALFORMED);
                    close_and_free_server(EV_A_ server);
                    return;
                }
                need_query = 1;
            }
        } else if ((atyp & ADDRTYPE_MASK) == 4) {
            // IP V6
            struct sockaddr_in6 *addr = (struct sockaddr_in6 *)&storage;
            size_t in6_addr_len       = sizeof(struct in6_addr);
            addr->sin6_family = AF_INET6;
            if (server->buf->len >= in6_addr_len + 3) {
                addr->sin6_addr = *(struct in6_addr *)(server->buf->array + offset);
                dns_ntop(AF_INET6, (const void *)(server->buf->array + offset),
                         host, INET6_ADDRSTRLEN);
                offset += in6_addr_len;
            } else {
                LOGE("invalid header with addr type %d", atyp);
                report_addr(server->fd, MALFORMED);
                close_and_free_server(EV_A_ server);
                return;
            }
            addr->sin6_port  = *(uint16_t *)(server->buf->array + offset);
            info.ai_family   = AF_INET6;
            info.ai_socktype = SOCK_STREAM;
            info.ai_protocol = IPPROTO_TCP;
            info.ai_addrlen  = sizeof(struct sockaddr_in6);
            info.ai_addr     = (struct sockaddr *)addr;
        }

        if (offset == 1) {
            LOGE("invalid header with addr type %d", atyp);
            report_addr(server->fd, MALFORMED);
            close_and_free_server(EV_A_ server);
            return;
        }

        port = (*(uint16_t *)(server->buf->array + offset));

        offset += 2;

        if (server->buf->len < offset) {
            report_addr(server->fd, MALFORMED);
            close_and_free_server(EV_A_ server);
            return;
        } else {
            server->buf->len -= offset;
            memmove(server->buf->array, server->buf->array + offset, server->buf->len);
        }

        if (verbose) {
            if ((atyp & ADDRTYPE_MASK) == 4)
                LOGI("connect to [%s]:%d", host, ntohs(port));
            else
                LOGI("connect to %s:%d", host, ntohs(port));
        }

        if (!need_query) {
            remote_t *remote = connect_to_remote(EV_A_ &info, server);

            if (remote == NULL) {
                LOGE("connect error");
                close_and_free_server(EV_A_ server);
                return;
            } else {
                server->remote = remote;
                remote->server = server;

                // XXX: should handle buffer carefully
                if (server->buf->len > 0) {
                    memcpy(remote->buf->array, server->buf->array, server->buf->len);
                    remote->buf->len = server->buf->len;
                    remote->buf->idx = 0;
                    server->buf->len = 0;
                    server->buf->idx = 0;
                }

                // waiting on remote connected event
                ev_io_stop(EV_A_ & server_recv_ctx->io);
                ev_io_start(EV_A_ & remote->send_ctx->io);
            }
        } else {
            query_t *query = ss_malloc(sizeof(query_t));
            memset(query, 0, sizeof(query_t));
            query->server = server;
            snprintf(query->hostname, 256, "%s", host);

            server->stage = STAGE_RESOLVE;
            server->query = resolv_query(host, server_resolve_cb,
                                         query_free_cb, query, port);

            ev_io_stop(EV_A_ & server_recv_ctx->io);
        }

        return;
    }
    // should not reach here
    FATAL("server context error");
}

static void
server_send_cb(EV_P_ ev_io *w, int revents)
{
    server_ctx_t *server_send_ctx = (server_ctx_t *)w;
    server_t *server              = server_send_ctx->server;
    remote_t *remote              = server->remote;

    if (remote == NULL) {
        LOGE("invalid server");
        close_and_free_server(EV_A_ server);
        return;
    }

    if (server->buf->len == 0) {
        // close and free
        if (verbose) {
            LOGI("server_send close the connection");
        }
        close_and_free_remote(EV_A_ remote);
        close_and_free_server(EV_A_ server);
        return;
    } else {
        // has data to send
        ssize_t s = send(server->fd, server->buf->array + server->buf->idx,
                         server->buf->len, 0);
        if (s == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ERROR("server_send_send");
                close_and_free_remote(EV_A_ remote);
                close_and_free_server(EV_A_ server);
            }
            return;
        } else if (s < server->buf->len) {
            // partly sent, move memory, wait for the next time to send
            server->buf->len -= s;
            server->buf->idx += s;
            return;
        } else {
            // all sent out, wait for reading
            server->buf->len = 0;
            server->buf->idx = 0;
            ev_io_stop(EV_A_ & server_send_ctx->io);
            if (remote != NULL) {
                ev_io_start(EV_A_ & remote->recv_ctx->io);
                return;
            } else {
                LOGE("invalid remote");
                close_and_free_remote(EV_A_ remote);
                close_and_free_server(EV_A_ server);
                return;
            }
        }
    }
}

static void
block_list_clear_cb(EV_P_ ev_timer *watcher, int revents)
{
    clear_block_list();
}

static void
server_timeout_cb(EV_P_ ev_timer *watcher, int revents)
{
    server_ctx_t *server_ctx
        = cork_container_of(watcher, server_ctx_t, watcher);
    server_t *server = server_ctx->server;
    remote_t *remote = server->remote;

    if (verbose) {
        LOGI("TCP connection timeout");
    }

    if (server->stage < STAGE_PARSE) {
        if (verbose) {
            size_t len = server->stage ?
                         server->header_buf->len : server->buf->len;
#ifdef __MINGW32__
            LOGI("incomplete header: %u", len);
#else
            LOGI("incomplete header: %zu", len);
#endif
        }
        report_addr(server->fd, SUSPICIOUS);
    }

    close_and_free_remote(EV_A_ remote);
    close_and_free_server(EV_A_ server);
}

static void
query_free_cb(void *data)
{
    if (data != NULL) {
        ss_free(data);
    }
}

static void
server_resolve_cb(struct sockaddr *addr, void *data)
{
    query_t *query       = (query_t *)data;
    server_t *server     = query->server;
    struct ev_loop *loop = server->listen_ctx->loop;

    server->query = NULL;

    if (addr == NULL) {
        LOGE("unable to resolve %s", query->hostname);
        close_and_free_server(EV_A_ server);
    } else {
        if (verbose) {
            LOGI("successfully resolved %s", query->hostname);
        }

        struct addrinfo info;
        memset(&info, 0, sizeof(struct addrinfo));
        info.ai_socktype = SOCK_STREAM;
        info.ai_protocol = IPPROTO_TCP;
        info.ai_addr     = addr;

        if (addr->sa_family == AF_INET) {
            info.ai_family  = AF_INET;
            info.ai_addrlen = sizeof(struct sockaddr_in);
        } else if (addr->sa_family == AF_INET6) {
            info.ai_family  = AF_INET6;
            info.ai_addrlen = sizeof(struct sockaddr_in6);
        }

        remote_t *remote = connect_to_remote(EV_A_ &info, server);

        if (remote == NULL) {
            close_and_free_server(EV_A_ server);
        } else {
            server->remote = remote;
            remote->server = server;

            // XXX: should handle buffer carefully
            if (server->buf->len > 0) {
                memcpy(remote->buf->array, server->buf->array + server->buf->idx,
                       server->buf->len);
                remote->buf->len = server->buf->len;
                remote->buf->idx = 0;
                server->buf->len = 0;
                server->buf->idx = 0;
            }

            // listen to remote connected event
            ev_io_start(EV_A_ & remote->send_ctx->io);
        }
    }
}

static void
remote_recv_cb(EV_P_ ev_io *w, int revents)
{
    remote_ctx_t *remote_recv_ctx = (remote_ctx_t *)w;
    remote_t *remote              = remote_recv_ctx->remote;
    server_t *server              = remote->server;

    if (server == NULL) {
        LOGE("invalid server");
        close_and_free_remote(EV_A_ remote);
        return;
    }

    ev_timer_again(EV_A_ & server->recv_ctx->watcher);

    ssize_t r = recv(remote->fd, server->buf->array, BUF_SIZE, 0);

    if (r == 0) {
        // connection closed
        if (verbose) {
            LOGI("remote_recv close the connection");
        }
        close_and_free_remote(EV_A_ remote);
        close_and_free_server(EV_A_ server);
        return;
    } else if (r == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // no data
            // continue to wait for recv
            return;
        } else {
            ERROR("remote recv");
            close_and_free_remote(EV_A_ remote);
            close_and_free_server(EV_A_ server);
            return;
        }
    }

    rx += r;

    server->buf->len = r;
    int err = ss_encrypt(&cipher_env, server->buf, server->e_ctx, BUF_SIZE);

    if (err) {
        LOGE("invalid password or cipher");
        close_and_free_remote(EV_A_ remote);
        close_and_free_server(EV_A_ server);
        return;
    }

    int s = send(server->fd, server->buf->array, server->buf->len, 0);

    if (s == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // no data, wait for send
            server->buf->idx = 0;
            ev_io_stop(EV_A_ & remote_recv_ctx->io);
            ev_io_start(EV_A_ & server->send_ctx->io);
        } else {
            ERROR("remote_recv_send");
            close_and_free_remote(EV_A_ remote);
            close_and_free_server(EV_A_ server);
        }
    } else if (s < server->buf->len) {
        server->buf->len -= s;
        server->buf->idx  = s;
        ev_io_stop(EV_A_ & remote_recv_ctx->io);
        ev_io_start(EV_A_ & server->send_ctx->io);
    }

    // Disable TCP_NODELAY after the first response are sent
    int opt = 0;
    setsockopt(server->fd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt));
    setsockopt(remote->fd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

static void
remote_send_cb(EV_P_ ev_io *w, int revents)
{
    remote_ctx_t *remote_send_ctx = (remote_ctx_t *)w;
    remote_t *remote              = remote_send_ctx->remote;
    server_t *server              = remote->server;

    if (server == NULL) {
        LOGE("invalid server");
        close_and_free_remote(EV_A_ remote);
        return;
    }

    if (!remote_send_ctx->connected) {
        struct sockaddr_storage addr;
        socklen_t len = sizeof(struct sockaddr_storage);
        memset(&addr, 0, len);
        int r = getpeername(remote->fd, (struct sockaddr *)&addr, &len);
        if (r == 0) {
            if (verbose) {
                LOGI("remote connected");
            }
            remote_send_ctx->connected = 1;

            // Clear the state of this address in the block list
            reset_addr(server->fd);

            if (remote->buf->len == 0) {
                server->stage = STAGE_STREAM;
                ev_io_stop(EV_A_ & remote_send_ctx->io);
                ev_io_start(EV_A_ & server->recv_ctx->io);
                ev_io_start(EV_A_ & remote->recv_ctx->io);
                return;
            }
        } else {
            ERROR("getpeername");
            // not connected
            close_and_free_remote(EV_A_ remote);
            close_and_free_server(EV_A_ server);
            return;
        }
    }

    if (remote->buf->len == 0) {
        // close and free
        if (verbose) {
            LOGI("remote_send close the connection");
        }
        close_and_free_remote(EV_A_ remote);
        close_and_free_server(EV_A_ server);
        return;
    } else {
        // has data to send
        ssize_t s = send(remote->fd, remote->buf->array + remote->buf->idx,
                         remote->buf->len, 0);
        if (s == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ERROR("remote_send_send");
                // close and free
                close_and_free_remote(EV_A_ remote);
                close_and_free_server(EV_A_ server);
            }
            return;
        } else if (s < remote->buf->len) {
            // partly sent, move memory, wait for the next time to send
            remote->buf->len -= s;
            remote->buf->idx += s;
            return;
        } else {
            // all sent out, wait for reading
            remote->buf->len = 0;
            remote->buf->idx = 0;
            ev_io_stop(EV_A_ & remote_send_ctx->io);
            if (server != NULL) {
                ev_io_start(EV_A_ & server->recv_ctx->io);
                if (server->stage != STAGE_STREAM) {
                    server->stage = STAGE_STREAM;
                    ev_io_start(EV_A_ & remote->recv_ctx->io);
                }
            } else {
                LOGE("invalid server");
                close_and_free_remote(EV_A_ remote);
                close_and_free_server(EV_A_ server);
            }
            return;
        }
    }
}

static remote_t *
new_remote(int fd)
{
    if (verbose) {
        remote_conn++;
    }

    remote_t *remote = ss_malloc(sizeof(remote_t));
    memset(remote, 0, sizeof(remote_t));

    remote->recv_ctx            = ss_malloc(sizeof(remote_ctx_t));
    remote->send_ctx            = ss_malloc(sizeof(remote_ctx_t));
    remote->buf                 = ss_malloc(sizeof(buffer_t));
    balloc(remote->buf, BUF_SIZE);
    memset(remote->recv_ctx, 0, sizeof(remote_ctx_t));
    memset(remote->send_ctx, 0, sizeof(remote_ctx_t));
    remote->fd                  = fd;
    remote->recv_ctx->remote    = remote;
    remote->recv_ctx->connected = 0;
    remote->send_ctx->remote    = remote;
    remote->send_ctx->connected = 0;
    remote->server              = NULL;

    ev_io_init(&remote->recv_ctx->io, remote_recv_cb, fd, EV_READ);
    ev_io_init(&remote->send_ctx->io, remote_send_cb, fd, EV_WRITE);

    return remote;
}

static void
free_remote(remote_t *remote)
{
    if (remote->server != NULL) {
        remote->server->remote = NULL;
    }
    if (remote->buf != NULL) {
        bfree(remote->buf);
        ss_free(remote->buf);
    }
    ss_free(remote->recv_ctx);
    ss_free(remote->send_ctx);
    ss_free(remote);
}

static void
close_and_free_remote(EV_P_ remote_t *remote)
{
    if (remote != NULL) {
        ev_io_stop(EV_A_ & remote->send_ctx->io);
        ev_io_stop(EV_A_ & remote->recv_ctx->io);
        close(remote->fd);
        free_remote(remote);
        if (verbose) {
            remote_conn--;
            LOGI("current remote connection: %d", remote_conn);
        }
    }
}

static server_t *
new_server(int fd, listen_ctx_t *listener)
{
    if (verbose) {
        server_conn++;
    }

    server_t *server;
    server = ss_malloc(sizeof(server_t));

    memset(server, 0, sizeof(server_t));

    server->recv_ctx            = ss_malloc(sizeof(server_ctx_t));
    server->send_ctx            = ss_malloc(sizeof(server_ctx_t));
    server->buf                 = ss_malloc(sizeof(buffer_t));
    server->header_buf          = ss_malloc(sizeof(buffer_t));
    memset(server->recv_ctx, 0, sizeof(server_ctx_t));
    memset(server->send_ctx, 0, sizeof(server_ctx_t));
    balloc(server->buf, BUF_SIZE);
    balloc(server->header_buf, BUF_SIZE);
    server->fd                  = fd;
    server->recv_ctx->server    = server;
    server->recv_ctx->connected = 0;
    server->send_ctx->server    = server;
    server->send_ctx->connected = 0;
    server->stage               = STAGE_INIT;
    server->query               = NULL;
    server->listen_ctx          = listener;
    server->remote              = NULL;

    if (listener->method) {
        server->e_ctx = ss_malloc(sizeof(enc_ctx_t));
        server->d_ctx = ss_malloc(sizeof(enc_ctx_t));
        enc_ctx_init(&cipher_env, server->e_ctx, 1);
        enc_ctx_init(&cipher_env, server->d_ctx, 0);
    } else {
        server->e_ctx = NULL;
        server->d_ctx = NULL;
    }

    int request_timeout = min(MAX_REQUEST_TIMEOUT, listener->timeout)
                          + rand() % MAX_REQUEST_TIMEOUT;

    ev_io_init(&server->recv_ctx->io, server_recv_cb, fd, EV_READ);
    ev_io_init(&server->send_ctx->io, server_send_cb, fd, EV_WRITE);
    ev_timer_init(&server->recv_ctx->watcher, server_timeout_cb,
                  request_timeout, listener->timeout);

    server->chunk = ss_malloc(sizeof(chunk_t));
    memset(server->chunk, 0, sizeof(chunk_t));
    server->chunk->buf = ss_malloc(sizeof(buffer_t));
    memset(server->chunk->buf, 0, sizeof(buffer_t));

    cork_dllist_add(&connections, &server->entries);

    return server;
}

static void
free_server(server_t *server)
{
    cork_dllist_remove(&server->entries);

    if (server->chunk != NULL) {
        if (server->chunk->buf != NULL) {
            bfree(server->chunk->buf);
            ss_free(server->chunk->buf);
        }
        ss_free(server->chunk);
    }
    if (server->remote != NULL) {
        server->remote->server = NULL;
    }
    if (server->e_ctx != NULL) {
        enc_ctx_release(&cipher_env, server->e_ctx);
        ss_free(server->e_ctx);
    }
    if (server->d_ctx != NULL) {
        enc_ctx_release(&cipher_env, server->d_ctx);
        ss_free(server->d_ctx);
    }
    if (server->buf != NULL) {
        bfree(server->buf);
        ss_free(server->buf);
    }
    if (server->header_buf != NULL) {
        bfree(server->header_buf);
        ss_free(server->header_buf);
    }

    ss_free(server->recv_ctx);
    ss_free(server->send_ctx);
    ss_free(server);
}

static void
close_and_free_server(EV_P_ server_t *server)
{
    if (server != NULL) {
        if (server->query != NULL) {
            resolv_cancel(server->query);
            server->query = NULL;
        }
        ev_io_stop(EV_A_ & server->send_ctx->io);
        ev_io_stop(EV_A_ & server->recv_ctx->io);
        ev_timer_stop(EV_A_ & server->recv_ctx->watcher);
        close(server->fd);
        free_server(server);
        if (verbose) {
            server_conn--;
            LOGI("current server connection: %d", server_conn);
        }
    }
}

static void
signal_cb(EV_P_ ev_signal *w, int revents)
{
    if (revents & EV_SIGNAL) {
        switch (w->signum) {
        case SIGINT:
        case SIGTERM:
            ev_unloop(EV_A_ EVUNLOOP_ALL);
        }
    }
}

static void
accept_cb(EV_P_ ev_io *w, int revents)
{
    listen_ctx_t *listener = (listen_ctx_t *)w;
    int serverfd           = accept(listener->fd, NULL, NULL);
    if (serverfd == -1) {
        ERROR("accept");
        return;
    }

    char *peer_name = get_peer_name(serverfd);
    if (peer_name != NULL) {
        int in_white_list = 0;
        if (acl) {
            if ((get_acl_mode() == BLACK_LIST && acl_match_host(peer_name) == 1)
                || (get_acl_mode() == WHITE_LIST && acl_match_host(peer_name) >= 0)) {
                LOGE("Access denied from %s", peer_name);
                close(serverfd);
                return;
            } else if (acl_match_host(peer_name) == -1) {
                in_white_list = 1;
            }
        }
        if (!in_white_list && check_block_list(peer_name)) {
            LOGE("block all requests from %s", peer_name);
#ifdef __linux__
            set_linger(serverfd);
#endif
            close(serverfd);
            return;
        }
    }

    int opt = 1;
    setsockopt(serverfd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt));
#ifdef SO_NOSIGPIPE
    setsockopt(serverfd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif
    setnonblocking(serverfd);

    if (verbose) {
        LOGI("accept a connection");
    }

    server_t *server = new_server(serverfd, listener);
    ev_io_start(EV_A_ & server->recv_ctx->io);
    ev_timer_start(EV_A_ & server->recv_ctx->watcher);
}

int
main(int argc, char **argv)
{
    int i, c;
    int pid_flags   = 0;
    int mptcp       = 0;
    int firewall    = 0;
    int mtu         = 0;
    char *user      = NULL;
    char *password  = NULL;
    char *timeout   = NULL;
    char *method    = NULL;
    char *pid_path  = NULL;
    char *conf_path = NULL;
    char *iface     = NULL;

    int server_num = 0;
    const char *server_host[MAX_REMOTE_NUM];

    char *nameservers[MAX_DNS_NUM + 1];
    int nameserver_num = 0;

    int option_index                    = 0;
    static struct option long_options[] = {
        { "fast-open",       no_argument,       0, 0 },
        { "acl",             required_argument, 0, 0 },
        { "manager-address", required_argument, 0, 0 },
        { "mtu",             required_argument, 0, 0 },
        { "help",            no_argument,       0, 0 },
#ifdef __linux__
        { "mptcp",           no_argument,       0, 0 },
        { "firewall",        no_argument,       0, 0 },
#endif
        {                 0,                 0, 0, 0 }
    };

    opterr = 0;

    USE_TTY();

    while ((c = getopt_long(argc, argv, "f:s:p:l:k:t:m:b:c:i:d:a:n:huUvA6",
                            long_options, &option_index)) != -1) {
        switch (c) {
        case 0:
            if (option_index == 0) {
                fast_open = 1;
            } else if (option_index == 1) {
                LOGI("initializing acl...");
                acl = !init_acl(optarg);
            } else if (option_index == 2) {
                manager_address = optarg;
            } else if (option_index == 3) {
                mtu = atoi(optarg);
                LOGI("set MTU to %d", mtu);
            } else if (option_index == 4) {
                usage();
                exit(EXIT_SUCCESS);
            } else if (option_index == 5) {
                mptcp = 1;
                LOGI("enable multipath TCP");
            } else if (option_index == 6) {
                firewall = 1;
                LOGI("enable firewall rules");
            }
            break;
        case 's':
            if (server_num < MAX_REMOTE_NUM) {
                server_host[server_num++] = optarg;
            }
            break;
        case 'b':
            bind_address = optarg;
            break;
        case 'p':
            server_port = optarg;
            break;
        case 'k':
            password = optarg;
            break;
        case 'f':
            pid_flags = 1;
            pid_path  = optarg;
            break;
        case 't':
            timeout = optarg;
            break;
        case 'm':
            method = optarg;
            break;
        case 'c':
            conf_path = optarg;
            break;
        case 'i':
            iface = optarg;
            break;
        case 'd':
            if (nameserver_num < MAX_DNS_NUM) {
                nameservers[nameserver_num++] = optarg;
            }
            break;
        case 'a':
            user = optarg;
            break;
#ifdef HAVE_SETRLIMIT
        case 'n':
            nofile = atoi(optarg);
            break;
#endif
        case 'u':
            mode = TCP_AND_UDP;
            break;
        case 'U':
            mode = UDP_ONLY;
            break;
        case 'v':
            verbose = 1;
            break;
        case 'h':
            usage();
            exit(EXIT_SUCCESS);
        case 'A':
            LOGI("The 'A' argument is deprecate! Ignored.");
            break;
        case '6':
            ipv6first = 1;
            break;
        case '?':
            // The option character is not recognized.
            LOGE("Unrecognized option: %s", optarg);
            opterr = 1;
            break;
        }
    }

    if (opterr) {
        usage();
        exit(EXIT_FAILURE);
    }

    if (argc == 1) {
        if (conf_path == NULL) {
            conf_path = DEFAULT_CONF_PATH;
        }
    }

    if (conf_path != NULL) {
        jconf_t *conf = read_jconf(conf_path);
        if (server_num == 0) {
            server_num = conf->remote_num;
            for (i = 0; i < server_num; i++)
                server_host[i] = conf->remote_addr[i].host;
        }
        if (server_port == NULL) {
            server_port = conf->remote_port;
        }
        if (password == NULL) {
            password = conf->password;
        }
        if (method == NULL) {
            method = conf->method;
        }
        if (timeout == NULL) {
            timeout = conf->timeout;
        }
        if (user == NULL) {
            user = conf->user;
        }
        if (mode == TCP_ONLY) {
            mode = conf->mode;
        }
        if (mtu == 0) {
            mtu = conf->mtu;
        }
        if (mptcp == 0) {
            mptcp = conf->mptcp;
        }
#ifdef TCP_FASTOPEN
        if (fast_open == 0) {
            fast_open = conf->fast_open;
        }
#endif
#ifdef HAVE_SETRLIMIT
        if (nofile == 0) {
            nofile = conf->nofile;
        }
#endif
        if (conf->nameserver != NULL) {
            nameservers[nameserver_num++] = conf->nameserver;
        }
        if (ipv6first == 0) {
            ipv6first = conf->ipv6_first;
        }
    }

    if (server_num == 0) {
        server_host[server_num++] = NULL;
    }

    if (server_num == 0 || server_port == NULL || password == NULL) {
        usage();
        exit(EXIT_FAILURE);
    }

    if (method == NULL) {
        method = "rc4-md5";
    }

    if (timeout == NULL) {
        timeout = "60";
    }

#ifdef HAVE_SETRLIMIT
    /*
     * no need to check the return value here since we will show
     * the user an error message if setrlimit(2) fails
     */
    if (nofile > 1024) {
        if (verbose) {
            LOGI("setting NOFILE to %d", nofile);
        }
        set_nofile(nofile);
    }
#endif

    if (pid_flags) {
        USE_SYSLOG(argv[0]);
        daemonize(pid_path);
    }

    if (ipv6first) {
        LOGI("resolving hostname to IPv6 address first");
    }

    if (fast_open == 1) {
#ifdef TCP_FASTOPEN
        LOGI("using tcp fast open");
#else
        LOGE("tcp fast open is not supported by this environment");
        fast_open = 0;
#endif
    }

    if (mode != TCP_ONLY) {
        LOGI("UDP relay enabled");
    }

    if (mode == UDP_ONLY) {
        LOGI("TCP relay disabled");
    }

#ifdef __MINGW32__
    winsock_init();
#else
    // ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    signal(SIGABRT, SIG_IGN);
#endif

    struct ev_signal sigint_watcher;
    struct ev_signal sigterm_watcher;
    ev_signal_init(&sigint_watcher, signal_cb, SIGINT);
    ev_signal_init(&sigterm_watcher, signal_cb, SIGTERM);
    ev_signal_start(EV_DEFAULT, &sigint_watcher);
    ev_signal_start(EV_DEFAULT, &sigterm_watcher);

    // setup keys
    LOGI("initializing ciphers... %s", method);
    int m = enc_init(&cipher_env, password, method);

    // initialize ev loop
    struct ev_loop *loop = EV_DEFAULT;

    // setup udns
    if (nameserver_num == 0) {
#ifdef __MINGW32__
        nameservers[nameserver_num++] = "8.8.8.8";
        resolv_init(loop, nameservers, nameserver_num, ipv6first);
#else
        resolv_init(loop, NULL, 0, ipv6first);
#endif
    } else {
        resolv_init(loop, nameservers, nameserver_num, ipv6first);
    }

    for (int i = 0; i < nameserver_num; i++)
        LOGI("using nameserver: %s", nameservers[i]);

    // initialize listen context
    listen_ctx_t listen_ctx_list[server_num];

    // bind to each interface
    while (server_num > 0) {
        int index        = --server_num;
        const char *host = server_host[index];

        if (mode != UDP_ONLY) {
            // Bind to port
            int listenfd;
            listenfd = create_and_bind(host, server_port, mptcp);
            if (listenfd == -1) {
                FATAL("bind() error");
            }
            if (listen(listenfd, SSMAXCONN) == -1) {
                FATAL("listen() error");
            }
            setfastopen(listenfd);
            setnonblocking(listenfd);
            listen_ctx_t *listen_ctx = &listen_ctx_list[index];

            // Setup proxy context
            listen_ctx->timeout = atoi(timeout);
            listen_ctx->fd      = listenfd;
            listen_ctx->method  = m;
            listen_ctx->iface   = iface;
            listen_ctx->loop    = loop;

            ev_io_init(&listen_ctx->io, accept_cb, listenfd, EV_READ);
            ev_io_start(loop, &listen_ctx->io);
        }

        // Setup UDP
        if (mode != TCP_ONLY) {
            init_udprelay(server_host[index], server_port, mtu,
                          atoi(timeout), iface, NULL, NULL);
        }

        if (host && strcmp(host, ":") > 0)
            LOGI("listening at [%s]:%s", host, server_port);
        else
            LOGI("listening at %s:%s", host ? host : "*", server_port);
    }

    if (manager_address != NULL) {
        ev_timer_init(&stat_update_watcher, stat_update_cb, UPDATE_INTERVAL, UPDATE_INTERVAL);
        ev_timer_start(EV_DEFAULT, &stat_update_watcher);
    }

    ev_timer_init(&block_list_watcher, block_list_clear_cb, UPDATE_INTERVAL, UPDATE_INTERVAL);
    ev_timer_start(EV_DEFAULT, &block_list_watcher);

    // setuid
    if (user != NULL && ! run_as(user)) {
        FATAL("failed to switch user");
    }

#ifndef __MINGW32__
    if (geteuid() == 0){
        LOGI("running from root user");
    } else if (firewall) {
        LOGE("firewall setup requires running from root user");
        exit(-1);
    }
#endif

    // init block list
    init_block_list(firewall);

    // Init connections
    cork_dllist_init(&connections);

    // start ev loop
    ev_run(loop, 0);

    if (verbose) {
        LOGI("closed gracefully");
    }

    // Free block list
    free_block_list();

    if (manager_address != NULL) {
        ev_timer_stop(EV_DEFAULT, &stat_update_watcher);
    }
    ev_timer_stop(EV_DEFAULT, &block_list_watcher);

    // Clean up
    for (int i = 0; i <= server_num; i++) {
        listen_ctx_t *listen_ctx = &listen_ctx_list[i];
        if (mode != UDP_ONLY) {
            ev_io_stop(loop, &listen_ctx->io);
            close(listen_ctx->fd);
        }
    }

    if (mode != UDP_ONLY) {
        free_connections(loop);
    }

    if (mode != TCP_ONLY) {
        free_udprelay();
    }

    resolv_shutdown(loop);

#ifdef __MINGW32__
    winsock_cleanup();
#endif

    ev_signal_stop(EV_DEFAULT, &sigint_watcher);
    ev_signal_stop(EV_DEFAULT, &sigterm_watcher);

    return 0;
}

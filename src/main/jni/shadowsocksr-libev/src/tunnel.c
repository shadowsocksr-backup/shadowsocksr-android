/*
 * tunnel.c - Setup a local port forwarding through remote shadowsocks server
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

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <getopt.h>

#ifndef __MINGW32__
#include <errno.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if defined(HAVE_SYS_IOCTL_H) && defined(HAVE_NET_IF_H) && defined(__linux__)
#include <net/if.h>
#include <sys/ioctl.h>
#define SET_INTERFACE
#endif

#ifdef __MINGW32__
#include "win32.h"
#endif

#include <libcork/core.h>
#include <udns.h>

#include "netutils.h"
#include "utils.h"
#include "tunnel.h"

#ifndef EAGAIN
#define EAGAIN EWOULDBLOCK
#endif

#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

#ifndef BUF_SIZE
#define BUF_SIZE 2048
#endif

#include "includeobfs.h" // I don't want to modify makefile

static void accept_cb(EV_P_ ev_io *w, int revents);
static void server_recv_cb(EV_P_ ev_io *w, int revents);
static void server_send_cb(EV_P_ ev_io *w, int revents);
static void remote_recv_cb(EV_P_ ev_io *w, int revents);
static void remote_send_cb(EV_P_ ev_io *w, int revents);

static remote_t *new_remote(int fd, int timeout);
static server_t *new_server(int fd, int method);

static void free_remote(remote_t *remote);
static void close_and_free_remote(EV_P_ remote_t *remote);
static void free_server(server_t *server);
static void close_and_free_server(EV_P_ server_t *server);

#ifdef ANDROID
int vpn = 0;
char *prefix;
#endif

int verbose        = 0;
int keep_resolving = 1;

static int ipv6first = 0;
static int mode = TCP_ONLY;
#ifdef HAVE_SETRLIMIT
static int nofile = 0;
#endif

#ifndef __MINGW32__
static int
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
create_and_bind(const char *addr, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int s, listen_sock;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family   = AF_UNSPEC;   /* Return IPv4 and IPv6 choices */
    hints.ai_socktype = SOCK_STREAM; /* We want a TCP socket */

    s = getaddrinfo(addr, port, &hints, &result);
    if (s != 0) {
        LOGI("getaddrinfo: %s", gai_strerror(s));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        listen_sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listen_sock == -1) {
            continue;
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

static void
server_recv_cb(EV_P_ ev_io *w, int revents)
{
    server_ctx_t *server_recv_ctx = (server_ctx_t *)w;
    server_t *server              = server_recv_ctx->server;
    remote_t *remote              = server->remote;

    if (remote == NULL) {
        close_and_free_server(EV_A_ server);
        return;
    }

    ssize_t r = recv(server->fd, remote->buf->array, BUF_SIZE, 0);

    if (r == 0) {
        // connection closed
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

    remote->buf->len = r;

    // SSR beg
    if (server->protocol_plugin) {
        obfs_class *protocol_plugin = server->protocol_plugin;
        if (protocol_plugin->client_pre_encrypt) {
            remote->buf->len = protocol_plugin->client_pre_encrypt(server->protocol, &remote->buf->array, remote->buf->len, &remote->buf->capacity);
        }
    }
    int err = ss_encrypt(&cipher_env, remote->buf, server->e_ctx, BUF_SIZE);

    if (err) {
        LOGE("server invalid password or cipher");
        close_and_free_remote(EV_A_ remote);
        close_and_free_server(EV_A_ server);
        return;
    }

    if (server->obfs_plugin) {
        obfs_class *obfs_plugin = server->obfs_plugin;
        if (obfs_plugin->client_encode) {
            remote->buf->len = obfs_plugin->client_encode(server->obfs, &remote->buf->array, remote->buf->len, &remote->buf->capacity);
        }
    }
    // SSR end

    if (r > 0 && remote->buf->len == 0) { // SSR pause recv
         remote->buf->idx = 0;
         ev_io_stop(EV_A_ & server_recv_ctx->io);
         return;
    }

    int s = send(remote->fd, remote->buf->array, remote->buf->len, 0);

    if (s == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // no data, wait for send
            remote->buf->idx = 0;
            ev_io_stop(EV_A_ & server_recv_ctx->io);
            ev_io_start(EV_A_ & remote->send_ctx->io);
            return;
        } else {
            ERROR("send");
            close_and_free_remote(EV_A_ remote);
            close_and_free_server(EV_A_ server);
            return;
        }
    } else if (s < remote->buf->len) {
        remote->buf->len -= s;
        remote->buf->idx  = s;
        ev_io_stop(EV_A_ & server_recv_ctx->io);
        ev_io_start(EV_A_ & remote->send_ctx->io);
        return;
    }
}

static void
server_send_cb(EV_P_ ev_io *w, int revents)
{
    server_ctx_t *server_send_ctx = (server_ctx_t *)w;
    server_t *server              = server_send_ctx->server;
    remote_t *remote              = server->remote;
    if (server->buf->len == 0) {
        // close and free
        close_and_free_remote(EV_A_ remote);
        close_and_free_server(EV_A_ server);
        return;
    } else {
        // has data to send
        ssize_t s = send(server->fd, server->buf->array + server->buf->idx,
                         server->buf->len, 0);
        if (s == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ERROR("send");
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
            } else {
                close_and_free_remote(EV_A_ remote);
                close_and_free_server(EV_A_ server);
                return;
            }
        }
    }
}

static void
remote_timeout_cb(EV_P_ ev_timer *watcher, int revents)
{
    remote_ctx_t *remote_ctx
        = cork_container_of(watcher, remote_ctx_t, watcher);

    remote_t *remote = remote_ctx->remote;
    server_t *server = remote->server;

    if (verbose) {
        LOGI("TCP connection timeout");
    }

    ev_timer_stop(EV_A_ watcher);

    close_and_free_remote(EV_A_ remote);
    close_and_free_server(EV_A_ server);
}

static void
remote_recv_cb(EV_P_ ev_io *w, int revents)
{
    remote_ctx_t *remote_recv_ctx = (remote_ctx_t *)w;
    remote_t *remote              = remote_recv_ctx->remote;
    server_t *server              = remote->server;

    ssize_t r = recv(remote->fd, server->buf->array, BUF_SIZE, 0);

    if (r == 0) {
        // connection closed
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

    server->buf->len = r;

    // SSR beg
    if (server->obfs_plugin) {
        obfs_class *obfs_plugin = server->obfs_plugin;
        if (obfs_plugin->client_decode) {
            int needsendback;
            server->buf->len = obfs_plugin->client_decode(server->obfs, &server->buf->array, server->buf->len, &server->buf->capacity, &needsendback);
            if ((int)server->buf->len < 0) {
                LOGE("client_decode");
                close_and_free_remote(EV_A_ remote);
                close_and_free_server(EV_A_ server);
                return;
            }
            if (needsendback) {
                obfs_class *obfs_plugin = server->obfs_plugin;
                if (obfs_plugin->client_encode) {
                    remote->buf->len = obfs_plugin->client_encode(server->obfs, &remote->buf->array, 0, &remote->buf->capacity);
                    ssize_t s = send(remote->fd, remote->buf->array, remote->buf->len, 0);
                    if (s == -1) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            ERROR("remote_send_cb_send");
                            // close and free
                            close_and_free_remote(EV_A_ remote);
                            close_and_free_server(EV_A_ server);
                        }
                        return;
                    } else if (s < (ssize_t)(remote->buf->len)) {
                        // partly sent, move memory, wait for the next time to send
                        remote->buf->len -= s;
                        remote->buf->idx += s;
                        return;
                    } else {
                        // all sent out, wait for reading
                        remote->buf->len = 0;
                        remote->buf->idx = 0;
                        ev_io_stop(EV_A_ & remote->send_ctx->io);
                        ev_io_start(EV_A_ & server->recv_ctx->io);
                    }
                }
            }
        }
    }
    if ( server->buf->len == 0 )
        return;

    int err = ss_decrypt(&cipher_env, server->buf, server->d_ctx, BUF_SIZE);

    if (err) {
        LOGE("invalid password or cipher");
        close_and_free_remote(EV_A_ remote);
        close_and_free_server(EV_A_ server);
        return;
    }

    if (server->protocol_plugin) {
        obfs_class *protocol_plugin = server->protocol_plugin;
        if (protocol_plugin->client_post_decrypt) {
            server->buf->len = protocol_plugin->client_post_decrypt(server->protocol, &server->buf->array, server->buf->len, &server->buf->capacity);
            if ((int)server->buf->len < 0) {
                LOGE("client_post_decrypt");
                close_and_free_remote(EV_A_ remote);
                close_and_free_server(EV_A_ server);
                return;
            }
            if ( server->buf->len == 0 )
                return;
        }
    }
    // SSR end

    int s = send(server->fd, server->buf->array, server->buf->len, 0);

    if (s == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // no data, wait for send
            server->buf->idx = 0;
            ev_io_stop(EV_A_ & remote_recv_ctx->io);
            ev_io_start(EV_A_ & server->send_ctx->io);
        } else {
            ERROR("send");
            close_and_free_remote(EV_A_ remote);
            close_and_free_server(EV_A_ server);
        }
    } else if (s < server->buf->len) {
        server->buf->len -= s;
        server->buf->idx  = s;
        ev_io_stop(EV_A_ & remote_recv_ctx->io);
        ev_io_start(EV_A_ & server->send_ctx->io);
    }
}

static void
remote_send_cb(EV_P_ ev_io *w, int revents)
{
    remote_ctx_t *remote_send_ctx = (remote_ctx_t *)w;
    remote_t *remote              = remote_send_ctx->remote;
    server_t *server              = remote->server;

    if (!remote_send_ctx->connected) {
        struct sockaddr_storage addr;
        socklen_t len = sizeof(struct sockaddr_storage);

        int r = getpeername(remote->fd, (struct sockaddr *)&addr, &len);
        if (r == 0) {
            remote_send_ctx->connected = 1;
            ev_io_stop(EV_A_ & remote_send_ctx->io);
            ev_timer_stop(EV_A_ & remote_send_ctx->watcher);

            buffer_t ss_addr_to_send;
            buffer_t *abuf = &ss_addr_to_send;
            balloc(abuf, BUF_SIZE);

            ss_addr_t *sa = &server->destaddr;
            struct cork_ip ip;
            if (cork_ip_init(&ip, sa->host) != -1) {
                if (ip.version == 4) {
                    // send as IPv4
                    struct in_addr host;
                    memset(&host, 0, sizeof(struct in_addr));
                    int host_len = sizeof(struct in_addr);

                    if (dns_pton(AF_INET, sa->host, &host) == -1) {
                        FATAL("IP parser error");
                    }
                    abuf->array[abuf->len++] = 1;
                    memcpy(abuf->array + abuf->len, &host, host_len);
                    abuf->len += host_len;
                } else if (ip.version == 6) {
                    // send as IPv6
                    struct in6_addr host;
                    memset(&host, 0, sizeof(struct in6_addr));
                    int host_len = sizeof(struct in6_addr);

                    if (dns_pton(AF_INET6, sa->host, &host) == -1) {
                        FATAL("IP parser error");
                    }
                    abuf->array[abuf->len++] = 4;
                    memcpy(abuf->array + abuf->len, &host, host_len);
                    abuf->len += host_len;
                } else {
                    FATAL("IP parser error");
                }
            } else {
                // send as domain
                int host_len = strlen(sa->host);

                abuf->array[abuf->len++] = 3;
                abuf->array[abuf->len++] = host_len;
                memcpy(abuf->array + abuf->len, sa->host, host_len);
                abuf->len += host_len;
            }

            uint16_t port = htons(atoi(sa->port));
            memcpy(abuf->array + abuf->len, &port, 2);
            abuf->len += 2;

            if (remote->buf->len > 0) {
                brealloc(remote->buf, remote->buf->len + abuf->len, BUF_SIZE);
                memmove(remote->buf->array + abuf->len, remote->buf->array, remote->buf->len);
                memcpy(remote->buf->array, abuf->array, abuf->len);
                remote->buf->len += abuf->len;
            } else {
                brealloc(remote->buf, abuf->len, BUF_SIZE);
                memcpy(remote->buf->array, abuf->array, abuf->len);
                remote->buf->len = abuf->len;
            }
            bfree(abuf);

            // SSR beg
            server_info _server_info;
            if (server->obfs_plugin) {
                server->obfs_plugin->get_server_info(server->obfs, &_server_info);
                _server_info.head_len = get_head_size(remote->buf->array, remote->buf->len, 30);
                server->obfs_plugin->set_server_info(server->obfs, &_server_info);
            }
            if (server->protocol_plugin) {
                obfs_class *protocol_plugin = server->protocol_plugin;
                if (protocol_plugin->client_pre_encrypt) {
                    remote->buf->len = protocol_plugin->client_pre_encrypt(server->protocol, &remote->buf->array, remote->buf->len, &remote->buf->capacity);
                }
            }

            int err = ss_encrypt(&cipher_env, remote->buf, server->e_ctx, BUF_SIZE);
            if (err) {
                LOGE("invalid password or cipher");
                close_and_free_remote(EV_A_ remote);
                close_and_free_server(EV_A_ server);
                return;
            }

            if (server->obfs_plugin) {
                obfs_class *obfs_plugin = server->obfs_plugin;
                if (obfs_plugin->client_encode) {
                    remote->buf->len = obfs_plugin->client_encode(server->obfs, &remote->buf->array, remote->buf->len, &remote->buf->capacity);
                }
            }

            int s = send(remote->fd, remote->buf->array, remote->buf->len, 0);

            if (s < remote->buf->len) {
                LOGE("failed to send addr");
                close_and_free_remote(EV_A_ remote);
                close_and_free_server(EV_A_ server);
                return;
            }
            remote->buf->len = 0;
            remote->buf->idx = 0;
            // SSR end

            ev_io_start(EV_A_ & remote->recv_ctx->io);
            ev_io_start(EV_A_ & server->recv_ctx->io);

            return;
        } else {
            ERROR("getpeername");
            // not connected
            close_and_free_remote(EV_A_ remote);
            close_and_free_server(EV_A_ server);
            return;
        }
    } else {
        if (remote->buf->len == 0) {
            // close and free
            close_and_free_remote(EV_A_ remote);
            close_and_free_server(EV_A_ server);
            return;
        } else {
            // has data to send
            ssize_t s = send(remote->fd, remote->buf->array + remote->buf->idx,
                             remote->buf->len, 0);
            if (s == -1) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    ERROR("send");
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
                ev_io_start(EV_A_ & server->recv_ctx->io);
            }
        }
    }
}

static remote_t *
new_remote(int fd, int timeout)
{
    remote_t *remote = ss_malloc(sizeof(remote_t));

    memset(remote, 0, sizeof(remote_t));

    remote->buf                 = ss_malloc(sizeof(buffer_t));
    remote->recv_ctx            = ss_malloc(sizeof(remote_ctx_t));
    remote->send_ctx            = ss_malloc(sizeof(remote_ctx_t));
    balloc(remote->buf, BUF_SIZE);
    memset(remote->recv_ctx, 0, sizeof(remote_ctx_t));
    memset(remote->send_ctx, 0, sizeof(remote_ctx_t));
    remote->fd                  = fd;
    remote->recv_ctx->remote    = remote;
    remote->recv_ctx->connected = 0;
    remote->send_ctx->remote    = remote;
    remote->send_ctx->connected = 0;

    ev_io_init(&remote->recv_ctx->io, remote_recv_cb, fd, EV_READ);
    ev_io_init(&remote->send_ctx->io, remote_send_cb, fd, EV_WRITE);
    ev_timer_init(&remote->send_ctx->watcher, remote_timeout_cb,
                  min(MAX_CONNECT_TIMEOUT, timeout), 0);

    return remote;
}

static void
free_remote(remote_t *remote)
{
    if (remote != NULL) {
        if (remote->server != NULL) {
            remote->server->remote = NULL;
        }
        if (remote->buf) {
            bfree(remote->buf);
            ss_free(remote->buf);
        }
        ss_free(remote->recv_ctx);
        ss_free(remote->send_ctx);
        ss_free(remote);
    }
}

static void
close_and_free_remote(EV_P_ remote_t *remote)
{
    if (remote != NULL) {
        ev_timer_stop(EV_A_ & remote->send_ctx->watcher);
        ev_io_stop(EV_A_ & remote->send_ctx->io);
        ev_io_stop(EV_A_ & remote->recv_ctx->io);
        close(remote->fd);
        free_remote(remote);
    }
}

static server_t *
new_server(int fd, int method)
{
    server_t *server = ss_malloc(sizeof(server_t));
    memset(server, 0, sizeof(server_t));

    server->buf                 = ss_malloc(sizeof(buffer_t));
    server->recv_ctx            = ss_malloc(sizeof(server_ctx_t));
    server->send_ctx            = ss_malloc(sizeof(server_ctx_t));
    balloc(server->buf, BUF_SIZE);
    memset(server->recv_ctx, 0, sizeof(server_ctx_t));
    memset(server->send_ctx, 0, sizeof(server_ctx_t));
    server->fd                  = fd;
    server->recv_ctx->server    = server;
    server->recv_ctx->connected = 0;
    server->send_ctx->server    = server;
    server->send_ctx->connected = 0;

    if (method) {
        server->e_ctx = ss_malloc(sizeof(struct enc_ctx));
        server->d_ctx = ss_malloc(sizeof(struct enc_ctx));
        enc_ctx_init(&cipher_env, server->e_ctx, 1);
        enc_ctx_init(&cipher_env, server->d_ctx, 0);
    } else {
        server->e_ctx = NULL;
        server->d_ctx = NULL;
    }

    ev_io_init(&server->recv_ctx->io, server_recv_cb, fd, EV_READ);
    ev_io_init(&server->send_ctx->io, server_send_cb, fd, EV_WRITE);

    return server;
}

static void
free_server(server_t *server)
{
    if (server != NULL) {
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
        if (server->buf) {
            bfree(server->buf);
            ss_free(server->buf);
        }
        // SSR beg
        if (server->obfs_plugin) {
            server->obfs_plugin->dispose(server->obfs);
            server->obfs = NULL;
            free_obfs_class(server->obfs_plugin);
            server->obfs_plugin = NULL;
        }
        if (server->protocol_plugin) {
            server->protocol_plugin->dispose(server->protocol);
            server->protocol = NULL;
            free_obfs_class(server->protocol_plugin);
            server->protocol_plugin = NULL;
        }
        // SSR end
        ss_free(server->recv_ctx);
        ss_free(server->send_ctx);
        ss_free(server);
    }
}

static void
close_and_free_server(EV_P_ server_t *server)
{
    if (server != NULL) {
        ev_io_stop(EV_A_ & server->send_ctx->io);
        ev_io_stop(EV_A_ & server->recv_ctx->io);
        close(server->fd);
        free_server(server);
    }
}

static void
accept_cb(EV_P_ ev_io *w, int revents)
{
    struct listen_ctx *listener = (struct listen_ctx *)w;
    int serverfd                = accept(listener->fd, NULL, NULL);
    if (serverfd == -1) {
        ERROR("accept");
        return;
    }
    setnonblocking(serverfd);
    int opt = 1;
    setsockopt(serverfd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt));
#ifdef SO_NOSIGPIPE
    setsockopt(serverfd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

    int index                    = rand() % listener->remote_num;
    struct sockaddr *remote_addr = listener->remote_addr[index];

    int remotefd = socket(remote_addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (remotefd == -1) {
        ERROR("socket");
        return;
    }

#ifdef ANDROID
    if (vpn) {
        int not_protect = 0;
        if (remote_addr->sa_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)remote_addr;
            if (s->sin_addr.s_addr == inet_addr("127.0.0.1"))
                not_protect = 1;
        }
        if (!not_protect) {
            if (protect_socket(remotefd) == -1) {
                ERROR("protect_socket");
                close(remotefd);
                return;
            }
        }
    }
#endif

    setsockopt(remotefd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt));
#ifdef SO_NOSIGPIPE
    setsockopt(remotefd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

    if (listener->mptcp == 1) {
        int err = setsockopt(remotefd, SOL_TCP, MPTCP_ENABLED, &opt, sizeof(opt));
        if (err == -1) {
            ERROR("failed to enable multipath TCP");
        }
    }

    // Setup
    setnonblocking(remotefd);
#ifdef SET_INTERFACE
    if (listener->iface) {
        if (setinterface(remotefd, listener->iface) == -1)
            ERROR("setinterface");
    }
#endif

    server_t *server = new_server(serverfd, listener->method);
    remote_t *remote = new_remote(remotefd, listener->timeout);
    server->destaddr = listener->tunnel_addr;
    server->remote   = remote;
    remote->server   = server;

    int r = connect(remotefd, remote_addr, get_sockaddr_len(remote_addr));

    if (r == -1 && errno != CONNECT_IN_PROGRESS) {
        ERROR("connect");
        close_and_free_remote(EV_A_ remote);
        close_and_free_server(EV_A_ server);
        return;
    }
    
    // listen to remote connected event
    ev_io_start(EV_A_ & remote->send_ctx->io);
    ev_timer_start(EV_A_ & remote->send_ctx->watcher);
}

void
signal_cb(int dummy)
{
    keep_resolving = 0;
    exit(-1);
}

int
main(int argc, char **argv)
{
    srand(time(NULL));

    int i, c;
    int pid_flags    = 0;
    int mptcp        = 0;
    int mtu          = 0;
    char *user       = NULL;
    char *local_port = NULL;
    char *local_addr = NULL;
    char *password   = NULL;
    char *timeout    = NULL;
    char *protocol = NULL; // SSR
    char *protocol_param = NULL; // SSR
    char *method = NULL;
    char *obfs = NULL; // SSR
    char *obfs_param = NULL; // SSR
    char *pid_path   = NULL;
    char *conf_path  = NULL;
    char *iface      = NULL;

    int remote_num = 0;
    ss_addr_t remote_addr[MAX_REMOTE_NUM];
    char *remote_port = NULL;

    ss_addr_t tunnel_addr = { .host = NULL, .port = NULL };
    char *tunnel_addr_str = NULL;

    int option_index                    = 0;
    static struct option long_options[] = {
        { "mtu",   required_argument, 0, 0 },
        { "mptcp", no_argument,       0, 0 },
        { "help",  no_argument,       0, 0 },
        {       0,                 0, 0, 0 }
    };

    opterr = 0;

    USE_TTY();

#ifdef ANDROID
    while ((c = getopt_long(argc, argv, "f:s:p:l:k:t:m:i:c:b:L:a:n:P:huUvVA6"
                            "O:o:G:g:",
                            long_options, &option_index)) != -1) {
#else
    while ((c = getopt_long(argc, argv, "f:s:p:l:k:t:m:i:c:b:L:a:n:huUvA6"
                            "O:o:G:g:",
                            long_options, &option_index)) != -1) {
#endif
        switch (c) {
        case 0:
            if (option_index == 0) {
                mtu = atoi(optarg);
                LOGI("set MTU to %d", mtu);
            } else if (option_index == 1) {
                mptcp = 1;
                LOGI("enable multipath TCP");
            } else if (option_index == 2) {
                usage();
                exit(EXIT_SUCCESS);
            }
            break;
        case 's':
            if (remote_num < MAX_REMOTE_NUM) {
                remote_addr[remote_num].host   = optarg;
                remote_addr[remote_num++].port = NULL;
            }
            break;
        case 'p':
            remote_port = optarg;
            break;
        case 'l':
            local_port = optarg;
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
        // SSR beg
        case 'O':
            protocol = optarg;
            break;
        case 'm':
            method = optarg;
            break;
        case 'o':
            obfs = optarg;
            break;
        case 'G':
            protocol_param = optarg;
            break;
        case 'g':
            obfs_param = optarg;
            break;
        // SSR end
        case 'c':
            conf_path = optarg;
            break;
        case 'i':
            iface = optarg;
            break;
        case 'b':
            local_addr = optarg;
            break;
        case 'u':
            mode = TCP_AND_UDP;
            break;
        case 'U':
            mode = UDP_ONLY;
            break;
        case 'L':
            tunnel_addr_str = optarg;
            break;
        case 'a':
            user = optarg;
            break;
#ifdef HAVE_SETRLIMIT
        case 'n':
            nofile = atoi(optarg);
            break;
#endif
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
#ifdef ANDROID
        case 'V':
            vpn = 1;
            break;
        case 'P':
            prefix = optarg;
            break;
#endif
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
        if (remote_num == 0) {
            remote_num = conf->remote_num;
            for (i = 0; i < remote_num; i++)
                remote_addr[i] = conf->remote_addr[i];
        }
        if (remote_port == NULL) {
            remote_port = conf->remote_port;
        }
        if (local_addr == NULL) {
            local_addr = conf->local_addr;
        }
        if (local_port == NULL) {
            local_port = conf->local_port;
        }
        if (password == NULL) {
            password = conf->password;
        }
        // SSR beg
        if (protocol == NULL) {
            protocol = conf->protocol;
            LOGI("protocol %s", protocol);
        }
        if (protocol_param == NULL) {
            protocol_param = conf->protocol_param;
            LOGI("protocol_param %s", protocol_param);
        }
        if (method == NULL) {
            method = conf->method;
            LOGI("method %s", method);
        }
        if (obfs == NULL) {
            obfs = conf->obfs;
            LOGI("obfs %s", obfs);
        }
        if (obfs_param == NULL) {
            obfs_param = conf->obfs_param;
            LOGI("obfs_param %s", obfs_param);
        }
        // SSR end
        if (timeout == NULL) {
            timeout = conf->timeout;
        }
        if (user == NULL) {
            user = conf->user;
        }
        if (tunnel_addr_str == NULL) {
            tunnel_addr_str = conf->tunnel_address;
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
#ifdef HAVE_SETRLIMIT
        if (nofile == 0) {
            nofile = conf->nofile;
        }
#endif
    }
    if (protocol && strcmp(protocol, "verify_sha1") == 0) {
        LOGI("The verify_sha1 protocol is deprecate! Fallback to origin protocol.");
        protocol = NULL;
    }

    if (remote_num == 0 || remote_port == NULL || tunnel_addr_str == NULL ||
        local_port == NULL || password == NULL) {
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

    if (local_addr == NULL) {
        local_addr = "127.0.0.1";
    }

    if (pid_flags) {
        USE_SYSLOG(argv[0]);
        daemonize(pid_path);
    }

    if (ipv6first) {
        LOGI("resolving hostname to IPv6 address first");
    }

    // parse tunnel addr
    parse_addr(tunnel_addr_str, &tunnel_addr);

    if (tunnel_addr.port == NULL) {
        FATAL("tunnel port is not defined");
    }

#ifdef __MINGW32__
    winsock_init();
#else
    // ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);
    signal(SIGABRT, SIG_IGN);
    signal(SIGINT, signal_cb);
    signal(SIGTERM, signal_cb);
#endif

    // Setup keys
    LOGI("initializing ciphers... %s", method);
    int m = enc_init(&cipher_env, password, method);

    // Setup proxy context
    struct listen_ctx listen_ctx;
    memset(&listen_ctx, 0, sizeof(struct listen_ctx));
    listen_ctx.tunnel_addr = tunnel_addr;
    listen_ctx.remote_num  = remote_num;
    listen_ctx.remote_addr = ss_malloc(sizeof(struct sockaddr *) * remote_num);
    memset(listen_ctx.remote_addr, 0, sizeof(struct sockaddr *) * remote_num);
    for (i = 0; i < remote_num; i++) {
        char *host = remote_addr[i].host;
        char *port = remote_addr[i].port == NULL ? remote_port :
                     remote_addr[i].port;
        struct sockaddr_storage *storage = ss_malloc(sizeof(struct sockaddr_storage));
        memset(storage, 0, sizeof(struct sockaddr_storage));
        if (get_sockaddr(host, port, storage, 1, ipv6first) == -1) {
            FATAL("failed to resolve the provided hostname");
        }
        listen_ctx.remote_addr[i] = (struct sockaddr *)storage;
    }
    listen_ctx.timeout = atoi(timeout);
    listen_ctx.iface   = iface;
    // SSR beg
    listen_ctx.protocol_name = protocol;
    listen_ctx.protocol_param = protocol_param;
    listen_ctx.method = m;
    listen_ctx.obfs_name = obfs;
    listen_ctx.obfs_param = obfs_param;
    listen_ctx.list_protocol_global = malloc(sizeof(void *) * remote_num);
    listen_ctx.list_obfs_global = malloc(sizeof(void *) * remote_num);
    memset(listen_ctx.list_protocol_global, 0, sizeof(void *) * remote_num);
    memset(listen_ctx.list_obfs_global, 0, sizeof(void *) * remote_num);
    // SSR end
    listen_ctx.mptcp   = mptcp;

    struct ev_loop *loop = EV_DEFAULT;

    if (mode != UDP_ONLY) {
        // Setup socket
        int listenfd;
        listenfd = create_and_bind(local_addr, local_port);
        if (listenfd == -1) {
            FATAL("bind() error:");
        }
        if (listen(listenfd, SOMAXCONN) == -1) {
            FATAL("listen() error:");
        }
        setnonblocking(listenfd);

        listen_ctx.fd = listenfd;

        ev_io_init(&listen_ctx.io, accept_cb, listenfd, EV_READ);
        ev_io_start(loop, &listen_ctx.io);
    }

    // Setup UDP
    if (mode != TCP_ONLY) {
        LOGI("UDP relay enabled");
        init_udprelay(local_addr, local_port, listen_ctx.remote_addr[0],
                      get_sockaddr_len(listen_ctx.remote_addr[0]),
                      tunnel_addr, mtu, listen_ctx.timeout, iface, protocol, protocol_param);
    }

    if (mode == UDP_ONLY) {
        LOGI("TCP relay disabled");
    }

    LOGI("listening at %s:%s", local_addr, local_port);

    // setuid
    if (user != NULL && ! run_as(user)) {
        FATAL("failed to switch user");
    }

#ifndef __MINGW32__
    if (geteuid() == 0){
        LOGI("running from root user");
    }
#endif

    ev_run(loop, 0);

#ifdef __MINGW32__
    winsock_cleanup();
#endif

    return 0;
}

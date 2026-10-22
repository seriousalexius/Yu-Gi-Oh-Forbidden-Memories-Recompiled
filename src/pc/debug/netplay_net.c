/* The online duel's socket (netplay_net.h). */
#define _GNU_SOURCE
#include "netplay_net.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET Socket;
#define NO_SOCKET INVALID_SOCKET
#define close_socket closesocket
#define SEND_FLAGS 0
static int last_error(void) { return WSAGetLastError(); }
static int would_block(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Socket;
#define NO_SOCKET (-1)
#define close_socket close
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL /* a peer gone is an error, not SIGPIPE */
#else
#define SEND_FLAGS 0
#endif
static int last_error(void) { return errno; }
static int would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
#endif

static Socket peer = NO_SOCKET;

static int set_blocking(Socket socket, int blocking)
{
#ifdef _WIN32
    u_long enabled = blocking ? 0 : 1;
    return ioctlsocket(socket, FIONBIO, &enabled);
#else
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(socket, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
#endif
}

/* Wait until the socket can be read (or written), up to timeout_ms. */
static int ready(Socket socket, int for_write, int timeout_ms)
{
    fd_set set;
    struct timeval wait;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    wait.tv_sec = timeout_ms / 1000;
    wait.tv_usec = (timeout_ms % 1000) * 1000;
    return select((int)socket + 1, for_write ? NULL : &set, for_write ? &set : NULL, NULL, &wait);
}

int NetplayNet_Connect(const char *host, unsigned port, int timeout_ms)
{
    struct addrinfo hints, *found = NULL, *at;
    char service[16];
    int error;
#ifdef _WIN32
    static int started;
    WSADATA data;
    if (!started && WSAStartup(MAKEWORD(2, 2), &data) == 0) started = 1;
#endif
    NetplayNet_Close();
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%u", port);
    error = getaddrinfo(host, service, &hints, &found);
    if (error || !found) {
        fprintf(stderr, "memories-pc: online: cannot find %s (%d)\n", host, error);
        return -1;
    }
    for (at = found; at && peer == NO_SOCKET; at = at->ai_next) {
        Socket socket_ = socket(at->ai_family, at->ai_socktype, at->ai_protocol);
        int one = 1;
        if (socket_ == NO_SOCKET) continue;
        set_blocking(socket_, 0);
        if (connect(socket_, at->ai_addr, (int)at->ai_addrlen) != 0 && !would_block()
#ifndef _WIN32
            && errno != EINPROGRESS
#endif
        ) {
            close_socket(socket_);
            continue;
        }
        if (ready(socket_, 1, timeout_ms) == 1) {
            int failed = 0;
            socklen_t length = sizeof(failed);
            getsockopt(socket_, SOL_SOCKET, SO_ERROR, (char *)&failed, &length);
            if (!failed) {
                setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
#ifdef SO_NOSIGPIPE
                setsockopt(socket_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
                peer = socket_;
                break;
            }
        }
        close_socket(socket_);
    }
    freeaddrinfo(found);
    if (peer == NO_SOCKET) {
        fprintf(stderr, "memories-pc: online: cannot connect to %s:%u (%d)\n", host, port, last_error());
        return -1;
    }
    return 0;
}

int NetplayNet_Send(const void *data, size_t size)
{
    const char *at = data;
    while (peer != NO_SOCKET && size) {
        long sent = (long)send(peer, at, (int)size, SEND_FLAGS);
        if (sent > 0) {
            at += sent;
            size -= (size_t)sent;
        } else if (sent < 0 && would_block()) {
            if (ready(peer, 1, 5000) != 1) break;
        } else {
            break;
        }
    }
    if (size) NetplayNet_Close();
    return size ? -1 : 0;
}

long NetplayNet_Receive(void *buffer, size_t size, int timeout_ms)
{
    long got;
    if (peer == NO_SOCKET) return -1;
    if (ready(peer, 0, timeout_ms) != 1) return 0;
    got = (long)recv(peer, buffer, (int)size, 0);
    if (got > 0) return got;
    if (got < 0 && would_block()) return 0;
    NetplayNet_Close(); /* 0: closed by the other end */
    return -1;
}

void NetplayNet_Close(void)
{
    if (peer != NO_SOCKET) close_socket(peer);
    peer = NO_SOCKET;
}

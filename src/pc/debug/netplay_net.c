/* The online duel's socket (netplay_net.h). */
#define _GNU_SOURCE
#include "netplay_net.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
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

/* The WebSocket mode (NetplayNet_ConnectWebSocket): what Send gathers until
 * Flush makes it one frame, and what came in but is not handed out yet. */
static int websocket;
static unsigned char out[8192], raw[16384];
static size_t out_length, raw_length;
static uint64_t data_left;          /* of the data frame being handed out */
static unsigned char data_mask[4]; /* servers send unmasked frames, but just in case */
static int data_masked;
static unsigned data_mask_at;
static uint32_t random_state;

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

/* All of the bytes on the socket, whatever the mode. */
static int send_all(const void *data, size_t size)
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

/* Not for secrets: the WebSocket key and the frames' masks (xorshift). */
static uint32_t next_random(void)
{
    if (!random_state) random_state = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)&random_state ^ 0x9E3779B9u;
    if (!random_state) random_state = 1;
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

/* One frame from the client: FIN, the opcode, the length (7, 16 or 64 bits)
 * and the payload under a fresh mask, as the client must. */
static int send_frame(int opcode, const unsigned char *data, size_t size)
{
    unsigned char chunk[14 + 1024], mask[4];
    size_t used = 0, done = 0;
    uint32_t key = next_random();
    int i;
    mask[0] = (unsigned char)key, mask[1] = (unsigned char)(key >> 8);
    mask[2] = (unsigned char)(key >> 16), mask[3] = (unsigned char)(key >> 24);
    chunk[used++] = (unsigned char)(0x80 | opcode);
    if (size < 126) {
        chunk[used++] = (unsigned char)(0x80 | size);
    } else if (size < 65536) {
        chunk[used++] = 0x80 | 126;
        chunk[used++] = (unsigned char)(size >> 8), chunk[used++] = (unsigned char)size;
    } else {
        chunk[used++] = 0x80 | 127;
        for (i = 7; i >= 0; i--) chunk[used++] = (unsigned char)((uint64_t)size >> (i * 8));
    }
    memcpy(chunk + used, mask, 4);
    used += 4;
    do {
        while (done < size && used < sizeof(chunk)) {
            chunk[used++] = data[done] ^ mask[done % 4];
            done++;
        }
        if (send_all(chunk, used)) return -1;
        used = 0;
    } while (done < size);
    return 0;
}

int NetplayNet_ConnectWebSocket(const char *host, unsigned port, const char *path, int timeout_ms)
{
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char key[18];
    char key64[25], request[1024], first[128], host_port[300];
    size_t i, end = 0, line;
    int status = 0, length;
    if (NetplayNet_Connect(host, port, timeout_ms)) return -1;
    for (i = 0; i < 16; i++) key[i] = (unsigned char)(next_random() >> 8);
    key[16] = key[17] = 0;
    for (i = 0; i < 6; i++) {
        uint32_t three = (uint32_t)key[i * 3] << 16 | (uint32_t)key[i * 3 + 1] << 8 | key[i * 3 + 2];
        key64[i * 4] = digits[three >> 18 & 63];
        key64[i * 4 + 1] = digits[three >> 12 & 63];
        key64[i * 4 + 2] = digits[three >> 6 & 63];
        key64[i * 4 + 3] = digits[three & 63];
    }
    key64[22] = key64[23] = '=', key64[24] = '\0'; /* 16 bytes: two of padding */
    if (port == 80) snprintf(host_port, sizeof(host_port), "%s", host);
    else snprintf(host_port, sizeof(host_port), "%s:%u", host, port);
    length = snprintf(request, sizeof(request),
                      "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
                      path, host_port, key64);
    if (length <= 0 || (size_t)length >= sizeof(request) || send_all(request, (size_t)length)) {
        NetplayNet_Close();
        return -1;
    }
    /* The answer's head, up to the empty line; what follows is frames. */
    while (!end) {
        long got;
        if (raw_length == sizeof(raw) || ready(peer, 0, timeout_ms) != 1) break;
        got = (long)recv(peer, (char *)raw + raw_length, (int)(sizeof(raw) - raw_length), 0);
        if (got <= 0 && !(got < 0 && would_block())) break;
        if (got > 0) raw_length += (size_t)got;
        for (i = 0; i + 4 <= raw_length; i++)
            if (!memcmp(raw + i, "\r\n\r\n", 4)) {
                end = i + 4;
                break;
            }
    }
    for (line = 0; line < raw_length && line + 1 < sizeof(first) && raw[line] != '\r'; line++) first[line] = (char)raw[line];
    first[line] = '\0';
    if (end) sscanf(first, "HTTP/%*s %d", &status);
    if (status != 101) {
        fprintf(stderr, "memories-pc: online: the server did not take the WebSocket: %s\n",
                end ? first : "no answer");
        NetplayNet_Close();
        return -1;
    }
    memmove(raw, raw + end, raw_length - end);
    raw_length -= end;
    websocket = 1;
    return 0;
}

int NetplayNet_Send(const void *data, size_t size)
{
    if (!websocket) return send_all(data, size);
    if (size > sizeof(out) - out_length && NetplayNet_Flush()) return -1;
    if (size > sizeof(out)) return send_frame(2, data, size);
    memcpy(out + out_length, data, size);
    out_length += size;
    return 0;
}

int NetplayNet_SendText(const char *text)
{
    if (!websocket) return -1;
    return send_frame(1, (const unsigned char *)text, strlen(text));
}

int NetplayNet_Flush(void)
{
    int failed;
    if (!websocket || !out_length) return 0;
    failed = send_frame(2, out, out_length);
    out_length = 0;
    return failed;
}

/* Hand out the data frames' payloads (text or binary) in `raw` as one
 * stream, up to size; answer pings. The count, or -1 at a close. */
static long take_frames(unsigned char *buffer, size_t size)
{
    size_t given = 0, at = 0;
    while (given < size) {
        size_t head = 2, length;
        int opcode;
        if (data_left) {
            size_t count = raw_length - at;
            if (count > size - given) count = size - given;
            if (count > data_left) count = (size_t)data_left;
            if (!count) break;
            memcpy(buffer + given, raw + at, count);
            if (data_masked) {
                size_t i;
                for (i = 0; i < count; i++) buffer[given + i] ^= data_mask[data_mask_at++ % 4];
            }
            given += count, at += count, data_left -= count;
            continue;
        }
        if (raw_length - at < 2) break;
        opcode = raw[at] & 0x0F;
        length = raw[at + 1] & 0x7F;
        if (length == 126) {
            if (raw_length - at < 4) break;
            length = (size_t)raw[at + 2] << 8 | raw[at + 3];
            head = 4;
        } else if (length == 127) {
            int i;
            uint64_t long_length = 0;
            if (raw_length - at < 10) break;
            for (i = 0; i < 8; i++) long_length = long_length << 8 | raw[at + 2 + i];
            length = (size_t)long_length;
            head = 10;
        }
        data_masked = (raw[at + 1] & 0x80) != 0;
        if (data_masked) head += 4;
        if (raw_length - at < head) break;
        if (data_masked) memcpy(data_mask, raw + at + head - 4, 4);
        data_mask_at = 0;
        if (opcode >= 8) {
            /* Control frames are short (125 bytes at most) and come whole. */
            unsigned char payload[128];
            size_t i;
            if (length > 125 || raw_length - at < head + length) {
                if (length > 125) return -1;
                break;
            }
            if (opcode == 8) {
                if (given) break; /* the data before it first */
                return -1;
            }
            for (i = 0; i < length; i++) payload[i] = raw[at + head + i] ^ (data_masked ? data_mask[i % 4] : 0);
            if (opcode == 9 && send_frame(10, payload, length)) return -1;
            at += head + length;
            continue;
        }
        data_left = length; /* text, binary or a continuation: all the same stream */
        at += head;
    }
    memmove(raw, raw + at, raw_length - at);
    raw_length -= at;
    return (long)given;
}

long NetplayNet_Receive(void *buffer, size_t size, int timeout_ms)
{
    long got;
    if (peer == NO_SOCKET) return -1;
    if (websocket) {
        for (;;) {
            long given = take_frames(buffer, size);
            if (given < 0 || peer == NO_SOCKET) {
                NetplayNet_Close(); /* a close frame, or a pong that could not go */
                return -1;
            }
            if (given) return given;
            if (raw_length == sizeof(raw)) {
                NetplayNet_Close(); /* a header that does not end: not a WebSocket */
                return -1;
            }
            if (ready(peer, 0, timeout_ms) != 1) return 0;
            got = (long)recv(peer, (char *)raw + raw_length, (int)(sizeof(raw) - raw_length), 0);
            if (got < 0 && would_block()) return 0;
            if (got <= 0) break;
            raw_length += (size_t)got;
            timeout_ms = 0; /* a frame cut in two: the rest is the caller's next turn */
        }
        NetplayNet_Close();
        return -1;
    }
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
    websocket = 0;
    out_length = raw_length = 0;
    data_left = 0;
}

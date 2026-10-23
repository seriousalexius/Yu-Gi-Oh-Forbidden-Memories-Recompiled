#ifndef MEMORIES_PC_DEBUG_NETPLAY_NET_H
#define MEMORIES_PC_DEBUG_NETPLAY_NET_H
/* The online duel's socket (netplay.h): one TCP connection to the relay,
 * through Winsock or POSIX sockets, plain (tools/relay.py) or as a minimal
 * WebSocket client (server/worker.js, ws:// without TLS). Kept apart from
 * netplay.c as control_net.c is from control.c. */
#include <stddef.h>

/* Connect to host:port within timeout_ms. 0 on success, -1 with the reason
 * on stderr. */
int NetplayNet_Connect(const char *host, unsigned port, int timeout_ms);
/* The same, then the HTTP Upgrade to a WebSocket on `path` (answer 101):
 * from then on Send gathers and Flush sends, and Receive gives the data
 * frames' bytes as one stream. */
int NetplayNet_ConnectWebSocket(const char *host, unsigned port, const char *path, int timeout_ms);
/* All of `size` bytes (in the WebSocket mode, kept for the next Flush). 0,
 * or -1 when the connection is gone. */
int NetplayNet_Send(const void *data, size_t size);
/* What Send kept, as one binary frame (each frame is a message the server
 * pays for); nothing to do on a plain connection. 0, or -1 when gone. */
int NetplayNet_Flush(void);
/* One text frame now (the lobby's JSON, pc/platform/lobby.h, which uses
 * this socket in a run with no online duel). 0, or -1 when gone. */
int NetplayNet_SendText(const char *text);
/* Bytes that came in, waiting up to timeout_ms: the count, 0 when none came
 * in time, -1 when the connection is gone. */
long NetplayNet_Receive(void *buffer, size_t size, int timeout_ms);
void NetplayNet_Close(void);
/* fork: one HTTP GET of `path` on a connection of its own (not the one
 * above), each wait up to timeout_ms: the body of a 200 answer in `body`,
 * ended by a zero; its length, or -1. Blocks: a thread's (news.c). */
long NetplayNet_HttpGet(const char *host, unsigned port, const char *path, char *body, size_t size, int timeout_ms);

#endif

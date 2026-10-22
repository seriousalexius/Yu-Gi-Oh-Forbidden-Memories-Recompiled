#ifndef MEMORIES_PC_DEBUG_NETPLAY_NET_H
#define MEMORIES_PC_DEBUG_NETPLAY_NET_H
/* The online duel's socket (netplay.h): one TCP connection to the relay,
 * through Winsock or POSIX sockets. Kept apart from netplay.c as
 * control_net.c is from control.c. */
#include <stddef.h>

/* Connect to host:port within timeout_ms. 0 on success, -1 with the reason
 * on stderr. */
int NetplayNet_Connect(const char *host, unsigned port, int timeout_ms);
/* All of `size` bytes. 0, or -1 when the connection is gone. */
int NetplayNet_Send(const void *data, size_t size);
/* Bytes that came in, waiting up to timeout_ms: the count, 0 when none came
 * in time, -1 when the connection is gone. */
long NetplayNet_Receive(void *buffer, size_t size, int timeout_ms);
void NetplayNet_Close(void);

#endif

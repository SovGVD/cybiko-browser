#pragma once
/* TCP sockets on POSIX systems and on Windows (Winsock), for the PC tools.
 * A socket is kept in an int on both (Winsock handles fit in one). */
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600          /* WSAPoll */
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#define MSG_NOSIGNAL 0
#define cy_sock_close(fd) closesocket((SOCKET)(fd))
#define cy_poll(p, n, ms) WSAPoll((p), (n), (ms))
/* Winsock needs starting once before the first socket. */
static inline void cy_sock_init(void)
{
    static int done;
    WSADATA d;
    if (!done && !WSAStartup(MAKEWORD(2, 2), &d)) done = 1;
}
#else
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#define cy_sock_close(fd) close(fd)
#define cy_poll(p, n, ms) poll((p), (n), (ms))
static inline void cy_sock_init(void) {}
#endif

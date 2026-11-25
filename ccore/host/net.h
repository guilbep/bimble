/* The sockets of the host's server (server.c), over POSIX or Winsock (ticket WIN1). Header only:
 * a few static functions, so no build script lists a new file. On POSIX each is the call
 * server.c made before, unchanged. On Windows: Winsock 2 (WSAStartup once, closesocket,
 * ioctlsocket FIONBIO, WSAPoll, WSAGetLastError), no SIGPIPE to ignore (a send to a closed peer
 * is an error there), SIGINT and SIGTERM through the C runtime's signal(). Link: -lws2_32
 * (PLATFORM_LIBS, portable.sh). Include it before any other system header (winsock2.h before
 * windows.h). */
#ifndef CCORE_HOST_NET_H
#define CCORE_HOST_NET_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <signal.h>
#include <stdio.h>

typedef SOCKET cc_sock;
#define CC_NO_SOCK INVALID_SOCKET
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
/* Accepted sockets inherit the listening socket's non-blocking mode on Windows. */
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

static int net_init(void) {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0 ? 0 : -1;
}
static void net_close(cc_sock s) { closesocket(s); }
static int net_nonblock(cc_sock s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0 ? 0 : -1;
}
static int net_poll(struct pollfd *p, int n, int ms) { return WSAPoll(p, (ULONG)n, ms); }
static long net_send(cc_sock s, const void *p, size_t n, int flags) {
    return send(s, (const char *)p, n > 0x7fffffff ? 0x7fffffff : (int)n, flags);
}
static long net_recv(cc_sock s, void *p, size_t n) {
    return recv(s, (char *)p, n > 0x7fffffff ? 0x7fffffff : (int)n, 0);
}
static int net_setopt(cc_sock s, int level, int name, int v) {
    return setsockopt(s, level, name, (const char *)&v, (int)sizeof v);
}
/* The listening socket's options. Not SO_REUSEADDR: on Windows it lets another process bind
 * the same port; SO_EXCLUSIVEADDRUSE is the safe one. */
static void net_listen_opts(cc_sock s) { net_setopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1); }
/* After a failed call: net_interrupted 1 if it should be retried at once (EINTR), net_would_block
 * 1 if it would block. */
static int net_interrupted(void) { return WSAGetLastError() == WSAEINTR; }
static int net_would_block(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
static const char *net_error(void) {
    static char m[32];
    snprintf(m, sizeof m, "Winsock error %d", WSAGetLastError());
    return m;
}
static void net_signals(void (*on_stop)(int)) {
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef int cc_sock;
#define CC_NO_SOCK (-1)
/* A write to a closed peer: EPIPE, never SIGPIPE (also ignored in net_signals). macOS has no
 * MSG_NOSIGNAL: SO_NOSIGPIPE on each accepted socket instead. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int net_init(void) { return 0; }
static void net_close(cc_sock s) { close(s); }
static int net_nonblock(cc_sock s) {
    int fl = fcntl(s, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(s, F_SETFL, fl | O_NONBLOCK);
}
static int net_poll(struct pollfd *p, int n, int ms) { return poll(p, (nfds_t)n, ms); }
static long net_send(cc_sock s, const void *p, size_t n, int flags) { return (long)send(s, p, n, flags); }
static long net_recv(cc_sock s, void *p, size_t n) { return (long)recv(s, p, n, 0); }
static int net_setopt(cc_sock s, int level, int name, int v) { return setsockopt(s, level, name, &v, sizeof v); }
static void net_listen_opts(cc_sock s) { net_setopt(s, SOL_SOCKET, SO_REUSEADDR, 1); }
static int net_interrupted(void) { return errno == EINTR; }
static int net_would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK; }
static const char *net_error(void) { return strerror(errno); }
static void net_signals(void (*on_stop)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}
#endif
#endif

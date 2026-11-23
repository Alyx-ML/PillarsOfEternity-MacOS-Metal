/*
 * WS2_32 (Windows Sockets 2) on BSD sockets.
 *
 * Mono (System.Net.Sockets and System.Net.Dns) uses these functions. A
 * guest SOCKET is a handle of the kernel object table (type OBJ_SOCKET) for
 * one host socket.
 *
 * Supported: IPv4 and IPv6 TCP and UDP sockets, blocking and non-blocking
 * mode (FIONBIO), select, the Windows Sockets 1.1 database functions
 * (gethostbyname and so on, IPv4 as on Windows), getaddrinfo and
 * freeaddrinfo, WSAAsyncGetHostByName (a resolver thread that posts the
 * reply message) and the Windows Sockets 2 scatter and gather calls
 * without overlapped I/O. Overlapped I/O,
 * protocol information structures and socket groups are not supported and
 * stop the process with a message.
 * Windows behavior that the runtime copies:
 *   - Error codes are WSA codes (WSAEWOULDBLOCK and so on). WSAGetLastError
 *     is the thread's last error value, as GetLastError.
 *   - A non-blocking connect that fails is reported in the exception set
 *     of select, not in the write set. getsockopt(SO_ERROR) then gives the
 *     error.
 *   - Sending on a closed connection gives WSAECONNRESET or WSAESHUTDOWN,
 *     not a signal.
 * Socket options, ioctl commands and address families that the runtime
 * does not translate stop the process with a message.
 */
#include "rt.h"
#include "rt_object.h"
#include "rt_file.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define INVALID_SOCKET 0xFFFFFFFFu
#define SOCKET_ERROR 0xFFFFFFFFu

#define WSAEINTR 10004u
#define WSAEBADF 10009u
#define WSAEACCES 10013u
#define WSAEFAULT 10014u
#define WSAEINVAL 10022u
#define WSAEMFILE 10024u
#define WSAEWOULDBLOCK 10035u
#define WSAEINPROGRESS 10036u
#define WSAEALREADY 10037u
#define WSAENOTSOCK 10038u
#define WSAEDESTADDRREQ 10039u
#define WSAEMSGSIZE 10040u
#define WSAEPROTOTYPE 10041u
#define WSAENOPROTOOPT 10042u
#define WSAEPROTONOSUPPORT 10043u
#define WSAESOCKTNOSUPPORT 10044u
#define WSAEOPNOTSUPP 10045u
#define WSAEAFNOSUPPORT 10047u
#define WSAEADDRINUSE 10048u
#define WSAEADDRNOTAVAIL 10049u
#define WSAENETDOWN 10050u
#define WSAENETUNREACH 10051u
#define WSAENETRESET 10052u
#define WSAECONNABORTED 10053u
#define WSAECONNRESET 10054u
#define WSAENOBUFS 10055u
#define WSAEISCONN 10056u
#define WSAENOTCONN 10057u
#define WSAESHUTDOWN 10058u
#define WSAETIMEDOUT 10060u
#define WSAECONNREFUSED 10061u
#define WSAEHOSTDOWN 10064u
#define WSAEHOSTUNREACH 10065u
#define WSAVERNOTSUPPORTED 10092u
#define WSANOTINITIALISED 10093u
#define WSATYPE_NOT_FOUND 10109u
#define WSAHOST_NOT_FOUND 11001u
#define WSATRY_AGAIN 11002u
#define WSANO_RECOVERY 11003u
#define WSANO_DATA 11004u
#define WSA_NOT_ENOUGH_MEMORY 8u

/* Windows constants. */
#define W_AF_INET 2u
#define W_AF_INET6 23u
#define W_SOL_SOCKET 0xFFFFu
#define W_SO_REUSEADDR 0x0004u
#define W_SO_KEEPALIVE 0x0008u
#define W_SO_BROADCAST 0x0020u
#define W_SO_LINGER 0x0080u
#define W_SO_DONTLINGER 0xFF7Fu
#define W_SO_SNDBUF 0x1001u
#define W_SO_RCVBUF 0x1002u
#define W_SO_SNDTIMEO 0x1005u
#define W_SO_RCVTIMEO 0x1006u
#define W_SO_ERROR 0x1007u
#define W_SO_TYPE 0x1008u
#define W_IPPROTO_TCP 6u
#define W_TCP_NODELAY 1u
#define W_FIONBIO 0x8004667Eu
#define W_FIONREAD 0x4004667Fu
#define W_MSG_OOB 1u
#define W_MSG_PEEK 2u
#define W_MSG_DONTROUTE 4u
#define W_FD_SETSIZE 64u

typedef struct wsock {
    int fd;
    int type;              /* SOCK_STREAM or SOCK_DGRAM */
    bool nonblocking;
    bool connect_pending;  /* non-blocking connect in progress */
    uint32_t error;        /* SO_ERROR value (WSA code) */
} wsock;

static pthread_mutex_t ws_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t startup_count;

static void lock(void)
{
    if (pthread_mutex_lock(&ws_lock) != 0) rt_fatal("Winsock lock failed");
}

static void unlock(void)
{
    if (pthread_mutex_unlock(&ws_lock) != 0) rt_fatal("Winsock unlock failed");
}

static bool started(void)
{
    bool r;
    lock();
    r = startup_count > 0;
    unlock();
    if (!r) rt_set_last_error(WSANOTINITIALISED);
    return r;
}

static uint32_t wsa_error(int e)
{
    switch (e)
    {
    case EINTR: return WSAEINTR;
    case EBADF: return WSAENOTSOCK;
    case EACCES: case EPERM: return WSAEACCES;
    case EFAULT: return WSAEFAULT;
    case EINVAL: return WSAEINVAL;
    case EMFILE: case ENFILE: return WSAEMFILE;
    case EAGAIN: return WSAEWOULDBLOCK;
    case EINPROGRESS: return WSAEWOULDBLOCK; /* Windows reports a pending connect as WSAEWOULDBLOCK */
    case EALREADY: return WSAEALREADY;
    case ENOTSOCK: return WSAENOTSOCK;
    case EDESTADDRREQ: return WSAEDESTADDRREQ;
    case EMSGSIZE: return WSAEMSGSIZE;
    case EPROTOTYPE: return WSAEPROTOTYPE;
    case ENOPROTOOPT: return WSAENOPROTOOPT;
    case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
    case ESOCKTNOSUPPORT: return WSAESOCKTNOSUPPORT;
    case EOPNOTSUPP: return WSAEOPNOTSUPP;
    case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
    case EADDRINUSE: return WSAEADDRINUSE;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETDOWN: return WSAENETDOWN;
    case ENETUNREACH: return WSAENETUNREACH;
    case ENETRESET: return WSAENETRESET;
    case ECONNABORTED: return WSAECONNABORTED;
    case ECONNRESET: case EPIPE: return WSAECONNRESET;
    case ENOBUFS: case ENOMEM: return WSAENOBUFS;
    case EISCONN: return WSAEISCONN;
    case ENOTCONN: return WSAENOTCONN;
    case ESHUTDOWN: return WSAESHUTDOWN;
    case ETIMEDOUT: return WSAETIMEDOUT;
    case ECONNREFUSED: return WSAECONNREFUSED;
    case EHOSTDOWN: return WSAEHOSTDOWN;
    case EHOSTUNREACH: return WSAEHOSTUNREACH;
    default:
        rt_log("Winsock: host error %d (%s) has no WSA code; reported as WSAEINVAL", e, strerror(e));
        return WSAEINVAL;
    }
}

static uint32_t fail(uint32_t code)
{
    rt_set_last_error(code);
    return SOCKET_ERROR;
}

static uint32_t fail_errno(void)
{
    return fail(wsa_error(errno));
}

/* The socket of handle s with one new reference, or NULL (error set). */
static rt_object *sock_get(uint32_t s)
{
    rt_object *o;
    if (!started()) return NULL;
    o = rt_handle_get(s, OBJ_SOCKET);
    if (o == NULL) rt_set_last_error(WSAENOTSOCK);
    return o;
}

static void sock_destroy(rt_object *o)
{
    wsock *w = o->data;
    if (w != NULL)
    {
        if (close(w->fd) != 0) rt_log("Winsock: close of socket fd %d failed: %s", w->fd, strerror(errno));
        free(w);
    }
}

/* ---------------------------------------------------------------- addresses */

/* Guest sockaddr to host sockaddr. Returns false (error set) if the
   address is not valid. */
static bool addr_in(uint32_t g, int32_t len, struct sockaddr_storage *h, socklen_t *hlen)
{
    uint16_t fam;
    memset(h, 0, sizeof *h);
    if (g == 0 || len < 2)
    {
        rt_set_last_error(WSAEFAULT);
        return false;
    }
    fam = rd16(g);
    if (fam == W_AF_INET)
    {
        struct sockaddr_in *a = (struct sockaddr_in *)h;
        if (len < 16)
        {
            rt_set_last_error(WSAEFAULT);
            return false;
        }
        a->sin_len = sizeof *a;
        a->sin_family = AF_INET;
        memcpy(&a->sin_port, G2H(g + 2), 2);
        memcpy(&a->sin_addr, G2H(g + 4), 4);
        *hlen = sizeof *a;
        return true;
    }
    if (fam == W_AF_INET6)
    {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)h;
        if (len < 24)
        {
            rt_set_last_error(WSAEFAULT);
            return false;
        }
        a->sin6_len = sizeof *a;
        a->sin6_family = AF_INET6;
        memcpy(&a->sin6_port, G2H(g + 2), 2);
        memcpy(&a->sin6_flowinfo, G2H(g + 4), 4);
        memcpy(&a->sin6_addr, G2H(g + 8), 16);
        if (len >= 28) memcpy(&a->sin6_scope_id, G2H(g + 24), 4);
        *hlen = sizeof *a;
        return true;
    }
    rt_set_last_error(WSAEAFNOSUPPORT);
    return false;
}

/* Windows form of a host sockaddr in buf (28 bytes). Returns the length
   (16 for IPv4, 28 for IPv6). */
static uint32_t addr_windows(const struct sockaddr *h, uint8_t buf[28])
{
    memset(buf, 0, 28);
    if (h->sa_family == AF_INET)
    {
        const struct sockaddr_in *a = (const struct sockaddr_in *)h;
        uint16_t fam = W_AF_INET;
        memcpy(buf, &fam, 2);
        memcpy(buf + 2, &a->sin_port, 2);
        memcpy(buf + 4, &a->sin_addr, 4);
        return 16;
    }
    if (h->sa_family == AF_INET6)
    {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)h;
        uint16_t fam = W_AF_INET6;
        memcpy(buf, &fam, 2);
        memcpy(buf + 2, &a->sin6_port, 2);
        memcpy(buf + 4, &a->sin6_flowinfo, 4);
        memcpy(buf + 8, &a->sin6_addr, 16);
        memcpy(buf + 24, &a->sin6_scope_id, 4);
        return 28;
    }
    rt_fatal("Winsock: host address family %d has no Windows form", h->sa_family);
}

/* Host sockaddr to the guest sockaddr at g, whose capacity is the int at
   plen (updated to the length). Returns false (WSAEFAULT) if the buffer
   is too small. */
static bool addr_out(const struct sockaddr *h, uint32_t g, uint32_t plen)
{
    uint8_t buf[28];
    uint32_t n = addr_windows(h, buf);
    if (g == 0) return true;
    if (plen == 0 || (int32_t)rd32(plen) < (int32_t)n)
    {
        rt_set_last_error(WSAEFAULT);
        return false;
    }
    memcpy(G2H(g), buf, n);
    wr32(plen, n);
    return true;
}

/* ---------------------------------------------------------------- startup */

/* int WSAStartup(WORD version, LPWSADATA) */
void rt_ws_WSAStartup(_cpu *cpu)
{
    uint32_t req = ARG(0) & 0xFFFFu, data = ARG(1), ver;
    uint8_t major = (uint8_t)req, minor = (uint8_t)(req >> 8);
    if (major < 1)
    {
        RET(WSAVERNOTSUPPORTED);
        return;
    }
    if (data == 0)
    {
        RET(WSAEFAULT);
        return;
    }
    /* The highest version is 2.2. */
    if (major > 2 || (major == 2 && minor > 2))
        ver = 0x0202u;
    else
        ver = req;
    memset(G2H(data), 0, 400);
    wr16(data, (uint16_t)ver);
    wr16(data + 2, 0x0202u);
    memcpy(G2H(data + 4), "WinSock 2.0", 12);
    memcpy(G2H(data + 261), "Running", 8);
    if (major < 2)
    {
        wr16(data + 390, 32767); /* iMaxSockets, used by Windows Sockets 1.1 only */
        wr16(data + 392, 65467); /* iMaxUdpDg */
    }
    lock();
    if (startup_count == UINT32_MAX) rt_fatal("WSAStartup count overflow");
    startup_count++;
    unlock();
    RET(0);
}

/* int WSACleanup(void) */
void rt_ws_WSACleanup(_cpu *cpu)
{
    lock();
    if (startup_count == 0)
    {
        unlock();
        RET(fail(WSANOTINITIALISED));
        return;
    }
    startup_count--;
    unlock();
    RET(0);
}

/* int WSAGetLastError(void) */
void rt_ws_WSAGetLastError(_cpu *cpu)
{
    RET(rt_get_last_error());
}

/* ---------------------------------------------------------------- byte order and text addresses */

void rt_ws_htonl(_cpu *cpu) { RET(htonl(ARG(0))); }
void rt_ws_ntohl(_cpu *cpu) { RET(ntohl(ARG(0))); }
void rt_ws_htons(_cpu *cpu) { RET(htons((uint16_t)ARG(0))); }
void rt_ws_ntohs(_cpu *cpu) { RET(ntohs((uint16_t)ARG(0))); }

/* unsigned long inet_addr(const char *): the forms a.b.c.d, a.b.c, a.b
   and a, with decimal, octal and hexadecimal parts. An empty string gives
   INADDR_ANY (Windows XP behavior); text that is not an address gives
   INADDR_NONE. */
void rt_ws_inet_addr(_cpu *cpu)
{
    char text[256];
    struct in_addr a;
    size_t i;
    if (ARG(0) == 0 || rt_gstr(text, sizeof text, ARG(0)) != 0)
    {
        RET(0xFFFFFFFFu);
        return;
    }
    for (i = 0; text[i] == ' '; i++)
        ;
    if (text[i] == 0)
    {
        RET(0);
        return;
    }
    if (inet_aton(text, &a) == 0)
    {
        RET(0xFFFFFFFFu);
        return;
    }
    RET(a.s_addr);
}

/* ---------------------------------------------------------------- sockets */

/* A new socket handle, or INVALID_SOCKET (error set). */
static uint32_t create_socket(uint32_t af, uint32_t type, uint32_t proto)
{
    uint32_t h;
    int fd, haf, one = 1;
    wsock *w;
    rt_object *o;
    if (!started()) return INVALID_SOCKET;
    if (af == W_AF_INET)
        haf = AF_INET;
    else if (af == W_AF_INET6)
        haf = AF_INET6;
    else
        return fail(WSAEAFNOSUPPORT);
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return fail(WSAESOCKTNOSUPPORT);
    if (proto != 0 && proto != IPPROTO_TCP && proto != IPPROTO_UDP) return fail(WSAEPROTONOSUPPORT);
    fd = socket(haf, (int)type, (int)proto);
    if (fd < 0) return fail_errno();
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0)
    {
        int e = errno;
        close(fd);
        return fail(wsa_error(e));
    }
    w = calloc(1, sizeof *w);
    if (w == NULL) rt_fatal("out of host memory");
    w->fd = fd;
    w->type = (int)type;
    o = rt_object_new(OBJ_SOCKET);
    o->data = w;
    o->destroy = sock_destroy;
    h = rt_handle_new(o);
    rt_object_unref(o); /* closes the socket if there is no handle */
    if (h == 0) return fail(WSAEMFILE);
    return h;
}

/* SOCKET socket(int af, int type, int protocol) */
void rt_ws_socket(_cpu *cpu)
{
    RET(create_socket(ARG(0), ARG(1), ARG(2)));
}

/* int closesocket(SOCKET) */
void rt_ws_closesocket(_cpu *cpu)
{
    rt_object *o = sock_get(ARG(0));
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    rt_object_unref(o);
    if (!rt_handle_close(ARG(0)))
    {
        RET(fail(WSAENOTSOCK));
        return;
    }
    RET(0);
}

/* int bind(SOCKET, const struct sockaddr *, int) */
void rt_ws_bind(_cpu *cpu)
{
    struct sockaddr_storage a;
    socklen_t n;
    rt_object *o = sock_get(ARG(0));
    uint32_t r = 0;
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    if (!addr_in(ARG(1), (int32_t)ARG(2), &a, &n))
        r = SOCKET_ERROR;
    else if (bind(((wsock *)o->data)->fd, (struct sockaddr *)&a, n) != 0)
        r = fail_errno();
    rt_object_unref(o);
    RET(r);
}

/* int listen(SOCKET, int backlog) */
void rt_ws_listen(_cpu *cpu)
{
    rt_object *o = sock_get(ARG(0));
    int backlog = (int)ARG(1);
    uint32_t r = 0;
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    if (backlog < 0) backlog = 0;
    if (backlog > SOMAXCONN) backlog = SOMAXCONN; /* Windows SOMAXCONN is 0x7FFFFFFF: the system maximum */
    if (listen(((wsock *)o->data)->fd, backlog) != 0) r = fail_errno();
    rt_object_unref(o);
    RET(r);
}

/* SOCKET accept(SOCKET, struct sockaddr *, int *) */
void rt_ws_accept(_cpu *cpu)
{
    struct sockaddr_storage a;
    socklen_t n = sizeof a;
    rt_object *o = sock_get(ARG(0)), *no;
    wsock *w, *nw;
    int fd, one = 1;
    uint32_t h;
    bool nonblocking;
    if (o == NULL)
    {
        RET(INVALID_SOCKET);
        return;
    }
    w = o->data;
    fd = accept(w->fd, (struct sockaddr *)&a, &n);
    nonblocking = w->nonblocking;
    if (fd < 0)
    {
        uint32_t e = wsa_error(errno);
        rt_object_unref(o);
        RET(fail(e));
        return;
    }
    rt_object_unref(o);
    /* The new socket has the properties of the listening socket
       (non-blocking mode included), as on Windows. */
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0 ||
        fcntl(fd, F_SETFL, nonblocking ? O_NONBLOCK : 0) != 0)
    {
        int e = errno;
        close(fd);
        RET(fail(wsa_error(e)));
        return;
    }
    if (ARG(1) != 0 && !addr_out((struct sockaddr *)&a, ARG(1), ARG(2)))
    {
        close(fd);
        RET(INVALID_SOCKET);
        return;
    }
    nw = calloc(1, sizeof *nw);
    if (nw == NULL) rt_fatal("out of host memory");
    nw->fd = fd;
    nw->type = SOCK_STREAM;
    nw->nonblocking = nonblocking;
    no = rt_object_new(OBJ_SOCKET);
    no->data = nw;
    no->destroy = sock_destroy;
    h = rt_handle_new(no);
    rt_object_unref(no);
    if (h == 0)
    {
        RET(fail(WSAEMFILE));
        return;
    }
    RET(h);
}

/* int connect(SOCKET, const struct sockaddr *, int) */
void rt_ws_connect(_cpu *cpu)
{
    struct sockaddr_storage a;
    socklen_t n;
    rt_object *o = sock_get(ARG(0));
    wsock *w;
    uint32_t r = 0;
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    w = o->data;
    if (!addr_in(ARG(1), (int32_t)ARG(2), &a, &n))
    {
        r = SOCKET_ERROR;
    }
    else if (connect(w->fd, (struct sockaddr *)&a, n) != 0)
    {
        int e = errno;
        lock();
        if (e == EINPROGRESS)
        {
            w->connect_pending = true;
            w->error = 0;
        }
        unlock();
        r = fail(wsa_error(e));
    }
    rt_object_unref(o);
    RET(r);
}

static uint32_t name_query(_cpu *cpu, bool peer)
{
    struct sockaddr_storage a;
    socklen_t n = sizeof a;
    rt_object *o = sock_get(ARG(0));
    int rc;
    uint32_t r = 0;
    if (o == NULL) return SOCKET_ERROR;
    rc = peer ? getpeername(((wsock *)o->data)->fd, (struct sockaddr *)&a, &n)
              : getsockname(((wsock *)o->data)->fd, (struct sockaddr *)&a, &n);
    if (rc != 0)
        r = fail_errno();
    else if (ARG(1) == 0 || !addr_out((struct sockaddr *)&a, ARG(1), ARG(2)))
        r = fail(WSAEFAULT);
    rt_object_unref(o);
    return r;
}

/* int getpeername(SOCKET, struct sockaddr *, int *) */
void rt_ws_getpeername(_cpu *cpu) { RET(name_query(cpu, true)); }
/* int getsockname(SOCKET, struct sockaddr *, int *) */
void rt_ws_getsockname(_cpu *cpu) { RET(name_query(cpu, false)); }

static int host_msg_flags(uint32_t f, bool *ok)
{
    int h = 0;
    *ok = (f & ~(W_MSG_OOB | W_MSG_PEEK | W_MSG_DONTROUTE)) == 0;
    if (f & W_MSG_OOB) h |= MSG_OOB;
    if (f & W_MSG_PEEK) h |= MSG_PEEK;
    if (f & W_MSG_DONTROUTE) h |= MSG_DONTROUTE;
    return h;
}

/* Check a guest buffer for a transfer of len bytes. */
static bool buffer_ok(uint32_t buf, int32_t len)
{
    if (len < 0) return false;
    if (len == 0) return true;
    return buf != 0 && rt_va_is_committed(buf, (uint32_t)len);
}

static uint32_t do_send(_cpu *cpu, bool to)
{
    uint32_t buf = ARG(1), flags = ARG(3);
    int32_t len = (int32_t)ARG(2);
    struct sockaddr_storage a;
    socklen_t n = 0;
    rt_object *o = sock_get(ARG(0));
    bool ok;
    int hf, e;
    ssize_t r;
    if (o == NULL) return SOCKET_ERROR;
    hf = host_msg_flags(flags, &ok);
    if (!ok || !buffer_ok(buf, len))
    {
        rt_object_unref(o);
        return fail(ok ? WSAEFAULT : WSAEOPNOTSUPP);
    }
    if (to && ARG(4) != 0 && !addr_in(ARG(4), (int32_t)ARG(5), &a, &n))
    {
        rt_object_unref(o);
        return SOCKET_ERROR;
    }
    if (to && ARG(4) != 0)
        r = sendto(((wsock *)o->data)->fd, G2H(buf), (size_t)len, hf, (struct sockaddr *)&a, n);
    else
        r = send(((wsock *)o->data)->fd, len ? G2H(buf) : "", (size_t)len, hf);
    e = r < 0 ? errno : 0;
    rt_object_unref(o);
    if (r < 0) return fail(wsa_error(e));
    return (uint32_t)r;
}

/* int send(SOCKET, const char *, int, int) */
void rt_ws_send(_cpu *cpu) { RET(do_send(cpu, false)); }
/* int sendto(SOCKET, const char *, int, int, const struct sockaddr *, int) */
void rt_ws_sendto(_cpu *cpu) { RET(do_send(cpu, true)); }

static uint32_t do_recv(_cpu *cpu, bool from)
{
    uint32_t buf = ARG(1), flags = ARG(3);
    int32_t len = (int32_t)ARG(2);
    struct sockaddr_storage a;
    socklen_t n = sizeof a;
    rt_object *o = sock_get(ARG(0));
    wsock *w;
    bool ok;
    int hf;
    ssize_t r;
    uint8_t dummy;
    bool dgram, truncated = false;
    int e = 0;
    if (o == NULL) return SOCKET_ERROR;
    w = o->data;
    hf = host_msg_flags(flags, &ok);
    if (!ok || !buffer_ok(buf, len))
    {
        rt_object_unref(o);
        return fail(ok ? WSAEFAULT : WSAEOPNOTSUPP);
    }
    dgram = w->type == SOCK_DGRAM;
    if (dgram)
    {
        /* recvmsg reports a datagram larger than the buffer with MSG_TRUNC
           in msg_flags (macOS does not accept MSG_TRUNC as an input flag). */
        struct iovec iov = { len ? G2H(buf) : &dummy, (size_t)len };
        struct msghdr m;
        memset(&m, 0, sizeof m);
        m.msg_name = &a;
        m.msg_namelen = n;
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        r = recvmsg(w->fd, &m, hf);
        n = m.msg_namelen;
        truncated = r >= 0 && (m.msg_flags & MSG_TRUNC) != 0;
    }
    else if (from)
        r = recvfrom(w->fd, len ? G2H(buf) : &dummy, (size_t)len, hf, (struct sockaddr *)&a, &n);
    else
        r = recv(w->fd, len ? G2H(buf) : &dummy, (size_t)len, hf);
    if (r < 0) e = errno;
    rt_object_unref(o);
    if (r < 0) return fail(wsa_error(e));
    /* A datagram larger than the buffer: Windows copies the first part and
       returns WSAEMSGSIZE. */
    if (truncated) return fail(WSAEMSGSIZE);
    if (from && ARG(4) != 0 && n > 0 && !addr_out((struct sockaddr *)&a, ARG(4), ARG(5))) return fail(WSAEFAULT);
    return (uint32_t)r;
}

/* int recv(SOCKET, char *, int, int) */
void rt_ws_recv(_cpu *cpu) { RET(do_recv(cpu, false)); }
/* int recvfrom(SOCKET, char *, int, int, struct sockaddr *, int *) */
void rt_ws_recvfrom(_cpu *cpu) { RET(do_recv(cpu, true)); }

/* int shutdown(SOCKET, int how): SD_RECEIVE 0, SD_SEND 1, SD_BOTH 2 */
void rt_ws_shutdown(_cpu *cpu)
{
    rt_object *o = sock_get(ARG(0));
    uint32_t r = 0;
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    if (ARG(1) > 2)
        r = fail(WSAEINVAL);
    else if (shutdown(((wsock *)o->data)->fd, (int)ARG(1)) != 0)
        r = fail_errno();
    rt_object_unref(o);
    RET(r);
}

/* int ioctlsocket(SOCKET, long cmd, u_long *arg) */
void rt_ws_ioctlsocket(_cpu *cpu)
{
    rt_object *o = sock_get(ARG(0));
    uint32_t cmd = ARG(1), arg = ARG(2), r = 0;
    wsock *w;
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    w = o->data;
    if (arg == 0)
    {
        r = fail(WSAEFAULT);
    }
    else if (cmd == W_FIONBIO)
    {
        bool nb = rd32(arg) != 0;
        int fl = fcntl(w->fd, F_GETFL);
        if (fl < 0 || fcntl(w->fd, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) != 0)
            r = fail_errno();
        else
            w->nonblocking = nb;
    }
    else if (cmd == W_FIONREAD)
    {
        int avail = 0;
        if (ioctl(w->fd, FIONREAD, &avail) != 0)
            r = fail_errno();
        else
            wr32(arg, (uint32_t)(avail < 0 ? 0 : avail));
    }
    else
    {
        rt_fatal("ioctlsocket command 0x%08x is not supported", cmd);
    }
    rt_object_unref(o);
    RET(r);
}

/* Translate a socket option: level and name. Returns false for options
   that the runtime does not support. */
static bool host_option(uint32_t level, uint32_t name, int *hl, int *hn)
{
    if (level == W_SOL_SOCKET)
    {
        *hl = SOL_SOCKET;
        switch (name)
        {
        case W_SO_REUSEADDR: *hn = SO_REUSEADDR; return true;
        case W_SO_KEEPALIVE: *hn = SO_KEEPALIVE; return true;
        case W_SO_BROADCAST: *hn = SO_BROADCAST; return true;
        case W_SO_LINGER: case W_SO_DONTLINGER: *hn = SO_LINGER; return true;
        case W_SO_SNDBUF: *hn = SO_SNDBUF; return true;
        case W_SO_RCVBUF: *hn = SO_RCVBUF; return true;
        case W_SO_SNDTIMEO: *hn = SO_SNDTIMEO; return true;
        case W_SO_RCVTIMEO: *hn = SO_RCVTIMEO; return true;
        case W_SO_ERROR: *hn = SO_ERROR; return true;
        case W_SO_TYPE: *hn = SO_TYPE; return true;
        default: return false;
        }
    }
    if (level == W_IPPROTO_TCP && name == W_TCP_NODELAY)
    {
        *hl = IPPROTO_TCP;
        *hn = TCP_NODELAY;
        return true;
    }
    return false;
}

/* int setsockopt(SOCKET, int level, int name, const char *value, int len) */
void rt_ws_setsockopt(_cpu *cpu)
{
    uint32_t level = ARG(1), name = ARG(2), val = ARG(3), r = 0;
    int32_t len = (int32_t)ARG(4);
    rt_object *o;
    wsock *w;
    int hl, hn, rc, iv;
    if (!host_option(level, name, &hl, &hn) || name == W_SO_ERROR || name == W_SO_TYPE)
        rt_fatal("setsockopt level 0x%x option 0x%x is not supported", level, name);
    o = sock_get(ARG(0));
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    w = o->data;
    if (val == 0 || len < 0 || !rt_va_is_committed(val, (uint32_t)len))
    {
        rt_object_unref(o);
        RET(fail(WSAEFAULT));
        return;
    }
    if (name == W_SO_LINGER && level == W_SOL_SOCKET)
    {
        /* struct linger { u_short l_onoff; u_short l_linger; } */
        struct linger lg;
        if (len < 4)
        {
            rt_object_unref(o);
            RET(fail(WSAEFAULT));
            return;
        }
        lg.l_onoff = rd16(val) != 0;
        lg.l_linger = rd16(val + 2);
        rc = setsockopt(w->fd, hl, hn, &lg, sizeof lg);
    }
    else if (name == W_SO_DONTLINGER && level == W_SOL_SOCKET)
    {
        struct linger lg = { 0, 0 };
        lg.l_onoff = len >= 1 && rd8(val) == 0; /* SO_DONTLINGER true: linger off */
        rc = setsockopt(w->fd, hl, hn, &lg, sizeof lg);
    }
    else if ((name == W_SO_SNDTIMEO || name == W_SO_RCVTIMEO) && level == W_SOL_SOCKET)
    {
        /* Windows: a DWORD in milliseconds. */
        struct timeval tv;
        uint32_t ms;
        if (len < 4)
        {
            rt_object_unref(o);
            RET(fail(WSAEFAULT));
            return;
        }
        ms = rd32(val);
        tv.tv_sec = (time_t)(ms / 1000u);
        tv.tv_usec = (suseconds_t)((ms % 1000u) * 1000u);
        rc = setsockopt(w->fd, hl, hn, &tv, sizeof tv);
    }
    else
    {
        /* Boolean and integer options: Windows accepts a one-byte BOOL. */
        if (len >= 4)
            iv = (int)rd32(val);
        else if (len >= 1)
            iv = rd8(val);
        else
        {
            rt_object_unref(o);
            RET(fail(WSAEFAULT));
            return;
        }
        rc = setsockopt(w->fd, hl, hn, &iv, sizeof iv);
    }
    if (rc != 0) r = fail_errno();
    rt_object_unref(o);
    RET(r);
}

/* int getsockopt(SOCKET, int level, int name, char *value, int *len) */
void rt_ws_getsockopt(_cpu *cpu)
{
    uint32_t level = ARG(1), name = ARG(2), val = ARG(3), plen = ARG(4), r = 0, out;
    rt_object *o;
    wsock *w;
    int hl, hn, iv = 0;
    socklen_t n = sizeof iv;
    if (!host_option(level, name, &hl, &hn) || name == W_SO_LINGER || name == W_SO_DONTLINGER ||
        name == W_SO_SNDTIMEO || name == W_SO_RCVTIMEO)
        rt_fatal("getsockopt level 0x%x option 0x%x is not supported", level, name);
    o = sock_get(ARG(0));
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    w = o->data;
    if (val == 0 || plen == 0 || (int32_t)rd32(plen) < 4)
    {
        rt_object_unref(o);
        RET(fail(WSAEFAULT));
        return;
    }
    if (name == W_SO_ERROR && level == W_SOL_SOCKET)
    {
        lock();
        out = w->error;
        w->error = 0;
        unlock();
        if (out == 0)
        {
            if (getsockopt(w->fd, SOL_SOCKET, SO_ERROR, &iv, &n) != 0)
            {
                r = fail_errno();
                rt_object_unref(o);
                RET(r);
                return;
            }
            out = iv != 0 ? wsa_error(iv) : 0;
        }
    }
    else
    {
        if (getsockopt(w->fd, hl, hn, &iv, &n) != 0)
        {
            r = fail_errno();
            rt_object_unref(o);
            RET(r);
            return;
        }
        out = (uint32_t)iv;
        if (hn == SO_REUSEADDR || hn == SO_KEEPALIVE || hn == SO_BROADCAST || hn == TCP_NODELAY) out = iv != 0;
    }
    wr32(val, out);
    wr32(plen, 4);
    rt_object_unref(o);
    RET(0);
}

/* ---------------------------------------------------------------- select */

/* int __WSAFDIsSet(SOCKET, fd_set *) */
void rt_ws___WSAFDIsSet(_cpu *cpu)
{
    uint32_t s = ARG(0), set = ARG(1), n, i;
    if (set == 0)
    {
        RET(0);
        return;
    }
    n = rd32(set);
    if (n > W_FD_SETSIZE) n = W_FD_SETSIZE;
    for (i = 0; i < n; i++)
    {
        if (rd32(set + 4 + 4 * i) == s)
        {
            RET(1);
            return;
        }
    }
    RET(0);
}

#define SEL_READ 1
#define SEL_WRITE 2
#define SEL_EXCEPT 4

/* int select(int nfds, fd_set *read, fd_set *write, fd_set *except,
   const struct timeval *timeout) */
void rt_ws_select(_cpu *cpu)
{
    uint32_t sets[3] = { ARG(1), ARG(2), ARG(3) }, tv = ARG(4);
    struct pollfd pfd[3 * W_FD_SETSIZE];
    rt_object *objs[3 * W_FD_SETSIZE];
    uint32_t handles[3 * W_FD_SETSIZE];
    int kind[3 * W_FD_SETSIZE];
    uint8_t ready[3 * W_FD_SETSIZE];
    uint32_t count = 0, k, i, total = 0;
    int timeout_ms, rc;
    if (!started())
    {
        RET(SOCKET_ERROR);
        return;
    }
    for (k = 0; k < 3; k++)
    {
        uint32_t n;
        if (sets[k] == 0) continue;
        n = rd32(sets[k]);
        if (n > W_FD_SETSIZE)
        {
            for (i = 0; i < count; i++) rt_object_unref(objs[i]);
            RET(fail(WSAEINVAL));
            return;
        }
        for (i = 0; i < n; i++)
        {
            uint32_t h = rd32(sets[k] + 4 + 4 * i);
            rt_object *o = rt_handle_get(h, OBJ_SOCKET);
            wsock *w;
            if (o == NULL)
            {
                uint32_t j;
                for (j = 0; j < count; j++) rt_object_unref(objs[j]);
                RET(fail(WSAENOTSOCK));
                return;
            }
            w = o->data;
            objs[count] = o;
            handles[count] = h;
            kind[count] = 1 << k;
            pfd[count].fd = w->fd;
            pfd[count].events = (short)(k == 0 ? POLLIN : k == 1 ? POLLOUT : POLLPRI);
            if (k == 2 && w->connect_pending) pfd[count].events |= POLLOUT;
            pfd[count].revents = 0;
            ready[count] = 0;
            count++;
        }
    }
    if (count == 0)
    {
        RET(fail(WSAEINVAL)); /* Windows: all sets empty */
        return;
    }
    if (tv == 0)
    {
        timeout_ms = -1;
    }
    else
    {
        int64_t ms = (int64_t)(int32_t)rd32(tv) * 1000 + ((int64_t)(int32_t)rd32(tv + 4) + 999) / 1000;
        if (ms < 0) ms = 0;
        timeout_ms = ms > INT32_MAX ? INT32_MAX : (int)ms;
    }
    do
        rc = poll(pfd, count, timeout_ms);
    while (rc < 0 && errno == EINTR && timeout_ms < 0);
    if (rc < 0)
    {
        uint32_t e = wsa_error(errno);
        for (i = 0; i < count; i++) rt_object_unref(objs[i]);
        RET(fail(e));
        return;
    }
    for (i = 0; i < count; i++)
    {
        wsock *w = objs[i]->data;
        short re = pfd[i].revents;
        if (re == 0) continue;
        if (w->connect_pending && (kind[i] == SEL_WRITE || kind[i] == SEL_EXCEPT) && (re & (POLLOUT | POLLERR | POLLHUP)))
        {
            /* The end of a non-blocking connect: success goes to the write
               set, failure to the exception set. */
            int err = 0;
            socklen_t n = sizeof err;
            if (getsockopt(w->fd, SOL_SOCKET, SO_ERROR, &err, &n) != 0) err = errno;
            lock();
            if (w->connect_pending)
            {
                w->connect_pending = false;
                w->error = err != 0 ? wsa_error(err) : 0;
            }
            unlock();
            if (err == 0 && kind[i] == SEL_WRITE) ready[i] = 1;
            if (err != 0 && kind[i] == SEL_EXCEPT) ready[i] = 1;
            /* The other set of the same socket in the same call. */
            {
                uint32_t j;
                for (j = 0; j < count; j++)
                {
                    if (objs[j] != objs[i] || j == i) continue;
                    if (err == 0 && kind[j] == SEL_WRITE) ready[j] = 1;
                    if (err != 0 && kind[j] == SEL_EXCEPT) ready[j] = 1;
                }
            }
            continue;
        }
        if (kind[i] == SEL_READ && (re & (POLLIN | POLLHUP | POLLERR))) ready[i] = 1;
        if (kind[i] == SEL_WRITE && (re & (POLLOUT | POLLERR | POLLHUP))) ready[i] = 1;
        if (kind[i] == SEL_EXCEPT && (re & POLLPRI)) ready[i] = 1;
    }
    /* Write back the sets: each set keeps only its ready sockets. */
    {
        uint32_t idx = 0;
        for (k = 0; k < 3; k++)
        {
            uint32_t n, kept = 0;
            if (sets[k] == 0) continue;
            n = rd32(sets[k]);
            for (i = 0; i < n; i++, idx++)
            {
                if (ready[idx])
                {
                    wr32(sets[k] + 4 + 4 * kept, handles[idx]);
                    kept++;
                }
            }
            wr32(sets[k], kept);
            total += kept;
        }
    }
    for (i = 0; i < count; i++) rt_object_unref(objs[i]);
    RET(total);
}

/* ---------------------------------------------------------------- name resolution */

static uint32_t eai_to_wsa(int e)
{
    switch (e)
    {
    case EAI_AGAIN: return WSATRY_AGAIN;
    case EAI_BADFLAGS: return WSAEINVAL;
    case EAI_FAIL: return WSANO_RECOVERY;
    case EAI_FAMILY: return WSAEAFNOSUPPORT;
    case EAI_MEMORY: return WSA_NOT_ENOUGH_MEMORY;
    case EAI_NONAME: return WSAHOST_NOT_FOUND;
#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
    case EAI_NODATA: return WSANO_DATA;
#endif
    case EAI_SERVICE: return WSATYPE_NOT_FOUND;
    case EAI_SOCKTYPE: return WSAESOCKTNOSUPPORT;
    case EAI_SYSTEM: return wsa_error(errno);
    default: return WSANO_RECOVERY;
    }
}


/* ---------------------------------------------------------------- per-thread result buffers */

/* The Windows Sockets 1.1 database functions and inet_ntoa return a
   pointer to a buffer of the calling thread. The buffer stays valid until
   the next call of the same kind on that thread. Each thread has one guest
   buffer per kind; the buffers are freed when the thread ends. */
enum { TB_HOST, TB_SERV, TB_PROTO, TB_NTOA, TB_COUNT };
#define TB_SIZE 1024u

typedef struct thread_bufs {
    uint32_t buf[TB_COUNT];
} thread_bufs;

static pthread_key_t tb_key;
static pthread_once_t tb_once = PTHREAD_ONCE_INIT;

static void tb_destroy(void *p)
{
    thread_bufs *t = p;
    unsigned i;
    for (i = 0; i < TB_COUNT; i++)
        if (t->buf[i] != 0) rt_heap_free(t->buf[i]);
    free(t);
}

static void tb_init(void)
{
    if (pthread_key_create(&tb_key, tb_destroy) != 0) rt_fatal("Winsock: pthread_key_create failed");
}

/* The zeroed buffer of the given kind of the calling thread, or 0 (error
   set) if there is no guest memory. */
static uint32_t thread_buffer(unsigned kind)
{
    thread_bufs *t;
    if (pthread_once(&tb_once, tb_init) != 0) rt_fatal("Winsock: pthread_once failed");
    t = pthread_getspecific(tb_key);
    if (t == NULL)
    {
        t = calloc(1, sizeof *t);
        if (t == NULL) rt_fatal("out of host memory");
        if (pthread_setspecific(tb_key, t) != 0) rt_fatal("Winsock: pthread_setspecific failed");
    }
    if (t->buf[kind] == 0)
    {
        t->buf[kind] = rt_heap_alloc(TB_SIZE);
        if (t->buf[kind] == 0)
        {
            rt_set_last_error(WSA_NOT_ENOUGH_MEMORY);
            return 0;
        }
    }
    memset(G2H(t->buf[kind]), 0, TB_SIZE);
    return t->buf[kind];
}

/* Sequential writer into a thread buffer. */
typedef struct gwriter {
    uint32_t base, pos;
    bool overflow;
} gwriter;

/* Reserve n bytes (4-byte aligned); returns the guest address or 0. */
static uint32_t gw_take(gwriter *w, uint32_t n)
{
    uint32_t at = (w->pos + 3u) & ~3u;
    if (w->overflow || at > TB_SIZE || n > TB_SIZE - at)
    {
        w->overflow = true;
        return 0;
    }
    w->pos = at + n;
    return w->base + at;
}

static uint32_t gw_str(gwriter *w, const char *s)
{
    size_t n = strlen(s);
    uint32_t g;
    if (n >= TB_SIZE) { w->overflow = true; return 0; }
    g = gw_take(w, (uint32_t)n + 1u);
    if (g != 0) memcpy(G2H(g), s, n + 1);
    return g;
}

/* A NULL-terminated guest array of at most max copies of host strings. */
static uint32_t gw_strlist(gwriter *w, char *const *list, unsigned max)
{
    unsigned n = 0, i;
    uint32_t arr;
    while (list != NULL && list[n] != NULL && n < max) n++;
    arr = gw_take(w, 4u * (n + 1u));
    if (arr == 0) return 0;
    for (i = 0; i < n; i++) wr32(arr + 4u * i, gw_str(w, list[i]));
    wr32(arr + 4u * n, 0);
    return arr;
}

/* ---------------------------------------------------------------- Windows Sockets 1.1 database */

#define MAX_HOST_ADDRS 32u
#define MAX_ALIASES 8u

/* Write a hostent (16 bytes: h_name, h_aliases, h_addrtype, h_length,
   h_addr_list) with the addresses addrs (naddr addresses of len bytes).
   Returns the guest hostent or 0 (error set). */
static uint32_t put_hostent(const char *name, uint16_t type, uint16_t len, const uint8_t *addrs, unsigned naddr)
{
    gwriter w;
    uint32_t h, list, i;
    w.base = thread_buffer(TB_HOST);
    if (w.base == 0) return 0;
    w.pos = 0;
    w.overflow = false;
    h = gw_take(&w, 16);
    list = gw_take(&w, 4u * (naddr + 1u));
    if (h != 0 && list != 0)
    {
        wr32(h + 0, gw_str(&w, name));
        wr32(h + 4, gw_strlist(&w, NULL, 0));
        wr16(h + 8, type);
        wr16(h + 10, len);
        wr32(h + 12, list);
        for (i = 0; i < naddr; i++)
        {
            uint32_t a = gw_take(&w, len);
            if (a != 0) memcpy(G2H(a), addrs + (size_t)len * i, len);
            wr32(list + 4u * i, a);
        }
        wr32(list + 4u * naddr, 0);
    }
    if (w.overflow) rt_fatal("Winsock: a hostent for %s does not fit in %u bytes", name, TB_SIZE);
    return h;
}

/* struct hostent *gethostbyname(const char *name): IPv4 addresses only, as
   on Windows. NULL gives the local host. */
void rt_ws_gethostbyname(_cpu *cpu)
{
    char name[256];
    struct addrinfo hints, *res = NULL, *p;
    uint8_t addrs[4 * MAX_HOST_ADDRS];
    unsigned n = 0, i;
    int rc;
    if (!started())
    {
        RET(0);
        return;
    }
    if (ARG(0) == 0)
    {
        if (gethostname(name, sizeof name) != 0) rt_fatal("gethostname failed: %s", strerror(errno));
        name[sizeof name - 1] = 0;
    }
    else if (rt_gstr(name, sizeof name, ARG(0)) != 0)
    {
        rt_set_last_error(WSAEFAULT);
        RET(0);
        return;
    }
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;
    rc = getaddrinfo(name, NULL, &hints, &res);
    if (rc != 0)
    {
        rt_set_last_error(eai_to_wsa(rc));
        RET(0);
        return;
    }
    for (p = res; p != NULL && n < MAX_HOST_ADDRS; p = p->ai_next)
    {
        const struct sockaddr_in *a = (const struct sockaddr_in *)p->ai_addr;
        bool dup = false;
        if (p->ai_family != AF_INET) continue;
        for (i = 0; i < n && !dup; i++) dup = memcmp(addrs + 4 * i, &a->sin_addr, 4) == 0;
        if (dup) continue;
        memcpy(addrs + 4 * n, &a->sin_addr, 4);
        n++;
    }
    if (n == 0)
    {
        freeaddrinfo(res);
        rt_set_last_error(WSANO_DATA);
        RET(0);
        return;
    }
    RET(put_hostent(res->ai_canonname != NULL ? res->ai_canonname : name, W_AF_INET, 4, addrs, n));
    freeaddrinfo(res);
}

/* ---------------------------------------------------------------- getaddrinfo */

/* Windows ADDRINFOA (32 bytes): ai_flags, ai_family, ai_socktype,
   ai_protocol, ai_addrlen, ai_canonname, ai_addr, ai_next. */
#define W_AI_PASSIVE 0x1u
#define W_AI_CANONNAME 0x2u
#define W_AI_NUMERICHOST 0x4u

/* void freeaddrinfo(PADDRINFOA): each entry is one guest block (the entry,
   the address and the canonical name). */
void rt_freeaddrinfo(_cpu *cpu)
{
    uint32_t ai = ARG(0);
    while (ai != 0)
    {
        uint32_t next = rd32(ai + 28);
        rt_heap_free(ai);
        ai = next;
    }
}

/* Free a partial guest list. */
static void free_guest_list(uint32_t ai)
{
    while (ai != 0)
    {
        uint32_t next = rd32(ai + 28);
        rt_heap_free(ai);
        ai = next;
    }
}

/* int getaddrinfo(PCSTR node, PCSTR service, const ADDRINFOA *hints,
   PADDRINFOA *result): 0 or a WSA error code (also the last error). The
   hint flags of Windows XP (AI_PASSIVE, AI_CANONNAME, AI_NUMERICHOST)
   have the same values on the host. */
void rt_getaddrinfo(_cpu *cpu)
{
    uint32_t gnode = ARG(0), gserv = ARG(1), ghints = ARG(2), pres = ARG(3), first = 0, last = 0;
    char node[256], serv[64];
    struct addrinfo hints, *res = NULL, *p;
    int rc;
    bool canon_done = false;

    if (!started())
    {
        RET(WSANOTINITIALISED);
        return;
    }
    if (pres == 0)
    {
        rt_set_last_error(WSAEINVAL);
        RET(WSAEINVAL);
        return;
    }
    if ((gnode != 0 && rt_gstr(node, sizeof node, gnode) != 0) || (gserv != 0 && rt_gstr(serv, sizeof serv, gserv) != 0))
    {
        rt_set_last_error(WSAEINVAL);
        RET(WSAEINVAL);
        return;
    }
    memset(&hints, 0, sizeof hints);
    if (ghints != 0)
    {
        uint32_t flags = rd32(ghints), fam = rd32(ghints + 4);
        if (rd32(ghints + 16) != 0 || rd32(ghints + 20) != 0 || rd32(ghints + 24) != 0 || rd32(ghints + 28) != 0)
        {
            rt_set_last_error(WSANO_RECOVERY);
            RET(WSANO_RECOVERY);
            return;
        }
        if (flags & ~(W_AI_PASSIVE | W_AI_CANONNAME | W_AI_NUMERICHOST))
        {
            rt_set_last_error(WSAEINVAL);
            RET(WSAEINVAL);
            return;
        }
        if (fam == 0)
            hints.ai_family = AF_UNSPEC;
        else if (fam == W_AF_INET)
            hints.ai_family = AF_INET;
        else if (fam == W_AF_INET6)
            hints.ai_family = AF_INET6;
        else
        {
            rt_set_last_error(WSAEAFNOSUPPORT);
            RET(WSAEAFNOSUPPORT);
            return;
        }
        hints.ai_flags = (int)flags;
        hints.ai_socktype = (int)rd32(ghints + 8);
        hints.ai_protocol = (int)rd32(ghints + 12);
    }
    rc = getaddrinfo(gnode != 0 ? node : NULL, gserv != 0 ? serv : NULL, ghints != 0 ? &hints : NULL, &res);
    if (rc != 0)
    {
        uint32_t e = eai_to_wsa(rc);
        rt_set_last_error(e);
        RET(e);
        return;
    }
    for (p = res; p != NULL; p = p->ai_next)
    {
        uint8_t sa[28];
        uint32_t alen, clen = 0, blk;
        if (p->ai_family != AF_INET && p->ai_family != AF_INET6) continue;
        alen = addr_windows(p->ai_addr, sa);
        if (!canon_done && p->ai_canonname != NULL) clen = (uint32_t)strlen(p->ai_canonname) + 1u;
        if (clen > 1024u) rt_fatal("getaddrinfo: canonical name of %u bytes", clen);
        blk = rt_heap_alloc(32u + 28u + clen);
        if (blk == 0)
        {
            freeaddrinfo(res);
            free_guest_list(first);
            rt_set_last_error(WSA_NOT_ENOUGH_MEMORY);
            RET(WSA_NOT_ENOUGH_MEMORY);
            return;
        }
        memset(G2H(blk), 0, 32u + 28u + clen);
        wr32(blk + 0, (uint32_t)p->ai_flags & (W_AI_PASSIVE | W_AI_CANONNAME | W_AI_NUMERICHOST));
        wr32(blk + 4, p->ai_family == AF_INET ? W_AF_INET : W_AF_INET6);
        wr32(blk + 8, (uint32_t)p->ai_socktype);
        wr32(blk + 12, (uint32_t)p->ai_protocol);
        wr32(blk + 16, alen);
        memcpy(G2H(blk + 32), sa, alen);
        wr32(blk + 24, blk + 32);
        if (clen != 0)
        {
            memcpy(G2H(blk + 60), p->ai_canonname, clen);
            wr32(blk + 20, blk + 60);
            canon_done = true;
        }
        if (last != 0)
            wr32(last + 28, blk);
        else
            first = blk;
        last = blk;
    }
    freeaddrinfo(res);
    if (first == 0)
    {
        rt_set_last_error(WSANO_DATA);
        RET(WSANO_DATA);
        return;
    }
    wr32(pres, first);
    rt_set_last_error(0);
    RET(0);
}

/* ---------------------------------------------------------------- WSAAsyncGetHostByName */

/*
 * One resolver thread works through a queue of at most MAX_ASYNC
 * requests. When a request is done, the thread writes the hostent into the
 * caller's buffer (all pointers point into the buffer) and posts the
 * message to the window: wParam is the task handle, lParam is
 * WSAMAKEASYNCREPLY(length, error). For WSAENOBUFS the length is the
 * buffer size that the result needs. A cancelled request posts no message.
 */
#define MAX_ASYNC 64u

typedef struct async_req {
    uint32_t handle;  /* 0: the entry is free */
    uint32_t hwnd, msg, buf, buflen;
    char name[256];
    bool cancelled, running;
} async_req;

static pthread_mutex_t async_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t async_cond = PTHREAD_COND_INITIALIZER;
static async_req async_reqs[MAX_ASYNC];
static uint32_t async_next_handle = 1;
static bool async_thread_started;

static void async_lock_take(void)
{
    if (pthread_mutex_lock(&async_lock) != 0) rt_fatal("Winsock: async lock failed");
}

static void async_lock_drop(void)
{
    if (pthread_mutex_unlock(&async_lock) != 0) rt_fatal("Winsock: async unlock failed");
}

/* Write a hostent with the IPv4 addresses into buf (len bytes). Returns
   the length that the hostent needs. Writes nothing if it does not fit. */
static uint32_t async_hostent(uint32_t buf, uint32_t len, const char *name, const uint8_t *addrs, uint32_t n)
{
    uint32_t nlen = (uint32_t)strlen(name) + 1u;
    uint32_t aliases = 16u, list = aliases + 4u, data = list + 4u * (n + 1u), str = data + 4u * n, need = str + nlen, i;
    if (need > len) return need;
    wr32(buf + 0, buf + str);
    wr32(buf + 4, buf + aliases);
    wr16(buf + 8, W_AF_INET);
    wr16(buf + 10, 4);
    wr32(buf + 12, buf + list);
    wr32(buf + aliases, 0);
    for (i = 0; i < n; i++)
    {
        memcpy(G2H(buf + data + 4u * i), addrs + 4u * i, 4);
        wr32(buf + list + 4u * i, buf + data + 4u * i);
    }
    wr32(buf + list + 4u * n, 0);
    memcpy(G2H(buf + str), name, nlen);
    return need;
}

static void *async_main(void *arg)
{
    (void)arg;
    for (;;)
    {
        async_req *r = NULL;
        struct addrinfo hints, *res = NULL, *p;
        uint8_t addrs[4 * MAX_HOST_ADDRS];
        char name[256], canon[256];
        uint32_t n = 0, i, err = 0, need = 0, handle, hwnd, msg;
        int rc;

        async_lock_take();
        while (r == NULL)
        {
            for (i = 0; i < MAX_ASYNC && r == NULL; i++)
                if (async_reqs[i].handle != 0 && !async_reqs[i].running && !async_reqs[i].cancelled) r = &async_reqs[i];
            if (r == NULL && pthread_cond_wait(&async_cond, &async_lock) != 0) rt_fatal("Winsock: async wait failed");
        }
        r->running = true;
        memcpy(name, r->name, sizeof name);
        async_lock_drop();

        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_CANONNAME;
        snprintf(canon, sizeof canon, "%s", name);
        rc = getaddrinfo(name, NULL, &hints, &res);
        if (rc != 0)
            err = eai_to_wsa(rc);
        else
        {
            for (p = res; p != NULL && n < MAX_HOST_ADDRS; p = p->ai_next)
            {
                const struct sockaddr_in *a = (const struct sockaddr_in *)p->ai_addr;
                bool dup = false;
                if (p->ai_family != AF_INET) continue;
                for (i = 0; i < n && !dup; i++) dup = memcmp(addrs + 4 * i, &a->sin_addr, 4) == 0;
                if (dup) continue;
                memcpy(addrs + 4 * n, &a->sin_addr, 4);
                n++;
            }
            if (res->ai_canonname != NULL) snprintf(canon, sizeof canon, "%s", res->ai_canonname);
            freeaddrinfo(res);
            if (n == 0) err = WSANO_DATA;
        }

        async_lock_take();
        handle = r->handle;
        hwnd = r->hwnd;
        msg = r->msg;
        if (!r->cancelled)
        {
            if (err == 0)
            {
                need = async_hostent(r->buf, r->buflen, canon, addrs, n);
                if (need > r->buflen) err = WSAENOBUFS;
            }
            /* The reply is posted under the lock, so that a cancel after
               this point fails with WSAEALREADY. */
            (void)rt_user32_post_message(hwnd, msg, handle, (need > 0xFFFFu ? 0xFFFFu : need) | (err << 16));
        }
        memset(r, 0, sizeof *r);
        async_lock_drop();
    }
    return NULL;
}

/* HANDLE WSAAsyncGetHostByName(HWND, u_int msg, const char *name,
   char *buf, int buflen) */
void rt_ws_WSAAsyncGetHostByName(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), msg = ARG(1), gname = ARG(2), buf = ARG(3), buflen = ARG(4), i, h;
    async_req *r = NULL;
    if (!started())
    {
        RET(0);
        return;
    }
    if (gname == 0 || buf == 0 || (int32_t)buflen <= 0 || !rt_va_is_committed(buf, buflen))
    {
        rt_set_last_error(WSAEFAULT);
        RET(0);
        return;
    }
    async_lock_take();
    for (i = 0; i < MAX_ASYNC && r == NULL; i++)
        if (async_reqs[i].handle == 0) r = &async_reqs[i];
    if (r == NULL)
    {
        async_lock_drop();
        rt_set_last_error(WSAENOBUFS);
        RET(0);
        return;
    }
    if (rt_gstr(r->name, sizeof r->name, gname) != 0)
    {
        async_lock_drop();
        rt_set_last_error(WSAEFAULT);
        RET(0);
        return;
    }
    if (!async_thread_started)
    {
        pthread_t t;
        if (pthread_create(&t, NULL, async_main, NULL) != 0) rt_fatal("Winsock: the resolver thread cannot start");
        async_thread_started = true;
    }
    h = async_next_handle++;
    if (async_next_handle == 0) async_next_handle = 1;
    r->handle = h;
    r->hwnd = hwnd;
    r->msg = msg;
    r->buf = buf;
    r->buflen = buflen;
    r->cancelled = false;
    r->running = false;
    if (pthread_cond_signal(&async_cond) != 0) rt_fatal("Winsock: async signal failed");
    async_lock_drop();
    RET(h);
}

/* int WSACancelAsyncRequest(HANDLE task): 0, or SOCKET_ERROR with
   WSAEINVAL (no such task) or WSAEALREADY (the task is done). */
void rt_ws_WSACancelAsyncRequest(_cpu *cpu)
{
    uint32_t h = ARG(0), i;
    if (!started())
    {
        RET(SOCKET_ERROR);
        return;
    }
    async_lock_take();
    for (i = 0; i < MAX_ASYNC; i++)
    {
        if (h != 0 && async_reqs[i].handle == h && !async_reqs[i].cancelled)
        {
            async_reqs[i].cancelled = true;
            /* A request that is not running yet is free now; a running
               request is freed by the resolver thread. */
            if (!async_reqs[i].running) memset(&async_reqs[i], 0, sizeof async_reqs[i]);
            async_lock_drop();
            RET(0);
            return;
        }
    }
    async_lock_drop();
    /* Handles are given in increasing order, so a smaller handle that is
       not pending is a task that is done. */
    rt_set_last_error(h != 0 && h < async_next_handle ? WSAEALREADY : WSAEINVAL);
    RET(SOCKET_ERROR);
}

/* struct hostent *gethostbyaddr(const char *addr, int len, int type) */
void rt_ws_gethostbyaddr(_cpu *cpu)
{
    uint32_t addr = ARG(0), type = ARG(2);
    int32_t len = (int32_t)ARG(1);
    struct sockaddr_storage ss;
    socklen_t sl;
    char host[NI_MAXHOST];
    uint8_t raw[16];
    int rc;
    if (!started())
    {
        RET(0);
        return;
    }
    memset(&ss, 0, sizeof ss);
    if (type == W_AF_INET && len == 4)
    {
        struct sockaddr_in *a = (struct sockaddr_in *)&ss;
        a->sin_len = sizeof *a;
        a->sin_family = AF_INET;
        sl = sizeof *a;
        if (addr == 0 || !rt_va_is_committed(addr, 4))
        {
            rt_set_last_error(WSAEFAULT);
            RET(0);
            return;
        }
        memcpy(&a->sin_addr, G2H(addr), 4);
    }
    else if (type == W_AF_INET6 && len == 16)
    {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
        a->sin6_len = sizeof *a;
        a->sin6_family = AF_INET6;
        sl = sizeof *a;
        if (addr == 0 || !rt_va_is_committed(addr, 16))
        {
            rt_set_last_error(WSAEFAULT);
            RET(0);
            return;
        }
        memcpy(&a->sin6_addr, G2H(addr), 16);
    }
    else
    {
        rt_set_last_error(type == W_AF_INET || type == W_AF_INET6 ? WSAEFAULT : WSAEAFNOSUPPORT);
        RET(0);
        return;
    }
    memcpy(raw, G2H(addr), (size_t)len);
    rc = getnameinfo((struct sockaddr *)&ss, sl, host, sizeof host, NULL, 0, NI_NAMEREQD);
    if (rc != 0)
    {
        rt_set_last_error(eai_to_wsa(rc));
        RET(0);
        return;
    }
    RET(put_hostent(host, (uint16_t)type, (uint16_t)len, raw, 1));
}

/* int gethostname(char *name, int namelen) */
void rt_ws_gethostname(_cpu *cpu)
{
    char name[256];
    uint32_t buf = ARG(0);
    int32_t len = (int32_t)ARG(1);
    size_t n;
    if (!started())
    {
        RET(SOCKET_ERROR);
        return;
    }
    if (gethostname(name, sizeof name) != 0) rt_fatal("gethostname failed: %s", strerror(errno));
    name[sizeof name - 1] = 0;
    n = strlen(name);
    if (buf == 0 || len < 0 || (size_t)len < n + 1 || !rt_va_is_committed(buf, (uint32_t)n + 1u))
    {
        RET(fail(WSAEFAULT));
        return;
    }
    memcpy(G2H(buf), name, n + 1);
    RET(0);
}

/* struct protoent *getprotobyname(const char *name): protoent is 12 bytes
   (p_name, p_aliases, p_proto). The host protocol database has the same
   names and numbers as the Windows protocol file. */
void rt_ws_getprotobyname(_cpu *cpu)
{
    char name[256];
    struct protoent *pe;
    gwriter w;
    uint32_t r = 0;
    if (!started())
    {
        RET(0);
        return;
    }
    if (ARG(0) == 0 || rt_gstr(name, sizeof name, ARG(0)) != 0)
    {
        rt_set_last_error(WSAEFAULT);
        RET(0);
        return;
    }
    w.base = thread_buffer(TB_PROTO);
    if (w.base == 0)
    {
        RET(0);
        return;
    }
    w.pos = 0;
    w.overflow = false;
    /* getprotobyname uses a static host buffer. */
    lock();
    pe = getprotobyname(name);
    if (pe != NULL)
    {
        r = gw_take(&w, 12);
        wr32(r + 0, gw_str(&w, pe->p_name));
        wr32(r + 4, gw_strlist(&w, pe->p_aliases, MAX_ALIASES));
        wr16(r + 8, (uint16_t)pe->p_proto);
    }
    unlock();
    if (w.overflow) rt_fatal("Winsock: the protocol entry %s does not fit in %u bytes", name, TB_SIZE);
    if (r == 0) rt_set_last_error(WSANO_DATA);
    RET(r);
}

/* Write a servent (16 bytes: s_name, s_aliases, s_port, s_proto) from the
   host entry. The caller holds the Winsock lock (static host buffer). */
static uint32_t put_servent(const struct servent *se, gwriter *w)
{
    uint32_t r = gw_take(w, 16);
    if (r == 0) return 0;
    wr32(r + 0, gw_str(w, se->s_name));
    wr32(r + 4, gw_strlist(w, se->s_aliases, MAX_ALIASES));
    wr16(r + 8, (uint16_t)se->s_port); /* network byte order */
    wr32(r + 12, gw_str(w, se->s_proto));
    return r;
}

static void serv_query(_cpu *cpu, bool by_port)
{
    char name[256], proto[64];
    const char *pp = NULL;
    struct servent *se;
    gwriter w;
    uint32_t r = 0;
    if (!started())
    {
        RET(0);
        return;
    }
    if ((!by_port && (ARG(0) == 0 || rt_gstr(name, sizeof name, ARG(0)) != 0)) ||
        (ARG(1) != 0 && rt_gstr(proto, sizeof proto, ARG(1)) != 0))
    {
        rt_set_last_error(WSAEFAULT);
        RET(0);
        return;
    }
    if (ARG(1) != 0) pp = proto;
    w.base = thread_buffer(TB_SERV);
    if (w.base == 0)
    {
        RET(0);
        return;
    }
    w.pos = 0;
    w.overflow = false;
    lock();
    se = by_port ? getservbyport((int)(uint16_t)ARG(0), pp) : getservbyname(name, pp);
    if (se != NULL) r = put_servent(se, &w);
    unlock();
    if (w.overflow) rt_fatal("Winsock: a service entry does not fit in %u bytes", TB_SIZE);
    if (r == 0) rt_set_last_error(WSANO_DATA);
    RET(r);
}

/* struct servent *getservbyname(const char *name, const char *proto) */
void rt_ws_getservbyname(_cpu *cpu)
{
    serv_query(cpu, false);
}

/* struct servent *getservbyport(int port, const char *proto); port is in
   network byte order. */
void rt_ws_getservbyport(_cpu *cpu)
{
    serv_query(cpu, true);
}

/* char *inet_ntoa(struct in_addr in) */
void rt_ws_inet_ntoa(_cpu *cpu)
{
    uint32_t a = ARG(0), buf = thread_buffer(TB_NTOA);
    if (buf == 0)
    {
        RET(0);
        return;
    }
    snprintf(G2H(buf), 16, "%u.%u.%u.%u", a & 0xFFu, (a >> 8) & 0xFFu, (a >> 16) & 0xFFu, a >> 24);
    RET(buf);
}

/* void WSASetLastError(int) */
void rt_WSASetLastError(_cpu *cpu)
{
    rt_set_last_error(ARG(0));
}

/* ---------------------------------------------------------------- Windows Sockets 2 */

#define W_WSA_FLAG_OVERLAPPED 0x01u
#define W_WSA_FLAG_NO_HANDLE_INHERIT 0x80u
#define W_SIO_GET_EXTENSION_FUNCTION_POINTER 0xC8000006u
#define W_SIO_KEEPALIVE_VALS 0x98000004u
#define W_TF_DISCONNECT 0x01u
#define W_TF_REUSE_SOCKET 0x02u
#define W_TF_WRITE_BEHIND 0x04u
#define W_TF_USE_SYSTEM_THREAD 0x10u
#define W_TF_USE_KERNEL_APC 0x20u
#define MAX_WSABUF 1024u

extern char TransmitFile_asm2c[];

/* SOCKET WSASocketW(int af, int type, int protocol,
   LPWSAPROTOCOL_INFOW info, GROUP g, DWORD flags) */
void rt_WSASocketW(_cpu *cpu)
{
    uint32_t flags = ARG(5);
    if (ARG(3) != 0) rt_fatal("WSASocketW with a protocol information structure is not supported");
    if (ARG(4) != 0) rt_fatal("WSASocketW with socket group %u is not supported", ARG(4));
    if ((flags & ~(W_WSA_FLAG_OVERLAPPED | W_WSA_FLAG_NO_HANDLE_INHERIT)) != 0)
        rt_fatal("WSASocketW flags 0x%x are not supported", flags);
    /* WSA_FLAG_OVERLAPPED only permits overlapped calls; the runtime has
       none. Socket handles are not inherited (no child processes). */
    RET(create_socket(ARG(0), ARG(1), ARG(2)));
}

/* Host iovec list for a guest WSABUF array (8 bytes each: len, buf).
   Returns false (error set) if a buffer is not valid. */
static bool wsabufs(uint32_t bufs, uint32_t count, struct iovec *iov)
{
    uint32_t i;
    if (count == 0 || count > MAX_WSABUF || bufs == 0 || !rt_va_is_committed(bufs, 8u * count))
    {
        rt_set_last_error(count > MAX_WSABUF ? WSAENOBUFS : WSAEFAULT);
        return false;
    }
    for (i = 0; i < count; i++)
    {
        uint32_t len = rd32(bufs + 8u * i), b = rd32(bufs + 8u * i + 4u);
        if (len > INT32_MAX || !buffer_ok(b, (int32_t)len))
        {
            rt_set_last_error(WSAEFAULT);
            return false;
        }
        iov[i].iov_base = len != 0 ? G2H(b) : NULL;
        iov[i].iov_len = len;
    }
    return true;
}

/* int WSASend(SOCKET, LPWSABUF, DWORD count, LPDWORD sent, DWORD flags,
   LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE) */
void rt_WSASend(_cpu *cpu)
{
    struct iovec iov[MAX_WSABUF];
    struct msghdr m;
    rt_object *o;
    uint32_t count = ARG(2), psent = ARG(3);
    bool ok;
    int hf, e;
    ssize_t r;
    if (ARG(5) != 0 || ARG(6) != 0) rt_fatal("WSASend with overlapped I/O is not supported");
    o = sock_get(ARG(0));
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    hf = host_msg_flags(ARG(4), &ok);
    if (!ok || psent == 0 || !rt_va_is_committed(psent, 4) || !wsabufs(ARG(1), count, iov))
    {
        rt_object_unref(o);
        if (!ok) rt_set_last_error(WSAEOPNOTSUPP);
        else if (psent == 0 || !rt_va_is_committed(psent, 4)) rt_set_last_error(WSAEFAULT);
        RET(SOCKET_ERROR);
        return;
    }
    memset(&m, 0, sizeof m);
    m.msg_iov = iov;
    m.msg_iovlen = (int)count;
    r = sendmsg(((wsock *)o->data)->fd, &m, hf);
    e = r < 0 ? errno : 0;
    rt_object_unref(o);
    if (r < 0)
    {
        RET(fail(wsa_error(e)));
        return;
    }
    wr32(psent, (uint32_t)r);
    RET(0);
}

/* int WSARecv(SOCKET, LPWSABUF, DWORD count, LPDWORD received,
   LPDWORD flags, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE) */
void rt_WSARecv(_cpu *cpu)
{
    struct iovec iov[MAX_WSABUF];
    struct msghdr m;
    rt_object *o;
    uint32_t count = ARG(2), precv = ARG(3), pflags = ARG(4);
    bool ok, dgram;
    int hf, e;
    ssize_t r;
    if (ARG(5) != 0 || ARG(6) != 0) rt_fatal("WSARecv with overlapped I/O is not supported");
    o = sock_get(ARG(0));
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    if (precv == 0 || pflags == 0 || !rt_va_is_committed(precv, 4) || !rt_va_is_committed(pflags, 4))
    {
        rt_object_unref(o);
        RET(fail(WSAEFAULT));
        return;
    }
    hf = host_msg_flags(rd32(pflags), &ok);
    if (!ok || !wsabufs(ARG(1), count, iov))
    {
        rt_object_unref(o);
        if (!ok) rt_set_last_error(WSAEOPNOTSUPP);
        RET(SOCKET_ERROR);
        return;
    }
    dgram = ((wsock *)o->data)->type == SOCK_DGRAM;
    memset(&m, 0, sizeof m);
    m.msg_iov = iov;
    m.msg_iovlen = (int)count;
    r = recvmsg(((wsock *)o->data)->fd, &m, hf);
    e = r < 0 ? errno : 0;
    rt_object_unref(o);
    if (r < 0)
    {
        RET(fail(wsa_error(e)));
        return;
    }
    wr32(precv, (uint32_t)r);
    wr32(pflags, 0);
    /* A datagram larger than the buffers: Windows fills the buffers and
       returns WSAEMSGSIZE. */
    if (dgram && (m.msg_flags & MSG_TRUNC) != 0)
    {
        RET(fail(WSAEMSGSIZE));
        return;
    }
    RET(0);
}

static const uint8_t wsaid_transmitfile[16] = { 0xf0, 0x7d, 0x36, 0xb5, 0xac, 0xcb, 0xcf, 0x11,
                                                0x95, 0xca, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92 };

/* int WSAIoctl(SOCKET, DWORD code, LPVOID in, DWORD inlen, LPVOID out,
   DWORD outlen, LPDWORD returned, LPWSAOVERLAPPED,
   LPWSAOVERLAPPED_COMPLETION_ROUTINE) */
void rt_WSAIoctl(_cpu *cpu)
{
    uint32_t code = ARG(1), in = ARG(2), inlen = ARG(3), out = ARG(4), outlen = ARG(5), pret = ARG(6), r = 0, n = 0;
    rt_object *o;
    wsock *w;
    if (ARG(7) != 0 || ARG(8) != 0) rt_fatal("WSAIoctl with overlapped I/O is not supported");
    o = sock_get(ARG(0));
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    w = o->data;
    if (pret == 0 || !rt_va_is_committed(pret, 4) || (inlen != 0 && (in == 0 || !rt_va_is_committed(in, inlen))) ||
        (outlen != 0 && (out == 0 || !rt_va_is_committed(out, outlen))))
    {
        rt_object_unref(o);
        RET(fail(WSAEFAULT));
        return;
    }
    if (code == W_FIONBIO)
    {
        if (inlen < 4)
            r = fail(WSAEFAULT);
        else
        {
            bool nb = rd32(in) != 0;
            int fl = fcntl(w->fd, F_GETFL);
            if (fl < 0 || fcntl(w->fd, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) != 0)
                r = fail_errno();
            else
                w->nonblocking = nb;
        }
    }
    else if (code == W_FIONREAD)
    {
        int avail = 0;
        if (outlen < 4)
            r = fail(WSAEFAULT);
        else if (ioctl(w->fd, FIONREAD, &avail) != 0)
            r = fail_errno();
        else
        {
            wr32(out, (uint32_t)(avail < 0 ? 0 : avail));
            n = 4;
        }
    }
    else if (code == W_SIO_GET_EXTENSION_FUNCTION_POINTER)
    {
        /* The runtime provider has the TransmitFile extension. Other
           extensions (AcceptEx, ConnectEx, DisconnectEx and so on) need
           overlapped I/O and are not provided: Windows gives WSAEINVAL for
           an extension that the provider does not have. */
        if (inlen < 16 || outlen < 4)
            r = fail(WSAEFAULT);
        else if (memcmp(G2H(in), wsaid_transmitfile, 16) == 0)
        {
            wr32(out, H2G(TransmitFile_asm2c));
            n = 4;
        }
        else
            r = fail(WSAEINVAL);
    }
    else if (code == W_SIO_KEEPALIVE_VALS)
    {
        /* struct tcp_keepalive: onoff, keepalivetime (ms), keepaliveinterval (ms) */
        if (inlen < 12)
            r = fail(WSAEFAULT);
        else
        {
            int on = rd32(in) != 0, idle = (int)((rd32(in + 4) + 999u) / 1000u), intvl = (int)((rd32(in + 8) + 999u) / 1000u);
            if (idle < 1) idle = 1;
            if (intvl < 1) intvl = 1;
            if (setsockopt(w->fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on) != 0 ||
                (on && (setsockopt(w->fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof idle) != 0 ||
                        setsockopt(w->fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl) != 0)))
                r = fail_errno();
        }
    }
    else
        rt_fatal("WSAIoctl code 0x%08x is not supported", code);
    rt_object_unref(o);
    if (r == 0) wr32(pret, n);
    RET(r);
}

/* Send all n bytes of p on fd. Waits for the socket in non-blocking mode.
   Returns 0 or a WSA error code. */
static uint32_t send_all(int fd, const uint8_t *p, size_t n)
{
    while (n > 0)
    {
        ssize_t r = send(fd, p, n, 0);
        if (r < 0)
        {
            if (errno == EINTR) continue;
            if (errno == EAGAIN)
            {
                struct pollfd pf = { fd, POLLOUT, 0 };
                if (poll(&pf, 1, -1) < 0 && errno != EINTR) return wsa_error(errno);
                continue;
            }
            return wsa_error(errno);
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* BOOL TransmitFile(SOCKET, HANDLE file, DWORD bytes, DWORD bytes_per_send,
   LPOVERLAPPED, LPTRANSMIT_FILE_BUFFERS, DWORD flags)
   Sends the head buffer, then the file from its current position (bytes,
   0 for the rest of the file), then the tail buffer. TF_DISCONNECT ends the
   connection for sending after the data. */
void rt_TransmitFile(_cpu *cpu)
{
    uint32_t hfile = ARG(1), bytes = ARG(2), tb = ARG(5), flags = ARG(6), err = 0;
    rt_object *o, *fo = NULL;
    int sfd, ffd = -1;
    static const uint32_t ignored = W_TF_WRITE_BEHIND | W_TF_USE_SYSTEM_THREAD | W_TF_USE_KERNEL_APC;
    if (ARG(4) != 0) rt_fatal("TransmitFile with overlapped I/O is not supported");
    if (flags & W_TF_REUSE_SOCKET) rt_fatal("TransmitFile with TF_REUSE_SOCKET is not supported");
    if ((flags & ~(W_TF_DISCONNECT | ignored)) != 0) rt_fatal("TransmitFile flags 0x%x are not supported", flags);
    o = sock_get(ARG(0));
    if (o == NULL)
    {
        RET(0);
        return;
    }
    sfd = ((wsock *)o->data)->fd;
    if (tb != 0 && !rt_va_is_committed(tb, 16)) err = WSAEFAULT;
    if (err == 0 && hfile != 0)
    {
        fo = rt_handle_get(hfile, OBJ_FILE);
        if (fo == NULL || (ffd = rt_file_object_fd(fo)) < 0) err = WSAEINVAL;
    }
    if (err == 0 && tb != 0 && rd32(tb + 4) != 0)
    {
        if (!buffer_ok(rd32(tb), (int32_t)rd32(tb + 4))) err = WSAEFAULT;
        else err = send_all(sfd, G2H(rd32(tb)), rd32(tb + 4));
    }
    if (err == 0 && ffd >= 0)
    {
        enum { CHUNK = 65536 };
        uint8_t *chunk = malloc(CHUNK);
        uint64_t left = bytes != 0 ? bytes : UINT64_MAX;
        if (chunk == NULL) rt_fatal("out of host memory");
        while (err == 0 && left > 0)
        {
            size_t want = left < CHUNK ? (size_t)left : CHUNK;
            ssize_t r = read(ffd, chunk, want);
            if (r < 0)
            {
                if (errno == EINTR) continue;
                err = WSAEINVAL;
                break;
            }
            if (r == 0) break;
            err = send_all(sfd, chunk, (size_t)r);
            left -= (uint64_t)r;
        }
        free(chunk);
    }
    if (err == 0 && tb != 0 && rd32(tb + 12) != 0)
    {
        if (!buffer_ok(rd32(tb + 8), (int32_t)rd32(tb + 12))) err = WSAEFAULT;
        else err = send_all(sfd, G2H(rd32(tb + 8)), rd32(tb + 12));
    }
    if (err == 0 && (flags & W_TF_DISCONNECT) && shutdown(sfd, SHUT_WR) != 0 && errno != ENOTCONN) err = wsa_error(errno);
    if (fo != NULL) rt_object_unref(fo);
    rt_object_unref(o);
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    RET(1);
}

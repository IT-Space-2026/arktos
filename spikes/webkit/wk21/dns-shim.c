/* WK21 audit shim — LD_PRELOAD logger.
 *
 * Logs every libc hostname-resolution entry point and every outbound
 * INET connect()/sendto()/sendmsg() to $WK21_DNSLOG, one line per call,
 * tagged with pid + /proc/self/comm so the UI / Web / Network process
 * roles stay distinguishable:
 *
 *   RESOLVE pid=123 comm=WPENetworkProc getaddrinfo host=wk21-x.test
 *   CONNECT  pid=123 comm=WPENetworkProc tcp 127.0.0.1:45678
 *   SENDTO   pid=123 comm=WPEWebProc udp 224.0.0.251:5353
 *
 * The leak questions this answers:
 *   - RESOLVE lines for test hostnames  -> the browser resolved the
 *     site name LOCALLY (dns prefetch, socks5-without-h, etc.) even
 *     though a SOCKS proxy was configured — a Tor-grade DNS leak.
 *   - CONNECT/SENDTO lines to non-loopback peers -> traffic bypassing
 *     the session proxy entirely (aux requests, mDNS, STUN).
 *
 * Deliberately dumb: open(O_APPEND)/write per call, no buffering, no
 * locks — appended lines may interleave across processes but never
 * corrupt. Async-safety is not attempted beyond "don't crash".
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void shim_log(const char *tag, const char *fmt, ...)
{
    const char *path = getenv("WK21_DNSLOG");
    if (!path || !*path)
        return;
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    char comm[64] = "?";
    int cfd = open("/proc/self/comm", O_RDONLY | O_CLOEXEC);
    if (cfd >= 0) {
        ssize_t n = read(cfd, comm, sizeof(comm) - 1);
        if (n > 0) {
            comm[n] = 0;
            char *nl = strchr(comm, '\n');
            if (nl) *nl = 0;
        }
        close(cfd);
    }
    char buf[1024];
    int off = snprintf(buf, sizeof(buf), "%s pid=%d comm=%s ",
                       tag, (int)getpid(), comm);
    va_list ap;
    va_start(ap, fmt);
    if (off > 0 && (size_t)off < sizeof(buf))
        vsnprintf(buf + off, sizeof(buf) - (size_t)off, fmt, ap);
    va_end(ap);
    size_t len = strlen(buf);
    if (len + 1 < sizeof(buf)) { buf[len++] = '\n'; buf[len] = 0; }
    (void)!write(fd, buf, len);
    close(fd);
}

static void log_peer(const char *tag, const struct sockaddr *sa,
                     socklen_t len)
{
    if (!sa)
        return;
    char addr[INET6_ADDRSTRLEN] = "?";
    if (sa->sa_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        inet_ntop(AF_INET, &in->sin_addr, addr, sizeof(addr));
        shim_log(tag, "tcp/udp %s:%u", addr, ntohs(in->sin_port));
    } else if (sa->sa_family == AF_INET6
               && len >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        inet_ntop(AF_INET6, &in6->sin6_addr, addr, sizeof(addr));
        shim_log(tag, "tcp/udp [%s]:%u", addr, ntohs(in6->sin6_port));
    }
    /* AF_UNIX (dbus, wayland, webkit ipc) is not a leak — skip. */
}

/* ---- libc resolution entry points ---- */

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res)
{
    static int (*real)(const char *, const char *,
                       const struct addrinfo *, struct addrinfo **);
    if (!real) real = dlsym(RTLD_NEXT, "getaddrinfo");
    if (node && *node)
        shim_log("RESOLVE", "getaddrinfo host=%s svc=%s",
                 node, service ? service : "-");
    return real(node, service, hints, res);
}

struct hostent *gethostbyname(const char *name)
{
    static struct hostent *(*real)(const char *);
    if (!real) real = dlsym(RTLD_NEXT, "gethostbyname");
    if (name && *name)
        shim_log("RESOLVE", "gethostbyname host=%s", name);
    return real(name);
}

struct hostent *gethostbyname2(const char *name, int af)
{
    static struct hostent *(*real)(const char *, int);
    if (!real) real = dlsym(RTLD_NEXT, "gethostbyname2");
    if (name && *name)
        shim_log("RESOLVE", "gethostbyname2 host=%s af=%d", name, af);
    return real(name, af);
}

/* _r variants are a separate symbol family — resolvers that use them
 * (some libsoup/glib paths) would otherwise slip past the log. */
int gethostbyname_r(const char *name, struct hostent *ret, char *buf,
                    size_t buflen, struct hostent **result, int *h_errnop)
{
    static int (*real)(const char *, struct hostent *, char *, size_t,
                       struct hostent **, int *);
    if (!real) real = dlsym(RTLD_NEXT, "gethostbyname_r");
    if (name && *name)
        shim_log("RESOLVE", "gethostbyname_r host=%s", name);
    return real(name, ret, buf, buflen, result, h_errnop);
}

int gethostbyname2_r(const char *name, int af, struct hostent *ret,
                     char *buf, size_t buflen, struct hostent **result,
                     int *h_errnop)
{
    static int (*real)(const char *, int, struct hostent *, char *,
                       size_t, struct hostent **, int *);
    if (!real) real = dlsym(RTLD_NEXT, "gethostbyname2_r");
    if (name && *name)
        shim_log("RESOLVE", "gethostbyname2_r host=%s af=%d", name, af);
    return real(name, af, ret, buf, buflen, result, h_errnop);
}

/* The res_* entry points go straight at the nameserver — a bypass of
 * NSS entirely. */
int res_query(const char *dname, int cls, int type,
              unsigned char *answer, int anslen)
{
    static int (*real)(const char *, int, int, unsigned char *, int);
    if (!real) real = dlsym(RTLD_NEXT, "res_query");
    if (dname && *dname)
        shim_log("RESOLVE", "res_query name=%s type=%d", dname, type);
    return real(dname, cls, type, answer, anslen);
}

int res_search(const char *dname, int cls, int type,
               unsigned char *answer, int anslen)
{
    static int (*real)(const char *, int, int, unsigned char *, int);
    if (!real) real = dlsym(RTLD_NEXT, "res_search");
    if (dname && *dname)
        shim_log("RESOLVE", "res_search name=%s type=%d", dname, type);
    return real(dname, cls, type, answer, anslen);
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags)
{
    static int (*real)(const struct sockaddr *, socklen_t,
                       char *, socklen_t, char *, socklen_t, int);
    if (!real) real = dlsym(RTLD_NEXT, "getnameinfo");
    if (!(flags & NI_NUMERICHOST))
        shim_log("RESOLVE", "getnameinfo (reverse)");
    return real(sa, salen, host, hostlen, serv, servlen, flags);
}

/* ---- outbound connections ---- */

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    static int (*real)(int, const struct sockaddr *, socklen_t);
    if (!real) real = dlsym(RTLD_NEXT, "connect");
    if (addr && (addr->sa_family == AF_INET || addr->sa_family == AF_INET6))
        log_peer("CONNECT", addr, len);
    return real(fd, addr, len);
}

ssize_t sendto(int fd, const void *buf, size_t n, int flags,
               const struct sockaddr *addr, socklen_t len)
{
    static ssize_t (*real)(int, const void *, size_t, int,
                           const struct sockaddr *, socklen_t);
    if (!real) real = dlsym(RTLD_NEXT, "sendto");
    if (addr && (addr->sa_family == AF_INET || addr->sa_family == AF_INET6))
        log_peer("SENDTO", addr, len);
    return real(fd, buf, n, flags, addr, len);
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
    static ssize_t (*real)(int, const struct msghdr *, int);
    if (!real) real = dlsym(RTLD_NEXT, "sendmsg");
    if (msg && msg->msg_name) {
        const struct sockaddr *sa = (const struct sockaddr *)msg->msg_name;
        if (sa->sa_family == AF_INET || sa->sa_family == AF_INET6)
            log_peer("SENDMSG", sa, (socklen_t)msg->msg_namelen);
    }
    return real(fd, msg, flags);
}

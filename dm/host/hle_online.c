// The game's side of the in-host online services (hle_online.h): the service names resolve to the loopback address, and a TCP socket connected there is the
// client end of an online_conn (dm/host/online), the server end living in this process. No host socket is made and nothing is sent anywhere.
// A game that needs its publisher's servers to start (The Duskbloods Network Test: "Online connection is required", also for play alone) gets the answers of a
// first sign-in; matchmaking and play with others are not served. Sockets of any other kind, and connections to any other address, keep the offline answers.
#undef NX_ONLINE
#define NX_ONLINE 1   // this file is the services' side: the declarations, not the stubs of a build without them
#include "hle.h"
#include "hle_online.h"
#include "online/online.h"
#include <pthread.h>
#include <sys/stat.h>

int dm_sk_err(int sk, int libc);     // hle_net.c: -1 with the socket error code and the guest's errno
void dm_guest_sleep_ns(int64_t ns);
enum { ONL_FD_BASE = 768, ONL_FD_MAX = 64 };   // below select's 1024; eventfds are 512.., the co-op module's sockets 256..
enum { SK_AF_INET = 2, SK_SOCK_STREAM = 1, SK_MSG_PEEK = 2, SK_MSG_DONTWAIT = 0x80, SK_O_NONBLOCK = 4, LIBC_O_NONBLOCK = 0x800, SK_F_GETFD = 1, SK_F_SETFD = 2, SK_F_GETFL = 3, SK_F_SETFL = 4,
       SK_SOL_SOCKET = 0xffff, SK_SO_ERROR = 0x1007, SK_SO_TYPE = 0x1008, SK_POLLIN = 1, SK_POLLOUT = 4, SK_POLLHUP = 0x10, SK_POLLRDNORM = 0x40 };
// FreeBSD's numbers (Horizon's sockets), then the C library's (Linux numbers), which gRPC reads
enum { E_BADF = 9, E_AGAIN = 35, E_INVAL = 22, E_NOTCONN = 57, E_CONNREFUSED = 61, E_ISCONN = 56, E_PIPE = 32, E_NETDOWN = 50, E_AFNOSUPPORT = 47,
       L_BADF = 9, L_AGAIN = 11, L_INVAL = 22, L_NOTCONN = 107, L_CONNREFUSED = 111, L_ISCONN = 106, L_PIPE = 32, L_NETDOWN = 100, L_AFNOSUPPORT = 97 };
typedef struct { uint8_t len, family; uint16_t port; uint32_t addr; uint8_t zero[8]; } sk_addr_in;   // BSD sockaddr_in: port and address in network order
typedef struct { int32_t flags, family, socktype, protocol; uint32_t addrlen; char *canonname; sk_addr_in *addr; void *next; } sk_addrinfo;   // BSD addrinfo (the length before the name)
typedef struct { void *name; uint32_t namelen; struct { void *base; uint64_t len; } *iov; int32_t iovlen; void *control; uint32_t controllen; int32_t flags; } sk_msghdr;
#define LOOPBACK 0x0100007fu   // 127.0.0.1 in network order (little-endian host)

enum { ONL_OPTS = 12 };
// opt: what SetSockOpt stored, which GetSockOpt answers. gRPC sets an option (TCP_NODELAY first), reads it back and closes the socket when the value differs
static struct { int used, nonblock; uint16_t port; online_conn *c; struct { int32_t level, name, value; } opt[ONL_OPTS]; int nopt; } g_sock[ONL_FD_MAX];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   // the table; a connection has its own lock (online.c)
static const char *g_hosts[33];
// The reply table of the game's own HTTP service (DM_ONLINE_HTTP=<tsv>, made by dm/tools/online_http_table.py: type, cookie or -, body). Kept for the process's life
static void http_table(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { LOG("[RED] online services: no HTTP reply table at %s (DM_ONLINE_HTTP): every request of the game's own service gets 404", path); return; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)len + 1); len = (long)fread(text, 1, (size_t)len, f); text[len] = 0; fclose(f);
    int cap = 1; for (char *p = text; *p; p++) cap += *p == '\n';
    online_http_reply *rows = calloc((size_t)cap + 1, sizeof *rows); int n = 0;
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n'); if (next) *next++ = 0;
        char *cr = strchr(line, '\r'); if (cr) *cr = 0;
        if (*line == '#' || !*line) continue;
        char *cookie = strchr(line, '\t'), *body = cookie ? strchr(cookie + 1, '\t') : NULL;
        if (!body) { LOG("[RED] online services: %s: a row without three columns (%.40s)", path, line); continue; }
        *cookie++ = 0; *body++ = 0;
        rows[n++] = (online_http_reply){line, body, strcmp(cookie, "-") ? cookie : NULL};
    }
    online_http_replies(rows);
    LOG("[online] HTTP reply table %s: %d rows", path, n);
}
static void onl_log(const char *line) { LOG("[online] %s", line); }

static int g_onl_on;
static void dm_onl_start(void);
int dm_onl_on(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, dm_onl_start); return g_onl_on; }
static void dm_onl_start(void) {
    const char *e = getenv("DM_ONLINE_HOSTS");
    if (!e || !*e) return;
    static char names[1024]; snprintf(names, sizeof names, "%s", e);
    int n = 0;
    for (char *t = strtok(names, ", "); t && n < 32; t = strtok(NULL, ", ")) g_hosts[n++] = t;
    if (!n) return;
    // The CA and the server key: a folder of the host's own (DM_ONLINE_DIR), else one in the temporary storage; never in the save folder
    char dir[1024]; const char *d = getenv("DM_ONLINE_DIR"), *tmp = getenv("DM_TEMP");
    if (d && *d) snprintf(dir, sizeof dir, "%s", d); else snprintf(dir, sizeof dir, "%s/online", tmp && *tmp ? tmp : ".");
    mkdir(dir, 0700);
    if (online_start(dir, g_hosts, onl_log)) { LOG("[RED] online services: not started (DM_ONLINE_HOSTS=%s); the game stays offline", e); return; }
    LOG("[online] services inside the host for %d names (DM_ONLINE_HOSTS=%s): their connections end in this process, nothing is sent; state in %s", n, e, dir);
    const char *table = getenv("DM_ONLINE_HTTP"); if (table && *table) http_table(table);
    g_onl_on = 1;
}
const uint8_t *dm_onl_ca(size_t *len) { return dm_onl_on() ? online_ca_der(len) : NULL; }

static int slot(int fd) { int i = fd - ONL_FD_BASE; return i >= 0 && i < ONL_FD_MAX && g_sock[i].used ? i : -1; }
int dm_onl_fd(int fd) { if (fd < ONL_FD_BASE || fd >= ONL_FD_BASE + ONL_FD_MAX || !g_onl_on) return 0; pthread_mutex_lock(&g_mu); int r = slot(fd) >= 0; pthread_mutex_unlock(&g_mu); return r; }
static online_conn *conn_of(int fd, int *nonblock) {   // NULL: not a socket of ours, or not connected
    pthread_mutex_lock(&g_mu); int i = slot(fd); online_conn *c = i >= 0 ? g_sock[i].c : NULL; if (i >= 0 && nonblock) *nonblock = g_sock[i].nonblock; pthread_mutex_unlock(&g_mu);
    return c;
}
int dm_onl_socket(int family, int type, int proto) {
    (void)proto;
    if (family != SK_AF_INET || (type & 0xf) != SK_SOCK_STREAM) { NET_TRACE("socket::Socket(%d, %d, %d) -> fail (the in-host services take TCP over IPv4 only)", family, type, proto); return dm_sk_err(family != SK_AF_INET ? E_AFNOSUPPORT : E_NETDOWN, family != SK_AF_INET ? L_AFNOSUPPORT : L_NETDOWN); }
    pthread_mutex_lock(&g_mu); int i = 0;
    while (i < ONL_FD_MAX && g_sock[i].used) i++;
    if (i < ONL_FD_MAX) { g_sock[i].used = 1; g_sock[i].nonblock = 0; g_sock[i].c = NULL; g_sock[i].port = 0; g_sock[i].nopt = 0; }
    pthread_mutex_unlock(&g_mu);
    if (i == ONL_FD_MAX) { LOG("[RED] online services: more than %d sockets open", ONL_FD_MAX); return dm_sk_err(24, 24); }   // EMFILE
    NET_TRACE("socket::Socket(TCP) -> %d (in-host services)", ONL_FD_BASE + i);
    return ONL_FD_BASE + i;
}
int dm_onl_connect(int fd, const void *addr, unsigned len) {
    const sk_addr_in *a = addr;
    if (!a || len < 8 || a->family != SK_AF_INET) return dm_sk_err(E_INVAL, L_INVAL);
    // Only the address the service names resolve to; the port is the service's (443) and not looked at: one server answers every name
    if (a->addr != LOOPBACK) { LOG("[online] connect to %u.%u.%u.%u:%u refused: not one of the services' names (nothing is sent outside)", a->addr & 255, a->addr >> 8 & 255, a->addr >> 16 & 255, a->addr >> 24, (unsigned)__builtin_bswap16(a->port)); return dm_sk_err(E_CONNREFUSED, L_CONNREFUSED); }
    online_conn *c = online_open();
    if (!c) return dm_sk_err(E_CONNREFUSED, L_CONNREFUSED);
    pthread_mutex_lock(&g_mu); int i = slot(fd), r = i < 0 ? -2 : g_sock[i].c ? -1 : 0;
    if (!r) { g_sock[i].c = c; g_sock[i].port = a->port; }
    pthread_mutex_unlock(&g_mu);
    if (r) { online_close(c); return r == -2 ? dm_sk_err(E_BADF, L_BADF) : dm_sk_err(E_ISCONN, L_ISCONN); }
    NET_TRACE("socket::Connect(%d, 127.0.0.1:%u) -> connected (in-host services)", fd, (unsigned)__builtin_bswap16(a->port));
    return 0;   // connected at once, also on a nonblocking socket (as a loopback connect may be)
}
long dm_onl_send(int fd, const void *buf, unsigned long n, int flags) {
    (void)flags; online_conn *c = conn_of(fd, NULL);
    if (!c) return dm_sk_err(E_NOTCONN, L_NOTCONN);
    long r = online_put(c, buf, n);
    return r < 0 ? dm_sk_err(E_PIPE, L_PIPE) : r;
}
long dm_onl_recv(int fd, void *buf, unsigned long n, int flags) {
    int nonblock = 0; online_conn *c = conn_of(fd, &nonblock);
    if (!c) return dm_sk_err(E_NOTCONN, L_NOTCONN);
    for (;;) {
        online_poll(c, now_ns() / 1000000);
        long r = flags & SK_MSG_PEEK ? online_peek(c, buf, n) : online_get(c, buf, n);
        if (r > 0) return r;
        if (r < 0) return 0;   // the server closed: end of stream
        if (nonblock || (flags & SK_MSG_DONTWAIT)) return dm_sk_err(E_AGAIN, L_AGAIN);
        dm_guest_sleep_ns(2000000);
        if (!conn_of(fd, NULL)) return dm_sk_err(E_BADF, L_BADF);   // closed by another thread while this one waited
    }
}
int dm_onl_plain(int fd, const char *host) { online_conn *c = conn_of(fd, NULL); return c ? online_plain(c, host) : -1; }
long dm_onl_sendmsg(int fd, const void *msg, int flags) {
    const sk_msghdr *m = msg; long total = 0;
    for (int i = 0; m && i < m->iovlen; i++) {
        if (!m->iov[i].len) continue;
        long r = dm_onl_send(fd, m->iov[i].base, m->iov[i].len, flags);
        if (r < 0) return total ? total : r;
        total += r;
    }
    NET_TRACE("socket::SendMsg(%d, %d vectors) -> %ld", fd, m ? m->iovlen : 0, total);
    return total;
}
long dm_onl_recvmsg(int fd, void *msg, int flags) {
    sk_msghdr *m = msg; long total = 0;
    if (!m) return dm_sk_err(E_INVAL, L_INVAL);
    m->controllen = 0; m->flags = 0;
    for (int i = 0; i < m->iovlen; i++) {
        if (!m->iov[i].len) continue;
        long r = dm_onl_recv(fd, m->iov[i].base, m->iov[i].len, total ? flags | SK_MSG_DONTWAIT : flags);   // after the first bytes: only what is there
        if (r <= 0) { NET_TRACE("socket::RecvMsg(%d, %d vectors, flags %#x) -> %ld", fd, m->iovlen, flags, total ? total : r); return total ? total : r; }
        total += r;
        if ((uint64_t)r < m->iov[i].len) break;
    }
    NET_TRACE("socket::RecvMsg(%d, %d vectors, flags %#x) -> %ld", fd, m->iovlen, flags, total);
    return total;
}
int dm_onl_close(int fd) {
    pthread_mutex_lock(&g_mu); int i = slot(fd); online_conn *c = i >= 0 ? g_sock[i].c : NULL; if (i >= 0) { g_sock[i].used = 0; g_sock[i].c = NULL; } pthread_mutex_unlock(&g_mu);
    if (i < 0) return dm_sk_err(E_BADF, L_BADF);
    // A reader in dm_onl_recv holds no reference: it sleeps outside every lock and looks the descriptor up again, so the connection goes once it has had the time to see that
    if (c) { dm_guest_sleep_ns(3000000); online_close(c); }
    return 0;
}
int dm_onl_shutdown(int fd, int how) { (void)how; return conn_of(fd, NULL) ? 0 : dm_sk_err(E_NOTCONN, L_NOTCONN); }
int dm_onl_fcntl(int fd, int cmd, long arg) {
    pthread_mutex_lock(&g_mu); int i = slot(fd), r = 0;
    if (i < 0) r = -2;
    // O_NONBLOCK in either numbering: Horizon's (4) and the C library's (0x800), which gRPC in The Duskbloods Network Test passes to nnsocketFcntl. Kept as given, so
    // F_GETFL answers the bit the caller set; a read on a socket taken as blocking never returned, with the handshake's next bytes never asked for
    else if (cmd == SK_F_GETFL) r = g_sock[i].nonblock;
    else if (cmd == SK_F_SETFL) g_sock[i].nonblock = (int)(arg & (SK_O_NONBLOCK | LIBC_O_NONBLOCK));
    else if (cmd != SK_F_GETFD && cmd != SK_F_SETFD) r = -3;
    pthread_mutex_unlock(&g_mu);
    NET_TRACE("socket::Fcntl(%d, %d, %#lx) -> %d", fd, cmd, arg, r);
    return r == -2 ? dm_sk_err(E_BADF, L_BADF) : r == -3 ? dm_sk_err(E_INVAL, L_INVAL) : r;
}
int dm_onl_setsockopt(int fd, int level, int opt, const void *value, unsigned len) {
    int32_t v = 0; if (value && len) memcpy(&v, value, len < 4 ? len : 4);
    pthread_mutex_lock(&g_mu); int i = slot(fd), k = 0, full = 0;
    if (i >= 0) {
        while (k < g_sock[i].nopt && (g_sock[i].opt[k].level != level || g_sock[i].opt[k].name != opt)) k++;
        if (k < ONL_OPTS) { g_sock[i].opt[k].level = level; g_sock[i].opt[k].name = opt; g_sock[i].opt[k].value = v; if (k == g_sock[i].nopt) g_sock[i].nopt++; } else full = 1;
    }
    pthread_mutex_unlock(&g_mu);
    if (full) { static int once; if (!once++) LOG("[RED] online services: more than %d socket options set on one socket; the rest read back as 0", ONL_OPTS); }
    return i < 0 ? dm_sk_err(E_BADF, L_BADF) : 0;   // the options change nothing here (no host socket): they are kept for GetSockOpt
}
int dm_onl_getsockopt(int fd, int level, int opt, void *value, unsigned *len) {
    pthread_mutex_lock(&g_mu); int i = slot(fd); int32_t v = level == SK_SOL_SOCKET && opt == SK_SO_TYPE ? SK_SOCK_STREAM : 0;   // SO_ERROR: none; an option never set reads 0
    for (int k = 0; i >= 0 && k < g_sock[i].nopt; k++) if (g_sock[i].opt[k].level == level && g_sock[i].opt[k].name == opt) v = g_sock[i].opt[k].value;
    pthread_mutex_unlock(&g_mu);
    if (i < 0) return dm_sk_err(E_BADF, L_BADF);
    if (value && len && *len >= 4) { *(int32_t *)value = v; *len = 4; }
    return 0;
}
int dm_onl_name(int fd, int peer, void *addr, unsigned *len) {
    pthread_mutex_lock(&g_mu); int i = slot(fd); uint16_t port = i >= 0 ? g_sock[i].port : 0; int connected = i >= 0 && g_sock[i].c; pthread_mutex_unlock(&g_mu);
    if (i < 0) return dm_sk_err(E_BADF, L_BADF);
    if (peer && !connected) return dm_sk_err(E_NOTCONN, L_NOTCONN);
    sk_addr_in a = {.len = sizeof a, .family = SK_AF_INET, .port = peer ? port : (uint16_t)__builtin_bswap16((uint16_t)(49152 + fd - ONL_FD_BASE)), .addr = LOOPBACK};
    if (addr && len) { unsigned n = *len < sizeof a ? *len : (unsigned)sizeof a; memcpy(addr, &a, n); *len = sizeof a; }
    return 0;
}
int dm_onl_revents(int fd, int events) {
    online_conn *c = conn_of(fd, NULL);
    if (!c) return dm_onl_fd(fd) ? 0 : 0x20;   // not connected: nothing to report; not a socket: POLLNVAL
    int st = online_poll(c, now_ns() / 1000000);
    return (events & (SK_POLLIN | SK_POLLRDNORM) & (st & 1 ? ~0 : 0)) | (events & SK_POLLOUT) | (st & 2 ? SK_POLLHUP : 0);
}

// ---- name resolution ----
#define AI_MAX 32
static void *g_ai[AI_MAX];
int dm_onl_getaddrinfo(const char *host, const char *service, void **res) {
    if (!dm_onl_on() || !host || !online_serves(host)) return -1;
    struct { sk_addrinfo ai; sk_addr_in addr; } *r = calloc(1, sizeof *r);
    r->addr = (sk_addr_in){.len = sizeof r->addr, .family = SK_AF_INET, .port = (uint16_t)__builtin_bswap16((uint16_t)(service ? atoi(service) : 0)), .addr = LOOPBACK};
    r->ai = (sk_addrinfo){.family = SK_AF_INET, .socktype = SK_SOCK_STREAM, .protocol = 6, .addrlen = sizeof r->addr, .addr = &r->addr};
    pthread_mutex_lock(&g_mu); int i = 0; while (i < AI_MAX && g_ai[i]) i++; if (i < AI_MAX) g_ai[i] = r; pthread_mutex_unlock(&g_mu);   // past the table: never freed (a leak of 64 bytes, said once)
    if (i == AI_MAX) { static int once; if (!once++) LOG("[RED] online services: more than %d name lookups not freed", AI_MAX); }
    NET_TRACE("socket::GetAddrInfo \"%.80s\" \"%.20s\" -> 127.0.0.1 (in-host services)", host, service ? service : "(null)");
    if (res) *res = r;
    return 0;
}
int dm_onl_freeaddrinfo(void *ai) {
    if (!ai) return 0;
    pthread_mutex_lock(&g_mu); int i = 0; while (i < AI_MAX && g_ai[i] != ai) i++; if (i < AI_MAX) g_ai[i] = NULL; pthread_mutex_unlock(&g_mu);
    if (i < AI_MAX) free(ai);
    return i < AI_MAX;
}

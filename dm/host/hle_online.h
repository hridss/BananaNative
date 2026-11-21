// The game's side of the in-host online services (dm/host/online, docs/reference/services.md "Online services answered inside the host"): name resolution and
// TCP sockets for the service names of DM_ONLINE_HOSTS, answered inside the process. Unset or empty, dm_onl_on() is 0 and every hook keeps its offline answer.
// The socket hooks (hle_net.c, hle_offline.c) ask dm_onl_fd first, as they ask for the co-op module's descriptors.
#pragma once
#include <stddef.h>
#include <stdint.h>

#if NX_ONLINE   // the host (dm/host/Makefile, android/host/CMakeLists.txt)
int dm_onl_on(void);                                          // the services run (started at the first call, when DM_ONLINE_HOSTS names any)
int dm_onl_fd(int fd);                                        // fd is one of the services' sockets
int dm_onl_socket(int family, int type, int proto);           // a TCP socket → its descriptor; any other kind → -1 with the offline error
int dm_onl_connect(int fd, const void *addr, unsigned len);
long dm_onl_send(int fd, const void *buf, unsigned long n, int flags);
long dm_onl_recv(int fd, void *buf, unsigned long n, int flags);
long dm_onl_sendmsg(int fd, const void *msg, int flags);      // BSD msghdr: the vectors gathered / filled
long dm_onl_recvmsg(int fd, void *msg, int flags);
int dm_onl_close(int fd);
int dm_onl_shutdown(int fd, int how);
int dm_onl_fcntl(int fd, int cmd, long arg);
int dm_onl_plain(int fd, const char *host);                   // the host's ssl service took this connection's TLS: its bytes are the application's from here on (online_plain)
int dm_onl_setsockopt(int fd, int level, int opt, const void *value, unsigned len);   // kept for GetSockOpt (a client that reads an option back)
int dm_onl_getsockopt(int fd, int level, int opt, void *value, unsigned *len);
int dm_onl_name(int fd, int peer, void *addr, unsigned *len); // GetSockName / GetPeerName
int dm_onl_revents(int fd, int events);                       // the poll events the socket has now
// Name resolution: -1 when the name is not one of the services' (the caller gives its own answer), else the resolver's result (0, *res a list to free with dm_onl_freeaddrinfo)
int dm_onl_getaddrinfo(const char *host, const char *service, void **res);
int dm_onl_freeaddrinfo(void *ai);                            // 1 when ai was one of dm_onl_getaddrinfo's
const uint8_t *dm_onl_ca(size_t *len);                        // the CA certificate the game has to trust (DER); NULL when the services are off
#else
// Built without the services (NX_ONLINE unset: the unit tests that take hle_net.c or hle_offline.c alone): off, so none of the calls below is ever reached with a
// socket of the services; they only let the hooks compile, as hle_coop.h's do for the co-op module.
static inline int dm_onl_on(void) { return 0; }
static inline int dm_onl_fd(int fd) { (void)fd; return 0; }
static inline int dm_onl_socket(int family, int type, int proto) { (void)family; (void)type; (void)proto; return -1; }
static inline int dm_onl_connect(int fd, const void *addr, unsigned len) { (void)fd; (void)addr; (void)len; return -1; }
static inline long dm_onl_send(int fd, const void *buf, unsigned long n, int flags) { (void)fd; (void)buf; (void)n; (void)flags; return -1; }
static inline long dm_onl_recv(int fd, void *buf, unsigned long n, int flags) { (void)fd; (void)buf; (void)n; (void)flags; return -1; }
static inline long dm_onl_sendmsg(int fd, const void *msg, int flags) { (void)fd; (void)msg; (void)flags; return -1; }
static inline long dm_onl_recvmsg(int fd, void *msg, int flags) { (void)fd; (void)msg; (void)flags; return -1; }
static inline int dm_onl_close(int fd) { (void)fd; return -1; }
static inline int dm_onl_shutdown(int fd, int how) { (void)fd; (void)how; return -1; }
static inline int dm_onl_fcntl(int fd, int cmd, long arg) { (void)fd; (void)cmd; (void)arg; return -1; }
static inline int dm_onl_plain(int fd, const char *host) { (void)fd; (void)host; return -1; }
static inline int dm_onl_setsockopt(int fd, int level, int opt, const void *value, unsigned len) { (void)fd; (void)level; (void)opt; (void)value; (void)len; return -1; }
static inline int dm_onl_getsockopt(int fd, int level, int opt, void *value, unsigned *len) { (void)fd; (void)level; (void)opt; (void)value; (void)len; return -1; }
static inline int dm_onl_name(int fd, int peer, void *addr, unsigned *len) { (void)fd; (void)peer; (void)addr; (void)len; return -1; }
static inline int dm_onl_revents(int fd, int events) { (void)fd; (void)events; return 0; }
static inline int dm_onl_getaddrinfo(const char *host, const char *service, void **res) { (void)host; (void)service; (void)res; return -1; }
static inline int dm_onl_freeaddrinfo(void *ai) { (void)ai; return 0; }
static inline const uint8_t *dm_onl_ca(size_t *len) { (void)len; return NULL; }
#endif

// In-host online services: the answers a game's online check needs, given inside the host process, so that no packet leaves the device (docs/reference/services.md
// "Online services answered inside the host"). The game keeps its own network stack (its TLS, HTTP and gRPC libraries); the host's socket hooks hand the bytes of a
// TCP connection to one of the service names to an online_conn, which is the server's side of that connection:
//   TLS (mbedTLS, a certificate from a CA made on the device, which the host also gives out as the system's built-in certificate) →
//     ALPN h2: gRPC (nghttp2), the NPLN platform's methods (online_grpc.c);  otherwise HTTP/1.1: a game service's JSON requests (online_http.c)
// A connection has no thread of its own: it works inside the calls that feed and drain it (online_put / online_get / online_poll), under its own lock.
// This header has no host dependency, so tests and the test server (tests/online_serve.c) build the services alone.
#pragma once
#include <stddef.h>
#include <stdint.h>

// The names the services answer for: a name is served when it is one of these or ends with '.' + one of these.
// NULL-terminated; set before online_start (the host: the profile's online_hosts).
int online_start(const char *dir, const char *const *hosts, void (*log)(const char *line));   // dir: where the CA and the server key are kept (made at the first start). 0 = ready
int online_serves(const char *name);                          // whether a host name is one of the services'
const uint8_t *online_ca_der(size_t *len);                    // the CA certificate (DER) the game has to trust; NULL before online_start

typedef struct online_conn online_conn;
online_conn *online_open(void);                               // a new connection (the game connected); NULL when the services are not started
void online_close(online_conn *c);
long online_put(online_conn *c, const void *data, size_t n);  // bytes the game sent. Returns n, or -1 when the connection is over
long online_get(online_conn *c, void *data, size_t n);        // bytes for the game: > 0 that many, 0 none now, -1 the server closed and all was read
int online_poll(online_conn *c, uint64_t now_ms);             // runs timers; bit 0: bytes to read (or closed), bit 1: closed by the server
long online_peek(online_conn *c, void *data, size_t n);       // as online_get, the bytes left in place
// A client that hands its TLS to the system's ssl service (the host's own hooks) has no TLS library of its own to speak to: from here on the connection's bytes are the
// application's (HTTP/1.1), for the host name the client set. Before any byte was sent. 0 = done
int online_plain(online_conn *c, const char *host);

// ---- between the layers (online.c ↔ online_http.c / online_grpc.c) ----
typedef struct online_buf { uint8_t *p; size_t n, cap; } online_buf;
void online_buf_add(online_buf *b, const void *data, size_t n);
void online_buf_drop(online_buf *b, size_t n);                // removes the first n bytes
void online_buf_free(online_buf *b);
void online_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// An application protocol on a connection: bytes in (plain, after TLS), bytes out appended to `out`. rx returns -1 to end the connection (after `out` is sent)
typedef struct online_app {
    void *state;
    int (*rx)(void *state, const uint8_t *data, size_t n, online_buf *out);
    int (*tick)(void *state, uint64_t now_ms, online_buf *out);   // may be NULL
    void (*done)(void *state);
} online_app;
int online_http_app(online_app *app, const char *sni);
int online_grpc_app(online_app *app);

// The JSON replies of a game's HTTP service, by request type: rows {"Request<X>Params", the whole reply body}, ended by a NULL type. A game without such a table has none
// (every request gets 404). Set by the host from the profile's data before the first connection; online_http.c keeps the pointer
typedef struct online_http_reply { const char *type, *body, *cookie; } online_http_reply;
void online_http_replies(const online_http_reply *rows);

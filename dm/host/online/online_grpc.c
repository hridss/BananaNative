// In-host online services (online.h): Nintendo's NPLN platform as a game's SDK calls it, gRPC over HTTP/2 (nghttp2). Every method is accepted:
//   · a unary call gets its reply (below), or an empty message, with status OK;
//   · a server stream (Subscribe…, Track…) gets one message at once, an empty one being a valid snapshot, and then stays open and silent: the client reads it for as long as
//     it is signed in. A game session creation ticket's stream ends after its message, which is what its client reads to;
//   · PresenceService/KeepAlive (both ways) gets an acknowledgement for each presence change (request field 1), and one whenever 5 s pass with nothing sent while the client
//     still sends: the client takes it as "the server has the presence" and puts its idle deadline off by the 30 s in it; left without, it cancels the stream and the game
//     reports a communication error. A heartbeat (request field 2) gets no answer of its own: answered, client and server ping each other without end;
//   · a stream both ways ends when the client ends its side.
// Protobuf messages are written by field number; the layouts are those the SDK's generated code reads (a resource name that is not exactly `tenants/<tenant>/users/<user>`
// makes it abort). One player: the device's own, user u-0000000000000001. Matchmaking, game sessions and peer connections are not served (their calls get empty replies).
#include "online.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <nghttp2/nghttp2.h>

#define USER "u-0000000000000001"
#define KEEPALIVE_BEAT_MS 5000
enum { UNARY, SERVER_STREAM, BIDI };
typedef struct stream {
    int32_t id; int kind, keepalive, ends_after_first;
    char path[192], tenant[96];
    online_buf req, out;        // the request's bytes not yet a whole message; reply messages (framed) not yet sent
    int responded, half_closed, finish;
    uint64_t last_tx_ms;
    struct stream *next;
} stream;
typedef struct { nghttp2_session *s; stream *streams; uint64_t now_ms; } grpc_t;

// ---- protobuf ----
static void pb_varint(online_buf *b, uint64_t v) { uint8_t t[10]; int n = 0; do { t[n] = (uint8_t)((v & 0x7f) | (v > 0x7f ? 0x80 : 0)); v >>= 7; } while (t[n++] & 0x80); online_buf_add(b, t, (size_t)n); }
static void pb_int(online_buf *b, int field, uint64_t v) { pb_varint(b, (uint64_t)field << 3); pb_varint(b, v); }   // written also when 0, as the reference server does
static void pb_bytes(online_buf *b, int field, const void *p, size_t n) { pb_varint(b, (uint64_t)field << 3 | 2); pb_varint(b, n); online_buf_add(b, p, n); }
static void pb_str(online_buf *b, int field, const char *s) { pb_bytes(b, field, s, strlen(s)); }
static void pb_msg(online_buf *b, int field, online_buf *m) { pb_bytes(b, field, m->p, m->n); online_buf_free(m); }
static void pb_seconds(online_buf *b, int field, uint64_t s) { online_buf m = {0}; pb_int(&m, 1, s); pb_msg(b, field, &m); }   // a Timestamp or a Duration: seconds only
// The first length-delimited field `field` of a message → its bytes; 0 when there is none (or the message is damaged)
static int pb_find(const uint8_t *p, size_t n, int field, const uint8_t **out, size_t *len) {
    for (size_t i = 0; i < n;) {
        uint64_t key = 0, v = 0; int sh = 0;
        do { if (i >= n || sh > 63) return 0; key |= (uint64_t)(p[i] & 0x7f) << sh; sh += 7; } while (p[i++] & 0x80);
        switch (key & 7) {
        case 0: do { if (i >= n) return 0; } while (p[i++] & 0x80); break;
        case 1: i += 8; break;
        case 5: i += 4; break;
        case 2:
            sh = 0; do { if (i >= n || sh > 63) return 0; v |= (uint64_t)(p[i] & 0x7f) << sh; sh += 7; } while (p[i++] & 0x80);
            if (v > n - i) return 0;
            if ((int)(key >> 3) == field) { *out = p + i; *len = (size_t)v; return 1; }
            i += (size_t)v; break;
        default: return 0;
        }
    }
    return 0;
}
static void b64url(online_buf *b, const char *s) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)(uint8_t)s[i] << 16 | (i + 1 < n ? (uint32_t)(uint8_t)s[i + 1] << 8 : 0) | (i + 2 < n ? (uint8_t)s[i + 2] : 0);
        char o[4] = {T[v >> 18], T[v >> 12 & 63], T[v >> 6 & 63], T[v & 63]};
        online_buf_add(b, o, i + 2 < n ? 4 : i + 1 < n ? 3 : 2);   // no padding
    }
}
// The access token: a JWT with algorithm "none" and no signature. The client only keeps it and sends it back
static void token(online_buf *b) {
    b64url(b, "{\"alg\": \"none\"}"); online_buf_add(b, ".", 1);
    b64url(b, "{\"sub\": \"" USER "\", \"exp\": 4102444800}"); online_buf_add(b, ".", 1);
}

// ---- the replies ----
// IssuePrearrangedUserToken, the sign-in: request field 1 is the parent, `tenants/<tenant>` (else the npln-tenant-id header names the tenant).
// → {1: User {1 name, 2 "E", 3 and 4 Timestamps}, 2: UserToken {1 name, 2 access token, 3 refresh token, 4 Duration 3600 s}}
static void reply_login(stream *st, const uint8_t *req, size_t n, online_buf *out) {
    const uint8_t *p; size_t len; char name[224];
    if (pb_find(req, n, 1, &p, &len) && len > 8 && len < sizeof st->tenant + 8 && !memcmp(p, "tenants/", 8)) { memcpy(st->tenant, p + 8, len - 8); st->tenant[len - 8] = 0; }
    snprintf(name, sizeof name, "tenants/%s/users/" USER, st->tenant);
    uint64_t now = (uint64_t)time(NULL);
    online_buf user = {0}, tok = {0}, jwt = {0};
    pb_str(&user, 1, name); pb_str(&user, 2, "E"); pb_seconds(&user, 3, now); pb_seconds(&user, 4, now);
    token(&jwt);
    pb_str(&tok, 1, name); pb_bytes(&tok, 2, jwt.p, jwt.n); pb_str(&tok, 3, "refresh-" USER); pb_seconds(&tok, 4, 3600);
    online_buf_free(&jwt);
    pb_msg(out, 1, &user); pb_msg(out, 2, &tok);
    online_logf("NPLN sign-in for tenant %s as " USER, *st->tenant ? st->tenant : "(none named)");
}
// KeepAlive's acknowledgement: {1: {1: Duration 30 s, 2: Duration 30 s}}
static void reply_keepalive(online_buf *out) { online_buf m = {0}; pb_seconds(&m, 1, 30); pb_seconds(&m, 2, 30); pb_msg(out, 1, &m); }

static const char *method_of(const char *path) { const char *m = strrchr(path, '/'); return m ? m + 1 : path; }
static int is(const char *method, const char *const *list) { for (; *list; list++) if (!strcmp(method, *list)) return 1; return 0; }
static void classify(stream *st) {
    static const char *const server[] = {"SubscribeFriendUsers", "SubscribePresences", "SubscribeMaintenanceSchedules", "TrackMatchmakingTicket", "TrackGameSessionCreationTicket", NULL};
    static const char *const bidi[] = {"KeepAlive", "KeepUserSession", NULL};
    const char *m = method_of(st->path);
    st->kind = is(m, server) ? SERVER_STREAM : is(m, bidi) ? BIDI : UNARY;
    st->keepalive = !strcmp(m, "KeepAlive");
    st->ends_after_first = !strcmp(m, "TrackGameSessionCreationTicket");
}

// ---- HTTP/2 ----
static nghttp2_ssize on_read(nghttp2_session *s, int32_t id, uint8_t *buf, size_t cap, uint32_t *flags, nghttp2_data_source *src, void *user) {
    (void)user; stream *st = src->ptr;
    size_t n = st->out.n < cap ? st->out.n : cap;
    if (n) { memcpy(buf, st->out.p, n); online_buf_drop(&st->out, n); }
    if (!st->out.n && st->finish) {   // the last of the messages: the call's status follows as trailers
        static const nghttp2_nv trailer[] = {{(uint8_t *)"grpc-status", (uint8_t *)"0", 11, 1, NGHTTP2_NV_FLAG_NONE}};
        *flags |= NGHTTP2_DATA_FLAG_EOF | NGHTTP2_DATA_FLAG_NO_END_STREAM;
        nghttp2_submit_trailer(s, id, trailer, 1);
    } else if (!n) return NGHTTP2_ERR_DEFERRED;
    return (nghttp2_ssize)n;
}
// One reply message onto the stream (the response headers first, when it is the first); finish = the call ends after what is queued
static void send_msg(grpc_t *g, stream *st, const online_buf *msg, int finish) {
    if (msg) {
        uint8_t head[5] = {0, (uint8_t)(msg->n >> 24), (uint8_t)(msg->n >> 16), (uint8_t)(msg->n >> 8), (uint8_t)msg->n};   // not compressed, the length big-endian
        online_buf_add(&st->out, head, 5); online_buf_add(&st->out, msg->p, msg->n);
        st->last_tx_ms = g->now_ms;
    }
    if (finish) st->finish = 1;
    if (!st->responded) {
        static const nghttp2_nv head[] = {{(uint8_t *)":status", (uint8_t *)"200", 7, 3, NGHTTP2_NV_FLAG_NONE}, {(uint8_t *)"content-type", (uint8_t *)"application/grpc", 12, 16, NGHTTP2_NV_FLAG_NONE}};
        nghttp2_data_provider2 prd = {.source = {.ptr = st}, .read_callback = on_read};
        nghttp2_submit_response2(g->s, st->id, head, 2, &prd);
        st->responded = 1;
    } else nghttp2_session_resume_data(g->s, st->id);
}
static void on_message(grpc_t *g, stream *st, const uint8_t *msg, size_t n) {
    online_buf out = {0};
    if (st->kind == BIDI) {
        const uint8_t *p; size_t len;
        if (st->keepalive && pb_find(msg, n, 1, &p, &len)) { reply_keepalive(&out); send_msg(g, st, &out, 0); }
    } else if (!st->responded) {   // a unary call's or a server stream's one request
        const char *m = method_of(st->path);
        if (!strcmp(m, "IssuePrearrangedUserToken")) reply_login(st, msg, n, &out);
        online_logf("NPLN %s: %s (%zu bytes) -> %zu bytes%s", st->kind == UNARY ? "call" : "stream", st->path, n, out.n, st->kind == UNARY ? "" : st->ends_after_first ? ", ended" : ", kept open");
        send_msg(g, st, &out, st->kind == UNARY || st->ends_after_first);
    }
    online_buf_free(&out);
}
static int on_begin_headers(nghttp2_session *s, const nghttp2_frame *f, void *user) {
    grpc_t *g = user;
    if (f->hd.type != NGHTTP2_HEADERS || f->headers.cat != NGHTTP2_HCAT_REQUEST) return 0;
    stream *st = calloc(1, sizeof *st); st->id = f->hd.stream_id; st->next = g->streams; g->streams = st; st->last_tx_ms = g->now_ms;
    nghttp2_session_set_stream_user_data(s, st->id, st);
    return 0;
}
static int on_header(nghttp2_session *s, const nghttp2_frame *f, const uint8_t *name, size_t nl, const uint8_t *value, size_t vl, uint8_t flags, void *user) {
    (void)flags; (void)user;
    stream *st = nghttp2_session_get_stream_user_data(s, f->hd.stream_id);
    if (!st || f->hd.type != NGHTTP2_HEADERS) return 0;
    if (nl == 5 && !memcmp(name, ":path", 5) && vl < sizeof st->path) { memcpy(st->path, value, vl); st->path[vl] = 0; classify(st); }
    if (nl == 14 && !memcmp(name, "npln-tenant-id", 14) && vl < sizeof st->tenant) { memcpy(st->tenant, value, vl); st->tenant[vl] = 0; }
    return 0;
}
static int on_data(nghttp2_session *s, uint8_t flags, int32_t id, const uint8_t *data, size_t n, void *user) {
    (void)flags; grpc_t *g = user; stream *st = nghttp2_session_get_stream_user_data(s, id);
    if (!st) return 0;
    online_buf_add(&st->req, data, n);
    while (st->req.n >= 5) {   // whole messages: a flag byte (compressed or not; requests are not) and the length, big-endian
        size_t len = (size_t)st->req.p[1] << 24 | (size_t)st->req.p[2] << 16 | (size_t)st->req.p[3] << 8 | st->req.p[4];
        if (len > (1u << 20)) { online_logf("[RED] NPLN %s: a request message of %zu bytes: the stream is reset", st->path, len); nghttp2_submit_rst_stream(s, NGHTTP2_FLAG_NONE, id, NGHTTP2_INTERNAL_ERROR); st->req.n = 0; break; }
        if (st->req.n < 5 + len) break;
        on_message(g, st, st->req.p + 5, len);
        online_buf_drop(&st->req, 5 + len);
    }
    return 0;
}
static int on_frame(nghttp2_session *s, const nghttp2_frame *f, void *user) {
    grpc_t *g = user;
    if (f->hd.type != NGHTTP2_HEADERS && f->hd.type != NGHTTP2_DATA) return 0;
    stream *st = nghttp2_session_get_stream_user_data(s, f->hd.stream_id);
    if (!st || st->half_closed) return 0;
    // A stream both ways is open for the server's messages from its request headers on (KeepAlive's 5 s acknowledgement does not wait for a request message)
    if (f->hd.type == NGHTTP2_HEADERS && st->kind == BIDI && !st->responded) { online_logf("NPLN stream both ways: %s", st->path); send_msg(g, st, NULL, 0); }
    if (!(f->hd.flags & NGHTTP2_FLAG_END_STREAM)) return 0;
    st->half_closed = 1;
    if (!st->responded) {   // a call that sent no message at all: answered as one with an empty request
        online_logf("NPLN %s: %s (no request message)", st->kind == UNARY ? "call" : "stream", st->path);
        online_buf none = {0}; send_msg(g, st, st->kind == BIDI ? NULL : &none, st->kind != SERVER_STREAM || st->ends_after_first);
    } else if (st->kind == BIDI) send_msg(g, st, NULL, 1);   // the client ended its side: the call ends
    return 0;
}
static int on_close(nghttp2_session *s, int32_t id, uint32_t error, void *user) {
    (void)s; (void)error; grpc_t *g = user;
    for (stream **p = &g->streams; *p; p = &(*p)->next) if ((*p)->id == id) { stream *st = *p; *p = st->next; online_buf_free(&st->req); online_buf_free(&st->out); free(st); break; }
    return 0;
}
static int flush(grpc_t *g, online_buf *out) {
    for (;;) {
        const uint8_t *p; nghttp2_ssize n = nghttp2_session_mem_send2(g->s, &p);
        if (n < 0) { online_logf("[RED] HTTP/2 send: %s", nghttp2_strerror((int)n)); return -1; }
        if (!n) break;
        online_buf_add(out, p, (size_t)n);
    }
    return nghttp2_session_want_read(g->s) || nghttp2_session_want_write(g->s) ? 0 : -1;
}
static int grpc_rx(void *state, const uint8_t *data, size_t n, online_buf *out) {
    grpc_t *g = state;
    nghttp2_ssize r = nghttp2_session_mem_recv2(g->s, data, n);
    if (r < 0) { online_logf("[RED] HTTP/2 receive: %s", nghttp2_strerror((int)r)); return -1; }
    return flush(g, out);
}
static int grpc_tick(void *state, uint64_t now_ms, online_buf *out) {
    grpc_t *g = state; g->now_ms = now_ms;
    for (stream *st = g->streams; st; st = st->next)
        if (st->keepalive && st->responded && !st->half_closed && now_ms - st->last_tx_ms >= KEEPALIVE_BEAT_MS) { online_buf m = {0}; reply_keepalive(&m); send_msg(g, st, &m, 0); online_buf_free(&m); }
    return flush(g, out);
}
static void grpc_done(void *state) {
    grpc_t *g = state; nghttp2_session_del(g->s);
    for (stream *st = g->streams, *nx; st; st = nx) { nx = st->next; online_buf_free(&st->req); online_buf_free(&st->out); free(st); }
    free(g);
}
int online_grpc_app(online_app *app) {
    grpc_t *g = calloc(1, sizeof *g); nghttp2_session_callbacks *cb;
    if (nghttp2_session_callbacks_new(&cb)) { free(g); return -1; }
    nghttp2_session_callbacks_set_on_begin_headers_callback(cb, on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(cb, on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, on_data);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cb, on_frame);
    nghttp2_session_callbacks_set_on_stream_close_callback(cb, on_close);
    int r = nghttp2_session_server_new(&g->s, cb, g);
    nghttp2_session_callbacks_del(cb);
    if (r) { online_logf("[RED] HTTP/2 session: %s", nghttp2_strerror(r)); free(g); return -1; }
    // The client's HTTP/2 keepalive pings are answered by nghttp2 as they come, however often: a server that counts them and says GOAWAY takes every open stream with it
    static const nghttp2_settings_entry iv[] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}};
    nghttp2_submit_settings(g->s, NGHTTP2_FLAG_NONE, iv, 1);
    *app = (online_app){.state = g, .rx = grpc_rx, .tick = grpc_tick, .done = grpc_done};
    return 0;
}

// In-host online services (online.h): a game's own HTTP service. One request a connection, as the games' clients send them (HTTP/1.1, a JSON body
// {"type": "Request<X>Params", "value": {...}}, Content-Length, no chunks), answered from the table the profile gives and closed.
// A table row's type is one of:  "Request<X>Params" (the body's type),  "/<path>" (a request path, which goes before the type),  "@<prefix>" (any request to a
// host whose name starts with the prefix, when no other row answers).  Anything else gets 404, as a server that does not know the request.
#include "online.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define REQ_MAX (1 << 20)   // a request larger than this is not one of a game's API calls
static const online_http_reply *g_rows;
void online_http_replies(const online_http_reply *rows) { g_rows = rows; }

typedef struct { online_buf req; char sni[128]; int answered; } http_t;

static const online_http_reply *row(const char *type) {
    for (const online_http_reply *r = g_rows; r && r->type; r++) if (!strcmp(r->type, type)) return r;
    return NULL;
}
// The string value of the top-level "type" key of a JSON object; "" when there is none. Enough of a reader for the envelope: the first "type" key, a string
static void json_type(const char *body, size_t n, char *out, size_t cap) {
    *out = 0;
    for (size_t i = 0; i + 6 < n; i++) {
        if (memcmp(body + i, "\"type\"", 6)) continue;
        size_t k = i + 6;
        while (k < n && (body[k] == ' ' || body[k] == '\t' || body[k] == '\r' || body[k] == '\n')) k++;
        if (k >= n || body[k] != ':') continue;
        for (k++; k < n && (body[k] == ' ' || body[k] == '\t' || body[k] == '\r' || body[k] == '\n'); k++) {}
        if (k >= n || body[k] != '"') return;
        size_t e = ++k; while (e < n && body[e] != '"' && body[e] != '\\') e++;
        if (e < n && body[e] == '"' && e - k < cap) { memcpy(out, body + k, e - k); out[e - k] = 0; }
        return;
    }
}
static void answer(online_buf *out, const online_http_reply *r) {
    char head[320]; int n;
    if (!r) n = snprintf(head, sizeof head, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    else n = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n%s%s%sConnection: close\r\n\r\n", strlen(r->body),
                      r->cookie ? "Set-Cookie: " : "", r->cookie ? r->cookie : "", r->cookie ? "\r\n" : "");
    online_buf_add(out, head, (size_t)n);
    if (r) online_buf_add(out, r->body, strlen(r->body));
}
static int http_rx(void *state, const uint8_t *data, size_t n, online_buf *out) {
    http_t *h = state;
    if (h->answered) return -1;
    online_buf_add(&h->req, data, n);
    if (h->req.n > REQ_MAX) { online_logf("[RED] HTTP request for %s larger than %d bytes: closed", h->sni, REQ_MAX); return -1; }
    char *p = (char *)h->req.p, *end = NULL;
    for (size_t i = 0; i + 4 <= h->req.n; i++) if (!memcmp(p + i, "\r\n\r\n", 4)) { end = p + i; break; }
    if (!end) return 0;
    size_t head = (size_t)(end - p) + 4, length = 0;
    for (char *l = memchr(p, '\n', head); l && l < end; l = memchr(l + 1, '\n', (size_t)(end - l - 1))) {
        if (!strncasecmp(l + 1, "content-length:", 15)) length = strtoul(l + 16, NULL, 10);
        if (!strncasecmp(l + 1, "transfer-encoding:", 18)) { online_logf("[RED] HTTP request for %s with a transfer encoding, which this server does not read: 404", h->sni); length = 0; }
    }
    if (length > REQ_MAX) return -1;
    if (h->req.n < head + length) return 0;
    char method[16] = "", path[256] = "", type[96];
    sscanf(p, "%15s %255s", method, path);
    json_type(p + head, length, type, sizeof type);
    const online_http_reply *r = row(path);
    if (!r && *type) r = row(type);
    for (const online_http_reply *k = g_rows; !r && k && k->type; k++) if (k->type[0] == '@' && !strncasecmp(h->sni, k->type + 1, strlen(k->type + 1))) r = k;
    online_logf("%s %s %s%s%s -> %s", h->sni, method, path, *type ? " " : "", type, r ? "200" : "[RED] 404 (no reply for it in the profile's table)");
    answer(out, r);
    h->answered = 1;
    return -1;   // one request a connection
}
static void http_done(void *state) { http_t *h = state; online_buf_free(&h->req); free(h); }
int online_http_app(online_app *app, const char *sni) {
    http_t *h = calloc(1, sizeof *h); snprintf(h->sni, sizeof h->sni, "%s", sni);
    *app = (online_app){.state = h, .rx = http_rx, .done = http_done};
    return 0;
}

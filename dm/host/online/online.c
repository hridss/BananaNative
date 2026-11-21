// In-host online services (online.h): the device's own CA and server key, TLS on a connection's bytes, and the choice of the application protocol by ALPN.
// The game checks the server's certificate against the system's built-in certificates and the name it connected to, so the host gives out this CA as the built-in
// certificate (hle_net.c) and each connection presents a certificate for exactly the name in its ClientHello (SNI), signed by that CA when the name first comes up:
// a wildcard covers one label only, and the service names have several below the listed ones (*.lp1.t.npln.srv.nintendo.net).
#include "online.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/oid.h>
#include <mbedtls/error.h>
#include <mbedtls/platform_util.h>
#include <psa/crypto.h>

#define LEAF_MAX 32   // distinct names a session connects to (The Duskbloods Network Test: a handful); past it the default certificate is presented
typedef struct { char name[128]; mbedtls_x509_crt crt; } leaf_t;
static struct {
    int ready;
    const char *const *hosts;
    void (*log)(const char *);
    pthread_mutex_t tls_mu, leaf_mu;
    mbedtls_pk_context ca_key, key;        // the CA's key and the one key every server certificate has
    mbedtls_x509_crt ca, chain;            // the CA; the default chain (a certificate for the listed names and their one-label wildcards)
    mbedtls_ssl_config conf;
    leaf_t leaf[LEAF_MAX]; int nleaf;
} G = {.tls_mu = PTHREAD_MUTEX_INITIALIZER, .leaf_mu = PTHREAD_MUTEX_INITIALIZER};
static const char *ALPN[] = {"h2", "http/1.1", NULL};

void online_logf(const char *fmt, ...) {
    char line[512]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    if (G.log) G.log(line); else fprintf(stderr, "[online] %s\n", line);
}
void online_buf_add(online_buf *b, const void *data, size_t n) {
    if (!n) return;
    if (b->n + n > b->cap) { size_t cap = b->cap ? b->cap : 4096; while (cap < b->n + n) cap *= 2; b->p = realloc(b->p, cap); if (!b->p) abort(); b->cap = cap; }
    memcpy(b->p + b->n, data, n); b->n += n;
}
void online_buf_drop(online_buf *b, size_t n) { if (n >= b->n) b->n = 0; else { memmove(b->p, b->p + n, b->n - n); b->n -= n; } }
void online_buf_free(online_buf *b) { free(b->p); *b = (online_buf){0}; }

// mbedTLS 4 draws its random numbers and keeps its keys in PSA, which is built without its threading layer: every mbedTLS and PSA call is made under G.tls_mu
// (connections are pumped from the guest's threads). The random generator is PSA's own, seeded from the system's entropy by psa_crypto_init
static void tls_lock(void) { pthread_mutex_lock(&G.tls_mu); }
static void tls_unlock(void) { pthread_mutex_unlock(&G.tls_mu); }
static const char *tls_err(int r) { static _Thread_local char s[96]; mbedtls_strerror(r, s, sizeof s); return s; }

// ---- the CA and the server certificates ----
static int file_get(const char *dir, const char *name, uint8_t **data, size_t *n) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    *data = malloc(len > 0 ? (size_t)len : 1); *n = len > 0 ? fread(*data, 1, (size_t)len, f) : 0; fclose(f);
    return *n == (size_t)len && len > 0 ? 0 : (free(*data), -1);
}
static int file_put(const char *dir, const char *name, const uint8_t *data, size_t n) {   // written beside, then renamed: a start that is killed leaves no half file
    char path[1024], tmp[1040]; snprintf(path, sizeof path, "%s/%s", dir, name); snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb"); if (!f) return -1;
    int ok = fwrite(data, 1, n, f) == n; ok = !fclose(f) && ok;
    return ok && !rename(tmp, path) ? 0 : -1;
}
// One certificate: a CA (issuer = itself) or a server certificate for the names in `names` (count of them; each a DNS subjectAltName). → DER in out (malloc), 0 on success
static int make_cert(mbedtls_pk_context *subject_key, const char *subject, const char *issuer, int is_ca, const char *const *names, int count, uint8_t **out, size_t *out_len) {
    mbedtls_x509write_cert w; mbedtls_x509write_crt_init(&w);
    unsigned char serial[16], *buf = malloc(8192); int r;
    mbedtls_x509_san_list *san = calloc((size_t)(count ? count : 1), sizeof *san);
    if (psa_generate_random(serial, sizeof serial) != PSA_SUCCESS) { free(buf); free(san); online_logf("[RED] certificate %s: no random serial", subject); return -1; }
    serial[0] = (serial[0] & 0x7f) | 0x40;   // positive, and no leading zero byte
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, subject_key);
    mbedtls_x509write_crt_set_issuer_key(&w, &G.ca_key);
    // Valid from a fixed past date: the guest clock is the device's, but a device set back a year should still pass
    if ((r = mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof serial)) || (r = mbedtls_x509write_crt_set_subject_name(&w, subject)) ||
        (r = mbedtls_x509write_crt_set_issuer_name(&w, issuer)) || (r = mbedtls_x509write_crt_set_validity(&w, "20240101000000", "20491231235959")) ||
        (r = mbedtls_x509write_crt_set_basic_constraints(&w, is_ca, is_ca ? 0 : -1)) || (r = mbedtls_x509write_crt_set_subject_key_identifier(&w)) ||
        (r = mbedtls_x509write_crt_set_key_usage(&w, is_ca ? MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN : MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_ENCIPHERMENT))) goto end;
    if (!is_ca) {
        mbedtls_asn1_sequence eku = {.buf = {.tag = MBEDTLS_ASN1_OID, .len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH), .p = (unsigned char *)MBEDTLS_OID_SERVER_AUTH}};
        for (int i = 0; i < count; i++) {
            san[i].node.type = MBEDTLS_X509_SAN_DNS_NAME;
            san[i].node.san.unstructured_name = (mbedtls_x509_buf){.p = (unsigned char *)names[i], .len = strlen(names[i])};
            san[i].next = i + 1 < count ? &san[i + 1] : NULL;
        }
        if ((r = mbedtls_x509write_crt_set_authority_key_identifier(&w)) || (r = mbedtls_x509write_crt_set_ext_key_usage(&w, &eku)) ||
            (count && (r = mbedtls_x509write_crt_set_subject_alternative_name(&w, san)))) goto end;
    }
    r = mbedtls_x509write_crt_der(&w, buf, 8192);   // written at the end of the buffer; the length, or an error
    if (r > 0) { *out = malloc((size_t)r); memcpy(*out, buf + 8192 - r, (size_t)r); *out_len = (size_t)r; r = 0; }
end:
    mbedtls_x509write_crt_free(&w); free(buf); free(san);
    if (r) online_logf("[RED] certificate %s: %s", subject, tls_err(r));
    return r;
}
#define CA_NAME "CN=Banana Native local services,O=Banana Native"
// A key file is the key's DER (PKCS#1 RSAPrivateKey, as PSA exports an RSA key pair; keys written by mbedTLS 3's mbedtls_pk_write_key_der read the same).
// A new key: a 2048-bit RSA key pair made in PSA, exported, saved, and read back like a saved one
static int key_load_or_make(const char *dir, const char *file, mbedtls_pk_context *key) {
    uint8_t *d; size_t n; int r;
    mbedtls_pk_init(key);
    if (!file_get(dir, file, &d, &n)) { r = mbedtls_pk_parse_key(key, d, n, NULL, 0); free(d); if (!r) return 0; mbedtls_pk_free(key); mbedtls_pk_init(key); }
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT; mbedtls_svc_key_id_t id;
    psa_set_key_type(&a, PSA_KEY_TYPE_RSA_KEY_PAIR); psa_set_key_bits(&a, 2048); psa_set_key_usage_flags(&a, PSA_KEY_USAGE_EXPORT);
    psa_status_t s = psa_generate_key(&a, &id);
    if (s != PSA_SUCCESS) return (int)s;
    unsigned char *buf = malloc(4096); size_t len = 0;
    s = psa_export_key(id, buf, 4096, &len); psa_destroy_key(id);
    r = s != PSA_SUCCESS ? (int)s : mbedtls_pk_parse_key(key, buf, len, NULL, 0);
    if (!r && file_put(dir, file, buf, len)) r = -1;
    mbedtls_platform_zeroize(buf, 4096); free(buf);
    return r ? (r < 0 ? r : -r) : 1;   // 1 = new
}
static int pki_start(const char *dir) {
    uint8_t *d = NULL; size_t n = 0; int r, fresh;
    if ((fresh = key_load_or_make(dir, "ca.key.der", &G.ca_key)) < 0) { online_logf("[RED] CA key: %s", tls_err(fresh)); return -1; }
    if ((r = key_load_or_make(dir, "server.key.der", &G.key)) < 0) { online_logf("[RED] server key: %s", tls_err(r)); return -1; }
    mbedtls_x509_crt_init(&G.ca); mbedtls_x509_crt_init(&G.chain);
    // The CA certificate stays with its key: a new one would not match what a game cached of the old
    if (fresh || file_get(dir, "ca.crt.der", &d, &n) || mbedtls_x509_crt_parse_der(&G.ca, d, n)) {
        free(d); d = NULL; mbedtls_x509_crt_free(&G.ca); mbedtls_x509_crt_init(&G.ca);
        if (make_cert(&G.ca_key, CA_NAME, CA_NAME, 1, NULL, 0, &d, &n) || file_put(dir, "ca.crt.der", d, n) || mbedtls_x509_crt_parse_der(&G.ca, d, n)) { free(d); return -1; }
        online_logf("made the local CA in %s (first start)", dir);
    }
    free(d);
    // the default chain: the listed names and their one-label wildcards, for a client that sends no name
    const char *names[64]; char wild[32][140]; int count = 0;
    for (int i = 0; G.hosts[i] && i < 32; i++) { snprintf(wild[i], sizeof wild[i], "*.%s", G.hosts[i]); names[count++] = wild[i]; names[count++] = G.hosts[i]; }
    if (make_cert(&G.key, "CN=local services", CA_NAME, 0, names, count, &d, &n)) return -1;
    r = mbedtls_x509_crt_parse_der(&G.chain, d, n) || mbedtls_x509_crt_parse_der(&G.chain, G.ca.raw.p, G.ca.raw.len); free(d);
    return r ? -1 : 0;
}
// The certificate for one name (SNI), made when the name first comes up. Caller holds no lock. NULL = use the default
static mbedtls_x509_crt *leaf_for(const char *name) {
    mbedtls_x509_crt *got = NULL;
    pthread_mutex_lock(&G.leaf_mu);
    for (int i = 0; i < G.nleaf && !got; i++) if (!strcasecmp(G.leaf[i].name, name)) got = &G.leaf[i].crt;
    if (!got && G.nleaf < LEAF_MAX && strlen(name) < sizeof G.leaf[0].name) {
        leaf_t *l = &G.leaf[G.nleaf]; uint8_t *d; size_t n; char subject[160]; const char *names[1] = {name};
        snprintf(subject, sizeof subject, "CN=%.64s", name);   // a common name holds 64 characters; the subjectAltName carries the whole name
        mbedtls_x509_crt_init(&l->crt);
        if (!make_cert(&G.key, subject, CA_NAME, 0, names, 1, &d, &n)) {
            if (!mbedtls_x509_crt_parse_der(&l->crt, d, n) && !mbedtls_x509_crt_parse_der(&l->crt, G.ca.raw.p, G.ca.raw.len)) { snprintf(l->name, sizeof l->name, "%s", name); G.nleaf++; got = &l->crt; }
            else mbedtls_x509_crt_free(&l->crt);
            free(d);
        }
    }
    pthread_mutex_unlock(&G.leaf_mu);
    return got;
}

// ---- a connection ----
struct online_conn {
    pthread_mutex_t mu;
    mbedtls_ssl_context ssl;
    online_buf in, out, plain;   // from the game (TLS records), for the game (TLS records), the application's bytes not yet written to TLS
    online_app app;
    char sni[128];
    int shook, has_app, ending, closed;   // ending: the application is done, close once `plain` is written
    int no_tls;                           // online_plain: `in` and `out` hold the application's bytes
};
static int bio_recv(void *ctx, unsigned char *buf, size_t n) {
    online_conn *c = ctx;
    if (!c->in.n) return MBEDTLS_ERR_SSL_WANT_READ;
    if (n > c->in.n) n = c->in.n;
    memcpy(buf, c->in.p, n); online_buf_drop(&c->in, n); return (int)n;
}
static int bio_send(void *ctx, const unsigned char *buf, size_t n) { online_conn *c = ctx; online_buf_add(&c->out, buf, n); return (int)n; }
static int on_sni(void *arg, mbedtls_ssl_context *ssl, const unsigned char *name, size_t len) {
    (void)arg; online_conn *c = mbedtls_ssl_get_user_data_p(ssl);
    if (len >= sizeof c->sni) return 0;   // the default certificate
    memcpy(c->sni, name, len); c->sni[len] = 0;
    if (!online_serves(c->sni)) { online_logf("[RED] connection for %s, which is not one of the services' names: the default certificate is presented", c->sni); return 0; }
    mbedtls_x509_crt *crt = leaf_for(c->sni);
    return crt ? mbedtls_ssl_set_hs_own_cert(ssl, crt, &G.key) : 0;
}

int online_serves(const char *name) {
    size_t n = strlen(name);
    for (int i = 0; G.hosts && G.hosts[i]; i++) {
        size_t h = strlen(G.hosts[i]);
        if (n == h ? !strcasecmp(name, G.hosts[i]) : n > h && name[n - h - 1] == '.' && !strcasecmp(name + n - h, G.hosts[i])) return 1;
    }
    return 0;
}
const uint8_t *online_ca_der(size_t *len) { if (!G.ready) return NULL; *len = G.ca.raw.len; return G.ca.raw.p; }

static int start_locked(const char *dir) {
    int r = psa_crypto_init() == PSA_SUCCESS ? 0 : -1;
    if (r) { online_logf("[RED] PSA crypto (the random generator) did not start"); return -1; }
    if (pki_start(dir)) { online_logf("[RED] no CA or server certificate in %s: the services stay off", dir); return -1; }
    mbedtls_ssl_config_init(&G.conf);
    if ((r = mbedtls_ssl_config_defaults(&G.conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) ||
        (r = mbedtls_ssl_conf_own_cert(&G.conf, &G.chain, &G.key)) || (r = mbedtls_ssl_conf_alpn_protocols(&G.conf, ALPN))) { online_logf("[RED] TLS setup: %s", tls_err(r)); return -1; }
    mbedtls_ssl_conf_sni(&G.conf, on_sni, NULL);
    mbedtls_ssl_conf_min_tls_version(&G.conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&G.conf, MBEDTLS_SSL_VERSION_TLS1_2);   // what gRPC and the games' HTTP clients need; no session tickets or early data to keep state for
    return 0;
}
int online_start(const char *dir, const char *const *hosts, void (*log)(const char *line)) {
    if (G.ready) return 0;
    G.hosts = hosts; G.log = log;
    tls_lock(); int r = start_locked(dir); tls_unlock();
    if (r) return -1;
    G.ready = 1;
    return 0;
}

online_conn *online_open(void) {
    if (!G.ready) return NULL;
    online_conn *c = calloc(1, sizeof *c);
    pthread_mutex_init(&c->mu, NULL);
    tls_lock();
    mbedtls_ssl_init(&c->ssl);
    int r = mbedtls_ssl_setup(&c->ssl, &G.conf);
    if (r) { online_logf("[RED] TLS connection: %s", tls_err(r)); mbedtls_ssl_free(&c->ssl); tls_unlock(); free(c); return NULL; }
    mbedtls_ssl_set_user_data_p(&c->ssl, c);
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
    tls_unlock();
    return c;
}
void online_close(online_conn *c) {
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    if (c->has_app && c->app.done) c->app.done(c->app.state);
    tls_lock(); mbedtls_ssl_free(&c->ssl); tls_unlock();
    online_buf_free(&c->in); online_buf_free(&c->out); online_buf_free(&c->plain);
    pthread_mutex_unlock(&c->mu); pthread_mutex_destroy(&c->mu); free(c);
}
static void fail(online_conn *c, const char *what, int r) { online_logf("[RED] %s%s%s: %s", what, *c->sni ? " for " : "", c->sni, tls_err(r)); c->closed = 1; }
// The TLS side of pump. Caller holds c->mu and G.tls_mu
static void pump_tls(online_conn *c) {
    if (!c->shook) {
        int r = mbedtls_ssl_handshake(&c->ssl);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return;
        if (r) { fail(c, "TLS handshake", r); return; }
        const char *alpn = mbedtls_ssl_get_alpn_protocol(&c->ssl);
        c->shook = 1;
        r = alpn && !strcmp(alpn, "h2") ? online_grpc_app(&c->app) : online_http_app(&c->app, c->sni);
        if (r) { c->closed = 1; return; }
        c->has_app = 1;
        online_logf("connection for %s: %s, %s", *c->sni ? c->sni : "(no name)", mbedtls_ssl_get_version(&c->ssl), alpn ? alpn : "no ALPN (HTTP/1.1)");
    }
    unsigned char buf[16384];
    while (!c->ending) {
        int r = mbedtls_ssl_read(&c->ssl, buf, sizeof buf);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) break;
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) { c->closed = 1; return; }   // the game closed its side: nothing more to answer
        if (r < 0) { fail(c, "TLS read", r); return; }
        if (c->app.rx(c->app.state, buf, (size_t)r, &c->plain) < 0) c->ending = 1;
    }
    while (c->plain.n) {
        int r = mbedtls_ssl_write(&c->ssl, c->plain.p, c->plain.n);
        if (r <= 0) { fail(c, "TLS write", r); return; }
        online_buf_drop(&c->plain, (size_t)r);
    }
    if (c->ending) { mbedtls_ssl_close_notify(&c->ssl); c->closed = 1; }
}
// Everything that can be done now: the handshake, the bytes the game sent through the application, the application's bytes into TLS. Caller holds c->mu
static void pump(online_conn *c) {
    if (c->closed) return;
    if (c->no_tls) {
        if (c->in.n && !c->ending) { if (c->app.rx(c->app.state, c->in.p, c->in.n, &c->plain) < 0) c->ending = 1; online_buf_drop(&c->in, c->in.n); }
        if (c->plain.n) { online_buf_add(&c->out, c->plain.p, c->plain.n); online_buf_drop(&c->plain, c->plain.n); }
        if (c->ending) c->closed = 1;   // online_get still hands out what is in `out`
        return;
    }
    tls_lock(); pump_tls(c); tls_unlock();
}
long online_put(online_conn *c, const void *data, size_t n) {
    pthread_mutex_lock(&c->mu);
    long r = c->closed ? -1 : (long)n;
    if (r >= 0) { online_buf_add(&c->in, data, n); pump(c); }
    pthread_mutex_unlock(&c->mu);
    return r;
}
long online_get(online_conn *c, void *data, size_t n) {
    pthread_mutex_lock(&c->mu);
    long r = 0;
    if (c->out.n) { if (n > c->out.n) n = c->out.n; memcpy(data, c->out.p, n); online_buf_drop(&c->out, n); r = (long)n; }
    else if (c->closed) r = -1;
    pthread_mutex_unlock(&c->mu);
    return r;
}
long online_peek(online_conn *c, void *data, size_t n) {
    pthread_mutex_lock(&c->mu);
    long r = 0;
    if (c->out.n) { if (n > c->out.n) n = c->out.n; memcpy(data, c->out.p, n); r = (long)n; }
    else if (c->closed) r = -1;
    pthread_mutex_unlock(&c->mu);
    return r;
}
int online_plain(online_conn *c, const char *host) {
    pthread_mutex_lock(&c->mu);
    int r = c->shook || c->no_tls || c->in.n || c->closed ? -1 : 0;
    if (!r) {
        snprintf(c->sni, sizeof c->sni, "%s", host ? host : "");
        if (online_http_app(&c->app, c->sni)) r = -1;
        else { c->no_tls = c->shook = c->has_app = 1; online_logf("connection for %s: TLS by the host's ssl service, HTTP/1.1", *c->sni ? c->sni : "(no name)"); }
    }
    pthread_mutex_unlock(&c->mu);
    return r;
}
int online_poll(online_conn *c, uint64_t now_ms) {
    pthread_mutex_lock(&c->mu);
    if (!c->closed && c->has_app && c->app.tick) { if (c->app.tick(c->app.state, now_ms, &c->plain) < 0) c->ending = 1; pump(c); }
    int r = (c->out.n || c->closed ? 1 : 0) | (c->closed ? 2 : 0);
    pthread_mutex_unlock(&c->mu);
    return r;
}

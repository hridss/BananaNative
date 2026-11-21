// The in-host online services (dm/host/online) behind a real TCP port, so that ordinary clients can be pointed at them: curl for the HTTP side, a gRPC client for
// the NPLN side. The host itself never listens anywhere: there the same connections are fed from the game's virtual sockets (hle_net.c).
//   online_serve <port> <state folder> <name> [<name> ...]     the CA is <state folder>/ca.crt.der; stops with the process
// The HTTP reply table here is a test one (TestPing, /redirect and the "@ping" host prefix).
#include "../online/online.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static void say(const char *line) { printf("%s\n", line); fflush(stdout); }
static void *serve(void *arg) {
    int fd = (int)(intptr_t)arg; online_conn *c = online_open(); char buf[16384];
    for (int open = 1; c && open;) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 50) > 0) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n <= 0 || online_put(c, buf, (size_t)n) < 0) open = 0;
        }
        int state = online_poll(c, now_ms());
        for (long n; (n = online_get(c, buf, sizeof buf)) > 0;) if (write(fd, buf, (size_t)n) != n) open = 0;
        if (state & 2) open = 0;
    }
    online_close(c); close(fd);
    return NULL;
}
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: online_serve <port> <state folder> <name> [<name> ...]\n"); return 2; }
    static const online_http_reply rows[] = {
        {"RequestTestPingParams", "{\"type\": \"ResponseTestPingParams\", \"value\": {\"millisecond\": 0}}", "session-id=test; Path=/"},
        {"/redirect", "{\"response\": {\"redirect\": \"nochange\"}}", NULL},
        {"@ping", "{}", NULL},
        {NULL, NULL, NULL}};
    if (online_start(argv[2], (const char *const *)argv + 3, say)) return 1;
    online_http_replies(rows);
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[1])), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 16)) { perror("listen"); return 1; }
    printf("listening on 127.0.0.1:%s\n", argv[1]); fflush(stdout);
    for (;;) {
        int fd = accept(ls, NULL, NULL); pthread_t t;
        if (fd >= 0 && !pthread_create(&t, NULL, serve, (void *)(intptr_t)fd)) pthread_detach(t);
    }
}

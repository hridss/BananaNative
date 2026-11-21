# nghttp2

The HTTP/2 library, for the gRPC side of the in-host online services (`dm/host/online/online_grpc.c`): framing, header compression and flow control of a server session fed from memory.

- Source: https://github.com/nghttp2/nghttp2, release `v1.71.0` (the release archive `nghttp2-1.71.0.tar.gz`, SHA-256 `0cdd35ecec77919b4b7b245f87fe8d2216e3d62da854eb0838c594ff058042fc`), unmodified.
- Here: `lib/*.c`, `lib/*.h`, `lib/includes/nghttp2/nghttp2.h` and `COPYING`. One file is written here instead of generated: `lib/includes/nghttp2/nghttp2ver.h` (upstream's configure step makes it from `nghttp2ver.h.in`), with version 1.71.0 (`0x014700`). Left out: the applications, tests, documentation and the build system.
- Build: every `lib/*.c` into a static library with `NGHTTP2_STATICLIB`, `HAVE_ARPA_INET_H` and `HAVE_NETINET_IN_H` (`dm/host/Makefile` `gen/nghttp2`, `android/host/CMakeLists.txt` `nghttp2`).
- License: MIT (`COPYING`).

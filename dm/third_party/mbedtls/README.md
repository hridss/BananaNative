# mbedTLS

TLS and X.509 for the in-host online services (`dm/host/online/online.c`): the server side of the TLS a game's own network library speaks, the device's CA and the server certificates signed with it.

- Source: https://github.com/Mbed-TLS/mbedtls, release `mbedtls-4.3.0` (the release archive `mbedtls-4.3.0.tar.bz2`, SHA-256 `1c87cba094cb40f097ebe0eba440a74939b71dab4e1d0d566bc90749ff381d18`), unmodified. Since 4.0 the cryptography is TF-PSA-Crypto, which the archive carries as `tf-psa-crypto/`.
- Here, as upstream has them (`.c` and `.h` files only), with the default configurations (`include/mbedtls/mbedtls_config.h`, `tf-psa-crypto/include/psa/crypto_config.h`):
  - `library/` and `include/`: TLS and X.509, with the files upstream generates (`error.c`, `ssl_debug_helpers_generated.c`, the configuration checks), and `LICENSE`.
  - `tf-psa-crypto/`: `include/`, `core/` (with its generated driver wrappers and configuration checks), `dispatch/`, `extras/`, `platform/`, `utilities/`, `drivers/builtin/`, and the optional drivers its build lists, which the default configuration leaves off: `drivers/everest/` (`include/`, `library/`), `drivers/p256-m/`, `drivers/pqcp/` (`include/`, `src/`, and `mldsa-native/mldsa/` with `mldsa-native/LICENSE`); and `tf-psa-crypto/LICENSE`.
  - Left out: tests, programs, documentation, scripts, the build systems, the `framework` submodules (test infrastructure) and the rest of `mldsa-native` (its proofs, examples and development backends).
- Build: the sources upstream's `library/Makefile` builds (`library/*.c`; TF-PSA-Crypto's `core`, `dispatch`, `extras`, `platform`, `utilities` and `drivers/builtin/src`; Everest's `x25519.c` and `Hacl_Curve25519_joined.c`; p256-m's two files; `drivers/pqcp/src`) into one static library with its include directories (`dm/host/Makefile` `gen/mbedtls`, `android/host/CMakeLists.txt` `mbedtls`). Without the threading layer: the host makes every mbedTLS and PSA call under one lock (`online.c`).
- License: Apache-2.0 or GPL-2.0-or-later, at your choice (`LICENSE`, `tf-psa-crypto/LICENSE`); ML-DSA native: Apache-2.0, ISC or MIT (`tf-psa-crypto/drivers/pqcp/mldsa-native/LICENSE`).

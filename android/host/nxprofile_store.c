// SPDX-License-Identifier: MIT
// The common libnxhost.so's profile storage (NX_COMMON, -DNX_COMMON_PROFILE): `dm_game`, which dm/host/profile_rt.h reads, is filled by the game's profile library
// (android/profile/profile.c) from its constructor at load time, which runs before nxlaunch's JNI entry can start the host. A profile lib's tables and strings stay in its own
// rodata (it stays loaded), only the struct is copied here.
#include <string.h>
#include <stddef.h>

// profile.h declares `extern const struct dm_profile dm_game;`. This file must define the symbol as writable storage, which that declaration forbids (const, and a conflicting
// definition), so the header is read with the name moved aside (only for sizeof) and `dm_game` is then defined as an 8-aligned byte array of the struct's size. Other TUs
// see the declaration and use the bytes as the struct (same symbol, same address; the linker does not check types)
#define dm_game dm_game_decl_only
#include "profile.h"
#undef dm_game
__attribute__((visibility("hidden"), aligned(16))) unsigned char dm_game[sizeof(struct dm_profile)];

__attribute__((visibility("default"))) void nxhost_set_profile(const struct dm_profile *p) { memcpy(dm_game, p, sizeof *p); }

// The profile's prewarm seed list (dm/games/<id>/vk_prewarm.bin; len 0: none), read by vka_pso.c through common_inc/vk_prewarm_seed.h
const unsigned char *nxhost_prewarm_seed;
size_t nxhost_prewarm_seed_len;
__attribute__((visibility("default"))) void nxhost_set_prewarm_seed(const unsigned char *p, size_t len) { nxhost_prewarm_seed = p; nxhost_prewarm_seed_len = len; }

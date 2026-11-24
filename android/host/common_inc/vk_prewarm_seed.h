/* The common libnxhost.so's stand-in for the generated vk_prewarm_seed.h (dm/games/game.py seed): vka_pso.c reads dm_vk_prewarm_seed / dm_vk_prewarm_seed_len, here the pointer and
   length the game's profile library registered with nxhost_set_prewarm_seed (nxprofile_store.c); length 0 until then and for a profile without a seed */
#pragma once
#include <stddef.h>
extern const unsigned char *nxhost_prewarm_seed;
extern size_t nxhost_prewarm_seed_len;
#define dm_vk_prewarm_seed nxhost_prewarm_seed
#define dm_vk_prewarm_seed_len nxhost_prewarm_seed_len

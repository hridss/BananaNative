// SPDX-License-Identifier: MIT
// A game's profile as data: compiled with that game's generated game_profile.h (dm/games/game.py header), it fills struct dm_profile (dm/host/profile.h) from the DM_GAME_* macros and
// registers it with the common host (nxhost_set_profile) when the library loads. The tables are the compile-time flavour's static arrays (dm/host/profile_ct.h)
#include <stddef.h>
#include "game_profile.h"
#include "profile_ct.h"
#include "vk_prewarm_seed.h"   // the game's prewarm seed list (game.py seed): dm_vk_prewarm_seed, dm_vk_prewarm_seed_len

void nxhost_set_prewarm_seed(const unsigned char *p, size_t len);   // libnxhost.so (android/host/nxprofile_store.c)

#define P_STR(f, K) .f = DM_GAME_##K,
#define P_NUM(t, f, K) .f = DM_GAME_##K,
#define P_TAB(f, K) .f = DM_T_##K, .n_##f = DM_N_##K,

__attribute__((visibility("hidden"))) const struct dm_profile dm_game = {
    DM_PROFILE_STRINGS(P_STR)
    DM_PROFILE_NUMS(P_NUM)
    DM_PROFILE_TABLES(P_TAB)
    .render_quality = {DM_T_RENDER_QUALITY[0], DM_T_RENDER_QUALITY[1], DM_T_RENDER_QUALITY[2], DM_T_RENDER_QUALITY[3]},
    .has_nvn_api = DM_HAS_NVN_API,
    .has_shared_font_ttf = DM_HAS_SHARED_FONT_TTF,
};

__attribute__((constructor)) static void profile_register(void) { nxhost_set_profile(&dm_game); nxhost_set_prewarm_seed(dm_vk_prewarm_seed, dm_vk_prewarm_seed_len); }

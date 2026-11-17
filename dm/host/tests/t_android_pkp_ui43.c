// Synthetic unit tests for dm/host/android/pkp_ui43.c (pure C, no game files or POSIX-only APIs).
#include "../android/pkp_ui43.c"
#include <assert.h>
#include <stdio.h>

typedef struct {
    _Alignas(16) unsigned char pane[0x160];
    _Alignas(16) unsigned char tr[0x40];
    _Alignas(16) unsigned char glob[0x40];
    _Alignas(16) unsigned char node[0x20];
} fake_pane_t;

typedef struct {
    _Alignas(16) unsigned char bytes[0x40];
} fake_so_t;

static void init_so(fake_so_t *so, float sx, float sy) {
    memset(so, 0, sizeof *so);
    wr_f32(so->bytes, 0x10, sx);
    wr_f32(so->bytes, 0x14, sy);
    wr_f32(so->bytes, 0x18, 1.0f);
    wr_f32(so->bytes, 0x1c, 0.0f);
}

static void init_pane(fake_pane_t *fp, const char *name, uint8_t kind, uint16_t w, uint16_t h,
                      float tx, float ty, fake_pane_t *parent) {
    memset(fp, 0, sizeof *fp);
    uintptr_t p_name = (uintptr_t)name;
    uintptr_t p_tr = (uintptr_t)fp->tr;
    uintptr_t p_glob = (uintptr_t)fp->glob;
    uintptr_t p_par = (uintptr_t)parent;
    uint16_t nlen = name ? (uint16_t)strlen(name) : 0;
    memcpy(fp->pane + 0xb0, &p_name, sizeof p_name);
    memcpy(fp->pane + 0xd0, &p_par, sizeof p_par);
    memcpy(fp->pane + 0xe0, &p_glob, sizeof p_glob);
    memcpy(fp->pane + 0xe8, &p_tr, sizeof p_tr);
    memcpy(fp->pane + 0xf0, &w, sizeof w);
    memcpy(fp->pane + 0xf2, &h, sizeof h);
    wr_f32(fp->pane, 0xf4, 1.0f);
    wr_f32(fp->pane, 0xf8, 1.0f);
    fp->pane[0x10c] = 0x11; // centre-anchored (av = 0.5)
    memcpy(fp->pane + 0x110, &nlen, sizeof nlen);
    fp->pane[0x112] = kind;
    wr_f32(fp->tr, 0x30, tx);
    wr_f32(fp->tr, 0x34, ty);
    wr_f32(fp->glob, 0x00, 1.0f);
    wr_f32(fp->glob, 0x14, 1.0f);
}

static int nearf(float a, float b) { return fabsf(a - b) < 1e-4f; }

static void test_lite_and_edge_rules(void) {
    fake_so_t so;
    fake_pane_t root, child, bg, guide, mini;
    init_so(&so, 1.0f, 0.75f);
    init_pane(&root, "Root", 0, 1920, 1080, 0.0f, 0.0f, NULL);
    init_pane(&child, "Child", 0, 200, 100, 0.0f, 400.0f, &root);

    pkp_ui43_pre_bake(so.bytes, child.pane);
    // T = 450 > 360 -> up = 540 - 0.75*450 = 202.5, clamped to +135
    assert(nearf(rd_f32(so.bytes, 0x14), 0.75f));
    assert(nearf(rd_f32(so.bytes, 0x1c), 135.0f));
    pkp_ui43_post_bake(so.bytes);
    assert(nearf(rd_f32(so.bytes, 0x10), 1.0f));
    assert(nearf(rd_f32(so.bytes, 0x14), 0.75f));
    assert(nearf(rd_f32(so.bytes, 0x1c), 0.0f));

    // Background Picture (>= 1824x1026) under screen-level root triggers T193 cover (sx = sc/0.75, sy = sc, add = 0)
    init_pane(&bg, "Bg", 1, 1920, 1080, 0.0f, 0.0f, &root);
    pkp_ui43_pre_bake(so.bytes, bg.pane);
    assert(nearf(rd_f32(so.bytes, 0x10), 1.0f / 0.75f));
    assert(nearf(rd_f32(so.bytes, 0x14), 1.0f));
    assert(nearf(rd_f32(so.bytes, 0x1c), 0.0f));
    pkp_ui43_post_bake(so.bytes);
    assert(nearf(rd_f32(so.bytes, 0x10), 1.0f));
    assert(nearf(rd_f32(so.bytes, 0x14), 0.75f));

    // T194 defensive reset if sx entered inflated
    wr_f32(so.bytes, 0x10, 1.333333f);
    pkp_ui43_pre_bake(so.bytes, child.pane);
    assert(nearf(rd_f32(so.bytes, 0x10), 1.0f));
    pkp_ui43_post_bake(so.bytes);

    // T27e/T30 key guide row under non-screen parent: -135 + 9 = -126
    fake_pane_t small_par;
    init_pane(&small_par, "Small", 0, 500, 500, 0.0f, 0.0f, NULL);
    init_pane(&guide, "KeyGuide", 0, 1920, 72, 0.0f, -463.0f, &small_par);
    pkp_ui43_pre_bake(so.bytes, guide.pane);
    assert(nearf(rd_f32(so.bytes, 0x1c), -126.0f));
    pkp_ui43_post_bake(so.bytes);

    // T31 minimap suppression
    init_pane(&mini, "Mini", 0, 512, 512, -685.0f, -265.0f, &root);
    pkp_ui43_pre_bake(so.bytes, mini.pane);
    assert(nearf(rd_f32(so.bytes, 0x1c), 0.0f));
    pkp_ui43_post_bake(so.bytes);

    // T27 (lite4) pause white card win_0300_prt_base_line: top-aligned (av=0), ty=251, h=677 -> centre -87.5 (< 200), no shift
    fake_pane_t pause_card;
    init_pane(&pause_card, "win_0300_prt_base_line", 3, 610, 677, 570.0f, 251.0f, &root);
    pause_card.pane[0x10c] = 0x10;
    pkp_ui43_pre_bake(so.bytes, pause_card.pane);
    assert(nearf(rd_f32(so.bytes, 0x1c), 0.0f));
    pkp_ui43_post_bake(so.bytes);

    // T29 (lite5) pause coin pill following a phone-body pane in the flat list keeps add = 0
    fake_pane_t body, pill;
    uint8_t nd_body[0x18] = {0}, nd_pill[0x18] = {0};
    init_pane(&body, "0300_prt_pokedex_open", 1, 1376, 1072, 902.0f, 0.0f, &root);
    init_pane(&pill, "CL_0000_parts_header_num_v2", 0, 224, 60, 818.0f, 492.0f, &root);
    uintptr_t p_body = (uintptr_t)body.pane, p_nd_body = (uintptr_t)nd_body, p_nd_pill = (uintptr_t)nd_pill;
    memcpy(nd_body + 0x10, &p_body, sizeof p_body);
    memcpy(nd_pill + 0x00, &p_nd_body, sizeof p_nd_body);
    memcpy(body.pane + 0xc8, &p_nd_body, sizeof p_nd_body);
    memcpy(pill.pane + 0xc8, &p_nd_pill, sizeof p_nd_pill);
    pkp_ui43_pre_bake(so.bytes, pill.pane);
    assert(nearf(rd_f32(so.bytes, 0x1c), 0.0f));
    pkp_ui43_post_bake(so.bytes);

    // Bug 2: centered square Null FX/icon containers inside 720-unit band stay add = 0 with sibling particles
    {
        fake_so_t so2;
        init_so(&so2, 1.0f, 0.75f);
        fake_pane_t ml_root, halo, sparkle, gather, aura, icon_ctr, icon_hud, star_hud, base_sk, fx_heart, hud_tl;
        init_pane(&ml_root, "RootPane", 0, 1920, 1080, 0.0f, 0.0f, NULL);
        init_pane(&halo, "Null_eff_halo", 0, 512, 512, 0.0f, 330.0f, &ml_root);
        init_pane(&sparkle, "Null_eff_sparkle_1", 0, 50, 50, 0.0f, 330.0f, &ml_root);
        init_pane(&gather, "Null_Gather", 0, 50, 50, 0.0f, 330.0f, &ml_root);
        init_pane(&aura, "Null_eff_aura", 0, 256, 256, 0.0f, 320.0f, &ml_root);
        init_pane(&icon_ctr, "Null_icon_skill", 0, 256, 256, 0.0f, 320.0f, &ml_root);
        init_pane(&icon_hud, "Null_icon_skill", 0, 256, 256, 763.0f, -410.0f, &ml_root);
        init_pane(&star_hud, "Null_eff_star", 0, 128, 128, 763.0f, -410.0f, &ml_root);
        init_pane(&base_sk, "Null_base_skill", 0, 444, 400, 760.0f, -411.0f, &ml_root);
        init_pane(&fx_heart, "inout", 0, 460, 460, 0.0f, 200.0f, &ml_root);
        init_pane(&hud_tl, "Null_hud", 0, 392, 180, -770.0f, 326.0f, &ml_root);

        fake_pane_t *zero_add[] = {&halo, &sparkle, &gather, &aura, &icon_ctr, &fx_heart};
        for (size_t i = 0; i < sizeof zero_add / sizeof zero_add[0]; i++) {
            pkp_ui43_pre_bake(so2.bytes, zero_add[i]->pane);
            assert(nearf(rd_f32(so2.bytes, 0x14), 0.75f));
            assert(nearf(rd_f32(so2.bytes, 0x1c), 0.0f));
            pkp_ui43_post_bake(so2.bytes);
        }
        fake_pane_t *neg_add[] = {&icon_hud, &star_hud, &base_sk};
        for (size_t i = 0; i < sizeof neg_add / sizeof neg_add[0]; i++) {
            pkp_ui43_pre_bake(so2.bytes, neg_add[i]->pane);
            assert(nearf(rd_f32(so2.bytes, 0x14), 0.75f));
            assert(nearf(rd_f32(so2.bytes, 0x1c), -135.0f));
            pkp_ui43_post_bake(so2.bytes);
        }
        pkp_ui43_pre_bake(so2.bytes, hud_tl.pane);
        assert(nearf(rd_f32(so2.bytes, 0x14), 0.75f));
        assert(nearf(rd_f32(so2.bytes, 0x1c), 135.0f));
        pkp_ui43_post_bake(so2.bytes);

        // Runtime ScreenLayout::Bake bakes root pane height first (1080 * 0.75 = 810, strips 500 * 0.75 = 375).
        // Verify baked root (1920x810) still matches screen = 1 while baked strip (1920x375) gets screen = 0.
        fake_pane_t ml_root_baked, hud_baked, strip_baked, strip_child_baked;
        init_pane(&ml_root_baked, "RootPane", 0, 1920, 810, 0.0f, 0.0f, NULL);
        init_pane(&hud_baked, "Null_hud", 0, 392, 180, -770.0f, 326.0f, &ml_root_baked);
        init_pane(&strip_baked, "Null_All", 0, 1920, 375, 0.0f, 0.0f, NULL);
        init_pane(&strip_child_baked, "Win_0400_win_build_info_base", 0, 1920, 384, 0.0f, -56.0f, &strip_baked);

        pkp_ui43_pre_bake(so2.bytes, hud_baked.pane);
        assert(nearf(rd_f32(so2.bytes, 0x14), 0.75f));
        assert(nearf(rd_f32(so2.bytes, 0x1c), 135.0f));
        pkp_ui43_post_bake(so2.bytes);

        pkp_ui43_pre_bake(so2.bytes, strip_child_baked.pane);
        assert(nearf(rd_f32(so2.bytes, 0x14), 0.75f));
        assert(nearf(rd_f32(so2.bytes, 0x1c), 0.0f));
        pkp_ui43_post_bake(so2.bytes);

        // Bug 3: build menu edge-clamp at loop_all ty=-190, and hlim screen-root check on wide strips
        fake_pane_t bm_static, bm_loop, dlg_180, null_all, win_base, prt_sq;
        fake_pane_t null_tab, dye_list, root_2c, bar_2c, null_all_38, inout_38;
        init_pane(&bm_static, "CL_0410_parts_build_material_set", 2, 1920, 500, 0.0f, -290.0f, &ml_root_baked);
        init_pane(&bm_loop, "CL_0410_parts_build_material_set", 2, 1920, 500, 0.0f, -190.0f, &ml_root_baked);
        init_pane(&dlg_180, "Null_dlg_centered", 0, 640, 200, 0.0f, 180.0f, &ml_root_baked);
        init_pane(&null_all, "Null_All", 0, 1920, 500, 0.0f, 0.0f, NULL);
        init_pane(&win_base, "Win_0400_win_build_info_base", 0, 1920, 512, 0.0f, -56.0f, &null_all);
        init_pane(&prt_sq, "0000_prt_square_w", 2, 1920, 234, 0.0f, -310.0f, &null_all);
        prt_sq.pane[0x10c] = 0x10; // ov = 0
        init_pane(&null_tab, "Null_tab", 0, 1920, 477, 0.0f, -332.0f, &ml_root_baked);
        init_pane(&dye_list, "CL_0400_parts_dyeing_list_0000", 0, 1920, 1080, 0.0f, 331.5f, &null_tab);
        init_pane(&root_2c, "RootPane", 0, 1920, 256, 0.0f, 0.0f, NULL);
        init_pane(&bar_2c, "Null_bar", 0, 1200, 80, 0.0f, 100.0f, &root_2c);
        init_pane(&null_all_38, "Null_All", 0, 1920, 512, 0.0f, -290.0f, NULL);
        init_pane(&inout_38, "inout", 0, 1920, 512, 0.0f, -600.0f, &null_all_38);
        inout_38.pane[0x10c] = 0x10; // ov = 0

        pkp_ui43_pre_bake(so2.bytes, bm_static.pane);
        assert(nearf(rd_f32(so2.bytes, 0x1c), -135.0f));
        pkp_ui43_post_bake(so2.bytes);

        pkp_ui43_pre_bake(so2.bytes, bm_loop.pane);
        assert(nearf(rd_f32(so2.bytes, 0x1c), -135.0f));
        pkp_ui43_post_bake(so2.bytes);

        pkp_ui43_pre_bake(so2.bytes, null_tab.pane);
        assert(nearf(rd_f32(so2.bytes, 0x1c), -135.0f));
        pkp_ui43_post_bake(so2.bytes);

        pkp_ui43_pre_bake(so2.bytes, dye_list.pane);
        assert(nearf(rd_f32(so2.bytes, 0x1c), 135.0f));
        pkp_ui43_post_bake(so2.bytes);

        fake_pane_t *zero_b3[] = {&dlg_180, &win_base, &prt_sq, &bar_2c, &inout_38};
        for (size_t i = 0; i < sizeof zero_b3 / sizeof zero_b3[0]; i++) {
            pkp_ui43_pre_bake(so2.bytes, zero_b3[i]->pane);
            assert(nearf(rd_f32(so2.bytes, 0x14), 0.75f));
            assert(nearf(rd_f32(so2.bytes, 0x1c), 0.0f));
            pkp_ui43_post_bake(so2.bytes);
        }
    }
}

// Bug 4: the Memories viewer's photo frame and photo bake at (0.75, 0.75): a 4:3 rectangle on the 4:3 display.
static void test_memories_photo_is_4x3(void) {
    fake_so_t so;
    fake_pane_t root, all, dim, photo, frame, pic, other_pic, other_frame, other_all, other_photo, other_dummy;
    init_so(&so, 1.0f, 0.75f);
    init_pane(&root, "Null_All", 0, 1920, 1080, 0.0f, 0.0f, NULL);
    init_pane(&all, "Null_photo_all", 0, 1920, 1080, 0.0f, 0.0f, &root);
    init_pane(&dim, "0000_prt_square_k", 1, 1920, 1080, 0.0f, 0.0f, &all);
    init_pane(&photo, "Null_photo", 0, 50, 50, 0.0f, 0.0f, &all);
    init_pane(&frame, "Window", 3, 1460, 830, 0.0f, 0.0f, &photo);
    init_pane(&pic, "0000_prt_dummy_0", 1, 1440, 810, 0.0f, 0.0f, &photo);
    // The same names elsewhere (other sizes, other parents) keep the layout's uniform bake.
    init_pane(&other_pic, "0000_prt_dummy_0", 1, 1060, 585, 0.0f, 0.0f, &photo);
    init_pane(&other_frame, "Window", 3, 1460, 830, 0.0f, 0.0f, &all);
    init_pane(&other_all, "Null_photo_all", 0, 1920, 1080, 0.0f, 0.0f, NULL);
    init_pane(&other_photo, "Null_photo", 0, 1120, 645, 0.0f, 41.5f, &other_all);
    init_pane(&other_dummy, "0000_prt_dummy_0", 1, 1440, 810, 0.0f, 0.0f, &other_photo);

    fake_pane_t *square[] = {&frame, &pic};
    for (size_t i = 0; i < sizeof square / sizeof square[0]; i++) {
        pkp_ui43_pre_bake(so.bytes, square[i]->pane);
        assert(nearf(rd_f32(so.bytes, 0x10), 0.75f));   // x scale: 1440 -> 1080 units = 720 px of 1280
        assert(nearf(rd_f32(so.bytes, 0x14), 0.75f));   // y scale: 810 -> 607.5 units = 540 px of 960
        assert(nearf(rd_f32(so.bytes, 0x1c), 0.0f));
        pkp_ui43_post_bake(so.bytes);
        assert(nearf(rd_f32(so.bytes, 0x10), 1.0f));    // the layout's own scale is restored
        assert(nearf(rd_f32(so.bytes, 0x14), 0.75f));
    }
    // With a layout scale s != 1 the photo follows it (0.75 s on both axes).
    fake_so_t so_s;
    init_so(&so_s, 0.9f, 0.675f);
    pkp_ui43_pre_bake(so_s.bytes, pic.pane);
    assert(nearf(rd_f32(so_s.bytes, 0x10), 0.675f));
    assert(nearf(rd_f32(so_s.bytes, 0x14), 0.675f));
    pkp_ui43_post_bake(so_s.bytes);
    assert(nearf(rd_f32(so_s.bytes, 0x10), 0.9f));

    fake_pane_t *kept[] = {&other_pic, &other_frame, &other_dummy};
    for (size_t i = 0; i < sizeof kept / sizeof kept[0]; i++) {
        pkp_ui43_pre_bake(so.bytes, kept[i]->pane);
        assert(nearf(rd_f32(so.bytes, 0x10), 1.0f));
        pkp_ui43_post_bake(so.bytes);
    }
    // The dim backdrop keeps its cover bake.
    pkp_ui43_pre_bake(so.bytes, dim.pane);
    assert(nearf(rd_f32(so.bytes, 0x10), 4.0f / 3.0f));
    pkp_ui43_post_bake(so.bytes);
}

static void test_bake_stack_nesting(void) {
    fake_so_t so_a, so_b;
    fake_pane_t root, bg, child;
    init_so(&so_a, 1.0f, 0.75f);
    init_so(&so_b, 2.0f, 1.5f);
    init_pane(&root, "Root", 0, 1920, 1080, 0.0f, 0.0f, NULL);
    init_pane(&bg, "Bg", 1, 1920, 1080, 0.0f, 0.0f, &root);
    init_pane(&child, "Child", 0, 200, 100, 0.0f, 400.0f, &root);

    pkp_ui43_pre_bake(so_a.bytes, bg.pane);
    assert(nearf(rd_f32(so_a.bytes, 0x10), 1.0f / 0.75f));
    pkp_ui43_pre_bake(so_b.bytes, child.pane);
    assert(nearf(rd_f32(so_b.bytes, 0x14), 1.5f));
    assert(nearf(rd_f32(so_b.bytes, 0x1c), 135.0f * 2.0f));

    pkp_ui43_post_bake(so_b.bytes);
    assert(nearf(rd_f32(so_b.bytes, 0x10), 2.0f));
    assert(nearf(rd_f32(so_b.bytes, 0x14), 1.5f));
    assert(nearf(rd_f32(so_b.bytes, 0x1c), 0.0f));

    pkp_ui43_post_bake(so_a.bytes);
    assert(nearf(rd_f32(so_a.bytes, 0x10), 1.0f));
    assert(nearf(rd_f32(so_a.bytes, 0x14), 0.75f));
    assert(nearf(rd_f32(so_a.bytes, 0x1c), 0.0f));
}

static void test_t175_t143_t135_t200_t205(void) {
    fake_so_t so;
    fake_pane_t root, pause_gp, pause_ani, pokedex_open, minimap;
    fake_pane_t wheel_all, wheel_key, wheel_child;
    fake_pane_t item_scale, item_base, item_btn;
    fake_pane_t shop_cursor, grow_tip;
    init_so(&so, 1.0f, 0.75f);
    init_pane(&root, "Root", 0, 1920, 1080, 0.0f, 0.0f, NULL);

    // T175 Pause body + minimap
    init_pane(&pause_gp, "PauseGP", 0, 1920, 1080, 0.0f, 0.0f, NULL);
    pause_gp.pane[0x104] = 0x0f;
    init_pane(&pause_ani, "Null_ani_all", 0, 1920, 1080, 0.0f, 0.0f, &pause_gp);
    init_pane(&pokedex_open, "0300_prt_pokedex_open", 0, 100, 100, 0.0f, 0.0f, &pause_ani);
    pkp_ui43_pre_bake(so.bytes, pokedex_open.pane);
    assert(nearf(rd_f32(pause_gp.pane, 0xf4), 4.0f / 3.0f));
    assert(nearf(rd_f32(pause_gp.pane, 0xf8), 4.0f / 3.0f));
    assert(nearf(rd_f32(pause_gp.tr, 0x30), -300.0f));
    assert((pause_gp.pane[0x104] & 4u) == 0);
    pkp_ui43_post_bake(so.bytes);

    init_pane(&minimap, "CL_0200_parts_hud_minimap", 0, 512, 512, -685.0f, -265.0f, &root);
    pkp_ui43_pre_bake(so.bytes, minimap.pane);
    assert(nearf(rd_f32(minimap.pane, 0xf4), 4.0f / 3.0f));
    assert(nearf(rd_f32(minimap.pane, 0xf8), 4.0f / 3.0f));
    assert(nearf(rd_f32(minimap.tr, 0x30), -604.5f));
    assert(nearf(rd_f32(so.bytes, 0x1c), -67.0f));
    pkp_ui43_post_bake(so.bytes);

    // T143 Wheel key + child
    init_pane(&wheel_key, "CL_0342_parts_wheel_key", 0, 128, 184, 0.0f, 0.0f, &root);
    pkp_ui43_pre_bake(so.bytes, wheel_key.pane);
    assert(nearf(rd_f32(wheel_key.pane, 0xf8), 0.75f));
    pkp_ui43_post_bake(so.bytes);

    init_pane(&wheel_all, "Null_All", 0, 128, 184, 0.0f, 0.0f, NULL);
    init_pane(&wheel_child, "0300_wheel_prt_btn_base_0", 1, 64, 64, 10.0f, 20.0f, &wheel_all);
    pkp_ui43_pre_bake(so.bytes, wheel_child.pane);
    assert(nearf(rd_f32(so.bytes, 0x14), 1.0f));
    assert(nearf(rd_f32(so.bytes, 0x1c), 0.0f));
    pkp_ui43_post_bake(so.bytes);

    // T135 item cell
    init_pane(&item_scale, "Null_item_scale", 0, 128, 128, 0.0f, 0.0f, &root);
    init_pane(&item_base, "Null_base", 0, 128, 128, 0.0f, 0.0f, &item_scale);
    init_pane(&item_btn, "0000_btn_item_list_0", 1, 128, 128, 0.0f, 0.0f, &item_base);
    pkp_ui43_pre_bake(so.bytes, item_btn.pane);
    assert(nearf(rd_f32(item_btn.pane, 0xf8), 0.75f));
    pkp_ui43_post_bake(so.bytes);

    // T200 PC Shop cursor + T205 Grow tooltip
    init_pane(&shop_cursor, "CL_0000_parts_cursor_triangle", 0, 84, 84, -610.0f, 351.0f, &root);
    pkp_ui43_pre_bake(so.bytes, shop_cursor.pane);
    assert(nearf(rd_f32(so.bytes, 0x1c), -135.0f));
    pkp_ui43_post_bake(so.bytes);

    init_pane(&grow_tip, "CL_0300_parts_mission_tutorial", 0, 642, 296, 639.0f, 273.0f, &root);
    pkp_ui43_pre_bake(so.bytes, grow_tip.pane);
    assert(nearf(rd_f32(so.bytes, 0x1c), 68.25f));
    pkp_ui43_post_bake(so.bytes);
}

static void test_fam2_details_cover_and_gmx_fix(void) {
    fake_so_t so;
    fake_pane_t hab_still, hab_child, col_cam, col_cap, tile_base, tile_grad;
    fake_pane_t det_feat, det_off, win_cover, pc_child;
    fake_pane_t pause_gp, pause_ani, pokedex_open, cur_root, cur_ani, cur_pic;
    init_so(&so, 1.0f, 0.75f);

    // in_fam2: Null_habitat_still descendant keeps stock (sy = 0.75, add = 0)
    init_pane(&hab_still, "Null_habitat_still", 0, 400, 400, 0.0f, 100.0f, NULL);
    init_pane(&hab_child, "Mask", 1, 400, 400, 0.0f, 100.0f, &hab_still);
    pkp_ui43_pre_bake(so.bytes, hab_child.pane);
    assert(nearf(rd_f32(so.bytes, 0x14), 0.75f));
    assert(nearf(rd_f32(so.bytes, 0x1c), 0.0f));
    pkp_ui43_post_bake(so.bytes);

    // Collection caption 1920x122 under Null_collection_camera (f == 0x110)
    init_pane(&col_cam, "Null_collection_camera", 0, 1920, 1080, 0.0f, 0.0f, NULL);
    init_pane(&col_cap, "CL_0000_parts_tab_caption_0000", 0, 1920, 122, 0.0f, 450.0f, &col_cam);
    pkp_ui43_pre_bake(so.bytes, col_cap.pane);
    assert(nearf(rd_f32(so.bytes, 0x14), 444.5f / 450.0f));
    assert(nearf(rd_f32(so.bytes, 0x1c), 0.0f));
    pkp_ui43_post_bake(so.bytes);

    // Collection tile Null_base exact width 344 (f & 0xff == 17): keeps 0.75, dead f >= 0x200 line stripped
    init_pane(&tile_base, "Null_base", 0, 344, 344, 0.0f, 100.0f, NULL);
    init_pane(&tile_grad, "Null_gradation", 1, 512, 512, 0.0f, 0.0f, &tile_base);
    pkp_ui43_pre_bake(so.bytes, tile_grad.pane);
    assert(nearf(rd_f32(so.bytes, 0x14), 0.75f));
    assert(nearf(rd_f32(so.bytes, 0x1c), 0.0f));
    pkp_ui43_post_bake(so.bytes);

    // in_details: off_screen add = -71
    init_pane(&det_feat, "Null_311_features", 0, 500, 500, 0.0f, 0.0f, NULL);
    init_pane(&det_off, "off_screen", 1, 200, 200, 0.0f, 0.0f, &det_feat);
    pkp_ui43_pre_bake(so.bytes, det_off.pane);
    assert(nearf(rd_f32(so.bytes, 0x1c), -71.0f));
    pkp_ui43_post_bake(so.bytes);

    // layout_cover2 (T199): overscan Window linked via pane+0xc8 flat list
    init_pane(&win_cover, "Win_0500_base_pokesen_top", 3, 1920, 1220, 0.0f, 0.0f, NULL);
    init_pane(&pc_child, "Btn", 0, 200, 100, 0.0f, 200.0f, NULL);
    uintptr_t p_win = (uintptr_t)win_cover.pane;
    uintptr_t p_pc = (uintptr_t)pc_child.pane;
    uintptr_t p_wnode = (uintptr_t)win_cover.node;
    uintptr_t p_pnode = (uintptr_t)pc_child.node;
    memcpy(win_cover.node + 0x08, &p_pnode, sizeof p_pnode);
    memcpy(win_cover.node + 0x10, &p_win, sizeof p_win);
    memcpy(pc_child.node + 0x00, &p_wnode, sizeof p_wnode);
    memcpy(pc_child.node + 0x10, &p_pc, sizeof p_pc);
    memcpy(win_cover.pane + 0xc8, &p_wnode, sizeof p_wnode);
    memcpy(pc_child.pane + 0xc8, &p_pnode, sizeof p_pnode);
    pkp_ui43_pre_bake(so.bytes, pc_child.pane);
    assert(nearf(rd_f32(so.bytes, 0x1c), 50.0f));
    pkp_ui43_post_bake(so.bytes);

    // gmx_fix (T183-T187): Pause root + 1b16778d cursor root
    init_pane(&pause_gp, "PauseGP", 0, 1920, 1080, 0.0f, 0.0f, NULL);
    init_pane(&pause_ani, "Null_ani_all", 0, 1920, 1080, 0.0f, 0.0f, &pause_gp);
    wr_f32(pause_ani.glob, 0x30, -300.0f);
    wr_f32(pause_ani.glob, 0x34, 0.0f);
    init_pane(&pokedex_open, "0300_prt_pokedex_open", 0, 100, 100, 0.0f, 0.0f, &pause_ani);
    pkp_ui43_pre_bake(so.bytes, pokedex_open.pane);
    pkp_ui43_post_bake(so.bytes);

    init_pane(&cur_root, "Null_All", 0, 100, 100, 0.0f, 0.0f, NULL);
    wr_f32(cur_root.glob, 0x00, 4.0f / 3.0f);
    wr_f32(cur_root.glob, 0x14, 4.0f / 3.0f);
    wr_f32(cur_root.glob, 0x30, 404.0f);
    wr_f32(cur_root.glob, 0x34, 224.0f);
    init_pane(&cur_ani, "Null_ani_all", 0, 100, 100, 0.0f, 0.0f, &cur_root);
    init_pane(&cur_pic, "0000_prt_cursor_triangle_000_00", 1, 32, 32, 0.0f, 0.0f, &cur_ani);
    pkp_ui43_pre_bake(so.bytes, cur_pic.pane);
    pkp_ui43_post_bake(so.bytes);

    uint64_t r[31] = {0};
    // C = (404 - (-300)) * 0.25 = 176, (224 - 0) * 0.25 = 56
    wr_f32(cur_ani.glob, 0x30, 404.0f);
    wr_f32(cur_ani.glob, 0x34, 224.0f);
    r[19] = (uintptr_t)cur_ani.pane;
    r[20] = 1; // full recompute
    pkp_ui43_gmx_fix(r);
    assert(nearf(rd_f32(cur_ani.glob, 0x30), 228.0f));
    assert(nearf(rd_f32(cur_ani.glob, 0x34), 168.0f));

    // w20 == 2 (translation fast path): incremental change +40 on x -> C_new = 186 -> delta = +10 -> 268 - 10 = 258
    wr_f32(cur_root.glob, 0x30, 444.0f);
    wr_f32(cur_ani.glob, 0x30, 228.0f + 40.0f);
    wr_f32(cur_ani.glob, 0x34, 168.0f);
    r[20] = 2;
    pkp_ui43_gmx_fix(r);
    assert(nearf(rd_f32(cur_ani.glob, 0x30), 258.0f));
    assert(nearf(rd_f32(cur_ani.glob, 0x34), 168.0f));
}

int main(void) {
    test_lite_and_edge_rules();
    test_bake_stack_nesting();
    test_t175_t143_t135_t200_t205();
    test_fam2_details_cover_and_gmx_fix();
    test_memories_photo_is_4x3();
    puts("pkp ui43: all ScreenLayout rule, bake stack, and gmx_fix tests passed");
    return 0;
}

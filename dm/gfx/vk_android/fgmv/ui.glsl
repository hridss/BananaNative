// SPDX-License-Identifier: MIT
// the UI flow's buffer (vka_fgmv.c ui_*; DM_FG_MV_UIFLOW). One storage buffer of 32-bit words; the offsets below (in words) are also in vka_fgmv.c (UI_*).
// Displays up to 1920x1088 (pc.W x pc.H), tiles 32x32 (60 x 34; past the display: no coverage), Q4 = 4x4 coverage sums (480 x 272), Q16 = 4x4 sums of Q4 (120 x 68).
const int NTX = 60, NTY = 34, NT = 2040, Q4W = 480, Q4H = 272, Q16W = 120, Q16H = 68;
const int O_Q4 = 0;            // 2 slots of 130560 (this frame's is the next frame's A)
const int O_Q16 = 261120;      // 2 slots of 8160
const int O_TILE = 277440;     // per tile: cov, s0, candidate (dy + 64) << 8 | (dx + 64), state (0 inactive, 1 static alone, 2 searched)
const int O_LAB = 285600;      // per tile: its group (the smallest tile index of its 8-connected component; NONE inactive)
const int O_HK = 287640;       // 4096: the (group, candidate) hash table
const int O_EK = 291736;       // per entry: group << 14 | (dy + 48) << 7 | (dx + 48)
const int O_ES = 293776;       // per entry: sum of the members' tile SAD of Q4
const int O_SE = 295816;       // the entries ordered by group
const int O_GCN = 297856;      // per group: entries
const int O_GST = 299896;      // per group: first entry in O_SE
const int O_GCU = 301936;      // per group: cursor
const int O_WO = 303976;       // per tile: first work item (2041)
const int O_GS0 = 306020;      // per group: sum of s0
const int O_GM1 = 308060;      // per group: the smallest candidate SAD
const int O_GM2 = 310100;      // per group: the order key of the best candidate at that SAD
const int O_GD = 312140;       // per group: accepted at Q4: 1 << 31 | (dy + 64) << 8 | (dx + 64), else 0
const int O_GSZ = 314180;      // per group: tiles
const int O_GREF = 316220;     // per group: 26 full-res SADs (25 refine flows, then f = 0)
const int O_GF = 369260;       // per group: final flow 1 << 31 | (fy + 512) << 10 | (fx + 512) when the final accept passed, else 0
const int O_GVM = 371300;      // per group: vmax (float bits)
const int O_GSTAT = 373340;    // per group: 0 -, 1 vetoed by the MV gate, 2 moving, 3 vetoed as inconsistent; a re-picked group stopped earlier: 4 Q4 accept, 5 full-res accept, 6 too small
const int O_MV = 375380;       // per tile: moving 1 << 31 | (fy + 512) << 10 | (fx + 512), else 0
const int O_SH = 377420;       // per tile: paste shift (sy + 512) << 10 | (sx + 512)
const int O_DN = 379460;       // per destination tile: number of sources
const int O_DL = 381500;       // per destination tile: 8 source tiles (raster order)
const int O_ML = 397820;       // the moving tiles in raster order
const int O_CNT = 399860;      // counters (below)
const int O_GE = 399924;       // per group: e(f) (float bits; +inf none), the smallest max(|vy - fy|, |vx - fx|) over the MV gate's samples
const int O_GRP = 401964;      // per group: re-picked 1 << 31 | the first pick pk(d); too many seeds 1 << 30 | their number; else 0
const int O_GSM = 404004;      // per group: 1 when the final accept passed in the small band (2 <= max|f| < 6)
const int O_GEP = 406044;      // per group: e(4 d4) of its first pick (phase 6; +inf none)
const int O_PL = 408084;       // phase 6: the groups to re-pick
// the split rule (usplit.comp, R_SPLIT): per tile flags (1 examined, 2 its changed-pixel test is needed, 4 a pixel changed A -> C, 8 a kept sub-group's tile, 16 a split
// tile (moving), 32 an R2 tile (the rank-2 paste)), changed coverage mass, sub-group ordinal (NONE: none), kept sub-group whose first tile it is (ordinal + 1, else 0);
// the tile list of the changed-pixel pass; per sub-group 16 words (SG_*); the split counters (SC_*)
const int O_SFL = 410124, O_SMS = 412164, O_SLB = 414204, O_SUF = 416244, O_SXL = 418284, O_SG = 420324, O_SCN = 425444;
// the exact speed-ups' words (push constant lev, L_* below): L_SFAST per tile its examined slot and its changed-pixel count, per examined slot 1024 words, the changed
// pixels' list (r | cC << 10); L_RLIST the refine's list of the accepted tiles (in any order)
const int O_SEXS = 425460, O_SCC = 427500, O_SPX = 429540, O_RFL = 757220;
// the split rule's passes (DM_FG_MV_UISPASS) and its extra counters: X_* below (32 words, cleared by usplit.comp phase 0); the sub-groups 320.. of the later passes
// (16 words each, sgb)
const int O_SX17 = 759260, O_SGX = 759292;
const int O_END = 769532;
const int X_MORE = 0, X_P2 = 1, X_P3 = 2, X_IADD = 3, X_IDROP = 4, X_TORN = 5, X_G2P = 6, X_OVR = 7,   // kept units of passes 2 and 3, of pass 2, of pass 3; integrity's
          X_IND = 8, X_PB = 14;   // added and dropped tiles, kept tiles linked to a tile left behind, guard-2 passes, loop over its bound; + 8 / + 11 the indirect
                                  // arguments of passes 2 / 3; + 14 .. + 17 the first ordinal of each pass (and the end)
const int SGTOT = 960;        // sub-groups in a frame: at most 320 a pass, 3 passes
int sgb(uint g) { return g < 320u ? O_SG + int(g) * 16 : O_SGX + int(g - 320u) * 16; }   // a sub-group's words
const int L_SFAST = 1, L_RLIST = 2, L_REPAR = 4;   // the host sets all three (vka_fgmv.c LEV): 1 the split's phase 3 over the changed-pixel lists,
                                      // 2 the refine (not the big pass's) over the list of the accepted tiles, not their box, 4 phase 6's
                                      // re-picks a workgroup per listed group (phase 7), then its accept (phase 8)
const int SG_N = 0, SG_FIRST = 1, SG_WHY = 2, SG_D4 = 3, SG_F = 4, SG_KT = 5, SG_GUARD = 6, SG_R2 = 7, SG_S = 8, SG_S0 = 9, SG_M = 10, SG_NC = 11, SG_PASS = 12;
const int SC_EX = 0, SC_NSG = 1, SC_COVER = 2, SC_NL = 3;
// a sub-group's verdict: 0 keep, 1 no candidate, 2 tilerej, 3 fullrej, 4 massrej, 5 fmin, 6 veto_gate, 7 veto_cons, 8 guard, 9 partial
const int SMAXN = 64, EXMAX = 320, SGMAX = 320;
const int O_SL = O_PL;         // the tiles to search (utile.comp's classify pass; phase 6 uses the words later in the pair)
// counters
const int C_NE = 0, C_W = 1, C_CAP = 2, C_GROUPS = 3, C_ACTIVE = 4, C_MOVG = 5, C_VETO = 6, C_MOVED = 7, C_OVER = 8, C_MAXSRC = 9, C_ON = 10, C_SEARCHED = 11, C_NM = 12,
          C_NEEDBIG = 13, C_BIGS = 14, C_WBIG = 15,   // (a group of more than 128 tiles is "big": it can only overflow, see ugrp.comp)
          C_FL = 16,   // phase 2's 8 workgroups' verdicts, 2 bits each (0 fine, 1 nothing moves, 2 a destination with more than 8 sources): the UI flow is on
                       // when the word is 0; resolve2.comp sets C_MAXSRC, C_ON, C_NM from it
          C_VGATE = 17, C_VCONS = 18, C_REPICK = 19, C_SMALL = 20, C_POVER = 21,   // groups vetoed by the MV gate / as inconsistent, re-picked, kept in the small band, seeds over the cap
          C_PARTU = 22, C_PARTT = 23, C_SRCMX = 24,   // (the tile cap rule) kept groups dropped by the partial keep and their tiles; the most sources into one destination tile
          C_STRIED = 25, C_SKEPT = 26, C_STILES = 27, C_SGUARD = 28, C_SR2 = 29, C_SOVER = 30, C_SNOA2 = 31,   // (the split rule) sub-groups tried, kept, split tiles moving,
                                                                     // dropped by the guard, R2 tiles; more than EXMAX examined tiles; no A2 layer
          C_IND = 32;  // indirect dispatch arguments (x, y, z): + 0 the refine (+ 3, + 4: its first tile column, row), + 5 the same for the big pass, + 10 the big pass's
                       // one-workgroup phases, + 13 its phase 3, + 16 phase 3, + 19 the tile search (utile.comp), + 22 the split's changed-pixel pass, + 25 its sub-groups, + 28 phase 6's re-picks (L_REPAR)
// the rules of the MV gate's push constant word: 1 the gate, 2 MV-consistent pick, 4 kept groups consistent, 8 small moves, 16 the tile cap (UICAP tiles, partial keep;
// groups of more than 128 tiles never move)
const int R_GATE = 1, R_PICK = 2, R_CONS = 4, R_SMALL = 8, R_CAP = 16, R_SPLIT = 32, R_SGUARD = 64, R_SPASTE = 128;   // 32 the split rule, 64 its guard, 128 its rank-2 paste
const uint UICAP = 384u;       // (R_CAP) the most moving tiles
const uint NONE = 0xFFFFFFFFu;
uint pk(ivec2 d) { return uint(d.y + 64) << 8 | uint(d.x + 64); }
ivec2 upk(uint v) { return ivec2(int(v & 255u) - 64, int((v >> 8) & 255u) - 64); }   // (x, y)
uint pkf(ivec2 f) { return 1u << 31 | uint(f.y + 512) << 10 | uint(f.x + 512); }
ivec2 upkf(uint v) { return ivec2(int(v & 1023u) - 512, int((v >> 10) & 1023u) - 512); }   // (x, y)

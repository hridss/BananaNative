// SPDX-License-Identifier: MIT
// shared declarations of the frame generation UI / cut shaders (vka_fgui.c). One set layout for all four.
layout(set = 0, binding = 0, rgba8) uniform readonly image2D imgA;     // previous game frame (chain input)
layout(set = 0, binding = 1, rgba8) uniform readonly image2D imgC;     // this game frame (chain input)
layout(set = 0, binding = 2, std430) buffer Thumbs { float th[]; };     // luma thumbnails: A at 0, C at TW*TH
layout(set = 0, binding = 3, rg16f) uniform readonly image2D mvimg;    // this frame's DLSS motion vectors (raw)
layout(set = 0, binding = 4, std430) buffer Tiles { float tl[]; };      // per tile: n, sum|C-wA|, sC, sA, sCC, sAA, sCA, sum|mv| (render px), nmv
layout(set = 0, binding = 5, rgba8) uniform image2D gen;               // a generated frame (read and written in place)
layout(set = 0, binding = 6, std430) buffer Result { float rs[]; };     // flag, res, corr_med, mvmean, valid tiles, dlss change
layout(set = 0, binding = 7, rgba8) uniform readonly image2D layer;    // this frame's UI layer
layout(push_constant) uniform PC {
    uint W, H;            // picture size
    uint iw, ih;          // motion vector image size (render size)
    float mvx, mvy;       // the game's motion vector scale (raw x scale = render px)
    float t_res, t_corr, t_mv;
    uint flags;           // 1 have mv, 2 DLSS ran in one of A, C only, 4 cut detection on, 8 UI layer over the output, 16 DLSS rule on, 32 repeat forced
} pc;
const uint TW = 240u, TH = 135u, TS = 15u, NTX = 16u, NTY = 9u;

# Native FG: frame generation from the game's own motion vectors and depth

How the host's own frame generator works, what a game must give it, every switch and profile row that steers it, and what it costs. To add a game, follow [Adding a game to Native FG](../guides/native-frame-generation-new-game.md). The switch list that the checks read is [docs/env_switches.tsv](../env_switches.tsv); the tables here say what the switches mean together.

Costs in this page were measured on a OnePlus Pad 3 (Snapdragon 8 Elite, Adreno 830, Turnip driver), Fire Emblem: Fortune's Weave at 30 fps shown at 60, render 1132x638 to display 1920x1080, unless said otherwise. They are GPU milliseconds per generated frame.

## What it is
**Native FG** is a frame generator that runs in the place of LSFG for games that run on the DLSS stand-in (`dlss_standin 1`). It needs no `Lossless.dll`. LSFG estimates motion from the two finished pictures; Native FG reads what the game already computes for DLSS, **per-pixel motion vectors and a depth buffer** at the game's render size. From the previous game frame A and the current game frame C it makes one picture at the time in between (t = 0.5, 2x), under the game's own UI. It interpolates, so the generated picture is shown before C and the game's input latency grows by about one game frame (not measured).

Why it can look better than LSFG on a game that supports it: the motion is exact where the game provides it, occlusion is decided by depth, and the HUD is taken out before generation and put back on top, so labels and bars are not smeared or doubled.

What it does not do: work without the game's DLSS data; estimate motion for things the game gives no vector for (a shadow decal gets a rule of its own, see "Rules in the resolve"); make more than one frame per game frame.

In the launcher, **Frame generation** is Off, Native FG or LSFG (Game settings and the in-game panel). A game whose profile has no `fg_mv on` row uses LSFG under that choice ("Choosing the engine").

## Pipeline
Per game frame the host records one **op-side pass** where the game's DLSS call is replayed. In the present thread it then records the generation passes for the pair (A, C). Everything is compute; the images are the engine's own.

| # | Stage | When | Input | Output | Cost |
|---|---|---|---|---|---|
| 1 | **Op-side pass** (`fgmv/op.comp`, with a variant for the bit-mask dither test) | at the game's DLSS op, once per game frame, in any `DM_VK_DLSS` mode | the op's depth and motion vectors (RG16F, raw x scale = render pixels, direction current to previous) | per frame: Z (R32F copy), DZ (R32F, 3x3 nearest-depth dilation), DMV (RG16F, that neighbour's vector), E (R8, distance to the nearest depth edge, 0 to 5 px), DF (R8UI, screen-door dither flag) | 0.51-0.58 ms. A ring of 8 frames; the images grow to the largest render size seen and are reused below it |
| 2 | **Snapshots** (`vka_fgui.c`) | at the game's passes | the pass whose fragment shader is `fg_ui_pre` (colour target 0 at the pass's end) and the texture the `fg_ui_layer` composite draw samples | the **picture without UI** P and the **UI layer** L (RGBA8, alpha = transmittance), in a ring of copies | Paired with the frame by the render pass that writes the presented window texture. The engine reads P and L in place in that ring instead of copying them (saves 0.21 ms) |
| 3 | **Cut guard** (`fgui/thumb*.comp`, `tile.comp`, `decide.comp`) | present thread, first | 240x135 luma thumbnails of A and C, A warped by C's vectors; whether DLSS ran in both frames | one cut / no-cut decision | A cut shows frame C again. 0.25 ms |
| 4 | **Splat** (`fgmv/splat.comp`) | per generated frame | C's DZ, DMV, DF | a depth-priority key image: each C texel moves by (1-t) of its vector and claims 2x2 texels with one 32-bit `imageAtomicMax` (key = depth code << index bits \| source index; the nearest wins) | 0.68 ms |
| 5 | **Hole fill** (`fill2.comp`) | after the splat | the key image | a source map: each hole takes the source of the 3x3 neighbour with the smallest depth, 6 iterations; a texel still empty takes its own index | 0.47-0.80 ms, depends on the motion |
| 6 | **UI flow** (`uq4`, `utile` with its classify variant, `ugrp`, `uref`; rule `uiflow`) | after the fill | the coverage (255 - alpha) of A's and C's UI layers; C's vectors | a flow per moving UI group, to paste C's layer at the generated time | 0.58-0.80 ms |
| 7 | **Static candidate** (`stws.comp`, two passes; rule `static`) | after the fill | A, C, the field, depth | a half-size weight map where unwarped A and C explain the pair better than the vectors | 0.73 ms |
| 8 | **Resolve + UI composite** (`resolve2.comp`) | last | A and C (Catmull-Rom, 9 bilinear taps), Z, DZ, DMV, E, the source map, the static weight, the UI flow's table, C's layer | the generated frame, RGBA8 at display size | 1.18 ms (the UI composite is about 0.2 of it) |

At start-up, before anything runs, a self-test of 32768 image atomics decides whether the device can run the engine at all.

Total per generated frame in this version: **4.61 ms in Dagsion walking and turning, 4.69 ms in the battle map with the cursor panning** (op side 0.58 / 0.51 included). Battle, per pass: cut guard 0.25, splat 0.68, fill 0.60, UI flow 0.73, static 0.73, resolve with UI 1.18. All rules off cost about 3.4 ms in the version before the speed-ups. Frames are paced by the existing frame-generation pacing (hold, refresh cap, frame limit) through a four-function seam in `vka_framegen.c`.

## Rules in the resolve
1. **Field.** For each display pixel the nearest (by depth) of the 2x2 render texels around it gives the source texel and its vector `v`.
2. **Sampling.** `a = q + t v` in A and `c = q - (1 - t) v` in C, both by Catmull-Rom.
3. **Occlusion.** A side is rejected where the nearest depth of that frame at its sample position is more than `ratio` (5 %) nearer than the field's depth. If only one side is usable it is taken. If both are, they are blended by `t` where the two lumas agree (within `tol`) and C is taken where they do not. A soft band of 5 render pixels around depth edges blends by distance to the edge instead (`soft`).
4. **Both occluded** (`bothfar`): the side whose occluder is farther.
5. **Screen-door dither** (`dither`): where the game dithers near-camera geometry in 2x2 cells, depth is not dilated and the splat is 1x1.
6. **UI flow** (`uiflow`, `uigate`, `uipick`, `uicons`, `uismall`, `uicap`, `uisplit`): world-anchored HUD (NPC names, markers, unit status icons) lives in the UI layer and moves with the world. Without a rule it would show C's position half a step early. The layer's coverage is searched in 32x32-pixel tiles (60x34 tiles: displays up to 1920x1088). Moving tile groups get the flow from the search, are checked against C's vectors under them (`uigate`), and are pasted at the generated time. Three rules refine the gate for look-alike icons and small moves: `uipick` matches again, among the vectors' own moves, a group of at most 64 tiles whose coverage match disagrees with the vectors (an icon next to an identical one is not taken for it); `uicons` keeps a group only when its move agrees with a vector around it (within 8 px or half the move); `uismall` also moves groups by 2 to 5 px when the vectors agree within 2 px (the end of an eased pan). More than 128 moving tiles, or more than 8 sources into one tile, keeps C's layer for the whole frame. With `uicap` the cap is 384 tiles and a frame over it keeps the groups in the order of their first tile while their running total fits, the rest stay at C; a group of more than 128 tiles never moves (the 8-sources rule stays whole-frame). With `uisplit` the tiles of groups that do not move (a label touching the minimap) are searched again as smaller parts, and a part with a supported, consistent move moves alone; next to still HUD its paste keeps the HUD's pixels and restores the label part hidden in C from A (this needs the layer of the frame before A). Screen-space UI (menus, the minimap as a block) stays at C.
7. **Static candidate** (`static`, `staticz`): content with no vector of its own (a character's shadow on the floor) would be warped by the floor's vectors, which smears it. Where unwarped A and C explain the pair better, the unwarped blend is used. The depth gate (`staticz`) keeps it off where the generated surface is in front of what A and C show there (a flickering brazier, thin poles). With `stfix` a screen-fixed overlay drawn into the scene without vectors of its own (a battle cursor that adds light over a moving floor) also takes the unwarped blend: both frames are brighter at the same screen point than their vector prediction, by about the same amount, over a 3x3 block of weight pixels.
8. **UI composite.** Out = `L.rgb + P.rgb * L.a` in linear light, stored sRGB (measured on the first game: 99.98 % of pixels within 1 LSB of the presented picture). It is the only composite implemented.

## Requirements for a game
All must hold. Each failure has a status text and a log line ("Gates and why a game frame is shown again"); none crashes.
1. **A profile with `dlss_standin 1`.** The game's DLSS call is replaced by the host's upscaler (`vka_dlss.c`), which gives the engine its op record.
2. **A usable op record.** Depth and motion textures present and not in an atlas (no offset). NGX flags with `(flags & 0xE) == 0xA`: depth inverted (8, reversed Z, 1 near), motion vectors low-resolution (2), **not** jittered (4). Motion vectors R16G16_SFLOAT; depth a sampled depth image. Render size at most 2^23 texels. Anything else is refused with the flags in the log. Vectors are read as `render pixels = raw x motion scale`, pointing from the current frame to the previous one; that is a convention checked on the first game, not by the code.
3. **UI separation: two fragment shader keys.** `fg_ui_pre` names the pass whose colour target 0 is the picture without UI (RGBA8 UNORM or sRGB). `fg_ui_layer` names the full-screen draw that composites a UI layer (RGBA8, the same size, alpha = transmittance) over it. **A game that draws its UI straight into the picture cannot use the engine yet**: there is no mode without a layer, and the frame is shown again with the reason "no picture without UI".
4. **A window write the host can pair.** The game's next render pass that writes one of the two last presented window textures pairs the frame's snapshots, vectors and op data with that texture. A game that fills its window texture by a copy or blit does not pair and always repeats.
5. **A stable render size.** A size change between two frames repeats that pair. The op images grow to the largest size seen, so a game with dynamic resolution works but repeats a pair at each change.
6. **A frame budget.** About 4.6-4.7 ms of GPU time per generated frame at the first game's sizes on the Adreno 830. The engine adds one game frame of latency and needs a presented rate the panel can show: a 60 fps game on a 60 Hz panel has no free refresh, as with LSFG (the refresh cap handles it).
7. **A hand-checked profile.** `fg_mv on` says someone validated the game: vector sign and scale, the two keys, the composite rule, and which rules (`fg_feature`) hold. The guide gives the steps.

## Gates and why a game frame is shown again
When one of these fails for a pair, the generated slot shows **frame C itself** (under its UI layer): LSFG and Native FG never run on the same frame. The panel shows `generating (Native FG), showing the game frame again: <reason>`. The log gets one line at the 1st, 300th and every 9000th pair of a cause (`fg mv: game frame shown again: ...`), and every 900 pairs a summary (`fg mv: N frame pairs: G generated, ...`).

| Cause | Reason text | Typical origin |
|---|---|---|
| No DLSS op in A or C | no motion data: no DLSS op in this game frame (a map, a full-screen menu) / in the frame before | menus, maps, movies: normal |
| The op was refused | no motion data: the DLSS op's flags %#x are a kind the engine was never validated on / start at an offset (an atlas) / depth or motion vectors cannot be read / render size over 2^23 texels / op images could not be made | a game with other DLSS conventions |
| No picture without UI | no picture without UI: the profile names no pre-UI pass and UI layer / the pre-UI pass key never matched in N frames / the UI composite key never matched / the pre-UI target or UI layer is not RGBA8 / it is WxH, the window's picture WxH / the window's picture is cropped at an offset / no snapshot for this frame's window texture / the frame before had none | wrong or missing keys; a game that pairs by copy; the earlier frame's snapshot already overwritten |
| Input kind changed | the input changed between the picture without UI and the window texture | a frame with and a frame without snapshots in a row |
| Size changed | the render size changed between the two game frames | dynamic resolution |
| Scene cut | none (the guard decided) | camera jumps, menus and the map opening; `DM_FG_CUT_LOG` shows the numbers |

Gates that stop the engine before any pair: no push descriptors or null descriptors, a missing storage format or storage atomics, a failed pipeline, the start-up atomic self-test. Each logs a `[RED]` line. Inside the UI flow the group gates (`uigate` and its three rules) and the caps above are gates too, with counters in the `fg mv ui:` line.

The cut guard's motion-vector rules need the FSR 3.1 upscaler (`DM_VK_DLSS=4`): the vector copy it reads is taken only on that path, while the op pass runs in every mode. In another upscaler mode the guard decides on the picture residual and correlation alone (no mean-motion rule, no "DLSS ran in one frame only" rule).

## Choosing the engine
Native FG is a global choice in the launcher, but only a game whose profile has `fg_mv on` runs the engine. For any other game the host decides **at each choice** (at start, and on a live switch of the in-game panel), never per frame, so the two engines do not alternate:
- with a Lossless.dll present it runs LSFG with the player's saved LSFG settings (multiplier, target rate, flow scale), and the status says `Native FG is not available for this game: using LSFG, ...`;
- without a DLL, frame generation stays off, with the status `... LSFG's DLL is missing, frame generation off`.
A game with a profile never needs the DLL. `DM_FG_ENGINE=mv` forces the engine on any game (bring-up). If the start-up self-test fails on a game with a profile, the generator takes the LSFG chain when the DLL is there, else frame generation is off; the log has a `[RED]` line, the status no fallback text.

## Limits
- **2x only.** One generated frame per game frame at the fixed rate; the engine ignores `DM_LSFG_MULT` and `DM_LSFG_TARGET`.
- **Render size at most 2^23 texels**. The splat key keeps `max(20, bitlen(w*h-1))` bits of source index and gives the rest of the 32 bits to the depth code (12 bits up to 2^20 texels, 9 bits at 2^23). Above 2^20 the depth steps get coarser: 1.6 % at 1080p, 3.1 % at 1440p, 6.25 % at 4K, which is coarser than the 5 % occlusion ratio at 4K.
- **UI flow on displays up to 1920x1088**; larger displays run without it (logged once).
- **Reversed-Z, unjittered, low-resolution vectors only** (the flags above).
- **No mode without a UI layer, no extrapolation, no 3x or 4x.**
- **Thin or very fast objects** break the splat (there is no vector confidence); the static candidate and `soft` only reduce the visible error.
- **Character shadows and UI icons judder at 30 Hz in places**: the static weight's rim flickers from frame to frame, and an icon that neither the search nor the vectors move correctly stays at C.
- **A screen-fixed cursor over a depth edge** (a wall or a rock) still loses its diamond in generated frames: `stfix` needs both full-vector samples unoccluded.
- **Cost depends on the content** (fill 0.47-0.80, UI flow 0.58-0.80).
- **GPL build only** ("Licence").

### The battle cursor vanishes or doubles in generated frames

- **Symptom:** (Android) with Native FG on, Fire Emblem: Fortune's Weave's battle cursor (the yellow diamond and its glow) is missing in every generated frame, or shows two half-bright glows, while the camera follows the cursor at speed (zoomed in, fast pan). Real frames are correct; with the camera orbiting the cursor it is correct.
- **Cause:** the cursor is an additive 3D draw before the upscaler, with no depth write and no motion vectors: its depth and vectors are the floor's. The engine warps it with the floor, both warped samples agree on the floor, so the diamond is replaced by the floor; the static candidate erases the displaced copies but does not restore the true place (it acts only where the warped samples disagree).
- **Fix:** `fg_feature stfix` (`DM_FG_MV_STFIX`): the static pass also takes the unwarped blend where A(q) - C(q - v) and C(q) - A(q + v) are both above 12 luma and alike, both samples unoccluded, eroded by a 3x3 min. Off by default in the core; the game's profile turns it on.
- **Found on:** Fire Emblem: Fortune's Weave on the AYN Thor (Adreno 740) and the OnePlus Pad 3, 2026-11-22: in a battle zoomed in, a fast left pan, 0 cursor pixels in 8 of 8 generated frames before. Not fixed: the cursor over a wall or a rock.

## Profile rows (`game.tsv`)
One value a row; `game.py` validates them (`fg_rows`) and writes `DM_GAME_FG_*` macros into `game_profile.h`. A game with no `fg_*` row gets no macros and the core defaults.

| Row | Values | Default (no row) | Effect | Env override |
|---|---|---|---|---|
| `fg_mv` | `on` / `off`; `on` needs `dlss_standin 1` | off | The engine was validated on this game: Native FG runs on it, any other game falls back to LSFG | `DM_FG_ENGINE=mv` forces the engine |
| `fg_ui_pre` | one fragment shader key, 16 lowercase hex digits | none | The pass whose colour target 0, at the pass's end, is the picture without UI | `DM_FG_UI_PRE` |
| `fg_ui_layer` | one fragment shader key (16 hex) | none | The full-screen draw that puts the UI layer over the picture; the layer is the **last texture it samples** (no binding choice) | `DM_FG_UI_COMP` |
| `fg_feature <rule>` | one rule a row, from the table below | all off | Turns a rule on | the rule's switch |
| `fg_tune <name> <value>` | see "Tuning" | the core default | A tuning number | the number's switch |

`fg_ui_pre` and `fg_ui_layer` go together. The layer goes over each generated frame by transmittance, the only composite there is; a game whose composite differs cannot have the layer rows.

### Rules (`fg_feature`)
| Rule | Bit | Env switch | Needs | What it does | Cost (earlier version) |
|---|---|---|---|---|---|
| `uiflow` | 1 | `DM_FG_MV_UIFLOW` | both UI keys | Moves world-anchored UI with its tile group to the generated time | 0.62 walking / 0.81 battle |
| `uigate` | 2 | `DM_FG_MV_UIGATE` | `uiflow` | Keeps a moving group only where the vectors under it are about as large as its flow | 0.02-0.04 |
| `static` | 4 | `DM_FG_MV_STATIC` (0 off, 1 half size, 2 quarter size) | | Unwarped blend for content without its own vector (shadows) | 0.86; quarter size saves 0.31 |
| `staticz` | 8 | `DM_FG_MV_STATICZ` | `static` | Not where the generated surface is in front of what A and C show | 0.06 |
| `bothfar` | 16 | `DM_FG_MV_BOTHFAR` | | Both sides occluded: the side whose occluder is farther | about 0 |
| `dither` | 32 | `DM_FG_MV_DITHER` | | Screen-door dither regions: no dilation, 1x1 splat | in the op pass |
| `cut` | 64 | `DM_FG_CUT` (`repeat`) | | Scene cut guard: a cut shows frame C again | 0.21 walking / 0.26 battle |
| `uipick` | 128 | `DM_FG_MV_UIPICK` | `uigate` | Re-match a group among the vectors' own moves when its coverage match disagrees with them | together +0.07 |
| `uicons` | 256 | `DM_FG_MV_UICONS` | `uigate` | Keep a group only when its move agrees with a vector around it | |
| `uismall` | 512 | `DM_FG_MV_UISMALL` | `uigate` | Also move groups by 2 to 5 px when the vectors agree | |
| `uicap` | 1024 | `DM_FG_MV_UICAP` | `uigate` | Tile cap 384 with a partial keep instead of the 128-tile whole-frame cliff; groups of more than 128 tiles never move | small, only on frames over 128 tiles |
| `stfix` | 4096 | `DM_FG_MV_STFIX` | `static` | Unwarped blend also for a screen-fixed additive overlay without vectors (a 3D cursor that the camera follows) | about +0.03 (in the static pass) |
| `uisplit` | 2048 | `DM_FG_MV_UISPLIT` (`DM_FG_MV_UISGUARD`, `DM_FG_MV_UISPASTE`) | `uipick`, `uicons`, `uismall` | Move the label part of a group that does not move (a label touching HUD); a guard next to changing HUD; a paste that keeps still HUD | only on frames with groups that do not move |

The first game's rows set every rule but `dither` (mask `0x1fdf`). The costs are those measured on the version before the speed-ups of the same stages (4.91 ms in total); the three UI rules were measured in this version.

### Tuning (`fg_tune`)
| Name | Env | Core default | Range | Effect |
|---|---|---|---|---|
| `ratio` | `DM_FG_MV_RATIO` | 0.05 | 0 to 1 | A side is occluded where its depth is more than this fraction nearer than the generated field's |
| `tol` | `DM_FG_MV_TOL` | 40 | 0 to 255 | Luma difference (8-bit) under which A's and C's samples agree and are blended instead of taking C |
| `edge` | `DM_FG_MV_EDGE_THR` | 1.15 | 1 to 4 | 3x3 depth ratio over which a render texel is a depth edge |
| `fill` | `DM_FG_MV_FILL` | 6 | 0 to 6 (whole) | Hole-fill steps; 0 leaves holes to take C's own vector. 0 saves 0.35 walking / 0.13 battle, 3 saves 0.16 / 0.06 |
| `soft` | `DM_FG_MV_SOFT` | 1 | 0 or 1 | Soft blend within 5 render px of a depth edge (0.11) |
| `uisr` | `DM_FG_MV_UISR` | 12 | 1 to 12 (whole) | UI flow coarse search radius in 16-px cells (12 = +-192 px) |
| `uissupr` | `DM_FG_MV_UISSUPR` | 1 | 1 or 2 | Split rule: a part's tile its move does not explain within 1 px is tested again within 2 px |
| `uisinteg` | `DM_FG_MV_UISINTEG` | 0 | 0 or 2 | Split rule: object integrity (2: a label cut between a kept and a left tile takes the left tile along when weakly supported, else stays whole at C) |
| `uispass` | `DM_FG_MV_UISPASS` | 1 | 1 to 3 (whole) | Split rule passes: what a moving part did not keep is searched again as parts of its own |
| `uisguard` | `DM_FG_MV_UISGUARD` | 1 | 0 to 2 (whole) | Split rule guard: 0 off, 1 any changed still HUD tile next to the part, 2 only its pixels the paste can reach and the move does not explain |
| `cut_res` | `DM_FG_CUT_RES` | 12 | 0 to 255 | Cut guard: mean luma residual after the DLSS warp over which a low-correlation pair is a cut |
| `cut_corr` | `DM_FG_CUT_CORR` | 0.5 | 0 to 1 | Cut guard: median tile correlation under which a large residual is a cut |
| `cut_mv` | `DM_FG_CUT_MV` | 0.220848056537 | 0 to 4 | Cut guard: mean motion over which a pair is a cut. **The row is a fraction of the render width; the switch is render pixels** |

## Which value wins
1. The core default, compiled in `vka_fgprof.c`: no UI keys, every rule off, the tuning above.
2. The game's `fg_*` rows (one host per game, so the macros are constants).
3. The launcher's Frame generation choice: `DM_FG_MENU=mv|lsfg` at start, `nxhost_framegen_engine()` while the game runs.
4. The environment (`env.txt`, the launcher's Extra environment, a live `ctl` line): every `DM_FG_*` switch wins over its row. The host re-reads on each change, so all of them are live except where `docs/env_switches.tsv` says start.
One `[profile] fg: ...` log line lists the values taken.

## Switches
All in [docs/env_switches.tsv](../env_switches.tsv) (the meaning there is the full one). "A/B" marks a switch whose off value is the earlier, slower path with the same output.

| Group | Switches |
|---|---|
| Engine and launcher | `DM_FG_ENGINE` (`mv` / `lsfg`, wins over the launcher), `DM_FG_MENU` (the launcher's choice at start), `DM_LSFG` (frame generation on or off, both engines), `DM_LSFG_MULT`, `DM_LSFG_TARGET`, `DM_LSFG_FLOW`, `DM_LSFG_MIN_FPS`, `DM_LSFG_DLL` (LSFG only, except `MIN_FPS`, which paces both engines), `DM_VK_FG_REFRESH_CAP`, `DM_VK_FG_OWNFMT` (both engines) |
| UI under the generated frame | `DM_FG_UI` (`layer`; implied by the engine, also usable with LSFG), `DM_FG_UI_PRE`, `DM_FG_UI_COMP` (the profile's two keys), `DM_FG_UI_SNAP` (snapshots without using them), `DM_FG_UI_T2T_LOG` |
| Cut guard | `DM_FG_CUT`, `DM_FG_CUT_RES`, `DM_FG_CUT_CORR`, `DM_FG_CUT_MV`, `DM_FG_CUT_DLSS` (a pair with DLSS in one frame only is a cut), `DM_FG_CUT_LOG` |
| Rules and tuning | the rules and numbers above |
| Diagnostics | `DM_FG_MV_DEBUG`, `DM_FG_MV_OPSYNC`, `DM_FG_MV_REPLAY`, `DM_FG_MV_REPLAY_DUMP`, `DM_VK_FG_GPUTIME` and `DM_VK_FG_GPUTIME_SPLIT` |

`DM_FG_MV_DEBUG` shows in the generated frames: 1 state (red A only, green C only, blue blend, dim blue the two disagree), 2 vectors, 3 C's depth-edge distance, 4 the frame without its UI layer, 5 the UI layer the composite uses (6 reaches the replay's static-weight dump only). `DM_VK_FG_GPUTIME=<s>` logs per-pass times every s seconds (`fg gpu:`, `fg mv gpu:` with the cut guard, splat, fill, UI flow, static and resolve apart; `fg mv op gpu:` is valid only with `DM_FG_MV_OPSYNC=1`); with `DM_VK_FG_GPUTIME_SPLIT=1` also the input copy, the shared passes and, for the engine, the UI flow's sub-passes (`fg mv ui gpu:`: coverage, tiles, labels, candidate SADs, pick, refine, final, big pass, paste lists).

The UI flow's counters are logged every 10 s whenever it runs: `fg mv ui: N pairs, M with tiles moved, tiles moved sum S max X, moving groups sum G, overflow O, vetoed gate V cons C, repicked R, small Z[, pick_over P] (work cap W; big groups left B; rules R)`. In a screen-space menu "tiles moved" must be about 0; `vetoed cons` near 0 means the consistency rule rarely overrules the search; `rules` is the bit set of the gate's rules in force (1 gate, 2 pick, 4 cons, 8 small).

## Where the code is
| Path | Part |
|---|---|
| `dm/gfx/vk_android/vka_fgprof.c` | profile defaults, `DM_GAME_FG_*`, engine choice, fallback |
| `dm/gfx/vk_android/vka_fgmv.c`, `fgmv/*.comp`, `vka_fgmv_spv.h` | the engine: op pass, splat, fill, UI flow, static, resolve, self-test, replay, debug views, timing |
| `dm/gfx/vk_android/vka_fgui.c`, `fgui/*.comp`, `vka_fgui_spv.h` | snapshots, window pairing, cut guard |
| `dm/gfx/vk_android/vka_framegen.c` | engine selection, input ring, pacing seam, hand-over to the present (the LSFG chain is below it) |
| `dm/gfx/vk_android/vka_exec.c` (`dlss_eval`) | the op hook (`vka_fgmv_op`) and the cut guard's vector copy |
| `dm/games/game.py`, `dm/games/README.md`, `<id>/game.tsv` | the `fg_*` rows |
| `android/app/.../Lsfg.java`, `nxlaunch.c` | the Off / Native FG / LSFG choice, `nxhost_framegen_engine` |

## Licence
The engine's own files carry an `SPDX-License-Identifier: MIT` header, the generated `*_spv.h` included. The engine is reached only through `vka_fg_frame` in `vka_framegen.c`, which is built with the LSFG support (`NX_LSFG`, GPL-3.0-or-later), and its pacing goes through a four-function seam in that file onto the LSFG pacer (`lsfg/lsfg_pacer.c`). An APK that has it is therefore GPL-3.0-or-later as a whole, as every APK that carries the on-device packer already is ([android/NOTICE.md](../../android/NOTICE.md)). A build with `-PnxLsfg=false` has no Native FG. The algorithm and shaders are this project's own design; Catmull-Rom resampling and forward splatting are public techniques.

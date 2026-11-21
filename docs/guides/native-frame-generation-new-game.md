# Adding a game to Native FG

How to give a game that already runs on the Android host with the DLSS stand-in (`dlss_standin 1`) a Native FG profile. [Native FG](../reference/frame-generation.md) says what the engine is, what it needs and what every switch means. Fire Emblem: Fortune's Weave (`04007EF01F8AE000`) is the worked example.

All switches below are live: put them in `env.txt` or send an `env` line with `ctl`; no rebuild is needed. Use the test app ([android/TESTING.md](../../android/TESTING.md)) and a copied save. The profile rows are the **defaults**; every `DM_FG_*` switch still wins over its row, so a bug report can be reproduced with another set.

## 0. Is the game a fit? (10 minutes, no run)
| Question | Needs | If not |
|---|---|---|
| Does the game call NVN2 DLSS on the stand-in (`dlss_standin 1`)? | yes | No depth or vectors: not possible |
| Does it present at 30 fps or less on a 60 Hz panel? | A generated frame needs a free panel refresh; at 60 fps on 60 Hz the refresh cap leaves no room | Only a 120 Hz panel helps |
| Is its UI drawn into a layer that one full-screen draw composites over the 3D picture? | yes (section 3) | There is no mode without a layer yet: not possible |
| Is about one game frame of added latency acceptable? | yes | Fighting games and the like: do not |
| Are the DLSS flags depth inverted + low-resolution vectors + not jittered, and is the render size at most 2^23 texels? | yes (section 1) | Refused with a reason in the log: the engine needs work first |

## 1. Read the DLSS op
Start the game once with `DM_FG_ENGINE=mv` and look in the log for:
- `DLSS stand-in: colour NVN format ..., output NVN format ..., flags 0x... (LDR|HDR, auto exposure ...)`: once per change of the op's formats and flags.
- `FSR 3.1 evaluation: WxH -> WxH, jitter ..., motion scale X Y, reset N, depth format D, motion format M`: with `DM_VK_DLSS=4`, the first three evaluations.
- `fg mv: first op pass: render WxH, depth VkFormat ..., motion scale ..., flags ..., jitter ..., reset ...`: the engine's own read of the op. If you see `fg mv: <reason>: no motion data` instead, the op was refused and the text says why.

Check, in order:
1. **Flags.** NGX bits: 1 HDR, 2 MVLowRes, 4 MVJittered, 8 DepthInverted, 0x20 DoSharpening, 0x40 AutoExposure. The engine needs `(flags & 0xE) == 0xA`. Fire Emblem: Fortune's Weave has `0x2000000a`; Star Fox has `0x4a`, which fits as well.
2. **Formats.** Motion vectors R16G16_SFLOAT; depth a sampled depth image (D32 here). Depth and motion with offset 0 (no atlas).
3. **Render size.** `render WxH` at most 2^23 texels (1132x638 = 0.72 M here). With dynamic resolution, note the range: every size change repeats one pair.
4. **Motion scale.** `raw x scale = render pixels`; here the scale is (1132, 638), the render size. An odd scale means another convention.

The vector sign and the depth sense are confirmed in section 4, with the debug views, because the engine only generates once the UI keys are in.

## 2. Name the game
In `dm/games/<id>/game.tsv`, with `dlss_standin 1` already there:
```tsv
fg_mv	on
```
`game.py` refuses `fg_mv on` without `dlss_standin 1`. To iterate without rebuilding the profile header, leave the rows out and use the switches of the next sections; they win over the rows anyway. When it works, copy the values into the rows (section 7) and delete the env lines. Until the game has `fg_mv on`, the launcher's Native FG gives it LSFG; `DM_FG_ENGINE=mv` forces the engine for bring-up.

## 3. Find the UI: two fragment shader keys
Native FG generates on the **picture without UI** and puts the **UI layer** back over each generated frame. The game must give both.
1. List the passes at the window size: `DM_VK_RT_LOG=<W>x<H>` (the game's output size, e.g. `1920x1080`). Each pass logs `rt log pass N begin: frame F WxHxL ...` with its targets, and each draw `rt log draw: ... fragment <16 hex>`. Read one frame; `DM_VK_RT_LOG_LINES` (default 400) caps the lines.
2. Find the end of the frame: the **last full-screen draw that samples a separate UI render target** and writes the picture the game presents. Its fragment key is **`fg_ui_layer`**; the layer is the **last texture** the draw samples (no other binding can be named).
3. **`fg_ui_pre`** is the fragment key of the pass before it that writes the 3D picture (tone mapping, post): its colour target 0, at the pass's end, is copied as the picture without UI. Both targets must be **RGBA8 (UNORM or sRGB) and the same size** (here 1920x1080).
4. Test: `DM_FG_UI_SNAP=1 DM_FG_UI_PRE=<hex> DM_FG_UI_COMP=<hex>` takes the snapshots without using them. The log must show `fg ui: snapshots on` and, within a second, `fg ui: first pre-UI snapshot` and `fg ui: first UI layer snapshot`. A wrong key logs nothing, and the later status says `the pre-UI pass key ... never matched`. A target that is not RGBA8 gives `[RED] fg ui: the pre-UI target ... is VkFormat ..., not RGBA8`.
5. **Pairing.** The frame's snapshots go with the window texture its next render pass writes. `DM_FG_UI_T2T_LOG=20` logs the passes and draws that write the presented window texture (`fg ui pass`, `fg ui draw: ... vertex ... fragment ...`) and the copies of at least 1280 px (`fg ui t2t`). Here a full-screen pass (fragment `262bfc7f8d45e140`) copies the finished picture into the 3840x2160 window texture. A game that fills its window texture by a **copy or blit only** has no pass to pair with, gets no op data and always repeats: that needs a change in the engine.
6. **Crop.** The presented picture must be the window texture's crop at 0,0 with the size of the pre-UI target; otherwise the status says `cropped at an offset` or `it is WxH, the window's picture WxH`.

Put the keys in the profile when they work:
```tsv
fg_ui_pre	<fragment key of the pass before the composite>
fg_ui_layer	<fragment key of the composite draw>
```
They go together: `game.py` refuses one without the other.

## 4. Check vectors and depth with the debug views
With the keys set and `DM_FG_ENGINE=mv`, the panel status reads `generating (Native FG)`, and the log shows `fg mv: atomic self-test passed` and `fg mv: present side ready at WxH`. Then, in motion (a camera orbit, a character running, a pan):
- `DM_FG_MV_DEBUG=2` shows the vector at each pixel as colour: red = horizontal, green = vertical, 0.5 (grey with blue 0.5) = no vector, 1/64 of a unit per display pixel. Vectors point from the current frame to the previous one, to where the content was. Pan the camera to the right: the content moves left on screen, each point was further right before, its vector points right and red goes **above** grey, with one tint on the ground and the same direction on every object. If the tint is the opposite, the sign convention differs from the engine's and the game is **not** compatible; no vector-sign row exists. A moving character should add another tint on its body.
- `DM_FG_MV_DEBUG=3` shows the distance to the nearest depth edge (white = 5 px or more): outlines of near objects must be dark, flat areas white. Whole areas dark, or an inverted picture, means the depth is not reversed-Z; the flags check of section 1 should have caught it.
- `DM_FG_MV_DEBUG=1` shows the state: red = only A usable, green = only C, blue = blended, dim blue = A and C disagree. Large red or green bands that follow moving objects are the occlusions. Screen-edge strips on forward motion are normal.
- `DM_FG_MV_DEBUG=4` shows the generated frame without the UI layer; `=5` shows the UI layer the composite uses.

Look at live generated frames and their neighbours: stand still (no change), walk (smooth), spin the camera (edges), open a menu and the map (the guard must show the game frame, with no tearing of the menu).

## 5. The composite rule
The engine puts the UI layer back as `out = L.rgb + P.rgb * L.a` in linear light, the only composite implemented. Verify it on the game: with `DM_FG_UI_SNAP=1`, compare `comp(P, L)` with the presented picture; `DM_FG_MV_DEBUG=5` shows L. Expect 99.9 % of pixels within 1 LSB (here 99.98 %). If the game's HUD is premultiplied with coverage alpha, straight alpha, or blended in sRGB, HUD edges will be too dark or too light in generated frames, and the composite needs a new mode before the game can ship.

## 6. Turn the rules on, one at a time
Each rule is a `fg_feature` row, off for a game with no row. Enable it with its switch first, judge it, then add the row.

| Rule (switch) | Turn on when | Look at | Cost here |
|---|---|---|---|
| `cut` (`DM_FG_CUT=repeat`) | always | map opening, menus, loading, camera jump: the generated slot must be the game frame, not a blend. `DM_FG_CUT_LOG=50` logs residual, correlation and mean motion per decision; set `fg_tune cut_*` so that real cuts fire and ordinary play never does (orbit, run, menu, map, load-in). The mean-motion rule needs the FSR 3.1 upscaler (`DM_VK_DLSS=4`) | 0.25 ms |
| `bothfar` (`DM_FG_MV_BOTHFAR=1`) | rarely harmful | a fast foreground crossing a background | about 0 |
| `uiflow` + `uigate` (`DM_FG_MV_UIFLOW=1`, `DM_FG_MV_UIGATE=1`) | the HUD has world-anchored labels (names, markers, unit icons) | walk past labelled NPCs: a label must move at the NPC's pace in every other frame. Open a screen-space menu or the map: nothing may be pasted (`fg mv ui:` line, tiles moved about 0 in menus) | 0.6-0.8 ms |
| `uipick`, `uicons`, `uismall` (`DM_FG_MV_UIPICK`, `_UICONS`, `_UISMALL`) | the HUD has look-alike icons side by side (a row of portraits) or eased pans | fast pan and slow cursor steps over a row of identical icons: no icon pasted the wrong way. Counters `repicked`, `vetoed ... cons`, `small` in the `fg mv ui:` line | about 0.07 ms together |
| `static` + `staticz` (`DM_FG_MV_STATIC=1`, `DM_FG_MV_STATICZ=1`) | decals or shadows have no vector of their own | the shadow under a walking character: no double edge; flickering objects (fire) not faded | 0.7-0.9 ms |
| `stfix` (`DM_FG_MV_STFIX=1`, with `static`) | a cursor or marker drawn into the scene without vectors, that the camera follows | a fast pan with the camera locked on the cursor: the cursor stays in every frame | about 0.03 ms |
| `dither` (`DM_FG_MV_DITHER=1`) | near-camera geometry is dithered in a 2x2 pattern: a slab of near depth appears at awnings and railings | `DM_FG_MV_DEBUG=3` and a walk past an awning | in the op pass |

Tuning (`fg_tune`): keep the core defaults until a scene shows a problem. `fill` trades 0.35 ms (walking) for holes (0 to 6 steps), `soft` is 0 or 1, `ratio`, `tol` and `edge` are the occlusion, agreement and edge thresholds ([the tuning table](../reference/frame-generation.md#tuning-fg_tune)).

## 7. Write the rows
The rows of Fire Emblem: Fortune's Weave (`dm/games/04007EF01F8AE000/game.tsv`). `game.tsv` takes one value a row with tabs between the key and its values; **a `#` comment must be a line of its own** (a trailing one is read as part of the value and refused). Reasons are in the comments above each group:
```tsv
# The engine runs on this game: DLSS depth reversed Z (D32), unjittered render-size vectors (RG16F, flags 0x2000000a)
fg_mv	on
# The tonemap pass: colour target 0 (RGBA8) is the picture without UI
fg_ui_pre	4e9941d610533734
# The composite draw: the last texture it samples is the UI layer (out = L.rgb + P.rgb * L.a, 99.98 % within 1 LSB)
fg_ui_layer	c478e7c70173f0b8
# World-anchored labels and markers, with the vector gate and its three rules for look-alike icons and small moves
fg_feature	uiflow
fg_feature	uigate
fg_feature	uipick
fg_feature	uicons
fg_feature	uismall
# The character's shadow, with its depth gate; the battle cursor (screen-fixed, without vectors)
fg_feature	static
fg_feature	staticz
fg_feature	stfix
fg_feature	bothfar
fg_feature	cut
# The core defaults written out; cut_mv is 250 px at 1132 render width as a fraction of it
fg_tune	ratio	0.05
fg_tune	tol	40
fg_tune	edge	1.15
fg_tune	fill	6
fg_tune	uisr	12
fg_tune	cut_res	12
fg_tune	cut_corr	0.5
fg_tune	cut_mv	0.220848056537
```
No `dither` row: the game's near-camera dither did not need it. Only the rows that differ from the core defaults need to be written; the tuning lines above are the defaults.

Also required by the repo rules: a section in the game's note (`dm/games/<id>/README.md`) with the passes, the keys, the measured composite rule, the flags, the checks of section 8 and what is known to be wrong; a row in `docs/env_switches.tsv` (and `docs/advanced_settings.tsv`) for any switch you add. `python3 -m unittest dm/tools/tests/test_game_profile` (class `FrameGen`) validates your rows.

## 8. Checks before you call it done
Record the numbers in the game's note.
1. **Self-test and start-up:** `fg mv: atomic self-test passed`; the `[profile] fg:` line shows your keys and rules; no `[RED] fg mv` or `[RED] fg ui` line.
2. **Reasons:** play 3 minutes (gameplay, a menu, the map, a loading screen). The `fg mv: N frame pairs: G generated, game frame shown again: ...` lines (every 900 pairs) must show most pairs generated, and every repeat cause must be explainable: menus and maps have no DLSS op, a loading screen is a cut.
3. **Determinism:** `DM_FG_MV_REPLAY=<tag>` runs the frame pairs listed in `<DM_CTL folder>/fgmv/r<tag>/manifest.txt` through the same passes twice and prints equal hashes in `replay.txt`. The manifest format is in the comment above `mv_replay` in `vka_fgmv.c`; the pair files are raw dumps of two game frames, their depth and motion vectors and the UI layer.
4. **Cost:** `DM_VK_FG_GPUTIME=10 DM_FG_MV_OPSYNC=1`: the `fg mv gpu:` lines give the milliseconds per pass, the `fg mv op gpu:` line the op side (valid only with `DM_FG_MV_OPSYNC`). The sum must leave room in the frame at the game's rate (here 4.6-4.7 ms for 30 to 60). Run two scenes, one with moving UI.
5. **Pacing:** SurfaceFlinger `--latency` of the game's layer: presents per second at the output rate (60.0) and hitches per 10 s (here 0 to 1.7).
6. **Menus and maps:** the `fg mv ui:` counters show about 0 tiles moved in a screen-space menu.
7. **Sizes:** one run at another Resolution (here 1280x720 with render 67 %, render 854x480), to check that nothing depends on the first game's sizes.
8. **Drivers:** the system driver and a Turnip driver (the repo's rule for graphics changes); say which in the PR.
9. **Look:** the owner's visual check on a motion sheet: walk, orbit, menu open and close, a cut. Judge Native FG against LSFG on the same scene.

## 9. When something is wrong
| Symptom | Likely cause | Check |
|---|---|---|
| Status `no motion data: no DLSS op ...` all the time | `dlss_standin` is off, or the game calls DLSS only in some modes | the `DLSS stand-in:` lines |
| `no motion data: the DLSS op's flags ...` | other DLSS conventions | section 1: the engine needs the convention (forward Z, jitter) before this game works |
| `no motion data: render size ... over ...` | the render is over 2^23 texels | section 1 |
| `no picture without UI: ... never matched` | a wrong key, or the pass is not drawn in this mode | `DM_VK_RT_LOG`, the key from a frame where the HUD is visible |
| `... is VkFormat ..., not RGBA8` | the pre-UI target is RGB10A2, RGBA16F or another format | the engine needs a format path |
| `no picture without UI: the frame before had none` now and then | the earlier frame's snapshot was already overwritten | none (rare) |
| Generated frames show the UI doubled or shifted | `uiflow` on but the composite rule differs, or the layer is not the last texture sampled | `DM_FG_MV_DEBUG=4` and `5`, section 5 |
| Generated frames are C unchanged where the camera moves | the cut guard's thresholds are too tight | `DM_FG_CUT_LOG` |
| A slab or boundary of near depth at awnings | `dither` is needed | section 6 |
| A hard edge at the screen border on forward motion | the texture is beyond the screen: a screen-edge strip takes C (expected) | none |
| Pairs shown again with `input changed` | frames alternate between with-UI and without-UI inputs | `DM_FG_UI_T2T_LOG` |

## 10. Candidate games
What to expect from Switch 2 games that have DLSS. None runs on Android here unless said; the notes come from public reports, not from runs.
- **Cyberpunk 2077:** DLSS on in every mode, dynamic resolution 720 to 1080p, 30 fps quality. Sections 1 and 3 decide: a 1080p render fits the 2^23 limit, dynamic resolution repeats a pair at each size change, and the UI is probably drawn after the op at output size, so a layer composite may or may not exist. Best fit by frame rate.
- **Street Fighter 6:** fixed 540p to 1080p, 60 fps in battle. A poor fit (no free refresh at 60 on a 60 Hz panel, latency); only its 30 fps World Tour modes could benefit.
- **Fast Fusion:** the profile exists for the Mac only (no `dlss_standin`); 60 fps in most modes. The same objection as Street Fighter 6 on a 60 Hz panel; its HUD at 4K over the 3D suggests a clean layer boundary.
- **Star Fox** (`0400E88025B1A000`) already has `dlss_standin 1` and flags `0x4a`, which the op check accepts; dynamic resolution and GPU cost are its problems (its note: 19.5 to 20.5 ms a frame at 768x432).

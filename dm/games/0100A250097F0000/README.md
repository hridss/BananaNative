# DRAGON BALL FighterZ (`0100A250097F0000`)

Update 1.33 (v1703936) over the base game, with its add-on unlocks, SDK 4.5.0. Arc System Works / Bandai Namco, Unreal Engine 4.19, NVN, legacy version 4 paks (the shaders inline in the cooked packages), movie module.

**Status (2026-11-24, Android only): playable.** On a Retroid Pocket Nova (Snapdragon 8 Gen 2, Adreno 740, 12 GB, Android 13) with Turnip Gen8 V39: the title, the offline lobby and its warp menu, Local Battle's reception and options, the character select and CPU vs CPU battles, 3 vs 3 to the results screen, at 60 fps (GPU about 7 ms), at 16:9 and at every Aspect ratio choice. The stock Qualcomm driver hangs the GPU at the lobby's warp and draws only the sky in the lobby: pick Turnip. Not run on the Mac or iOS.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `masked_table` jump-table form | `dm/tools/patch_nso.py` | the jump table at main+0x252b78 checks its bound on a masked copy of the index and masks the index again from the source (`and w18,w8,#0xffff; cmp w18,#0x10; b.hi …; and x18,x8,#0xffff; ldrsw x18,[x14,x18,lsl #2]`): the solver kept the bound on `w18` only, so the re-masked `x18` read as 65,536 entries and the rewrite stopped |
| `mem_fileback off` | [`game.tsv`](game.tsv) | in battle the game makes and finalizes an 8 MiB shader-code pool every few seconds. File-backed, each placement read it back from flash (2,065 major faults, 10-34 ms frames) |
| `pool_giveback async` | [`game.tsv`](game.tsv) | each finalize gave the pool back on the render thread (8-19 ms, a dropped frame each time); a worker does it now (`vka_mem.c`) |
| Turnip | Settings › GPU driver | the stock driver: a GPU hang at the lobby's warp |

## Picture shapes

Game settings › Aspect ratio offers 21:9, 4:3, 10:9, 1:1 and Screen shape (`aspect` rows; [Aspect ratio without a variant](../README.md#aspect-ratio-without-a-variant)). The game renders at the chosen shape itself; nothing is stretched. `dm/host/android/dbfz_aspect.c`, [`patches.py`](patches.py), two observation gates on main 1.33:

- **Window** (`OBS_DBFZ_RES` at main+0x1e63a34, right after Unreal's `r.SetRes` sink parsed `<w>x<h>`): the width becomes the shape times the height. Docked at a taller shape the height is 1440 (1920x1440 at 4:3, downsampled to the screen, GPU 7-8 ms; `DM_DBFZ_HEIGHT=1080` keeps the game's); wider keeps 1080 (2520x1080 at 21:9, GPU about 7.3 ms). The render targets and the UMG anchors follow.
- **Cameras** (`OBS_DBFZ_RES | 1 << 8` at main+0x18c87e0, in `FMinimalViewInfo::CalculateProjectionMatrixGivenView` before it reads `bConstrainAspectRatio`): the letterbox is turned off.
  - **Taller:** MaintainXFOV, so the fighters keep their horizontal reach and the view shows more above and below.
  - **Wider:** the horizontal field of view widens by the shape's factor, so the fight keeps its 16:9 framing and more of the stage shows at the sides. Walls, stage limits and the camera's limits are the game's own.
  - **The view info:** it is a fresh copy for each projection; `DM_DBFZ_CAM_LOG` shows the game's own values every frame.
- **Character select:** its backdrop is a flat 16:9 card far behind the fighters, seen by a fixed camera (location 0 800 0, yaw -90, FOV 54).
  - **Without help:** another shape sees black past the card.
  - **The fix:** the camera dollies. It backs off along its axis (`FSceneViewProjectionData::ViewOrigin`, x3) and narrows its field of view by the same factor at the fighters' depth (680). The fighters then keep their size and place, while the card (depth 3650, measured from its width at three dolly distances) grows until it covers.
  - **Taller:** the fighters keep a 1.15 zoom against 16:9 (the team's third fighter just inside the window). What is still uncovered at the bottom is closed by the view info's off-centre projection offset (+0x550).
  - **Wider:** the fighters keep their 16:9 framing.
- **UI** (a vertex hook, `vka_draw.c` `vka_vertex_hook`, on Slate's two vertex shaders; `FSlateVertex` 40 bytes, the position at +24): the game's UMG layout at the window is already uniform and its top widgets sit at the top edge. Its bottom-anchored widgets (meters, assist and Ki counters, dialogue boxes) stop at the bottom of a centred 16:9 band.
  - **The bottom group:** elements ending in the band's last tenth, plus everything touching them. It is found on the distinct element boxes of the last 50 ms, not the previous pass alone: a screen can draw its UI over several vertex buffers, each starting at vertex 0, which once split the reception's name tab.
  - **Taller:** the group moves down to the window's edge.
  - **Wider:** each touching cluster of it moves out to its side.
  - **Full-screen pictures:** a full-width 16:9 one covers a taller window, and one about 16:9 wide (up to 1.2 times: the title's art, the menus' dim panel) covers a wider one.
  - **When a pass starts:** a new UI pass is told by a new Slate vertex buffer starting at vertex 0, not by the renderer's frame counter. One renderer frame can hold two passes or none, and the counter once let a pass straddle a boundary: a one-frame flicker of the title's background and the bottom bar on the first menus.
  - **Not moved:** world-anchored UI (the lobby's name tags and markers) is never moved; the hook takes only screen-space rows.
  - **Menus wider than 16:9:** they keep their centred 16:9 layout, as the other wide games here. Stretching the dialogue's full-width bars alone split the dialogue box (drawn in pieces of several widths).
- **Switches:** `DM_DBFZ_ASPECT=<w>:<h>` forces a shape; `DM_DBFZ_AXIS` picks another axis constraint; `DM_DBFZ_UI=0` (live) leaves the UI as the game draws it; `DM_DBFZ_UI_LOG=<n>` logs element boxes and decisions; `DM_DBFZ_COVER`, `DM_DBFZ_CS_D1`, `DM_DBFZ_CS_D2`, `DM_DBFZ_CS_BACK` and `DM_DBFZ_CS_SHIFT` tune the character select's camera ([env_switches.tsv](../../../docs/env_switches.tsv)).

## Frame rate

60 fps in battle on Turnip V39 (59.9 presented, GPU 7-8 ms at 1920x1440).
- **Battle:** with the two memory keys above, about 3 frames drop in two minutes of battle. They fall on a super's start, a KO's next fighter and the round's end, never in neutral play (matched frame by frame against a screen recording). They come from the game making about 19 effect render targets at once, 20-45 ms on the replay thread.
- **Menus and loads:** longer pauses there, while the game loads data.
- **Tried, no change:** a larger texture streaming pool (`r.Streaming.PoolSize` through a RomFS overlay of `UE4CommandLine.txt`).

## Found on this game

- **Slate passes are not renderer frames.** A UI decision kept per frame must start at Slate's own pass boundary (a new vertex buffer from vertex 0), or a pass that straddles two renderer frames loses state for a frame.
- **Unreal's character select backdrop is a 3D card, not UI.** Widening it needs the camera, not the UI hook. A dolly with a matching field of view keeps the foreground and grows the background.
- **Unreal 4.19's `r.SetRes` sink** is a reliable place to make the whole game render at another shape. Window, targets and UMG anchors all come from `GSystemResolution`.

## Open

- **The stock Qualcomm driver:** the GPU hang at the lobby's warp is not traced.
- **Not checked:** online modes, story mode, arcade.

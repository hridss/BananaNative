# Orbitals (`040032B023CA4000`)

A Switch 2 game: the Deluxe Edition, update v327680 (1.1.2.0-178968 on the title screen), SDK 21.4.2 built for the Switch 2 (`Ounce`). Shapefarm's two-player co-op adventure on Unreal Engine 5.6.1 (`SDK MW+EpicGames+UnrealEngine-5_6_1` in `main`; `SF_GLSL_SWITCH2` / `SP_GLSL_SWITCH2` shaders) on NVN2, with Wwise sound and Bink 2 movies. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`.

**Plays on the Mac** (M6, 16 GB, macOS 27.2, 2026-11-14), two players, in a window: the first-start language, gamma and terms screens, the title, Play Local with both players joined, the character select, the opening anime (Bink) and the first area of the story, both characters walking, at the game's own 30 fps with no red line beyond one known one. The same route also plays in an iPhone's address space on the Mac (`DM_ALIAS_SIZE=0x118000000`, 2026-11-14). Not run on an iPhone or iPad.

**Status on Android:** plays in split screen on an AYN Thor (2026-11-22, Turnip, with the host and translator changes of PRs #601-#606 and the buffer-texture translator change: [Android](#android)); in bring-up since 2026-11-14, ported from upstream's Mac profile; first runs on an AYN Thor Max (Snapdragon 8 Gen 2, Adreno 740, 16 GB, Android 13). The rewriter's Unreal and SDK rules apply to its modules as on the Mac (the FMallocBinned3 cut, the engine region, the SDK 21.4 node area). Its shaders need the game's own Oodle to be read: see [Shaders](#shaders).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x550000000`, `mem_alias 0x400000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | Android: the 16 GB alias region of the other Switch 2 profiles, address space only (upstream's profile has Engage's 5 GB block for an iPhone): see [Memory](#memory). The Switch 2's 9 GB pool |
| `sys_resource 0x1400000` | `game.tsv` | the NPDM declares 20 MB, so virtual address memory is on |
| `depth_mode zero` | `game.tsv` | the game sets near = 0 (`nvnDeviceSetDepthMode`), so pipeline bases made offline use `main0_z` |
| `pad_layout full` | `game.tsv` | a 3D field, both sticks in use |

There is no game-specific patch: the `sdk`'s node area is the SDK-wide rule's cut ([The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area)), and Unreal's allocator and engine region are the rewriter's engine-wide rules ([Unreal's FMallocBinned3 replaced by the host allocator](../../../docs/reference/memory.md#unreals-fmallocbinned3-replaced-by-the-host-allocator)). Everything else it needed is general: see [Found on this game](#found-on-this-game).

## Two players

Orbitals cannot be played alone: Play Local waits for a second player to join, and each picks a character (Maki or Omura). The screen splits in two. On the Mac the keyboard plays with player 1, and each game controller is a player of its own, so a second player needs a controller. Scripted input reaches player 2 through a `DM_CTL` line `P2 <keys>` ([Scripted input](../../../docs/agents/05-debugging.md#scripted-input-and-thread-scheduling)).

## Files

- One [NUP container](../../../docs/development.md#the-packing-script-in-detail) of 17.9 GB: the base game, update v327680 and the Deluxe add-on (`040032B023CA5001`, a licence with no content; the main menu shows the Deluxe concept gallery and a "Bonus content unlocked" notice). No NACP; the name comes from the profile.
- RomFS (9.9 GB, 53 files): `Orbitals/Content/Paks/Orbitals-Switch2.utoc` / `.ucas` (6.8 GB), `Orbitals-Switch2.pak` (110 MB), `global.utoc`, and 35 Bink 2 movies (`.bk2`) under `Orbitals/Content/Movies`: the story's animatics (the opening, `BK2_Orbitals_S01_Animatics.bk2`, 366 MB), memories, UI previews and the startup logos. The game decodes them itself.

## Shaders

- 23,432 unique in [`shader_map.tsv`](shader_map.tsv) (3,556 vertex, 16,459 pixel, 3,415 compute, 2 geometry) from the container's two IoStore shader libraries (global 4,515, game 36,480 shaders), split in 57 s: [Unreal Engine 5.6 IoStore shader libraries for the Switch 2](../../../docs/reference/shaders.md#unreal-engine-56-iostore-shader-libraries-for-the-switch-2). All build: a full Mac build took 19 minutes on the M6 the first time, 6 minutes on chain `73b94575300e` with the Metal compile cache warm.
- Open on Android (resolved upstream by nxnative 4032a11e, not ported yet): 5 global compute shaders (`3ea35c5a0f1ea749`, `45389d82b79dbf0c`, `50c768e8620cf18e`, `868f60d0648f0110`, `c2f9e9baad57e2d0`) stopped at `Ampere instruction 394 … is not implemented`: Unreal's ShaderPrint counter, an image atomic whose handle is in a register.

## Frame rate

- Current: 30 fps, the game's own: it sets present interval 2 at the title (`the game sets present interval 1 → 2`). In the first area: frame time p50 33.3 ms, p95 34.2 ms, an occasional long frame of 200 ms; CPU 113% of one core, 4.7 W.
- Not tried: 60 fps (`fps_target`).

## Memory

- Unreal's engine region: `nninitStartup` reserves 62 GB, and the host gives it what the alias region has room for. Its page allocator never gives address space back ([Unreal 5.6's page allocator never gives address space back](../../../docs/reference/memory.md#unreal-56s-page-allocator-never-gives-address-space-back)), and right at boot it hands out a 1 GB block that stays mostly unmapped (the high-water mark goes from the region's start to 1 GB past it with under 0.25 GB mapped).
- Mac (64 GB alias region): the region gets 62 GB at alias +0.06 GB. Before the node area cut it started at +1.00 GB, past the SDK's 1 GB node area, which no SDK-wide rule matched on this `sdk` build (SDK 21.4.2 for the Switch 2), and the first area took it to alias +5.00 GB with 2.56 GB mapped.
- iPhone (Engage's 5 GB block, a 4.375 GB alias region; run on the Mac with `DM_ALIAS_SIZE=0x118000000`, 2026-11-14): the region gets 4.06 GB at alias +0.06 GB. From the boot through the character select, the opening anime and about two minutes of both players walking in the settlement, the high-water mark rose to +4.00 GB of the region's end at +4.12 GB, with 2.51 GB mapped, and stayed there; no out-of-memory abort.
- Current: in the first area, 6.0 GB resident (footprint), 4.0 GB of Metal allocations, an Unreal heap of 1 GB.
- Open: the room left in the region is about 0.12 GB. Later chapters are not measured; if they run past it, Unreal calls its out-of-memory handler. An iPhone run.

## Saves

- `ProfileSave`, `SharedGameSettings` and `OrbInputUserSettings` after the first-start screens; the next start goes straight to the title. Story progress was not checked across a restart.

## Driving it on the Mac

The work folder has `boot.sh <log> [secs] [VAR=…]` (window + HUD, a renamed host copy, `DM_CTL` at `run/ctl.txt`), `ctl.sh <wait> <shot> [presses…]` and `route.sh` (boot to the character select, then the picks and both ready).

From a save past the first-start screens: at about 40 s the title, `A`; 22 s later the main menu, `A` (Play Local), `A` (player 1 joins), `P2 A` (player 2 joins), `A` (Start). At the character select wait until it takes input (the first presses after it appears are ignored; look with a shot): `Left` (player 1 to Maki), `P2 Right` (player 2 to Omura), then `A` and `P2 A`: the load and the opening anime. The anime shows `HOLD B TO SKIP`, but scripted holds (`B~4`, `B~5`) did not skip it on 2026-11-14 and `Plus` opens the pause menu (`B` closes it): wait it out, about 3 minutes, then the settlement. `LUp` / `P2 LUp` walk.

On a first start the language, gamma and terms screens come before the title: `A` (title), `Down` three times and `A` (Apply), `A` (gamma), `A` (terms).

## Android

AYN Thor Max (Snapdragon 8 Gen 2, Adreno 740, 16 GB, Android 13), the Deluxe NUP, the test app (2026-11-14):

- **Current (2026-11-22, Turnip Mesa main, Docked 1080p, split-screen gameplay with both cameras turning):** the title, Play Local with two players, the character select, the opening and the first area play with the lighting, walls and textures right. The game drops its own resolution floor to 5 % in split screen; with the profile's options (Game options: resolution floor 67 %, temporal upscaling, lighter shading, simpler characters) about 24-25 fps, GPU 38-39 ms a frame; the floor at 50 % about 27 fps. The profile renders in system memory on Turnip (`turnip_env`), joins placed segments always (`place_coalesce_pct 0`), starts in handheld mode (`op_mode handheld`) and recommends Turnip over Qualcomm's drivers before 512.800 (`recommend`). The frame is bound by its geometry (about 7 M primitives a frame, the shader launch block busy almost all of it); 30 fps in split screen needs resolution or geometry cuts.

- **Shaders:** the two IoStore libraries are Oodle-compressed, decoded with the game's own Oodle on the phone (`DM_OODLE_JOB`, one job of 2,748 container blocks, none failed; the shader groups themselves are not compressed again): [Unreal Engine 5.6 IoStore shader libraries for the Switch 2](../../../docs/reference/shaders.md#unreal-engine-56-iostore-shader-libraries-for-the-switch-2). The split gives upstream's 23,432 keys exactly. The Android library: 22,173 translate, 1,046 fail in the translator (mostly `SUATOM handle in bank N`, which upstream's translator takes since its 9507c221), 213 in spirv-val (`ID … has not been defined` 189, an explicit layout on Workgroup memory 24).
- **Driver (current): Turnip, not the stock driver.** On Qualcomm's 512.676 every run crashed within 25 to 75 s, each time elsewhere: the host's completion-effect arrays and the game's own objects (an RHI command list's memory stack, an upload allocation) overwritten with values of other live blocks. Not the shared heap (the same with the Unreal heap in address space of its own), not a write after the game's free (a poisoned quarantine saw none), not the host's double free or a race on its arrays (both checked). Run under `malloc_debug` `free_track`, the process stopped at once on a read of freed memory inside the driver: `libadreno_app_profiles.so` `ApplyApplicationProfile` under `vkCreateInstance`. With Turnip T30 (PurpleVK 26.3.0) the game ran 8 minutes, through the first-start screens, the title, the main menu, Play Local with both players (the second through `DM_CTL` `P2 A`) and the character select, at 30 fps (frame time p50 33.3 ms, p95 about 36 ms), GPU p50 10.6 ms against about 25 ms on the stock driver, the app at 1.4 to 1.9 GB. The profile's `recommend` row picks Turnip on Qualcomm's drivers before 512.800.
- **Open:** the device was lost (GPU hang, `VK_EXT_device_fault` absent) in a submission of 86 draws at the load after the character select (frame 14,467), on Turnip. The title's 3D background is black; the menus draw.

## Found on this game

- Unreal Engine 5.6 IoStore shader libraries for the Switch 2: the split stopped at `shader 0 is not an LSLG record`.
- [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area): the engine region started past a 1 GB node area and did not fit an iPhone's address space; the SDK-wide rule now has this SDK 21.4 build's constructor.
- Ampere forms with a handle in a register: 5 compute shaders stopped at `Ampere instruction 394`.
- A frame the Mac dropped stays queued in the frame history: after both players readied up, the load stopped presenting, and Unreal aborted with `GameThread timed out waiting for …`.
- [A second controller on a player the game does not read](../../../docs/reference/services.md#a-second-controller-on-a-player-the-game-does-not-read): on Android an Xbox controller paired during play ended up on player 3, which this two-player game never reads, and Play Local's controller applet was canceled at every try.
- [Depth and stencil as two planes in a pass that samples its depth target (Android)](../../../docs/reference/rendering.md#depth-and-stencil-as-two-planes-in-a-pass-that-samples-its-depth-target-android): its lights' stencil masks no longer end the lighting pass, about 225 → 103 passes a frame.
- [The controller applet as a join screen](../../../docs/reference/services.md#the-controller-applet-as-a-join-screen): Play Local's controller screen (two players) shows on Android, where the second player joins by L + R.
- [Float storage images bound to integer textures on the Vulkan replay](../../../docs/reference/shaders.md#float-storage-images-bound-to-integer-textures-on-the-vulkan-replay): on Android the GPU timed out about 20 s into the first area; Niagara's draw-argument compute `742f0130cd7af9e4` read particle counts through a float image over an R32_UINT texel buffer and wrote instance counts of about 924 million.
- [A draw skipped for a background pipeline left a kept texture empty](../../../docs/reference/rendering.md#a-draw-skipped-for-a-background-pipeline-left-a-kept-texture-empty-android): on Android, the colour grading table's one draw skipped on a first start; the main menu's backgrounds stayed black.
- [A vertex-heavy depth prepass in GMEM on Turnip (Android)](../../../docs/reference/rendering.md#a-vertex-heavy-depth-prepass-in-gmem-on-turnip-android): 8.7 ms a 600-draw prepass part in GMEM, 5.4 in sysmem; the profile's `turnip_env`.
- [Joining contiguous placed segments holds frames at the same spots](../../../docs/reference/memory.md#joining-contiguous-placed-segments-holds-frames-at-the-same-spots-android): the other way round here: without the joins (upstream's gate, ported on 2026-11-15) the game's pools remade on the same storage were given back and placed again, about 1.3 placements a frame, 16.5-17.5 fps against 17.7-18.1; the profile's `place_coalesce_pct 0`.
- [Wwise loses the span header in a virtual block from Unreal's heap](../../../docs/reference/memory.md#wwise-loses-the-span-header-in-a-virtual-block-from-unreals-heap): Wwise runs on Unreal's heap, which on Android is the host's.
- [A finalized pool's images freed while other holders still had them (Vulkan replay)](../../../docs/reference/nvn-objects.md#a-finalized-pools-images-freed-while-other-holders-still-had-them-vulkan-replay): on Android it crashed a minute or so in, each time elsewhere. On v0.3.53-preview, which had neither this fix nor the Wwise alignment above, it crashed about 15 s after boot in 5 runs of 10 (`main+0x1538fc4`, once in the host on a garbage pointer); with both, in none of 8 (which of the two that needs is not isolated).

## Open
- [A present copy that waited for its frame's timeline value came after the next frame's work (Android)](../../../docs/reference/frame-rate.md#a-present-copy-that-waited-for-its-frames-timeline-value-came-after-the-next-frames-work-android): from the last submission to the screen p50 96-102 ms, 50-54 after.
- [Frame generation's pacing fed on its own present backlog (Android)](../../../docs/reference/frame-rate.md#frame-generations-pacing-fed-on-its-own-present-backlog-android): with frame generation the latency grew from 83 to 163 ms and the game fell from 19 to 15 fps within a minute; 55-57 ms and steady after.

- 60 fps, later chapters (and the engine region's room on an iPhone there: [Memory](#memory)), online play (Gameshare, Play Online), the sound (Wwise opens with no red line; not listened to).
- An IPA (none built yet) and an iPhone or iPad run.
- **Android, resolved (2026-11-16): the GPU timeout about 20 s into the first area.** Cause: Unreal's Niagara draw-argument compute `742f0130cd7af9e4` (global shader 1117) takes each sprite emitter's instance count either from a culled-count buffer texture or, for task flags 0, from its `RWBuffer<uint>` counts (binding 3, an R32_UINT storage texel buffer) with an `imageLoad` through a float image of unknown format; Turnip converted the count (75 read as `0x37160000`, 75 x 2^-23; 100 as `0x37480000`), and the shader wrote that as the instance count of the emitter's indexed indirect draw of one quad (vertex shaders `ec1fb403caef4dd3`, `bdf384c443a2a6ce`). Fix: texel buffer bindings are retyped to their texture's integer class like images ([Found on this game](#found-on-this-game)). Found with `DM_VK_IND_PEEK=4` (the arguments read once the parts before are done, and the dispatch that wrote them: `ind check` lines). Checked on Turnip Gen8 V39: with `DM_VK_IND_PEEK=4` no argument over the limits in 60 s of the first area (before the fix, 2 to 8 a run, from the first seconds); three plain runs reached 27 to 30 fps in the first area with no GPU hang line and no new `gpu timeout` in the kernel log, but each ended within 30 to 60 s when the phone stopped responding (the low-memory killer's `device is not responding`, a `system_server` watchdog backtrace, load average 30): a phone that had been up 33 hours through many GPU hangs; a run after a reboot, and one on the system driver, are still to do. Earlier record of the investigation (2026-11-15/16): the timeout came every run on Turnip T30, mainline and Gen8 V39 (AYN Thor, Adreno 740; `GPU hang: vkWaitSemaphores -4`, the kernel log `adreno-gen7-gmu: gpu timeout`). Background pipelines (`DM_VK_PSO_ASYNC=0`), the pipeline cache, Turnip's LRZ and tiled rendering (`TU_DEBUG=nolrz`, `sysmem`) and indirect counts read on the CPU (`DM_VK_MDI_GPU=0`) did not change it; the GPU never finished the first 20-draw part of the hung submission (`DM_VK_CHUNK=20`, `DM_VK_CHUNK_PASS=20`, `DM_VK_PARTLOG=1`); the arguments guest memory held while the draws were recorded (`DM_VK_IND_PEEK=1`) were sane until a frame or two before, so skipping on them (`=2`) did not help, and a capture (`=3`) ran into the timeout. Open: a run with texel buffers unbound (`DM_VK_TBUF=0`, the counts then read as zero) was recorded as timing out too, which this cause does not explain; not repeated. The runs are scripted with `DM_CTL` (`P2 <keys>` for player 2).

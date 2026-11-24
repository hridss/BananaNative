# Mario Golf: Super Rush (`0100C9C00E25C000`)

Update v262144 (4.0.0) over the base game. SDK 11.4.0, NVN. Camelot's engine on NintendoWare (G3d, Vfx), with NEX and Pia for online play. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`, plus one NRO that `main` loads with `nn::ro`.

**Ported from upstream nxnative `7eaf79116`; not run on Android.** No pack was made and no shader library built here (no game files on this machine). Upstream plays it on the Mac (M6, 2026-11-13): the opening movie, the title, the main menu, Play Golf's menus, character and course select, and Standard Golf on the Rookie Course's first hole at 60 fps; Golf Adventure starts with a Mii the host makes in the editor's place and plays its opening.

**It needs open PR #554 to draw the course.** The game copies each model's set-up out of one scratch recording with `nvnCommandBufferCopyCommands` and records over that recording at once. This tree's `CopyCommands` records a call that reads the source at submit, so without PR #554 (`CopyCommands` copies the records into the caller's command memory: upstream's [A recording's end and links are in control memory](https://github.com/seedds/nxnative/blob/7eaf79116d35a58ed93c1564525bf2a4759b19fb/docs/reference/nvn-objects.md#a-recordings-end-and-links-are-in-control-memory), f02d0474c and a780d2cd6) upstream's reporter saw a course with no golfers, trees or buildings.

Upstream commits for this game: 8d44aaf8f (the profile), 275b11a43 (the 2021 layout of Camelot's archive, PR #2216), b9615a1a3 (a provisional profile's build row names every module, PR #2215), 2fb820088 (front and back stencil references on Metal, PR #2351), 281267503 (a Mii made in the editor's place), 2a40e528b (a hull shader's control points in the stage's circular buffer), 48dca7a4a and 8472ffbc3 (tessellation without a Metal tessellator, PR #2426), 7eaf79116 (wide lines as quads, PR #2510); the storage-takeover index came with PR #2229.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main subsdk0 sdk` | [`game.tsv`](game.tsv) | the NRO imports `movie::Extractor` and the rest of the movie API from `subsdk0`, which `main` does not import. An Android pack loads the modules of `modules` that its ExeFS has (`pack_android.py`, `pack.tsv`); upstream's host loads those of the `build` row, so upstream also lists `subsdk0` there ([A module the NROs import from, missing from the build row](https://github.com/seedds/nxnative/blob/7eaf79116d35a58ed93c1564525bf2a4759b19fb/docs/reference/services.md#a-module-the-nros-import-from-missing-from-the-build-row)), kept here as upstream wrote it |
| `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | upstream's NSP script's guess for a game with a system resource that never sets a heap (Engage's 5 GB block). `nninitStartup` hands `StandardAllocator` an 8 GB address region, past the 4.38 GB alias region; the host's allocator gate gives it what the alias region has room for ([StandardAllocators over 12 to 62 GB of address space](../../../docs/reference/memory.md#standardallocators-over-12-to-62-gb-of-address-space)) |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM declares 16 MB, so virtual address memory is on |

No rewriter patch, no per-game host code. Outside the profile it needs open PR #554 (above) and the shader split's `cra.index` ([One scrambled archive indexed by the NRO](../../../docs/reference/shaders.md#one-scrambled-archive-indexed-by-the-nro)).

## Files

- `main` is 2.3 MB of code. **Current (2026-11-24):** it is a boot stub: the game's code, a 10.7 MB NRO (11,202,560 bytes, SHA-256 `d5dc317e…31c14`, which the RomFS's `.nrr` registers), is packed into `main`'s data segment in three parts, which three threads decode at the start of `nnMain` (main+0x600) before `nn::ro::LoadModule`; no RomFS file holds it. `boot_nro.py` decodes it for 4.0.0 (`main` c83882f4352bc4d9bba8e0907a414c46) and `nro_collect.py` collects it; the split reads the archive's file table from it (`DM_NRO_DIR`). Checked on the game's files, packed from an `.nx`.
- The RomFS is `42` (3.8 GB, Camelot's CRA archive) and the NRR. `42` holds 5,101 files, listed only in the NRO, in the archive's 2021 layout: `dm/gfx/tools/cra.py` reads it ([One scrambled archive indexed by the NRO](../../../docs/reference/shaders.md#one-scrambled-archive-indexed-by-the-nro)).
- Movies are VP9 at 1920x1080, which the game feeds the SDK's decoder in CPU mode (output format 4, NV12).

## Shaders

25,270 in [`shader_map.tsv`](shader_map.tsv), all in `42`; all translate upstream. [`pso_manifest.tsv`](pso_manifest.tsv) (1,310 lines) was recorded over upstream's Mac runs: boot, the movie, the title, the menus, the Rookie Course's first hole. No Android library has been built.

## Rendering

- Upstream on the Mac: 1920x1080; on the first hole frame time p50 16.67 ms, GPU p50 6.5 ms, 3.9 GB resident and 3.6 GB of GPU allocations (M6).
- **Unknown on Vulkan:** upstream's fixes for this game's tessellated grass ([Tessellation stages drawn without a Metal tessellator](https://github.com/seedds/nxnative/blob/7eaf79116d35a58ed93c1564525bf2a4759b19fb/docs/reference/shaders.md#tessellation-stages-drawn-without-a-metal-tessellator)), its aiming and putting grids drawn as lines 4 and 5 pixels wide ([Lines wider than a pixel](https://github.com/seedds/nxnative/blob/7eaf79116d35a58ed93c1564525bf2a4759b19fb/docs/reference/rendering.md#lines-wider-than-a-pixel)) and All-Star Summit's 90,000 shared textures ([A storage takeover walked the sharing table for each texture made](https://github.com/seedds/nxnative/blob/7eaf79116d35a58ed93c1564525bf2a4759b19fb/docs/reference/rendering.md#a-storage-takeover-walked-the-sharing-table-for-each-texture-made)) are in the Metal replay only. Whether the Vulkan replay draws the grass (hull, domain and geometry stages), the wide lines, and loads All-Star Summit in time is not known.
- The hull shader's control points written into the stage's circular buffer are the translator's ([upstream's entry](https://github.com/seedds/nxnative/blob/7eaf79116d35a58ed93c1564525bf2a4759b19fb/docs/reference/shaders.md#a-hull-shaders-control-points-written-into-the-stages-circular-buffer)); this tree's translator has that change. Front and back stencil references ([upstream's entry](https://github.com/seedds/nxnative/blob/7eaf79116d35a58ed93c1564525bf2a4759b19fb/docs/reference/rendering.md#front-and-back-stencil-reference-values), the driving range's decals) were a Metal bug; the Vulkan replay sets each face's own (`vka_draw.c`).

## Open

- **Current: not run on Android.** Nothing here has been checked on a phone: boot, the movie, the menus, a round.
- **Current: needs open PR #554** (above).
- **Current: the player's Mii has no head**, since the MiiModel archive is the host's blank stand-in ([A Mii made in the editor's place](../../../docs/reference/services.md#a-mii-made-in-the-editors-place)); this tree reads no console files.
- Upstream's open items (its note at `7eaf79116`): an intermittent black screen while the movie decodes, cause unknown; one `[RED] texture type mismatch` a run in play; Golf Adventure's driving range not reached; other modes not played; no online play.

## Found on this game

- [A Mii made in the editor's place](../../../docs/reference/services.md#a-mii-made-in-the-editors-place): Golf Adventure asks for a Mii and went back to its save slots at a cancel (upstream's run).
- [One scrambled archive indexed by the NRO](../../../docs/reference/shaders.md#one-scrambled-archive-indexed-by-the-nro): the archive's older layout, which the split did not ask for.
- [The game's code packed into `main` by a boot stub](../../../docs/reference/services.md#the-games-code-packed-into-main-by-a-boot-stub): resolved (2026-11-24). A pack stopped at `[RED] 1 registered NROs not found`: 4.0.0's build was not in `boot_nro.py`, and its stream ends are relocated GOT slots.

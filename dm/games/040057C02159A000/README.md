# Nintendo Switch 2 Welcome Tour (`040057C02159A000`)

A Switch 2 game: update v262144 over the base game, SDK 20.5.29 built for the Switch 2 (`Ounce`), NVN2. NintendoWare's Bezel Engine, modules `main`, `subsdk0`, `subsdk1` (movie), `subsdk3` and `sdk`.

**Status on Android:** in bring-up. The profile, its shader tables and three host answers come from upstream nxnative's pull request [#1425](https://github.com/seedds/nxnative/pull/1425) (open on 2026-11-16), where the game plays on the Mac (M1 Pro, macOS 27.2): past its intro video to the tour, its menus and exhibits, at about 60 fps. It has been packed and its Android shader library built here (below); it has not been run on a phone yet (current).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_alias 0x400000000`, `mem_reserve 0x550000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | the Switch 2's 9 GB, as Splatoon Raiders and Star Fox: with `profile_guess.py`'s 5 GB block (a 4.4 GB alias region and a 1 GB pool) Bezel's allocator found no room at boot on the Mac ([Bezel Engine's two memory setups](../../../docs/reference/memory.md#bezel-engines-two-memory-setups)) |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM declares 16 MB, so virtual address memory is on |
| `modules` without `subsdk2`, `subsdk4` | `game.tsv` | C / libc modules main takes nothing from that the host does not supply |

There is no game-specific patch, and only v262144's build row: the base game alone has not been run.

## Files

- A NUP v2 set: `Nintendo Switch 2 Welcome Tour.nup` (the base) and `Nintendo Switch 2 Welcome Tour.update.262144.nup`. The display version `v262144` is a placeholder: no control data gave a better one.
- RomFS: 722 files, 1.49 GB, most of it Bezel Engine archives (`Archive/*.bea`).

## Shaders

6,766 in [`shader_map.tsv`](shader_map.tsv), from upstream (BNSH / BAHS in the RomFS and `main`; 7 mesh shaders and a task shader of `forward_plus.bfsha` are read by `main`'s mesh reader). On upstream's chain `3df44a5a4e0c` (2026-11-09), 29 failed to translate: 28 on Ampere instruction `0x3ad`, which is not implemented, and one `AST` with an offset register and an address; in 9 more the translator deleted 429 bindless accesses (their results read 0). 6,102 pipeline pairs prebuild.

The Android library (2026-11-16, Windows, vk chain `59e8d600c339`, from the NUP v2 set): the split finds all 6,766; the 12 variations of `gfxshaderrt/*.bnsh` in program slot 6 (ray tracing, stage byte 8) are left out (`shader_split.py` leaves such a shader out alone; before, it dropped the whole archive: the shaders.md entry "A ray tracing shader in NVN2 BNSH program slot 6 drops its whole archive", from the shader fixes pull request). 6,753 translate, into a 45.7 MB `dmsh_vk.lib` (7,515 modules, all valid for spirv-val vulkan1.3); 3.5 minutes with `J=12`. Not translated (current, open): the 7 mesh and the task program of `forward_plus.bfsha` (`mesh program without a mesh layout`: no `mesh` job for them), one hull shader (`AST` with an offset register and an address), and 4 pixel shaders whose reflection has more bindings in one stage than the range. The split also logs that 5 mesh programs' per-primitive values are not their pixel shaders' flat inputs `[36, 40]` (not looked into).

## Controllers

The exhibits use the Joy-Con 2's features: the mouse sensor, the Joy-Con 2 npad styles and HD rumble. Upstream drives them with its Switch 2 controller transport on the Mac (issue [#1267](https://github.com/seedds/nxnative/issues/1267)); on Android the host still answers that no controller has a mouse sensor ([The Joy-Con mouse pointer](../../../docs/reference/services.md#the-joy-con-mouse-pointer)), so the game should run, but its cursor will not move and the mouse exhibits cannot be played (open).

## Open

- No phone run yet: boot, the 9 GB layout on a phone's address space, and the picture are unchecked.
- Ray tracing (upstream, on the Mac): `nvnQueueBuilderSetRayTracingMemorySize`, `nvnProgramBuilderSetRayTracingModules`, `nvnProgramBuilderSetProgramRayTracingFlags`, `nvnProgramBuilderSetMaxTraceRecursionDepth`, `nvnProgramBuilderSetMaxCallableRecursionDepth`, `nvnProgramGetShaderGroupHandle` and `nvnDeviceCalculateAccelerationStructureMemoryRequirements` are unimplemented (`[RED]`, once each at boot), as is `nvnDeviceRegisterFastClearStencil`. What the game draws with them is not known.
- Upstream also logged `[RED] nn::codec H.264: a packet refused with 12 pictures waiting for a released buffer` a few times in the videos, and `[RED] stage 0 cb7 has no uniform buffer bound` once.

## Found on this game

- [`vi::InitializeWithVrrLfcEnabled`](../../../docs/reference/services.md#viinitializewithvrrlfcenabled): the first crash at boot.
- [A Nintendo application's scene id](../../../docs/reference/services.md#a-nintendo-applications-scene-id): a null read as each scene module starts.
- [The application's GPU time span](../../../docs/reference/services.md#the-applications-gpu-time-span): a null read after the first frame.
- [Bezel Engine's two memory setups](../../../docs/reference/memory.md#bezel-engines-two-memory-setups): the virtual-memory path, with the Switch 2 layout.

# Super Mario Sunshine in Super Mario 3D All-Stars (`010049900F546002`)

Program 2 of [Super Mario 3D All-Stars](../010049900F546000/README.md), packed with it, update 1.1.4 (v393216), SDK 9.5.1. Modules `main`, `subsdk0` (the movie library) and `sdk`.

NERD's Hagi layer, as in Super Mario Galaxy ([010099C022B96000](../010099C022B96000/README.md)):

- the GameCube's PowerPC code is recompiled to arm64 in an LZ4-packed NRO (`data/gekko/MarioSunshine/nx/MarioSunshine.nrolz`), which Hagi unpacks and loads with `nn::ro`;
- it renders with Vulkan, with GX shaders generated at run time (upstream captured 1,154 modules on the Mac, `vk_capture.tsv`).

## Status

- **Current (2026-11-24, Android):** packed into the game's pack (`p2/`: `main`, `subsdk0`, `sdk` and the `.nrolz` game as an NRO, no shader library: it has no NVN shaders), and refused at its start: it renders with Vulkan, and the Android host has no guest Vulkan layer ([A game that renders with Vulkan is refused on Android](../../../docs/reference/services.md#a-game-that-renders-with-vulkan-is-refused-on-android)). The launcher program comes back and says why.
- Upstream nxnative plays it on the Mac, 30 fps: the opening, the file select, a new file's plane scene and control at Delfino Airstrip, through its Vulkan layer on Metal (`dm/gfx/vk/hle_vk.m`).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `launch_param lang8 f32:0.5 f32:1` | [`game.tsv`](game.tsv) | what the launcher hands over, for a run of this program alone; without one the game exits at boot |
| 6 GB layout: `mem_reserve 0x180000000`, `mem_alias 0x110000000`, `mem_stack 0x10000000`, `app_pool 0x50000000`, `sys_resource 0x800000` | `game.tsv` | see [Memory](#memory) |
| Coroutine stack region 1 GB → 64 MB | [`patches.py`](patches.py), `main` build `b21e3733…` | see [Memory](#memory). Binding `build`: a size in Hagi's own code. 1.1.4 is the last update |
| `vk_capture.tsv` | profile | the keys of the SPIR-V modules Hagi generates (upstream's capture); it also marks the profile as one with no NVN shaders, so the Android pack has no shader library for it |
| `.nrolz` collected | `dm/tools/nro_collect.py` | a big-endian u32 size, then one LZ4 block; the collector matches the unpacked image against the NRRs ([Loading NROs with `ro::LoadModule`](../../../docs/reference/services.md#loading-nros-with-roloadmodule)) |
| `pad_layout full`, `op_mode handheld` | `game.tsv` | as Galaxy: a 3D pad, and Hagi's preset (`hagi.config`) has its pointer on the touch screen in handheld mode. Sunshine reads only the pad |

## Memory

Hagi maps the GameCube's 32-bit address space 1:1 in one 4 GB + 2 MB address region, then reserves its coroutine stacks as a second region of 1 GB (`main+0x151218`). The SDK-wide cuts ([The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area)) already apply to this `sdk`, so the node area is 62 MB and regions align to 2 MB. Both regions and a heap do not fit the 6 GB iOS allows one reservation unless the second shrinks: Hagi hands its stacks out with a bump allocator and under 2 MB of them were measured on 1.0.0, so 64 MB is ample. Android takes the same layout.

## What Android lacks for it

- **Guest Vulkan** (as [Super Mario 64](../010049900F546001/README.md#what-android-lacks-for-it)). This game's own needs, from upstream's Mac bring-up: occlusion queries that span render passes, logic op pipelines (`VK_LOGIC_OP_INVERT`, about a dozen at boot), two GX geometry shaders (points and lines expanded to quads, not translated upstream either), and its SPIR-V generated at run time, which the phone's driver can take as it is (no offline library needed, unlike the Mac's Metal translation).

## Controls

A talks and picks a block at the file select, Plus skips scenes; B closes a talk; Minus opens the launcher's pause page (an offline HTML page, not in this tree).

## Found on this game

- [A game that renders with Vulkan is refused on Android](../../../docs/reference/services.md#a-game-that-renders-with-vulkan-is-refused-on-android).

## Open

- Guest Vulkan on Android ([What Android lacks for it](#what-android-lacks-for-it)).

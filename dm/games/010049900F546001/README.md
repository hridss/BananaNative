# Super Mario 64 in Super Mario 3D All-Stars (`010049900F546001`)

Program 1 of [Super Mario 3D All-Stars](../010049900F546000/README.md), packed with it, update 1.1.4 (v393216), SDK 10.8.1. Modules `main` and `sdk`.

Nintendo's N64 layer ("EmuAppNX64") on the Japanese Shindou ROM with English text patched in by Lua scripts:

- the MIPS code is recompiled to arm64 in an NRO (`rom/Stardust_JP/05_UNSMJ3.002.nro`), which the game loads with `nn::ro`;
- a precompiled block list goes through a JIT plugin (`nro/EmuSystemJITPlugin.nro`) under `nn::jit`, 14 MB of code at boot, and a few blocks are compiled in play;
- it renders with Vulkan.

## Status

- **Current (2026-11-24, Android):** packed into the game's pack (`p1/`: `main`, `sdk` and the two NROs the NRRs register, no shader library: it has no NVN shaders). A host built without a guest Vulkan layer refuses it at its start ([A game that renders with Vulkan is refused on Android](../../../docs/reference/services.md#a-game-that-renders-with-vulkan-is-refused-on-android)): the launcher program comes back and shows why as a notice (tested 2026-11-24 on the AYN Thor, Qualcomm's driver and Turnip: [the launcher's note](../010049900F546000/README.md#tested-on-the-phone)).
- **With the guest-Vulkan backend** (its own pull request, branch `guest-vulkan-android`, which also brings upstream's `nn::jit` take-over, `hle_jit.c`, to Android): it plays on the AYN Thor on Qualcomm's driver and on Turnip at 30 fps (frame time 33.4 ms): the Mario head, the file select, a new file through Peach's letter and Lakitu to the castle grounds, and walking toward the castle (tested 2026-11-24 with this branch and that one merged). `nn::jit` makes the 14.2 MB of code at boot in 34-122 ms. What the phone's drivers needed for it is in that pull request's reference entries (R4G4 images, which neither driver samples; a barrier the game records into a command buffer it has already ended, which crashed Qualcomm's driver).
- Upstream nxnative plays it on the Mac (the Mario head, the file select, a new file to the castle grounds, 30 fps), through its Vulkan layer on Metal (`dm/gfx/vk/hle_vk.m`) and its `nn::jit` take-over (`hle_jit.c`), and on iOS from an image of the plugin's output captured on a Mac (`jit_capture.py`).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `launch_param lang8 f32:1.12 f32:2.24` | [`game.tsv`](game.tsv) | what the launcher hands over, for a run of this program alone; without one the game exits at boot |
| `jit_key_data 4:0-7,24-27`, `jit_out_code 4:entries=4,count=0x20004,size=0x20000`, `jit_route` | `game.tsv` | upstream's image an iPhone serves `nn::jit` from: command 4's request leaves 4 padding bytes uninitialised, and its output lists the entry points the game computes from the address it asked for. Not read by this tree's tools |
| `modules main sdk` | `game.tsv` | `subsdk0` (Vulkan), `subsdk1` (OpenGL) and `subsdk2` (GLSLC) are not loaded: the host supplies what `main` takes from them (upstream's Vulkan layer) |
| 3.5 GB layout: `mem_reserve 0xe0000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0` | `game.tsv` | `main` sets a heap of the pool − 64 MB − 520 MB of bss, then cuts it into fixed heaps: with a smaller pool the last size goes negative and the boot crashes |
| `shader_note … no NVN shaders` | `game.tsv` | its shader map is empty (its SPIR-V goes through the Vulkan layer): the Android pack has no shader library for it (`pack_android.py` `no_nvn_shaders`) |

## What Android lacks for it

- On this branch alone, a guest Vulkan layer and `nn::jit`: both are in the guest-Vulkan pull request (a passthrough to the phone's own Vulkan with the Tegra X1's identity, its memory types and the swap chain on the launcher's window; `hle_jit.c` with `nro_load_alone`, the code memory one read + write + execute mapping on Android).

## Controls

A closes text, B jumps, Y attacks; Plus starts at the head, Minus opens the launcher's pause page (an offline HTML page, not in this tree).

## Found on this game

- [A game that renders with Vulkan is refused on Android](../../../docs/reference/services.md#a-game-that-renders-with-vulkan-is-refused-on-android).

## Open

- Without the guest-Vulkan pull request: refused ([What Android lacks for it](#what-android-lacks-for-it)). With it: a course was not played through on the phone, and the pause page (an offline HTML page) is a stand-in that closes at once.

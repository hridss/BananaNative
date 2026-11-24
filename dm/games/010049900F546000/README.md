# Super Mario 3D All-Stars (`010049900F546000`)

Update 1.1.4 (v393216). One game of four programs, packed as one, as on the cartridge: one Android pack holds the programs, and the launcher program starts the games inside it.

| Program | What it is | Android |
|---|---|---|
| 0, the launcher (this profile) | the menu and the soundtrack player: sead / agl / gsys with NintendoWare layouts on NVN, SDK 10.8.1, H.264 movies through the SDK's movie library | see [Status](#status) |
| 1, [Super Mario 64](../010049900F546001/README.md) | Nintendo's N64 layer: the MIPS code recompiled in an NRO, the rest through a JIT plugin under `nn::jit`, on Vulkan | packed; refused at its start (no guest Vulkan on Android yet), back to the launcher |
| 2, [Super Mario Sunshine](../010049900F546002/README.md) | NERD's Hagi GameCube layer: the PowerPC code recompiled in an LZ4-packed NRO, on Vulkan with shaders generated at run time | packed; refused at its start (no guest Vulkan on Android yet), back to the launcher |
| 3, Super Mario Galaxy | the Wii game, built for **32-bit ARM** (its NPDM's 64-bit flag clear) | cannot run: the host runs a game's own code on an arm64 CPU. Its tile, artwork and soundtrack stay in the menu. [Super Mario Galaxy](../010099C022B96000/README.md), the later 64-bit release, has its own profile |

The profiles come from upstream nxnative, where the launcher, Super Mario 64 and Sunshine play on the Mac (Super Mario 64 and Sunshine through its Vulkan layer on Metal, `dm/gfx/vk/hle_vk.m`, and Super Mario 64's `nn::jit` plugin through its host's `hle_jit.c`; this tree's Android host has neither).

## Status

- **Current (2026-11-24, AYN Thor, [Tested on the phone](#tested-on-the-phone)):** the launcher program plays at 60 fps on Qualcomm's driver and on Turnip: the opening movie, the title, the menu with its movies and the soundtrack. Super Mario 64 and Sunshine are refused at their start with the reason, and the launcher program comes back; Galaxy's tile starts the launcher program again.

## How it is one game

- **Profiles.** The key `programs` of this profile lists the others (`1=010049900F546001 2=010049900F546002`); each of those says `program_of` and has its own modules, memory layout and tables, and no row in the games table.
- **One host.** The host built for this profile (`libnxhost_010049900F546000.so`) holds a table of the three (`game.py` `header_multi`): the macros every program has with the same value stay constants, the others read the running program's entry, which `DM_PROGRAM` picks at start (`main.c` `dm_game_cur`).
- **Packing.** `pack_android.py` packs program 0 into the pack folder and each program of `programs` with `--program n` into `<pack>/p<n>/`, with its work folder `<work>/p<n>/`; program 0's `pack.tsv` lists them (`program_pack`). Super Mario 64 and Sunshine have no NVN shaders, so their packs have no shader library.
- **Switching.** [Multi-program applications: `oe::ExecuteProgram`](../../../docs/reference/services.md#multi-program-applications-oeexecuteprogram). The host records the next program for the launcher and ends; the launcher starts the game's screen again with that program ([Programs of one game](../../../android/README.md#programs-of-one-game)).
- **Launch parameters.** The launcher hands each game 16 bytes: its language code and two volumes, which differ by game (Super Mario 64 1.12 and 2.24, Sunshine 0.5 and 1, Galaxy 0.35 and 0.7). A game's `launch_param` gives the same when it is run alone.
- **Saves.** One save folder, the application's, as on Horizon; each program keeps its own files in it.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | `game.tsv` | what the survey's rules give for a game with no system resource; `main` sets a heap of the pool − 64 MB |
| `modules main subsdk0 sdk` | `game.tsv` | `subsdk0` plays the menu's movies |
| `render_cond on` | `game.tsv` | the menu wraps its clears in conditional rendering; without evaluating it every frame is black ([Conditional rendering evaluated at replay](../../../docs/reference/rendering.md#conditional-rendering-evaluated-at-replay)) |
| the extension font's rings | `dm/tools/make_ext_font.py` | the launcher takes U+E06B as its font's alternate character at boot without a null check (`main+0x63780` on 1.1.4): the stand-in draws a ring for every code point of U+E000-E1FF it has no icon for |

No rewriter patch: nothing here depends on a build.

## Galaxy's tile

- **Current:** A on the tile calls `oe::ExecuteProgram(3)`. No program 3 is packed, so the launcher starts again (a red line in the log, [the rule](../../../docs/reference/services.md#multi-program-applications-oeexecuteprogram)): the screen fades, and the title comes back.

## Tested on the phone

2026-11-24, AYN Thor (Adreno 740, Android 13, 11 GB RAM), the player's own console dump as one `.nx` (programs 0-3, update 1.1.4), packed on a PC (`pack_android.py`: program 0 with its 3,900-shader library, 16 MB; `p1/` 21.5 MB with two NROs; `p2/` 59.2 MB with the `.nrolz` game; 128 MB in all), the test app built from the working branch at its shader chain `ea01255b0e46` with this change merged (and the reader fix for this `.nx` of the `.nx` support, which upstream does not have yet). The first runs were on a build of the newer chain `ec7e76fe202a`, with the same results:

| Driver | What ran | Result |
|---|---|---|
| Qualcomm's own (`DM_VK_DRIVER=system`, driver ID 8, version 0x802a4035 built 2023-12-27) | the opening movie, the title (the button icon drawn), the menu with each tile's movie and the soundtrack; A on Super Mario 64, on Sunshine and on Galaxy | 60 fps; frame time p50 16.7 ms, p95 18.2-19.6 ms, GPU p50 1.7-2.0 ms. Each game's start: its program refused with the red line, the launcher program back in about a second with the line as a notice; Galaxy: the launcher program again. No crash, no other red line |
| Turnip v27.0.0-R3 (the launcher's pick, driver ID 18) | the same, Sunshine then Super Mario 64 | 60 fps; frame time p50 16.7 ms, p95 17.5-18.5 ms, GPU p50 about 1.1 ms. The same switches. No crash, no red line |

Screenshots were taken of the menu, the title and the notice on both drivers (not committed: the game's own pictures).
## Found on this game

General host changes first needed here; each applies to every game:

- [Multi-program applications: `oe::ExecuteProgram`](../../../docs/reference/services.md#multi-program-applications-oeexecuteprogram).
- [A game that renders with Vulkan is refused on Android](../../../docs/reference/services.md#a-game-that-renders-with-vulkan-is-refused-on-android) (Super Mario 64 and Sunshine).

## Open

- Super Mario 64 and Sunshine on Android need a guest Vulkan layer (upstream's is Vulkan on Metal) and, for Super Mario 64, `nn::jit` (code made at run time); see their notes.
- The offline HTML pages each game's pause menu opens (upstream's `hle_web.c`) are not in this tree: `web::ShowOfflineHtmlPage` stays cut.

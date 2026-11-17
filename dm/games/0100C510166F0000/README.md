# Chained Echoes (`0100C510166F0000`)

Update v1310720 over the base game, with the Ashes of Elrant DLC (`0100C510166F1001`), SDK 14.3.0. **Unity 2020.3.36f1** (IL2CPP), rendering with **NVN**. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`.

**Status (current): provisional profile.** Peanuts Native (igawa6/peanuts-native) wrote it from a survey of the game files and packed the game with it on Linux; it has not been booted with this build. Upstream nxnative has a profile of update 1.1.1 (v131072, `main` `143439c4…`) that plays on the Mac through the prologue at 60 fps; its memory layout is taken here (below), and its other findings hold for this update as far as the shared modules go: the same `subsdk0` and `sdk` (`2aef2cff…`, Batman: Arkham Knight 1.0.5's, [0100ACD0163D0000](../0100ACD0163D0000/README.md)), whose node area cut and 2 MB region alignment the SDK-wide rules take ([The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area)).

## What was found

- **Modules.** `main` `17b6a110…`, `subsdk0` `c781dd63…`, `sdk` `2aef2cff…`. The rewriter takes all three.
- **NPDM.** 64-bit, 16 MB of system resource, so the game has virtual address memory. `main` also imports `nn::os::SetMemoryHeapSize`.
- **Graphics.** `main` imports `nvnBootstrapLoader` and nothing of EGL, OpenGL, Vulkan or GLSLC.
- **Shaders.** 3,870 after duplicates: 1,896 vertex, 1,900 pixel, 73 compute and 1 geometry shader. They are Unity Shader objects: 2,899 keys first appear in `Data/globalgamemanagers.assets`, 546 in `Data/Resources/` (the engine's built-ins), 130 in `Data/resources.assets`, about 280 in the Addressables bundles (`Data/StreamingAssets/aa/Switch/*.bundle`), and 11 GLSLC outputs are in `main` (Unity's player's own). `shader_pairs.tsv` has 1,899 vertex / pixel pairs, `shader_facts.tsv` the 73 compute shaders' thread groups.
- **Movies.** No movie file in the RomFS. `main` takes 25 functions from `subsdk0` (`movie::Extractor`, `movie::Decoder`: Unity's video player), none of which the host supplies itself, so the module is loaded.
- **Network.** `nn::socket` (24 imports), `nn::nifm`, `nn::ssl`, `nn::friends` (67) and `nn::ec` (63, the shop) are imported, as by every Unity player; the game has no online mode.
- **Sound.** Unity's own audio: two `.resource` files of FSB5 banks (46 MB) and the music in bundles.
- **RomFS.** 661 files, 0.88 GB: 637 UnityFS bundles in `Data/StreamingAssets/aa/Switch` (0.81 GB; the largest, 105 MB, is the DLC's music), the player's files in `Data/`, `Data/Managed/Metadata/global-metadata.dat`. The DLC's own RomFS has two small files; its content is in the update.

## What is guessed

| Setting | Why it is a guess |
|---|---|
| 5 GB block: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `sys_resource 0x1000000` | `profile_guess.py`'s rule for a game with a system resource. Not measured |
| `app_pool 0xCD500000` | upstream nxnative's profile of update 1.1.1, which played the prologue on the Mac with it (alias region up to +3.5 GB of 64 GB, 1.95 GB resident); Peanuts Native's guess was the 1 GB of Patrick's Parabox and AI: THE SOMNIUM FILES - nirvanA Initiative. No heap check has run on either build |
| No `mem_used` | the heap request was not seen |
| `shader_groups /Resources/=engine /StreamingAssets/=assets /Data/=data` | Fire Emblem Engage's groups (Unity 2020.3 with Addressables). They only name the package files |
| `pad_layout full` | a 2D role-playing game; the 3D layout has the D-pad. No file decides it |
| `romfs_marker Data/globalgamemanagers`, `zip_store` | as the other Unity profiles; the rule for the file types |
| `build` row's display version `?` | the import keeps no control data, so the version's name was not read |

`bf_want.tsv` and `pso_manifest.tsv` are empty. No `patches.py`. No `depth_mode`: the NVN depth mode the game sets was not seen (Fire Emblem Engage sets `zero`).

## Open

- Not booted with this update. The run-time keys (`render_cond`, `tex_flush_upload`, `tex_dirty_upload`, `core_preempt`, `present_interval`, …) keep their defaults.
- The memory layout: measured only on 1.1.1 on the Mac (above); the heap `main` asks for was not seen.
- The `sdk` build has no registered cut of its own; upstream's run of 1.1.1 (the same `sdk`) took the five cuts of the two SDK-wide rules. Not checked from this update's rewrite.
- The DLC: what `nn::aoc` must report for the game to open Ashes of Elrant was not checked.
- No readiness rows (`readiness.tsv`): the game is not marked as supported on Android.

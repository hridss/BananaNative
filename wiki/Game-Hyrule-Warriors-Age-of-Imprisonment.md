# Hyrule Warriors: Age of Imprisonment (`04003F40232DE000`)

Supported. On a 16 GB AYN Thor (Adreno 740) with Turnip, a 71-minute session (61,140 frames) played through the initial settings, opening VP9 movie, realtime cutscenes, exploration and full combat stages at 35-56 fps (median 46.4 fps across 337-1,546 draws/frame, holding 35.5 fps at a peak of 1,546 draws/frame). On Adreno 740 pick a Turnip driver; Qualcomm's stock system driver is not tested yet.

<!-- TODO screenshot: Hyrule Warriors: Age of Imprisonment first field -->

## At a glance

| | |
|---|---|
| Status | Supported |
| Version | update 196608 (NUP container with update only, 47 GB) |
| Frame rate | 59.8 fps in menus and VP9 movies, 52-56 fps in exploration fields, 35-53 fps in combat battles (median 46.4 fps across 67 field/combat windows; 35.5 fps at 1,546 draws/frame) |
| Memory | 4.5-6.9 GiB PSS (1,922 MiB Vulkan at frame 60,900: 1,437 MiB placed pools, 421 MiB images, 64 MiB scratch) on a 16 GB handheld. Phones under 12 GB get a required memory warning; under 16 GB gets a degraded memory notice |
| Driver | Tested on Turnip (`Mesa 27.0.0-devel`, Vulkan 1.4.363) ([GPU drivers](GPU-Drivers)); Qualcomm's system driver is not tested yet |
| Package | A `.nup` holding the update only (47 GB, no NACP or icon; `readiness.tsv` notes 160 compute shaders in the full archive request up to 48,128 bytes of shared memory) |

## What to expect

- **Large shader archive and stage pipeline compiles:** the full archive contains 101,860 shaders (0 mesh or task programs). Across a 71-minute session into combat, 498 graphics + 50 compute pipelines compiled with 0 failures and 0 shared-memory rejections; brief frame dips occur the first time a stage compiles new pipelines on the frame path, then settle once cached.
- **Saves:** the game keeps a single flat 8 MB `DATA.BIN` save file created at first boot and updated at autosave checkpoints.

## Settings that matter for this game

Common settings are in [Settings overview](Settings-Overview). No game-specific presets are defined for `04003F40232DE000` yet; tests ran at `1920x1080` with Turnip.

## Known issues

- Qualcomm's own system driver, phones with less than 16 GB RAM, and chapter-end save writes/reloads have not been tested yet.
- If something goes wrong, please open an issue with the log ([Troubleshooting](Troubleshooting)).

More games: [Games](Games). Phones that were tested: [Tested devices](Tested-Devices).

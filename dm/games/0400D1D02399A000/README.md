# The Duskbloods Network Test (`0400D1D02399A000`)

Base build v0 (Switch 2), FromSoftware's engine, SDK 20.5.19 (Ounce), NVN, with NVN2 task and mesh programs (94 in the shader map). The profile is provisional: generated from the survey and corrected by hand where a run showed it wrong. The dump has no control data; the display version `0.900.0` comes from the delivery cache file the game asks for (`regulation_nt_0_900.bin`) and the title menu's "App Ver. 0.900", its third part is assumed.

Status (2026-11-21, AYN Thor, Adreno 740): with the in-host online services the game signs in, reaches its main menu ("ONLINE"), goes through the first setup and plays its opening cutscene, on Turnip and on Qualcomm's own driver. Play after the cutscene is not tried. Not run on the Mac host (black picture there, see Open).

## What it needs

- **The publisher's servers, also for play alone.** The game shows "Online connection is required" without them. The host answers them itself: the in-host online services (`dm/host/online`, [online.h](../../host/online/online.h); `DM_ONLINE_HOSTS` in `docs/env_switches.tsv`). A run sets `DM_ONLINE_HOSTS=npln.srv.nintendo.net,npln.nintendo.net,n.n.srv.nintendo.net,p.srv.nintendo.net,fromsoftware-game.net` and `DM_ONLINE_HTTP=<reply table>`. The reply table of the game's own HTTP service (`phx-redirect-nt` and `phx-ping-nt.fromsoftware-game.net`) is the profile's [online_http.tsv](online_http.tsv): 63 rows, each reply its type's fields at their empty values, one local player (ID 1001), one ping server a region. `dm/tools/online_http_table.py` made it from the response schemas of playDB-CNT (github.com/sikoGD/playDB-CNT, which states no licence for its files; the schema file itself is not here). Nothing copies the table to the phone yet: a run puts it beside `env.txt` and names it in `DM_ONLINE_HTTP`.
- **The delivery cache.** The game reads its parameter file through `nn::bcat` as `Regulation/regulation_nt_<major>_<minor>.bin`, named from the display version, and shows "Failed to retrieve latest calibrations" without it. The host serves the delivery cache from a folder (`DM_BCAT_DIR`, default `<DM_ROOT>/bcat/<program ID>`); the file is a copy of the game's own `rom:/regulation_nt.bin` under that name.
- `core_mask 0x3f`: the NPDM allows cores 0 to 5. The game sizes a per-core table from `GetInfo(CoreMask)` (`main+0x51d640`: the bit length of `nn::os::GetThreadAvailableCoreMask`) and indexes it with `nn::os::GetCurrentCoreNumber` (`main+0x51d798`); its NPLN threads ask for core 4. With the host's old answer (cores 0 to 2) the write landed past the table, in a free block of the game's heap, and the allocator crashed at `main+0x46f61dc` on `nn.npln.Worker`. Key: [dm/games/README.md, Keys](../README.md#keys).
- `build … 0.900.0`: see above; the regulation file name is built from it.
- Memory: `app_pool 0x300000000`, `mem_alias 0x220000000`, `mem_reserve 0x548000000` (the game asks for an 8.82 GB heap); about 3.2 GiB in use at the menu and 4.3 GiB in the opening cutscene on the phone.

No `patches.py`.

## Files and shaders

- Shaders: in FromSoftware's containers (DCX with Oodle Kraken, BND4, CGSB), which `shader_split.py` reads; the DCX streams are decoded with the game's own decoder. The UI is Coherent Gameface (shaders `CohStandardVS aa6b2fa6917e66ec`, `CohStandardPS 73090e06557f8850`, `CohStencilPS 8361e38b3adc2a01`, `CohStandardRarePS eed9cb499bd9dfef`); its glyph atlas is drawn on the GPU with a depth-stencil target (vertex `cdbc90e84aebce2f`, fragment `eb493d31b0120ca4`).
- 40 mesh programs, the FLVER material meshes of `shader/gxflvershader.shaderbnd.dcx` (`f73673e585626e6e`, `GXFlver_ColDifSpcBumpEmiIblGlow_Dpt.mpo`, and the other `.mpo` variations), write clip distances 0 and 1 after their generic vectors. The split refused them and the library left them out (`mesh program without a mesh layout`). Since 2026-11-21 their [`shader_facts.tsv`](shader_facts.tsv) rows have mesh layouts with the clip distances as plane codes 133 and 134, as the split now writes them (no other row changes; these programs have no paired pixel shader), and the translator writes the clip distances ([Mesh programs that write clip distances had no mesh layout](../../../docs/reference/shaders.md#mesh-programs-that-write-clip-distances-had-no-mesh-layout)). Not seen on screen yet.
- The final blend (`GXSimple_BlendUI_Upsample_SDR2SDR_F`, `a18dff97d4670366`) builds its texture handles by ORing a word of one uniform block with a word of another, the sampler words in the block the translator takes for the texture's: the Android layer reads both IDs from the OR (`vka_bind.c`).

## Found on this game

- The core mask from the NPDM (`core_mask`).
- [Mesh programs that write clip distances had no mesh layout](../../../docs/reference/shaders.md#mesh-programs-that-write-clip-distances-had-no-mesh-layout): the 40 FLVER mesh programs.
- The delivery cache from a host folder (`hle_fs.c`).
- Fiber pointer authentication in its SDK 20 form: the `sdk` signs the fiber entry with `pacia x1, x9` / `pacia x8, x9` and checks with one `autiasp`, the module's only APIAKey instructions; the loader switches them to the B key (`loader.c` `fiber_pac_apply`).
- gRPC reads a socket option back after setting it and closes the socket when it differs; it passes the C library's `O_NONBLOCK` (0x800) to `nnsocketFcntl` (`hle_online.c`).
- The game's HTTP library uses the system's ssl service through the `nnssl…` C interface (callers at `main+0x971160..0x971f20`): served on the in-host services' sockets (`hle_net.c`).

## Open

- **Qualcomm's driver (512.762): UI text and clipped panels lose square blocks**; clean on Turnip. The UI library's stencil passes are affected; the 3D picture is not. `DM_VK_LRZ_TOUCH=1`, `DM_VK_UBWC=0`, `DM_VK_DS_FIRST_HERE=0` and `DM_VK_ZSHARE=0` change nothing. Cause unknown.
- Two shaders are missing from the library in the opening cutscene (`0515db4f6e826149`, `8baebff206e27d61`).
- Mac host: black picture. The Mac translation guards computed uniform reads with a size block (`c25`) that the Mac host leaves zero.
- No unit test covers the delivery cache, the `nnssl…` hooks or `core_mask` yet; the reference entries for them are not written.

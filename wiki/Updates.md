# Updates

How the app updates itself, what happens to your game's shaders afterwards, and where updates come from. Updates come only from the project's official releases.

## Updating the app

Since v0.1.4 the app offers a newer release at start (**Update** or **Later**), downloads it from the project's [releases](https://git.eden-emu.dev/moceure/BananaNative/releases) and installs it after you confirm. Settings > **Updates** > **Check for updates** checks at any time. Updates download from the official site only.

An update keeps the app's storage, so your saves, packs and copied games stay.

### Big updates and small fixes (0.3.42)

Every release says what kind it is in its notes: **major** (new things, the default), **hotfix** (a small fix, optional) or **critical** (a small fix everyone should take). Settings > **Updates** > **Small fix updates** decides what the app offers when it starts. Turn it off and the app offers only major and critical releases; the next major release always carries every small fix before it, so nothing is lost by skipping them. **Check for updates** always shows the newest release of any kind.

## After an app update

A new version may come with new shaders. What you see depends on what changed:

- **No shader changes:** the pack's shaders are taken over at once.
- **Some shaders changed:** the card says **Ready · updating shaders ...** with the count and time left. Only the changed shaders are translated, in the background, from your dump on the phone, and you can keep playing with the old shaders meanwhile.
- **A quick check first:** when an update changes how shaders are made, the app first checks a sample of a game's existing shaders in the background. If they come out the same, nothing is rebuilt and a short **Shaders up to date** note appears. While it runs, the game's tile shows a small ring and its page has a **Checking shaders** card with a bar and **Skip**. A skipped check is not asked again for 7 days (or until the app updates); the page then shows **Shader check skipped**, and tapping it checks again. The game plays throughout.
- **The app cannot update them itself:** the card says **Ready · update** and the game plays as before. Open the game's page: a small **Shaders can be updated** row has an **Update shaders** button (or its button reads **Prepare shaders**, or **Storage > Prepare again**). The update is optional. A pack made on a computer can be made again there and imported instead ([First start and Prepare](First-Start-and-Prepare)).
- **A small optional row (0.3.40):** when your pack's shaders come from an older version of the app that this version still plays, and the game is a Switch 2 title, the game's page shows a small row, **Shaders: older, built while playing**, with **Update shaders** and **Not now**, instead of the big card (also when the older shaders come from other shader tables). **Not now** hides it for that game until the shaders change again. The game plays either way.

Shader updates can take a long time on a phone for games with many shaders (for example all 8,161 of Pokémon Pokopia `0400A3A01EE4C000` in the worst case); keep the phone charged.

## Shaders built while you play (0.3.40)

A game that asks for a shader your pack lacks gets it built on the phone while you play, with a **Building shaders (N)** label. See [Live shader building](Live-Shader-Building).

## What is new in 0.3.41

- **Fix:** Prepare stopped at "Finding the shaders" with `No module named 'pack_dump'` for games with add-ons (Ocarina of Time, Donkey Kong Bananza, others). Prepare again; nothing else needs doing.
- **Fix:** button prompts show pictures again instead of "?".
- **Fix:** with no game added, the **+** (Add game) button now stays on Home.
- **Home:** a layout choice (row, grid with names, list), an options menu for each game (hold it, or **X** on a controller: Move, Set cover image, Remove game), your own cover pictures with a crop screen, a hint strip at the bottom ([Playing](Playing)).
- **Pokopia (experimental):** Online events, Mods, Host a world ([Pokopia](Game-Pokemon-Pokopia)).

## What is new in 0.3.40

- **Shaders are built while you play**, and Prepare can skip them: [Live shader building](Live-Shader-Building). After an app update a quick sample check can keep your old shaders.
- **A new launcher look** (Switch 2 style): a row of game tiles on Home with a dock, a game page with Play in its top bar, Settings as a list of sections (wide screens: sections at the left, one at the right), a progress ring on a tile that is being prepared. Settings > Appearance > Language picks the app's language (Spanish, Turkish, Chinese, French, Arabic and Polish were machine translated and not yet reviewed).
- **Order of the games**: hold a game on Home, then Move left or Move right ([Playing](Playing)).
- **Ocarina of Time**: sliders for draw distance, model detail, shadow range, foliage distance and fog at the cut, live in the in-game panel ([Ocarina of Time](Game-Zelda-Ocarina-of-Time)); Keep cave walls switches live too.
- **Mario Tennis Fever**: an extended view for wide screens ([Mario Tennis Fever](Game-Mario-Tennis-Fever)). The frame-skip speed fix is not in this version.
- After this update every game asks to be **Prepared again**. This is the short code step only, because the game-code rewriter changed: shaders are not rebuilt in full (your existing shaders are kept or checked, missing ones are built while you play), and saves and settings are kept.

## Release history

The project's release notes are on the releases page. A short list is in the repository's `docs/players/history.md`.

## Safety

Download the APK only from the official releases page. Nobody connected to the project will ask for money, keys or game files.

<p align="center">
  <img src="images/Switify_banner.png" alt="Switify" width="760">
</p>

<h1 align="center">Switify</h1>

<p align="center">
  A native, controller-first Spotify client and background playback overlay for Nintendo Switch homebrew.
</p>

<p align="center">
  <a href="https://github.com/DomazinUS/Switify/releases/latest"><img src="https://img.shields.io/github/v/release/DomazinUS/Switify?label=release" alt="Latest release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/app%20license-GPL--3.0--or--later-1DB954" alt="App license: GPL-3.0-or-later"></a>
  <a href="LICENSES/GPL-2.0.txt"><img src="https://img.shields.io/badge/overlay%20license-GPL--2.0--only-1DB954" alt="Overlay license: GPL-2.0-only"></a>
</p>

Created by **HiroshiYamauchi**.

Switify plays Spotify directly on the Switch. It provides two complementary,
independently installed experiences:

| Component | Current version | Purpose |
| --- | ---: | --- |
| **Switify NRO** | **0.6.3** | Full-screen Home, Search, Library, collection browsing, first-time login, and native playback. |
| **Switify Overlay** | **0.4.3** | Lightweight Ultrahand player and browser backed by an on-demand Atmosphere background service that can keep playing over games. |

The two products do not share a process or live playback engine. They share
only persistent authentication, settings, session metadata, and selected cache
files under `sdmc:/config/Switify/`. Do not run both playback engines at the
same time.

## Features

### Full-screen NRO

- Spotify Home with **All**, **Music**, and **Podcasts** filters
- Search filters for songs, artists, albums, playlists, and podcasts
- Liked Songs, playlists, saved albums, shows, episodes, and collection pages
- Paginated large libraries and responsive playlist rendering
- Persistent mini-player plus full Now Playing and theater modes
- Native shuffle, Previous, Play/Pause, Next, Repeat All, and Repeat One
- Stateful liked-track heart, queue, seeking, and persistent volume
- Optional global `Y`, `L`, `R`, `ZL`, and `ZR` playback shortcuts
- Persistent Bass/Mid/Treble equalizer
- Normal 160 kbps and High 320 kbps Spotify Vorbis quality modes
- Persistent artwork, encoded-image, and texture caches
- Last session restored **paused**; opening Switify never resumes audio by itself

### Ultrahand overlay

- Streamfin-inspired Now Playing card with artwork and progress
- Shuffle, transport, Off/All/One repeat, queue, playlist, and seeking
- Visible, focusable volume slider with persistent percentage
- Lightweight Home, Search, Library, and paginated 25-track collection views
- Shared Normal/High streaming-quality preference
- Background audio that can continue while another title is running
- A dormant service that starts only when the overlay requests it; Switify does
  not reserve its background resources at console boot
- Last session restored paused and silent until an explicit Play command

## Requirements

- A Nintendo Switch capable of running Atmosphere homebrew
- Recent Atmosphere and Homebrew Menu installations
- A Spotify **Premium** account (required by native Spotify playback)
- Network access; a phone or other Spotify client on the same local network is
  needed for the first pairing
- Ultrahand Overlay for the optional overlay package

The standalone NRO should be launched through title override for full memory:
hold `R` while opening a game, continue holding it until Homebrew Menu appears,
then launch Switify.

## Downloads

The [latest release](https://github.com/DomazinUS/Switify/releases/latest)
contains:

- `Switify-0.6.3.zip` — standalone full-screen application
- `Switify-Overlay-0.4.3.zip` — Ultrahand overlay and dormant background service
- `Switify-source-0.6.3.zip` — corresponding buildable source snapshot
- `SHA256SUMS.txt` — SHA-256 checksums for all three archives

## Install the standalone app

1. Download `Switify-0.6.3.zip` and extract it to the root of the SD card.
2. Confirm that the resulting file is
   `sdmc:/switch/Switify/Switify.nro`.
3. Return the SD card to the console and launch Switify through title override.
4. Complete the first login below.

### First login

Switify does **not** require a Spotify developer Client ID or client secret.
The first login uses Spotify Connect discovery:

1. Connect the Switch and a phone to the same local network.
2. Leave Switify open on the Switch while it waits for authentication.
3. Open Spotify on the phone and open **Connect to a device**.
4. Select **Switify**.
5. Wait for the Switch to confirm the connection.

Switify stores the reusable session at
`sdmc:/config/Switify/credentials.json`. Later launches use it automatically,
although playback remains paused until you press Play. The credentials file is
private account data: never attach it to an issue or include it in an SD-card
backup intended for someone else.

If Switify does not appear in the device picker, verify that both devices are
on the same non-guest Wi-Fi network, grant the phone app local-network access,
and retry with Switify still open.

## Install the overlay

Authenticate successfully in the NRO **before** installing or opening the
overlay. The overlay intentionally has no separate login flow.

1. Close the standalone NRO.
2. Download `Switify-Overlay-0.4.3.zip` and extract it to the SD-card root.
3. Confirm these installed files:

       sdmc:/switch/.overlays/Switify.ovl
       sdmc:/atmosphere/contents/4200000000535754/exefs.nsp
       sdmc:/atmosphere/contents/4200000000535754/toolbox.json

4. Reboot once so Atmosphere discovers the replaced service content.
5. Open Ultrahand and select **Switify**. The overlay launches the dormant
   service automatically and then connects to it.

There is deliberately no `boot2.flag`; the service does not start with the
console. Stop the service from Switify before launching a known-incompatible
title or before using the standalone NRO.

## Updating

Extract the newer package over the existing files. For an overlay/service
update, stop the old Switify service first and reboot once after replacing the
Atmosphere content. Authentication, volume, quality, equalizer, caches, and
session state live under `sdmc:/config/Switify/` and are not part of the
release archive.

## Known limitations

- Spotify Premium is required.
- **Nintendo 64 Nintendo Switch Online:** games currently hang while the
  Switify background service is resident, even when playback is paused. Stop
  the service before launching that application. The standalone NRO is not
  affected after it has been closed.
- The standalone NRO and background service are separate native Spotify/audio
  engines and must not run simultaneously.
- Switify streams audio and does not download Spotify music for offline use.
- Switify depends on undocumented Spotify protocols that Spotify may change.

## Diagnostics

Before reporting a problem, preserve the log generated by the affected build:

    sdmc:/config/Switify/app.log
    sdmc:/config/Switify/app.log.previous
    sdmc:/config/Switify/background.log
    sdmc:/config/Switify/background.log.previous
    sdmc:/atmosphere/crash_reports/

Review logs before posting them because account identifiers may be present.
Never publish `credentials.json`. Hardware crash reports can be symbolicated
with `symbolicate-crash.ps1` and the matching private build ELF.

## Build from source

See [BUILDING.md](BUILDING.md) for the devkitPro requirements, standalone and
overlay build commands, outputs, NetLoader loop, and release packaging.

## Repository layout

    app/                 Full-screen UI, catalog, state, and resources
    targets/switch/      Shared libnx audio, player, logging, and discovery
    vendor/cspot/        Modified cspot/bell Spotify and playback stack
    vendor/borealis/     Controller-first UI framework
    phase2/              Background service, IPC, tests, and Ultrahand overlay
    images/              Public repository artwork
    LICENSES/            Consolidated primary license texts and index

## Project history

Switify began as a hardware feasibility proof and grew into the standalone
native application plus an independent background product. The complete public
development history is in [CHANGELOG.md](CHANGELOG.md). Release-specific notes
are in [RELEASE_NOTES.md](RELEASE_NOTES.md).

## Credits

- **HiroshiYamauchi** — creator and maintainer
- [cspot](https://github.com/feelfreelinux/cspot) — Spotify protocol and native playback foundation
- [Borealis](https://github.com/natinusala/borealis) — standalone controller-first UI framework
- [libultrahand](https://github.com/ppkantorski/libultrahand) / libtesla — overlay framework
- [Streamfin Switch](https://github.com/dammitjeff/streamfin-switch) — background-player and overlay UX reference
- [sys-tune](https://github.com/ppkantorski/sys-tune) and sys-clk — sysmodule/IPC architectural references
- [librespot](https://github.com/librespot-org/librespot), [psst](https://github.com/jpochyla/psst), [Spotix](https://github.com/wrexik/Spotix), and [Spotube](https://github.com/KRTirtho/spotube) — protocol, platform, and UX research references

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and
[phase2/THIRD_PARTY_NOTICES.md](phase2/THIRD_PARTY_NOTICES.md) for the exact
attribution boundary.

## License

- The standalone application and background engine are distributed under
  **GPL-3.0-or-later**. See [LICENSE](LICENSE).
- The separately built Ultrahand overlay is **GPL-2.0-only**. Its license is
  retained with the overlay source and consolidated under
  [LICENSES/GPL-2.0.txt](LICENSES/GPL-2.0.txt).
- Borealis is Apache-2.0. Every bundled dependency retains its license beside
  its source; [LICENSES/README.md](LICENSES/README.md) is the public license
  index.

Switify is independent, unofficial, and not affiliated with or endorsed by
Spotify, Nintendo, or their subsidiaries. Spotify and Nintendo names and marks
belong to their respective owners.

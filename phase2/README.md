# Switify Phase 2

This directory is the independent background-playback product. It shares
persistent authentication and optional cached files with the standalone NRO,
but it does not share a process, UI, navigation stack, or runtime playback
state with that app.

## Product boundary

- **Switify NRO:** the full standalone Spotify application and the only
  interactive authentication surface. It writes
  `/config/Switify/credentials.json`.
- **Switify background service:** a dormant, headless Spotify and audio engine.
  It reads the NRO-created credentials and owns background playback after the
  overlay launches it.
- **Switify overlay:** a separately installed Ultrahand/Tesla application. It
  launches/stops the background service and controls it through IPC. It never
  calls into, launches, or requires the NRO.
- **Shared disk data:** credentials are the required shared contract. The
  current artwork URL, persistent artwork cache, and playback-session snapshot
  are also shared through `/config/Switify`.

After the NRO has authenticated once, it can be closed. The overlay and
background service then operate independently. If credentials are missing, the
background service reports that the user must open the NRO and sign in; it does
not expose a second pairing flow.

## Current milestone: release 0.5.0

Version 0.3.5 is withdrawn. Its experimental resource reductions caused missing
audio, indefinitely loading playlist views, and an overlay navigation trap on
hardware. Version 0.3.6 restored the proven 0.3.4 runtime profile. Version
0.3.7 assigned the service to the Applet memory partition and deferred opening
audout until the first PCM frame. Hardware testing showed that neither change
fixed Nintendo 64 Nintendo Switch Online. Version 0.3.8 reduced the remaining
network footprint to Streamfin's two-session TCP-only background socket
profile, but hardware testing confirmed that N64 games still hang whenever the
Switify service is resident, even while playback is paused. The service remains
opt-in and dormant at boot.
Version 0.3.9 moves forward with user-facing controls while retaining the 0.3.8
resource profile. Version 0.4.0 keeps that hardware-tested runtime profile,
updates shared browse/search support, and establishes the public release
metadata and packaging baseline. Version 0.4.1 corrects the shared catalog
search transport without changing playback, IPC, memory, socket, or audio
behavior. Version 0.4.2 adds persistent shared volume and Normal/High streaming
quality, improves conversion and buffering, and adopts smaller browse/artwork
caches for a lighter overlay-service profile. Version 0.4.3 corrects live
Shuffle and repeat behavior. Version 0.4.4 applies the proven lite resource
profile to the artwork build, makes artwork networking just-in-time, remotely
paginates large collections, and restores Nintendo 64 Nintendo Switch Online
compatibility in hardware testing. Version 0.5.0 adds followed podcasts and
consolidates the resilience work: supervised automatic startup, render-thread
isolation, local loading-screen escape, exclusive overlay input, explicit and
coalesced player controls, and resource teardown when Ultrahand closes.

Implemented:

- fixed, versioned C ABI between the sysmodule and overlay;
- 216-byte status snapshot with playback, connection, queue, and track data;
- bounded, thread-safe command mailbox between IPC and the engine;
- libnx client bindings for status and playback commands;
- dormant, on-demand sysmodule scaffold on core 3 using the Applet memory pool;
- host tests for ABI stability, string bounds, state publication, command
  validation, FIFO ordering, and backpressure;
- a developer-only service probe NRO that can inspect IPC but is not the
  launcher or part of the user package;
- an independent Ultrahand overlay that launches its dormant service on open;
- service-manager registration on both ends, plus a one-shot internal Ping
  that validates CMIF independently of the overlay process;
- Atmosphere-ready SD layout generation;
- real background playback that remains active while a game is running;
- a Streamfin-inspired Now Playing card with cached cover art, progress,
  controller-native transport controls, playlist, and queue;
- asynchronous IPC browsing for Home, Search, Your Library, collections, and
  tracks. Home exposes Jump Back In, Recently Played, Top Tracks, playlists,
  and Spotify's personalized shelves;
- demand-loaded 50-track Spotify collection pages presented as 25-song overlay
  pages, so Liked Songs remain complete without downloading up to 1,000 tracks
  before the first page appears;
- the complete libnx exception-path syscall capability set, preventing a
  service fault or interruption from escalating into a nested Mesosphere
  kernel panic;
- bounded exponential backoff for Spotify access-token refresh failures, so a
  temporary DNS outage cannot create a 100 ms retry and log storm;
- a 3 MiB fixed heap and Vorbis-only decoder image, with four audout buffers,
  48 decoded PCM chunks, and smaller, non-duplicating Home, Library, playlist,
  and queue caches;
- a 100-track background playback window around the selected item, retaining
  full paginated browsing for Liked Songs without copying a thousand-track
  library into every native playback layer;
- a local B-button fail-safe: B immediately leaves a Loading view without
  waiting for service IPC, so a sick request cannot trap Ultrahand;
- paused session restoration: the last track, playlist, and position are shown
  after startup, but audio starts only after an explicit Play command;
- Applet-partition process memory, isolating Switify's comparatively large
  6.39 MiB resident image from system-side helpers such as Nintendo's N64 JIT;
- lazy audout acquisition: connecting and browsing do not open audio output.
- two long-lived BSD sessions instead of six, with unused UDP buffers disabled,
  leaving Nintendo online applications access to the shared networking service;
- just-in-time overlay artwork networking: disk-cached JPEGs use no extra BSD
  session, while a cache miss holds one only for the transfer and retains just
  the decoded cover needed by the renderer;
- a visible, focusable volume slider on the main player card, with a numeric
  percentage, 5% controller steps, and a value persistent across sessions;
- three repeat modes (Off, All, and One) exposed by the overlay player;
- Normal 160 kbps and High 320 kbps Spotify Vorbis quality selection, shared
  with the standalone NRO and applied at the next background-service start;
- a compact windowed-sinc 44.1-to-48 kHz converter, larger network/PCM buffers,
  and slow-CDN/PCM-underrun diagnostics;

The production candidate wraps the proven standalone `SpotifyEngine` behind
the mailbox. It reuses `/config/Switify/credentials.json` and
`playback-session.json`, reconnects in the background, publishes live metadata,
and owns audout. Interactive phone pairing remains exclusively in the NRO. Its
current loaded image is about 5.45 MiB including a fixed 3 MiB heap. Heap use is
recorded at startup, after initialization, after a browse play request, and
every 30 seconds. Background logs rotate at 512 KB:

    sdmc:/config/Switify/background.log
    sdmc:/config/Switify/background.log.previous

Build the current engine and overlay from PowerShell:

    .\build-phase2-engine.ps1

Outputs:

    phase2\build_engine\switify-background.nsp
    phase2\dist_engine\atmosphere\contents\4200000000535754\exefs.nsp
    phase2\dist_engine\switch\.overlays\Switify.ovl
    phase2\Switify-Phase2-engine-0.5.0.zip

The installed sysmodule is dormant. There is deliberately no `boot2.flag`, so
Atmosphere will not start it during boot. Opening the overlay launches it once
through `pm:shell`; the overlay then polls readiness without blocking its UI.
If launch fails, the next reboot starts normally with Switify inactive.

### Hardware checkpoint order

1. Stop any currently running Switify background service from the old overlay.
2. Extract `Switify-Phase2-engine-0.5.0.zip` to the SD root and reboot once so
   Atmosphere discovers the replaced ExeFS. The service remains dormant because
   the package contains no `boot2.flag`.
3. Open `Switify` in Ultrahand. The background service starts automatically;
   there is no normal manual launch step.
4. Verify that the last session is shown paused and remains silent until Play
   is pressed. Then verify audio, open a playlist, and confirm B can always
   leave a Loading view. Collection and network requests never execute on
   Ultrahand's render thread.
5. The standalone NRO and service own independent cspot/audout engines. They
   can coexist, but close one unless overlapping playback is intentional.
6. As a resource-compatibility regression check, launch an N64 game once while
   Switify is restored-paused, then once while it is actively playing.

## Boundary rules

1. The IPC thread only reads snapshots and queues fixed-size control messages.
2. The engine thread exclusively owns cspot, sockets, decoder, and audout.
3. The overlay remains a disposable IPC client and never streams audio itself.
4. ABI structures contain only fixed-width integers and fixed-size UTF-8 text.
5. Clients check the API version, structure size, ABI version, and capabilities.
6. The standalone NRO never becomes an overlay client. It stays a complete,
   independent application and shares only persistent authentication plus
   explicitly selected cache/config files.

## Reference projects

- Streamfin demonstrates persistent network audio plus an Ultrahand overlay.
- sys-tune demonstrates the sysmodule, CMIF service, and overlay split.

Both references are GPL-2.0. See `THIRD_PARTY_NOTICES.md` for what was and was
not reused in this scaffold.

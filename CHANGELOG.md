# Changelog

All notable Switify changes are documented here. Standalone NRO and overlay
versions are tracked independently because they are separate applications.

## Standalone NRO 0.6.4 — 2026-08-18

### Added

- Saved shows now use Spotify's native `Podcasts & Shows` `libraryV3` filter
  and appear in both the Podcasts Home filter and a dedicated Saved podcasts
  Library row.
- Restored complete Liked Songs and playlist sessions in the NRO, including
  Play All. Multi-thousand-track collections remain cached after their first
  load, while the interface materializes only 100 rows at a time.

### Changed

- Removed unused AAC, MP3, ALAC, and Opus decoder paths. Switify explicitly
  requests Spotify's 160 or 320 kbps OGG Vorbis streams, so this reduces the
  NRO image and working set without removing a selectable format.
- Shuffle now retries a live queue reorder after cspot finishes promoting the
  current stream. The highlighted state can no longer silently differ from
  the audible queue when Shuffle is pressed during track preparation.

## Overlay and background service 0.4.4 — 2026-08-18

### Fixed

- Restored compatibility with Nintendo 64 Nintendo Switch Online by reducing
  persistent heap, decoder, socket, and artwork resource pressure.
- Liked Songs and playlists no longer wait for up to 1,000 serially fetched
  tracks. The browser fetches 50 remotely paged tracks at a time and retains a
  bounded 100-track playback window.
- The overlay retries automatic service launch four times with bounded
  backoff instead of requiring Retry automatic startup after a slow launch.
- Centered decoded artwork within the player frame and moved the volume
  percentage above its slider.
- Shares the deterministic, deferred Shuffle queue update used by the NRO.

### Changed

- Applied the lite service profile to the artwork-enabled overlay: a 3 MiB
  fixed heap and only the Vorbis decoder paths Switify can actually select.
- Made cover-art networking just-in-time. Cached JPEGs use no overlay BSD
  session; a cache miss claims one session only for the download, releases it
  immediately, discards the encoded buffer after decoding, and retains only
  the currently drawable RGBA cover.

## Standalone NRO 0.6.3 — 2026-08-16

### Fixed

- Shuffle now reorders the queue that is already playing while retaining the
  current stream. Previously the control changed its icon/state but was only
  consulted the next time a playlist was started.
- Repeat One now rebuilds the current track immediately after every natural
  end instead of being checked only when the complete queue was exhausted.
- Repeat All now wraps the native queue directly after its last track instead
  of using a delayed UI-layer restart that could race end-of-playback cleanup.

## Overlay and background service 0.4.3 — 2026-08-16

- Shares the corrected live Shuffle queue and native Repeat All/Repeat One
  implementation with the standalone playback engine.

## Standalone NRO 0.6.2 — 2026-08-16

### Added

- Persistent output volume shared with the background service. The last volume
  selected in either interface is restored on the next session instead of
  returning to 80%.
- Normal (Spotify 160 kbps Vorbis) and High (Spotify 320 kbps Vorbis) streaming
  quality choices in Settings. A quality change applies the next time that
  playback engine starts.
- Playback diagnostics for slow CDN reads and recovered PCM underruns.

### Changed

- Replaced the proof-of-concept linear sample-rate conversion with a compact
  windowed-sinc 44.1 kHz-to-48 kHz converter and a float-first audio path to
  reduce subtle conversion noise and repeated rounding.
- Increased decoded PCM and CDN buffering to better absorb short network or
  scheduler stalls.
- Expanded the in-app Changelog into a controller-scrollable history covering
  every standalone, overlay, and Phase-2 development milestone.

## Overlay and background service 0.4.2 — 2026-08-16

- Shares the persistent output-volume and Normal/High quality settings with the
  standalone NRO.
- Reduces overlay pages to 25 items, trims background catalog duplication,
  keeps only the useful lightweight browse categories, and lowers artwork
  decode/transfer limits.
- Uses the improved resampler, playback buffers, and CDN/underrun diagnostics
  from the standalone audio path.
- Raises the shared IPC API minor version and advertises the new audio-settings
  capability without breaking older major-version clients.

## Standalone NRO 0.6.1 — 2026-08-15

### Fixed

- Restored catalog search to Spotify's authenticated `searchDesktop`
  Pathfinder query. Search no longer consumes the public Web API rate-limit
  bucket and still returns songs, artists, albums, playlists, podcasts, and
  episodes for the filter chips.
- Replaced the overflowing About and Changelog dialogs with full-screen,
  controller-scrollable pages. B and the visible Back button both return to
  Settings.

## Overlay and background service 0.4.1 — 2026-08-15

- Shares the corrected Pathfinder catalog search used by the standalone app.
- Updates release/version markers without changing the hardware-tested 0.4.0
  playback, IPC, memory, socket, or audio profile.

## Standalone NRO 0.6.0 — 2026-08-15

### Added

- Home filters for All, Music, and Podcasts.
- Saved podcast cards and paginated playable show episodes.
- Search filters for Songs, Artists, Albums, Playlists, and Podcasts.
- Search results for playlists, shows, and individual episodes.
- Creator credit and an in-app changelog in Settings.
- New dark-theme Switify icon supplied by HiroshiYamauchi.

### Fixed

- Library save/remove requests now send Spotify URIs in both the query and JSON
  payload, matching the current generic library API behavior.
- The liked-state lookup can no longer overwrite a newer heart mutation.
- Successful heart mutations are verified against Spotify before remaining
  visually committed.

## Overlay and background service 0.4.0 — 2026-08-15

- Retains the stable 0.3.9 player, numeric volume slider, tri-state repeat,
  paused session restoration, and on-demand dormant-service behavior.
- Incorporates the updated shared browse model and public release metadata.
- Keeps the 4 MiB Applet-pool heap, lazy audout, and two-session TCP-only
  compatibility profile.

Known issue: Nintendo 64 Nintendo Switch Online games do not start while the
Switify background service is resident, even when playback is paused. Stop the
service before opening that application.

## Standalone NRO 0.5.3 — 2026-08-15

- Added paged Liked Songs and long Now Playing sessions.
- Restored responsive controller navigation and retained loaded content between
  tabs rather than re-fetching it on every visit.

## Standalone NRO 0.5.0–0.5.2 — 2026-08-15

- Added font-independent shuffle, previous, play/pause, next, and repeat
  artwork; Off/All/One repeat modes; live liked-state UI; optional Y/L/R/ZL/ZR
  shortcuts; and a persistent three-band equalizer.

## Standalone NRO 0.4.9 — 2026-08-15

- Restored the last track, playlist, and position as paused metadata. Opening
  Switify no longer starts audio without an explicit Play command.

## Standalone NRO 0.4.x — 2026-08-14 to 2026-08-15

- Added persistent artwork caching, an inline playlist beneath Now Playing,
  Plus-button theater mode, Minus-button screen-off playback, symbolic player
  controls, and reliable B-button navigation.

## Standalone NRO 0.3.x — 2026-08-14

- Expanded Home, Search, Your Library, albums, playlists, queue, resumable
  playlist contexts, pagination, and local playback-session tracking.

## Standalone NRO 0.2.2 — 2026-08-14

- Fixed stale Borealis focus crashes, added safe retry focus, bounded Spotify
  access-point failover, and restored missing system UI assets.

## Standalone NRO 0.2.1 — 2026-08-14

- Serialized connection replacement and packet writes, protected Mercury
  callbacks, and prevented reconnect-time null-pointer crashes.

## Standalone NRO 0.2.0 — 2026-08-14

- Introduced the five-section Borealis interface, persistent mini-player,
  personalized Home, private library access, artwork workers, loading/error
  states, and on-device/file diagnostics.

## Standalone NRO 0.1.0 — 2026-08-14

- Established phone-assisted Spotify authentication, cached credentials,
  native CDN streaming/decryption/Vorbis decoding, audout playback, transport
  controls, basic search, and clean shutdown.

## Overlay and background service 0.3.9 — 2026-08-15

- Added a visible numeric volume slider and Off/All/One repeat modes.

## Overlay and background service 0.3.8 — 2026-08-15

- Reduced the service to two TCP-only BSD sessions and removed unused UDP
  reservations. Hardware testing confirmed that the N64 compatibility issue
  remained.

## Overlay and background service 0.3.7 — 2026-08-15

- Moved the service to the Applet memory pool and made audout acquisition lazy
  until the first decoded PCM frame.

## Overlay and background service 0.3.6 — 2026-08-15

- Restored the proven 4 MiB heap, playback buffers, browse caches, and local B
  escape from loading views after the withdrawn 0.3.5 experiment.

## Overlay and background service 0.3.5 — withdrawn

- Withdrawn after hardware tests found silent playback, infinite collection
  loading, and a trapped Ultrahand navigation state.

## Overlay and background service 0.3.0–0.3.4 — 2026-08-14

- Established the independent Ultrahand interface, versioned CMIF IPC, dormant
  Atmosphère service, native playback over games, artwork, Spotify browsing,
  queue controls, and large-library pages.

## Phase-2 probes 0.1.x–0.2.x — 2026-08-14

- Validated service discovery, on-demand launch behavior, IPC health,
  Atmosphère packaging, and the first background audio path before the
  production overlay was introduced.

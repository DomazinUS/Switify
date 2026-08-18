# Switify 0.6.4 / Overlay 0.4.4

Switify is a native, controller-first Spotify client for Nintendo Switch
homebrew, created by **HiroshiYamauchi**.

## Download choices

- `Switify-0.6.4.zip` — standalone full-screen NRO.
- `Switify-Overlay-0.4.4.zip` — independent Ultrahand overlay plus dormant
  Atmosphère background service.
- `Switify-source-0.6.4.zip` — complete corresponding source and vendored
  dependencies for both builds.
- `SHA256SUMS.txt` — checksums for all three archives.

## Installation

For the standalone app, extract `Switify-0.6.4.zip` to the SD root and launch
Switify through title override. On first run, open Spotify on a phone connected
to the same network and select **Switify** from the device picker.

Install the overlay only after the NRO has authenticated successfully. Extract
`Switify-Overlay-0.4.4.zip` to the SD root and reboot once. Opening the Switify
overlay starts its dormant background service automatically. The NRO and
overlay are independent; if both are deliberately left running, they maintain
separate playback sessions and may produce two overlapping audio streams.

## Highlights

- Liked Songs and playlists load in 50-track pages in both interfaces rather
  than blocking while up to 1,000 tracks are downloaded and duplicated.
- Saved podcasts use Spotify's native library data and now appear in Home and
  Your Library.
- The NRO and background service carry only the Vorbis decoder that Spotify
  playback can select.
- The optimized 3 MiB service, just-in-time artwork networking, and bounded
  queues restore Nintendo 64 Nintendo Switch Online compatibility.
- Automatic service startup now retries safely during slow Atmosphère launch.
- Live Shuffle now reorders upcoming tracks without restarting the song that
  is currently audible.
- Repeat All wraps the complete queue, while Repeat One restarts the current
  song at each natural end; both run directly in the native player queue.
- Persistent volume shared by the app and overlay/background service.
- Normal (160 kbps) and High (320 kbps) Spotify Vorbis quality selection in
  both Settings interfaces. Changes apply on the next engine start.
- Higher-quality sample-rate conversion, stronger PCM/CDN buffering, and new
  slow-read/underrun diagnostics for rare gaps and subtle conversion noise.
- A lighter overlay/service browse profile with 25-item pages, smaller caches,
  and lower artwork memory limits.
- All, Music, and Podcasts Home filters.
- Search filters and results for songs, artists, albums, playlists, shows, and
  episodes, using Spotify's authenticated Pathfinder search instead of the
  rate-limited public Web API.
- Playable saved podcasts and paginated episode lists.
- Corrected Spotify library heart persistence and verification.
- Dark Switify icon, creator credit, and controller-friendly About plus full
  development-history pages.
- Background playback with artwork, queue, seek, repeat, shuffle, and visible
  volume controls.

Switify is unofficial, requires Spotify Premium for native playback, and is not
affiliated with Spotify or Nintendo.

# Switify 0.6.3 / Overlay 0.4.3

Switify is a native, controller-first Spotify client for Nintendo Switch
homebrew, created by **HiroshiYamauchi**.

## Download choices

- `Switify-0.6.3.zip` — standalone full-screen NRO.
- `Switify-Overlay-0.4.3.zip` — independent Ultrahand overlay plus dormant
  Atmosphère background service.
- `Switify-source-0.6.3.zip` — complete corresponding source and vendored
  dependencies for both builds.
- `SHA256SUMS.txt` — checksums for all three archives.

## Installation

For the standalone app, extract `Switify-0.6.3.zip` to the SD root and launch
Switify through title override. On first run, open Spotify on a phone connected
to the same network and select **Switify** from the device picker.

Install the overlay only after the NRO has authenticated successfully. Extract
`Switify-Overlay-0.4.3.zip` to the SD root and reboot once. Opening the Switify
overlay starts its dormant background service automatically. The NRO and
overlay are independent and should not run at the same time.

## Highlights

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

## Important known issue

Stop Switify's background service before launching Nintendo 64 Nintendo Switch
Online. Games in that application currently hang while the service is resident,
even when music is paused. The standalone NRO is unaffected because it closes
before another title starts.

Switify is unofficial, requires Spotify Premium for native playback, and is not
affiliated with Spotify or Nintendo.

# Switify 0.7.1 / Overlay 0.5.0

Switify is a native, controller-first Spotify client for Nintendo Switch
homebrew, created by **HiroshiYamauchi**.

This maintenance release fixes standalone shutdown ordering and safely handles
the failing Sphaira application-forwarder return path. The overlay remains at
0.5.0 and is byte-for-byte unchanged.

## Standalone NRO 0.7.1 changes

- Cancels and joins authenticated browse, search, collection, and artwork work
  before Borealis destroys its activity tree, sync queue, texture cache, and
  Switch platform services.
- Disconnects the Spotify playback session only after dependent workers have
  stopped, and interrupts active libcurl transfers immediately during exit.
- When Sphaira itself is hosted by an application forwarder, exits safely to
  the Switch HOME screen through Switify's still-valid applet self-controller.
  Returning through Sphaira's HBL after the child NRO has been unloaded fails
  in Sphaira's late `appletOE` exit path.
- Switify runtime logs, Sphaira logs, and Atmosphere crash reports confirm that
  Switify's engine, workers, playback session, artwork pipeline, and platform
  services have completely shut down before that upstream HBL failure occurs.
- HBMenu, applet-mode Sphaira, and direct Switify forwarder launches retain
  their normal exit behavior.

## Download choices

- `Switify-0.7.1.zip` — standalone full-screen NRO.
- `Switify-Overlay-0.5.0.zip` — independent Ultrahand overlay plus dormant
  Atmosphère background service.
- `Switify-source-0.7.1.zip` — complete corresponding source and vendored
  dependencies for both products.
- `SHA256SUMS.txt` — checksums for all three archives.

## Installation

For the standalone app, extract `Switify-0.7.1.zip` to the SD root and launch
Switify through title override. On first run, open Spotify on a phone connected
to the same network and select **Switify** from the device picker.

Install the overlay only after the NRO has authenticated successfully. Stop an
older Switify service, extract `Switify-Overlay-0.5.0.zip` to the SD root, and
reboot once so Atmosphère discovers the replacement. Opening the overlay then
starts its dormant service automatically.

The NRO and overlay remain independent playback engines. If both are left
running deliberately, they can produce two overlapping audio streams.

## Standalone NRO 0.7 series highlights

- Full lyrics view with a fixed current/upcoming-line panel, timestamps, and
  active-row highlighting when Spotify supplies Musixmatch line timing.
- Optional line-synced lyrics directly inside the full player, enabled by
  default and updated during normal playback or seeking.
- Black, animated RGB Gradient, and animated RGB Solid full-player
  backgrounds. RGB Solid is the default; the normal Now Playing tab remains
  unchanged.
- Followed podcast shows in Home and Your Library, with authenticated,
  paginated playable episode lists and podcast-aware labels.
- Complete multi-thousand-track Liked Songs and playlist sessions with Play
  All, session caching, and only 100 interface rows materialized at once.
- Large player, podcast, and collection artwork now uses Spotify's largest
  supplied image instead of the 300-pixel card endpoint.
- Session lyrics caching avoids repeat network requests for the same track.
- Removed unused AAC, MP3, ALAC, and Opus decoder paths; Spotify playback
  continues to use selectable 160 or 320 kbps OGG Vorbis streams.
- Preserved loaded dashboards, libraries, collections, and artwork across tab
  changes.
- Hardened playback-state publication and Shuffle/Repeat queue updates.

## Overlay and background service 0.5.0 highlights

- Restored Nintendo 64 Nintendo Switch Online compatibility in hardware
  testing by reducing persistent decoder, socket, queue, cache, and artwork
  pressure.
- Remote 50-track collection fetches, 25-item overlay pages, and a bounded
  100-track playback window replace thousand-track up-front loading.
- Followed podcasts are available under Your Library with playable episodes.
- Automatic service startup retries with bounded backoff, supervises Dormant
  transitions, and recovers without requiring the manual retry path during a
  normal launch.
- Loading screens retain a local B-button escape and catalog work stays away
  from Ultrahand's render thread.
- Search and browser input is claimed by the overlay instead of being repeated
  into the foreground game or application.
- Play/Pause uses an explicit requested state. Pending playback-state, seek,
  volume, Shuffle, Repeat, and quality requests coalesce so stale actions are
  not replayed later.
- Closing Ultrahand releases overlay-owned artwork, network, and IPC resources;
  reopening reconnects cleanly while the independent service keeps playing.
- Artwork networking is just-in-time: cache hits claim no BSD session and a
  cache miss releases its session immediately after the transfer.
- Centered artwork and a clearer volume layout keep the percentage readable
  while aligning the Volume label with its slider.
- The service remains dormant at console boot and opens audout only when audio
  playback actually begins.

## Notes

Switify is unofficial, requires Spotify Premium for native playback, and uses
undocumented Spotify protocols that Spotify may change. Lyrics availability
and line synchronization depend on Spotify's response for the track, account,
and region.

Switify is not affiliated with Spotify or Nintendo.

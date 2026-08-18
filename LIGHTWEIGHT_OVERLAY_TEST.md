# Switify Overlay 0.4.3-lite1

This is a compatibility test build for consoles where Atmosphere rejects the
Switify background process with `0x00010801 (M1 D132)`, the kernel
`OutOfResource` result. It does not change the Switify NRO or Spotify account
data and it is not a replacement for the public 0.4.3 release yet.

## Resource reductions

- The overlay no longer starts artwork networking before the background
  process.
- The lite build disables cover-art download and rendering completely. The
  existing music-note placeholder remains visible.
- The overlay image is approximately 0.54 MiB instead of 1.18 MiB.
- The background heap ceiling is 3 MiB instead of 4 MiB. Hardware logs showed
  a previous high-water arena near 2.2 MiB.
- The background build contains only the Vorbis playback path selected by
  Switify. AAC, MP3, ALAC, and Opus are disabled.
- Existing low-resource settings remain: two BSD sessions, TCP-only networking,
  lazy audout, and a 16 KiB main-thread stack.

Browsing, playback controls, volume, shuffle, repeat, queue, playlists, search,
and audio quality selection are unchanged. Normal and High quality continue to
use OGG Vorbis at 160 and 320 kbps.

## Install and test

1. Extract `Switify-Overlay-0.4.3-lite1.zip` to the SD-card root and allow it
   to replace the existing Switify overlay and background service.
2. Fully reboot the Switch so Atmosphere discovers the replaced `exefs.nsp`.
3. Open Ultrahand and then Switify. The header must show `v0.4.3-lite1`.
4. If startup succeeds, check that playback, browsing, and controls still work.
5. If startup fails, record the exact Service and Details values. Attach any
   new files from `sdmc:/config/Switify/` and any Atmosphere crash report whose
   filename ends in `_4200000000535754.log`.

Successful startup on a console that rejected 0.4.3 would confirm that the
console was just over a kernel resource boundary. The lite build cannot solve
an exhausted process slot: Switify still requires one background process.

## Roll back

Extract the public `Switify-Overlay-0.4.3.zip` to the SD-card root and reboot.

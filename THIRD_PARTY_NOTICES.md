# Third-party notices

Switify was created by **HiroshiYamauchi** and is an independent, unofficial
project. Spotify and Nintendo names and marks belong to their respective
owners.

## Components included in builds

- **cspot** — GPL-3.0-or-later. Switify vendors a modified fork with Nintendo
  Switch networking, authentication, playback, queue, and decoder changes.
  Upstream: <https://github.com/feelfreelinux/cspot>
- **Borealis** — Apache-2.0. Used by the standalone controller-first UI.
  Upstream: <https://github.com/natinusala/borealis>
- **libultrahand / libtesla** — GPL-2.0-only and subordinate licenses. Used by
  the independent overlay. Exact notices are retained in
  `phase2/overlay/lib/libultrahand/`.
- **libnx and devkitPro portlibs** — supplied by the user's development
  environment under their respective licenses.
- **nlohmann/json, fmt, nanopb, Opus, Tremor, Helix MP3, ALAC, cJSON, and
  mdnssvc** — bundled through cspot/bell. Their license files remain beside
  their sources under `vendor/cspot/bell/external/`.

The small CMIF server in `phase2/sysmodule/source/ipc_server.c` and `.h` is
based on work used by sys-clk and sys-tune and retains its Beer-Ware notice.

## Research and interface references

The following projects were studied for protocol behavior, platform
architecture, user experience, or compatibility. Their source was not copied
unless a notice above explicitly says otherwise.

- librespot: <https://github.com/librespot-org/librespot>
- psst: <https://github.com/jpochyla/psst>
- Spotix: <https://github.com/wrexik/Spotix>
- Spotube: <https://github.com/KRTirtho/spotube>
- Streamfin Switch: <https://github.com/dammitjeff/streamfin-switch>
- sys-tune: <https://github.com/ppkantorski/sys-tune>


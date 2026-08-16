# Third-party notices

`sysmodule/source/ipc_server.c` and `ipc_server.h` are based on the compact IPC
server written by the sys-clk authors and later used by sys-tune. Those files
retain the original Beer-Ware notice in full.

Streamfin and sys-tune were used as architectural references for the split
between a persistent background player, CMIF control service, and independent
overlay. Switify's GPL-2.0 overlay also adapts Streamfin's controller-first
player-card and bounded asynchronous artwork-loading patterns. Switify's
Spotify IPC/data implementation and rendering code are project-specific.

The overlay is built with libultrahand revision
`1b7a64a4d73489c870f3fb9caa9927e9a2347478`, vendored under
`overlay/lib/libultrahand`. Its GPL-2.0 license and subordinate license text
are retained there. The Switify overlay source carries an SPDX
`GPL-2.0-only` identifier. No playback or decoder implementation was copied
from Streamfin or sys-tune.

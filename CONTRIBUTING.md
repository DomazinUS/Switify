# Contributing

Contributions are welcome. Keep changes scoped, controller-first, and safe for
the Nintendo Switch's constrained applet environment.

See `BUILDING.md` for the complete toolchain, build, NetLoader, packaging, and
symbolication instructions.

Before submitting a change:

1. Build the standalone NRO with `build-app.ps1`.
2. If shared engine, IPC, or overlay code changed, run
   `build-phase2-engine.ps1` as well.
3. Do not commit build trees, binaries, logs, crash reports, Spotify
   credentials, or SD-card configuration.
4. Describe what was tested on real hardware and retain the matching ELF
   privately when reporting a crash.
5. Preserve third-party license notices and document newly introduced code.

The Nintendo 64 Nintendo Switch Online compatibility issue is known. Changes
to background-service resources, sockets, audout, or memory pools require a
specific hardware regression pass against that title.

# Building Switify

This repository builds two independent Nintendo Switch products from one
source tree: the standalone Switify NRO and the Ultrahand overlay/background
service package.

## Requirements

- Windows PowerShell
- devkitPro installed at `C:\devkitPro`
- `switch-dev`, `switch-mbedtls`, `switch-cmake`, and
  `dkp-toolchain-vars`
- Git subdependencies already present under `vendor/` and
  `phase2/overlay/lib/`

The repository vendors the exact cspot, Borealis, and libultrahand sources used
by the public binaries. Do not replace those directories with unrelated
upstream revisions and expect the Switch-specific patches to apply.

## Standalone NRO

From the repository root:

    .\build-app.ps1

Outputs:

    build_app\Switify.nro
    build_app\Switify.elf

Keep the matching ELF for every hardware-tested NRO. Atmosphere crash offsets
can only be symbolicated reliably against the exact binary that produced them.

## Overlay and background service

From the repository root:

    .\build-phase2-engine.ps1

Outputs include:

    phase2\build_engine\switify-background.nsp
    phase2\dist_engine\atmosphere\contents\4200000000535754\exefs.nsp
    phase2\dist_engine\switch\.overlays\Switify.ovl
    phase2\Switify-Phase2-engine-0.4.4.zip

See [phase2/README.md](phase2/README.md) for the IPC boundary, memory/resource
profile, host tests, hardware checkpoint order, and N64 Nintendo Switch Online
resource-compatibility regression check.

## Fast NetLoader loop

Open NetLoader in Homebrew Menu, then replace the address below with the IP
shown on the Switch:

    .\deploy-app.ps1 -Address 192.168.1.50

Send an already built NRO:

    .\deploy-app.ps1 -Address 192.168.1.50 -SkipBuild

Keep nxlink attached and save live diagnostics:

    .\deploy-app.ps1 -Address 192.168.1.50 -LiveLogs

NetLoader uploads on port 28280. Nxlink live logs use their separate client
connection on port 28771.

## Public release staging

Build both products and create the clean archives:

    .\prepare-release.ps1

Repackage already-built matching binaries:

    .\prepare-release.ps1 -SkipBuild

The script stages only public source, vendored dependencies, documentation,
licenses, and repository artwork. It excludes credentials, settings, session
state, logs, crash data, private handover notes, historical candidates, and
build trees. Outputs are written under `release/` with SHA-256 checksums.

## Crash symbolication

Copy the Atmosphere report to the PC and run:

    .\symbolicate-crash.ps1 -CrashReport C:\path\to\crash.log

Before drawing conclusions, verify that the report's module ID matches the
exact ELF retained from the tested build.

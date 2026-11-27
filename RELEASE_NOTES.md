# Switify 0.8.0 / Overlay 0.6.0

After pairing, you can browse, play, pause, and skip on the Switch. This release adds optional phone control for background playback and fixes a crash when loading tracks with long streaming URLs. The overlay service also uses the resource group that matches its applet memory pool, fixing a startup failure on affected consoles.

## Install or update

- **Standalone app** — extract `Switify-0.8.0.zip` to the SD-card root. Launch Switify from the Homebrew Menu using title override.
- **Optional overlay** — close Switify and stop any running Switify service. Extract `Switify-Overlay-0.6.0.zip` to the SD-card root, reboot once, and open Switify from Ultrahand.
- The release includes the corresponding source archive and SHA-256 checksums.

## Use your phone as a remote

In the standalone app, open **Settings → Playback controls → Allow Spotify remote control**. The setting defaults to off and takes effect the next time the background service starts. Close the app, start the service from Ultrahand, then select Switify in Spotify on your phone.

Selecting Switify resumes the Switch session. The phone displays the current track. If you move playback to another device, pause Switify yourself.

Thanks to FrcJules for the startup and playback fixes and the Spotify Connect implementation. [PR #9](https://github.com/DomazinUS/Switify/pull/9)

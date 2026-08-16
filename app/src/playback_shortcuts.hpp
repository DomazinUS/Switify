#pragma once

#include <algorithm>
#include <cstdint>

#include <borealis.hpp>

#include "app_settings.hpp"
#include "spotify_engine.hpp"

// Hidden actions preserve the normal Borealis footer while making the same
// transport controls available from every NRO screen when explicitly enabled.
template <typename Host>
void registerPlaybackShortcuts(Host* host) {
  host->registerAction(
      "Play / pause", brls::BUTTON_Y,
      [](brls::View*) {
        if (!swsettings::playbackShortcutsEnabled()) return false;
        SpotifyEngine::instance().playPause();
        return true;
      },
      true);
  host->registerAction(
      "Previous", brls::BUTTON_LB,
      [](brls::View*) {
        if (!swsettings::playbackShortcutsEnabled()) return false;
        SpotifyEngine::instance().previous();
        return true;
      },
      true);
  host->registerAction(
      "Next", brls::BUTTON_RB,
      [](brls::View*) {
        if (!swsettings::playbackShortcutsEnabled()) return false;
        SpotifyEngine::instance().next();
        return true;
      },
      true);
  host->registerAction(
      "Rewind", brls::BUTTON_LT,
      [](brls::View*) {
        if (!swsettings::playbackShortcutsEnabled()) return false;
        const auto playback = SpotifyEngine::instance().nowPlaying();
        SpotifyEngine::instance().seekToMs(
            playback.positionMs > 15000 ? playback.positionMs - 15000 : 0);
        return true;
      },
      true);
  host->registerAction(
      "Fast forward", brls::BUTTON_RT,
      [](brls::View*) {
        if (!swsettings::playbackShortcutsEnabled()) return false;
        const auto playback = SpotifyEngine::instance().nowPlaying();
        const uint64_t target = static_cast<uint64_t>(playback.positionMs) +
                                static_cast<uint64_t>(15000);
        SpotifyEngine::instance().seekToMs(static_cast<uint32_t>(
            playback.durationMs > 0
                ? std::min<uint64_t>(target, playback.durationMs)
                : target));
        return true;
      },
      true);
}

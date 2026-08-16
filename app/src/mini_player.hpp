#pragma once

#include <borealis.hpp>

#include <atomic>
#include <memory>
#include <string>

#include "spotify_engine.hpp"

class MiniPlayer : public brls::Box {
 public:
  MiniPlayer();
  ~MiniPlayer() override;

  static brls::View* create() { return new MiniPlayer(); }

 private:
  void refresh();

  brls::Image* artwork = nullptr;
  brls::Label* title = nullptr;
  brls::Label* subtitle = nullptr;
  brls::Label* state = nullptr;
  brls::Label* playPauseLabel = nullptr;
  std::string loadedArtworkUrl;
  SpotifyEngine::ObserverId observerId = 0;
  std::shared_ptr<std::atomic_bool> alive;
};

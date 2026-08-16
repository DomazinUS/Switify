#pragma once

#include <borealis.hpp>

#include <atomic>
#include <memory>
#include <vector>

#include "spotify_engine.hpp"

class TrackListActivity : public brls::Activity {
 public:
  explicit TrackListActivity(SpotifyEngine::Collection collection);
  ~TrackListActivity() override;

  CONTENT_FROM_XML_RES("activity/tracklist.xml");
  void onContentAvailable() override;

 private:
  void showTracks(SpotifyEngine::Result<std::vector<SpotifyEngine::Track>> result);
  void appendTrackRows(bool focusFirstNew = false);

  SpotifyEngine::Collection collection;
  std::vector<SpotifyEngine::Track> currentTracks;
  size_t renderedTrackCount = 0;
  brls::Box* loadMoreButton = nullptr;
  std::shared_ptr<std::atomic_bool> alive;

  BRLS_BIND(brls::Image, artwork, "artwork");
  BRLS_BIND(brls::Label, title, "title");
  BRLS_BIND(brls::Label, subtitle, "subtitle");
  BRLS_BIND(brls::Label, description, "description");
  BRLS_BIND(brls::Box, playAll, "playAll");
  BRLS_BIND(brls::Label, playAllLabel, "playAllLabel");
  BRLS_BIND(brls::Box, list, "list");
};

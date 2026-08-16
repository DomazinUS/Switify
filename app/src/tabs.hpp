#pragma once

#include <borealis.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

#include "spotify_engine.hpp"

namespace swui {
class TrackRow;
class RepeatIcon;
class ShuffleIcon;
class FilterChip;
}

void showTheaterMode();

class HomeTab : public brls::Box {
 public:
  HomeTab();
  ~HomeTab() override;
  brls::View* getDefaultFocus() override;
  static brls::View* create() { return new HomeTab(); }

 private:
  enum class Filter { All, Music, Podcasts };
  void refreshStatus();
  void load(bool forceRefresh = false);
  void show(SpotifyEngine::Result<SpotifyEngine::HomeData> result,
            uint64_t requestId);
  void selectFilter(Filter filter);
  void renderDashboard();
  void updateFilterChips();

  brls::Label* title = nullptr;
  brls::Label* account = nullptr;
  brls::Label* status = nullptr;
  brls::Box* content = nullptr;
  brls::Box* refreshButton = nullptr;
  std::vector<swui::FilterChip*> filterChips;
  SpotifyEngine::HomeData dashboardData;
  Filter filter = Filter::All;
  SpotifyEngine::ObserverId observerId = 0;
  std::shared_ptr<std::atomic_bool> alive;
  std::atomic<uint64_t> requestGeneration{0};
  bool loaded = false;
  bool loading = false;
  bool hasDashboardData = false;
  SpotifyEngine::State displayedState = SpotifyEngine::State::NoConfig;
};

class SearchTab : public brls::Box {
 public:
  SearchTab();
  ~SearchTab() override;
  brls::View* getDefaultFocus() override;
  static brls::View* create() { return new SearchTab(); }

 private:
  enum class Filter { All, Songs, Artists, Albums, Playlists, Podcasts };
  void openKeyboard();
  void searchFor(const std::string& query);
  void show(const std::string& query,
            SpotifyEngine::Result<SpotifyEngine::SearchResults> result,
            uint64_t requestId);
  void selectFilter(Filter filter);
  void renderResults();
  void updateFilterChips();

  brls::Label* status = nullptr;
  brls::Label* queryLabel = nullptr;
  brls::Box* content = nullptr;
  brls::Box* searchButton = nullptr;
  std::vector<swui::FilterChip*> filterChips;
  SpotifyEngine::SearchResults searchResults;
  std::string currentQuery;
  Filter filter = Filter::All;
  bool hasSearchResults = false;
  std::shared_ptr<std::atomic_bool> alive;
  std::atomic<uint64_t> requestGeneration{0};
};

class LibraryTab : public brls::Box {
 public:
  LibraryTab();
  ~LibraryTab() override;
  brls::View* getDefaultFocus() override;
  static brls::View* create() { return new LibraryTab(); }

 private:
  void refreshStatus();
  void load(bool forceRefresh = false);
  void show(SpotifyEngine::Result<SpotifyEngine::LibraryData> result,
            uint64_t requestId);

  brls::Label* status = nullptr;
  brls::Box* content = nullptr;
  brls::Box* refreshButton = nullptr;
  SpotifyEngine::ObserverId observerId = 0;
  std::shared_ptr<std::atomic_bool> alive;
  std::atomic<uint64_t> requestGeneration{0};
  bool loaded = false;
  bool loading = false;
  SpotifyEngine::State displayedState = SpotifyEngine::State::NoConfig;
};

class NowPlayingTab : public brls::Box {
 public:
  NowPlayingTab();
  ~NowPlayingTab() override;
  brls::View* getDefaultFocus() override;
  static brls::View* create() { return new NowPlayingTab(); }

  void draw(NVGcontext* vg, float x, float y, float width, float height,
            brls::Style style, brls::FrameContext* ctx) override;
  void willAppear(bool resetState = false) override;

 private:
  void refresh();
  void refreshPlaylist(const SpotifyEngine::PlaybackSessionInfo& session);
  void rebuildPlaylistPage(const SpotifyEngine::PlaybackSessionInfo& session,
                           bool focusFirstTrack = false);
  void requestPlaylistPage(size_t offset);

  brls::Image* artwork = nullptr;
  brls::Label* status = nullptr;
  brls::Label* track = nullptr;
  brls::Label* artist = nullptr;
  brls::Label* album = nullptr;
  brls::Label* elapsed = nullptr;
  brls::Label* duration = nullptr;
  brls::Label* volumeLabel = nullptr;
  brls::Label* playPauseLabel = nullptr;
  swui::ShuffleIcon* shuffleIcon = nullptr;
  swui::RepeatIcon* repeatIcon = nullptr;
  brls::Label* saveLabel = nullptr;
  brls::Header* playlistHeader = nullptr;
  brls::Box* playlistList = nullptr;
  brls::Box* playPauseButton = nullptr;
  brls::Slider* progress = nullptr;
  brls::Slider* volume = nullptr;
  SpotifyEngine::ObserverId observerId = 0;
  std::shared_ptr<std::atomic_bool> alive;
  std::string loadedArtworkUrl;
  std::vector<swui::TrackRow*> playlistRows;
  uint64_t playlistRevision = 0;
  size_t playlistPageStart = 0;
  size_t playlistPageEnd = 0;
  size_t playlistCurrentIndex = static_cast<size_t>(-1);
  bool updatingSliders = false;
  std::chrono::steady_clock::time_point lastSeek{};
  std::chrono::steady_clock::time_point lastRefresh{};
};

class SettingsTab : public brls::Box {
 public:
  SettingsTab();
  ~SettingsTab() override;
  static brls::View* create() { return new SettingsTab(); }

 private:
  void refresh();
  void commitEqualizer();

  brls::Label* connection = nullptr;
  brls::Label* account = nullptr;
  brls::Label* volumeValue = nullptr;
  brls::Slider* volume = nullptr;
  brls::BooleanCell* equalizerEnabled = nullptr;
  brls::SliderCell* bass = nullptr;
  brls::SliderCell* mid = nullptr;
  brls::SliderCell* treble = nullptr;
  SpotifyEngine::ObserverId observerId = 0;
  std::shared_ptr<std::atomic_bool> alive;
  bool updatingVolume = false;
};

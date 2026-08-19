#pragma once

#include "spotify_engine.hpp"
#include "switify/service_core.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace switify::phase2 {

class BackgroundBackend {
 public:
  explicit BackgroundBackend(ServiceCore& core);
  ~BackgroundBackend();

  bool start();
  void stop();

 private:
  static uint64_t monotonicMs();
  void run();
  void process(const ControlMessage& message);
  void publishSnapshot();
  void publishArtworkUrl(const std::string& url);
  SwitifyControlResult processBrowse(const ControlMessage& message);

  enum class BrowseEntryType {
    Section,
    Track,
    Collection,
    PreviousPage,
    NextPage,
    PreviousRemotePage,
    NextRemotePage,
  };
  enum class BrowsePlayMode { Tracks, Playlist, Queue };
  enum class BrowseSection {
    HomeResume,
    HomeRecent,
    HomePlaylists,
    LibraryPlaylists,
    LibraryPodcasts,
    SearchTracks,
    SearchPlaylists,
  };
  struct BrowseEntry {
    BrowseEntryType type = BrowseEntryType::Section;
    BrowseSection section = BrowseSection::HomeRecent;
    size_t sectionIndex = 0;
    std::string name;
    std::string subtitle;
    size_t sourceIndex = 0;
    SpotifyEngine::Track track;
    SpotifyEngine::Collection collection;
  };
  struct BrowsePage {
    std::string title;
    std::string subtitle;
    uint32_t depth = 0;
    BrowsePlayMode playMode = BrowsePlayMode::Tracks;
    size_t pageOffset = 0;
    bool episodePage = false;
    bool remotelyPaged = false;
    bool remoteHasMore = false;
    size_t remoteOffset = 0;
    size_t remoteNextOffset = 0;
    SpotifyEngine::Collection remoteCollection;
    std::vector<BrowseEntry> entries;
  };

  uint64_t beginRoot(std::string_view title, std::string_view subtitle);
  uint64_t beginChild(std::string_view title, std::string_view subtitle);
  void completeBrowse(uint64_t generation, BrowsePage page);
  void failBrowse(uint64_t generation, const std::string& message);
  void requestHome();
  void requestLibrary();
  void requestSearch(const std::string& query);
  void openBrowseItem(uint64_t generation, uint32_t index);
  void changeBrowsePage(size_t offset);
  void changeRemoteBrowsePage(size_t offset);
  void backBrowse(uint64_t generation);
  void playBrowseItem(uint64_t generation, uint32_t index);
  void browseCurrentPlaylist(bool queue);
  BrowsePage buildHomeRoot(const SpotifyEngine::HomeData& data) const;
  BrowsePage buildLibraryRoot(const SpotifyEngine::LibraryData& data) const;
  BrowsePage buildSearchRoot(const SpotifyEngine::SearchResults& data,
                             const std::string& query) const;
  BrowsePage buildSection(const BrowseEntry& section) const;
  BrowsePage buildTracks(std::string title, std::string subtitle,
                         std::vector<SpotifyEngine::Track> tracks,
                         uint32_t depth) const;
  BrowsePage buildRemoteTracks(const SpotifyEngine::Collection& collection,
                               SpotifyEngine::TrackPage page,
                               uint32_t depth) const;

  ServiceCore& core;
  SpotifyEngine& engine;
  std::atomic<bool> running{false};
  std::thread worker;
  std::chrono::steady_clock::time_point nextReconnect{};
  std::string lastArtworkUrl;
  bool artworkUrlPublished = false;
  mutable std::mutex browseMutex;
  uint64_t activeBrowseGeneration = 0;
  BrowsePage currentBrowsePage;
  std::vector<BrowseEntry> visibleBrowseEntries;
  std::vector<BrowsePage> browseHistory;
  SpotifyEngine::HomeData homeCache;
  SpotifyEngine::LibraryData libraryCache;
  SpotifyEngine::SearchResults searchCache;
  std::string searchQuery;
};

}  // namespace switify::phase2

#include "background_backend.hpp"

#include "NXConsoleLog.h"
#include "app_settings.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <new>

namespace switify::phase2 {
namespace {

constexpr auto PollInterval = std::chrono::milliseconds(50);
constexpr auto SnapshotInterval = std::chrono::milliseconds(250);
constexpr auto ReconnectInterval = std::chrono::seconds(30);
constexpr auto ResourceLogInterval = std::chrono::seconds(30);
constexpr const char* CredentialsFile = "/config/Switify/credentials.json";
constexpr const char* ArtworkUrlFile =
    "/config/Switify/current-artwork-url";
constexpr const char* ArtworkUrlTemporary =
    "/config/Switify/current-artwork-url.tmp";
constexpr size_t MaxBrowseItems = 32;
constexpr size_t BrowsePageSize = 25;
constexpr size_t RemoteTrackPageSize = 50;

std::string itemCountText(size_t count, const char* singular,
                          const char* plural) {
  return std::to_string(count) + " " + (count == 1 ? singular : plural);
}

void copyBrowseText(char* destination, size_t capacity,
                    const std::string& source) {
  if (capacity == 0) return;
  const size_t length = std::min(capacity - 1, source.size());
  if (length != 0) std::memcpy(destination, source.data(), length);
  destination[length] = '\0';
}

SwitifyBrowseItemKind collectionKind(SpotifyEngine::Collection::Kind kind) {
  switch (kind) {
    case SpotifyEngine::Collection::Kind::ALBUM:
      return SwitifyBrowseItemKind_Album;
    case SpotifyEngine::Collection::Kind::ARTIST:
      return SwitifyBrowseItemKind_Artist;
    case SpotifyEngine::Collection::Kind::PLAYLIST:
    case SpotifyEngine::Collection::Kind::LIKED:
    case SpotifyEngine::Collection::Kind::SHOW:
      return SwitifyBrowseItemKind_Playlist;
  }
  return SwitifyBrowseItemKind_Playlist;
}

SwitifyServiceState serviceState(SpotifyEngine::State state) {
  switch (state) {
    case SpotifyEngine::State::NoConfig:
      return SwitifyServiceState_WaitingForCredentials;
    case SpotifyEngine::State::WaitingForPhone:
      return SwitifyServiceState_Pairing;
    case SpotifyEngine::State::Connecting:
      return SwitifyServiceState_Connecting;
    case SpotifyEngine::State::Ready:
      return SwitifyServiceState_Ready;
    case SpotifyEngine::State::Error:
      return SwitifyServiceState_Error;
  }
  return SwitifyServiceState_Error;
}

}  // namespace

BackgroundBackend::BackgroundBackend(ServiceCore& core)
    : core(core), engine(SpotifyEngine::instance()) {}

BackgroundBackend::~BackgroundBackend() { stop(); }

uint64_t BackgroundBackend::monotonicMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool BackgroundBackend::start() {
  if (running.exchange(true)) return false;

  const uint64_t capabilities =
      SwitifyIpcCapability_Status |
      SwitifyIpcCapability_PlaybackControl | SwitifyIpcCapability_Seek |
      SwitifyIpcCapability_Queue | SwitifyIpcCapability_Browsing |
      SwitifyIpcCapability_Search | SwitifyIpcCapability_AudioSettings;
  core.configureCommands(true, capabilities, monotonicMs());
  swsettings::load();
  const auto settings = swsettings::get();
  engine.setVolume(settings.outputVolume);
  engine.setHighQuality(settings.highQuality);
  engine.setInteractiveAuthentication(false);
  engine.start();
  worker = std::thread([this] { run(); });
  nxlog::push("Background Spotify backend started");
  return true;
}

void BackgroundBackend::stop() {
  if (!running.exchange(false)) return;
  core.configureCommands(false, SwitifyIpcCapability_Status, monotonicMs());
  if (worker.joinable()) worker.join();
  engine.shutdown();
  nxlog::push("Background Spotify backend stopped");
}

void BackgroundBackend::process(const ControlMessage& message) {
  if (message.command >= SwitifyIpcCommand_BrowseHome &&
      message.command <= SwitifyIpcCommand_BrowseQueue) {
    core.publishResult(processBrowse(message), monotonicMs());
    publishSnapshot();
    return;
  }

  const bool ready = engine.state() == SpotifyEngine::State::Ready;
  auto result = ready ? SwitifyControlResult_Ok
                      : SwitifyControlResult_BackendUnavailable;

  switch (message.command) {
    case SwitifyIpcCommand_PlayPause:
      if (ready) engine.playPause();
      break;
    case SwitifyIpcCommand_Play:
      if (ready && engine.nowPlaying().paused) engine.playPause();
      break;
    case SwitifyIpcCommand_Pause:
      if (ready && !engine.nowPlaying().paused) engine.playPause();
      break;
    case SwitifyIpcCommand_Next:
      if (ready) engine.next();
      break;
    case SwitifyIpcCommand_Previous:
      if (ready) engine.previous();
      break;
    case SwitifyIpcCommand_SetVolume:
      engine.setVolume(message.value / 1000.0f);
      swsettings::setOutputVolume(message.value / 1000.0f);
      result = SwitifyControlResult_Ok;
      break;
    case SwitifyIpcCommand_Seek:
      if (ready) engine.seekToMs(message.value);
      break;
    case SwitifyIpcCommand_SetShuffle:
      if (ready && engine.nowPlaying().shuffle != (message.value != 0))
        engine.toggleShuffle();
      break;
    case SwitifyIpcCommand_SetRepeatMode:
      if (message.value > SwitifyRepeatMode_All) {
        result = SwitifyControlResult_InvalidArgument;
      } else if (ready) {
        SpotifyEngine::RepeatMode requested = SpotifyEngine::RepeatMode::Off;
        if (message.value == SwitifyRepeatMode_One)
          requested = SpotifyEngine::RepeatMode::One;
        else if (message.value == SwitifyRepeatMode_All)
          requested = SpotifyEngine::RepeatMode::All;
        if (engine.nowPlaying().repeatMode != requested)
          engine.setRepeatMode(requested);
      }
      break;
    case SwitifyIpcCommand_SetAudioQuality:
      swsettings::setHighQuality(message.value != 0);
      engine.setHighQuality(message.value != 0);
      result = SwitifyControlResult_Ok;
      break;
    default:
      result = SwitifyControlResult_InvalidCommand;
      break;
  }

  core.publishResult(result, monotonicMs());
  publishSnapshot();
}

uint64_t BackgroundBackend::beginRoot(std::string_view title,
                                      std::string_view subtitle) {
  std::scoped_lock lock(browseMutex);
  browseHistory.clear();
  currentBrowsePage = {};
  visibleBrowseEntries.clear();
  homeCache = {};
  libraryCache = {};
  searchCache = {};
  searchQuery.clear();
  activeBrowseGeneration =
      core.beginBrowse(title, subtitle, 0, monotonicMs());
  return activeBrowseGeneration;
}

uint64_t BackgroundBackend::beginChild(std::string_view title,
                                       std::string_view subtitle) {
  std::scoped_lock lock(browseMutex);
  if (!currentBrowsePage.title.empty())
    browseHistory.push_back(currentBrowsePage);
  activeBrowseGeneration = core.beginBrowse(
      title, subtitle, static_cast<uint32_t>(browseHistory.size()),
      monotonicMs());
  return activeBrowseGeneration;
}

void BackgroundBackend::completeBrowse(uint64_t generation, BrowsePage page) {
  std::vector<SwitifyIpcBrowseItem> items;
  std::string title;
  std::string subtitle;
  uint32_t depth = 0;
  {
    std::scoped_lock lock(browseMutex);
    if (!running.load() || generation != activeBrowseGeneration) return;
    page.depth = static_cast<uint32_t>(browseHistory.size());
    if (!page.entries.empty() && page.pageOffset >= page.entries.size())
      page.pageOffset = ((page.entries.size() - 1) / BrowsePageSize) *
                        BrowsePageSize;
    currentBrowsePage = std::move(page);

    const size_t first = currentBrowsePage.pageOffset;
    const size_t last = std::min(currentBrowsePage.entries.size(),
                                 first + BrowsePageSize);
    const bool isTrackPage = std::all_of(
        currentBrowsePage.entries.begin(), currentBrowsePage.entries.end(),
        [](const BrowseEntry& entry) {
          return entry.type == BrowseEntryType::Track;
        });
    const char* itemName = currentBrowsePage.episodePage
                               ? "episodes"
                               : (isTrackPage ? "songs" : "items");
    const char* itemTitle = currentBrowsePage.episodePage
                                ? "Episodes"
                                : (isTrackPage ? "Songs" : "Items");
    visibleBrowseEntries.clear();
    visibleBrowseEntries.reserve(BrowsePageSize + 2);
    if (first > 0) {
      BrowseEntry previous;
      previous.type = BrowseEntryType::PreviousPage;
      previous.name = std::string("Previous ") + itemName;
      previous.subtitle = "Back to " +
                          std::to_string(first > BrowsePageSize
                                             ? first - BrowsePageSize + 1
                                             : 1);
      previous.sourceIndex =
          first > BrowsePageSize ? first - BrowsePageSize : 0;
      visibleBrowseEntries.push_back(std::move(previous));
    } else if (currentBrowsePage.remotelyPaged &&
               currentBrowsePage.remoteOffset > 0) {
      BrowseEntry previous;
      previous.type = BrowseEntryType::PreviousRemotePage;
      previous.name = std::string("Previous ") + itemName;
      previous.subtitle = "Back to " +
                          std::to_string(
                              currentBrowsePage.remoteOffset >=
                                      RemoteTrackPageSize
                                  ? currentBrowsePage.remoteOffset -
                                        RemoteTrackPageSize + 1
                                  : 1);
      previous.sourceIndex =
          currentBrowsePage.remoteOffset >= RemoteTrackPageSize
              ? currentBrowsePage.remoteOffset - RemoteTrackPageSize
              : 0;
      visibleBrowseEntries.push_back(std::move(previous));
    }
    for (size_t index = first; index < last; ++index) {
      visibleBrowseEntries.push_back(currentBrowsePage.entries[index]);
      visibleBrowseEntries.back().sourceIndex = index;
    }
    if (last < currentBrowsePage.entries.size()) {
      BrowseEntry next;
      next.type = BrowseEntryType::NextPage;
      next.name = std::string("Next ") + itemName;
      next.subtitle = "Continue at " + std::to_string(last + 1);
      next.sourceIndex = last;
      visibleBrowseEntries.push_back(std::move(next));
    } else if (currentBrowsePage.remotelyPaged &&
               currentBrowsePage.remoteHasMore) {
      BrowseEntry next;
      next.type = BrowseEntryType::NextRemotePage;
      next.name = std::string("Next ") + itemName;
      next.subtitle = "Continue at " +
                      std::to_string(currentBrowsePage.remoteNextOffset + 1);
      next.sourceIndex = currentBrowsePage.remoteNextOffset;
      visibleBrowseEntries.push_back(std::move(next));
    }

    title = currentBrowsePage.title;
    subtitle = currentBrowsePage.subtitle;
    depth = currentBrowsePage.depth;
    if (currentBrowsePage.remotelyPaged &&
        !currentBrowsePage.entries.empty()) {
      subtitle = std::string(itemTitle) + " " +
                 std::to_string(currentBrowsePage.remoteOffset + first + 1) +
                 "-" +
                 std::to_string(currentBrowsePage.remoteOffset + last);
      if (currentBrowsePage.remoteHasMore) subtitle += "+";
    } else if (currentBrowsePage.entries.size() > BrowsePageSize) {
      subtitle = std::string(itemTitle) + " " +
                 std::to_string(first + 1) + "-" +
                 std::to_string(last) + " of " +
                 std::to_string(currentBrowsePage.entries.size());
    }

    items.reserve(std::min(visibleBrowseEntries.size(), MaxBrowseItems));
    for (size_t index = 0;
         index < visibleBrowseEntries.size() && index < MaxBrowseItems;
         ++index) {
      const auto& entry = visibleBrowseEntries[index];
      SwitifyIpcBrowseItem item{};
      switch (entry.type) {
        case BrowseEntryType::Section:
        case BrowseEntryType::PreviousPage:
        case BrowseEntryType::NextPage:
        case BrowseEntryType::PreviousRemotePage:
        case BrowseEntryType::NextRemotePage:
          item.kind = SwitifyBrowseItemKind_Section;
          break;
        case BrowseEntryType::Track:
          item.kind = SwitifyBrowseItemKind_Track;
          item.duration_ms = entry.track.durationMs;
          break;
        case BrowseEntryType::Collection:
          item.kind = collectionKind(entry.collection.kind);
          break;
      }
      copyBrowseText(item.name, sizeof(item.name), entry.name);
      copyBrowseText(item.subtitle, sizeof(item.subtitle), entry.subtitle);
      if (entry.type == BrowseEntryType::Track) {
        copyBrowseText(item.detail, sizeof(item.detail),
                       entry.track.album);
      } else if (entry.type == BrowseEntryType::Collection &&
                 entry.collection.itemCount > 0) {
        const bool podcast = entry.collection.kind ==
                             SpotifyEngine::Collection::Kind::SHOW;
        copyBrowseText(
            item.detail, sizeof(item.detail),
            itemCountText(entry.collection.itemCount,
                          podcast ? "episode" : "item",
                          podcast ? "episodes" : "items"));
      }
      items.push_back(item);
    }
  }
  core.publishBrowse(generation, title, subtitle, items.data(), items.size(),
                     depth, monotonicMs());
}

void BackgroundBackend::failBrowse(uint64_t generation,
                                   const std::string& message) {
  {
    std::scoped_lock lock(browseMutex);
    if (!running.load() || generation != activeBrowseGeneration) return;
  }
  core.publishBrowseError(generation,
                          message.empty() ? "Spotify content unavailable"
                                          : message,
                          monotonicMs());
}

void BackgroundBackend::requestHome() {
  const uint64_t generation =
      beginRoot("Home", "Your Spotify, organized for Switch");
  engine.fetchDashboard(
      [this, generation](SpotifyEngine::Result<SpotifyEngine::HomeData> result) {
        if (!result.ok()) {
          failBrowse(generation, result.error);
          return;
        }
        BrowsePage page = buildHomeRoot(result.value);
        {
          std::scoped_lock lock(browseMutex);
          if (generation != activeBrowseGeneration) return;
          homeCache = std::move(result.value);
          libraryCache = {};
          searchCache = {};
        }
        completeBrowse(generation, std::move(page));
      });
}

void BackgroundBackend::requestLibrary() {
  const uint64_t generation =
      beginRoot("Your Library", "Saved music and followed podcasts");
  engine.fetchLibrary(
      [this, generation](
          SpotifyEngine::Result<SpotifyEngine::LibraryData> result) {
        if (!result.ok()) {
          failBrowse(generation, result.error);
          return;
        }
        BrowsePage page = buildLibraryRoot(result.value);
        {
          std::scoped_lock lock(browseMutex);
          if (generation != activeBrowseGeneration) return;
          libraryCache = std::move(result.value);
          homeCache = {};
          searchCache = {};
        }
        completeBrowse(generation, std::move(page));
      });
}

void BackgroundBackend::requestSearch(const std::string& query) {
  const uint64_t generation =
      beginRoot("Search", query.empty() ? "Enter a search" : query);
  if (query.empty()) {
    failBrowse(generation, "Enter something to search for");
    return;
  }
  engine.search(
      query, [this, generation, query](
                 SpotifyEngine::Result<SpotifyEngine::SearchResults> result) {
        if (!result.ok()) {
          failBrowse(generation, result.error);
          return;
        }
        BrowsePage page = buildSearchRoot(result.value, query);
        {
          std::scoped_lock lock(browseMutex);
          if (generation != activeBrowseGeneration) return;
          searchCache = std::move(result.value);
          homeCache = {};
          libraryCache = {};
          searchQuery = query;
        }
        completeBrowse(generation, std::move(page));
      });
}

BackgroundBackend::BrowsePage BackgroundBackend::buildHomeRoot(
    const SpotifyEngine::HomeData& data) const {
  BrowsePage page;
  page.title = "Home";
  page.subtitle = data.profile.displayName.empty()
                      ? "Your Spotify"
                      : "Welcome, " + data.profile.displayName;
  const auto section = [&page](BrowseSection id, size_t index,
                               const std::string& name,
                               const std::string& subtitle) {
    BrowseEntry entry;
    entry.type = BrowseEntryType::Section;
    entry.section = id;
    entry.sectionIndex = index;
    entry.name = name;
    entry.subtitle = subtitle;
    page.entries.push_back(std::move(entry));
  };
  if (!data.resumeContexts.empty())
    section(BrowseSection::HomeResume, 0, "Jump back in",
            itemCountText(data.resumeContexts.size(), "collection",
                          "collections"));
  if (!data.recentlyPlayed.empty())
    section(BrowseSection::HomeRecent, 0, "Recently played",
            "Pick up where you left off");
  if (!data.playlists.empty())
    section(BrowseSection::HomePlaylists, 0, "Your playlists",
            itemCountText(data.playlists.size(), "playlist", "playlists"));
  return page;
}

BackgroundBackend::BrowsePage BackgroundBackend::buildLibraryRoot(
    const SpotifyEngine::LibraryData& data) const {
  BrowsePage page;
  page.title = "Your Library";
  page.subtitle = "Your saved Spotify collection";
  const auto add = [&page](BrowseSection id, const char* name, size_t count,
                           const char* singular, const char* plural) {
    if (count == 0) return;
    BrowseEntry entry;
    entry.type = BrowseEntryType::Section;
    entry.section = id;
    entry.name = name;
    entry.subtitle = itemCountText(count, singular, plural);
    page.entries.push_back(std::move(entry));
  };
  add(BrowseSection::LibraryPlaylists, "Playlists and liked songs",
      data.playlists.size(), "collection", "collections");
  add(BrowseSection::LibraryPodcasts, "Podcasts", data.podcasts.size(),
      "show", "shows");
  return page;
}

BackgroundBackend::BrowsePage BackgroundBackend::buildSearchRoot(
    const SpotifyEngine::SearchResults& data, const std::string& query) const {
  BrowsePage page;
  page.title = "Search";
  page.subtitle = "Results for " + query;
  const auto add = [&page](BrowseSection id, const char* name, size_t count) {
    if (count == 0) return;
    BrowseEntry entry;
    entry.type = BrowseEntryType::Section;
    entry.section = id;
    entry.name = name;
    entry.subtitle = itemCountText(count, "result", "results");
    page.entries.push_back(std::move(entry));
  };
  add(BrowseSection::SearchTracks, "Tracks", data.tracks.size());
  add(BrowseSection::SearchPlaylists, "Playlists", data.playlists.size());
  return page;
}

BackgroundBackend::BrowsePage BackgroundBackend::buildSection(
    const BrowseEntry& section) const {
  BrowsePage page;
  page.title = section.name;
  page.subtitle = section.subtitle;
  const auto addTrack = [&page](const SpotifyEngine::Track& track) {
    if (track.uri.empty()) return;
    BrowseEntry entry;
    entry.type = BrowseEntryType::Track;
    entry.track = track;
    entry.name = track.name.empty() ? "Unknown track" : track.name;
    entry.subtitle = track.artist;
    page.entries.push_back(std::move(entry));
  };
  const auto addCollection = [&page](const SpotifyEngine::Collection& value) {
    if (value.uri.empty()) return;
    BrowseEntry entry;
    entry.type = BrowseEntryType::Collection;
    entry.collection = value;
    entry.name = value.name.empty() ? "Untitled collection" : value.name;
    entry.subtitle = value.subtitle;
    page.entries.push_back(std::move(entry));
  };

  std::scoped_lock lock(browseMutex);
  switch (section.section) {
    case BrowseSection::HomeResume:
      for (const auto& item : homeCache.resumeContexts)
        addCollection(item.collection);
      break;
    case BrowseSection::HomeRecent:
      for (const auto& item : homeCache.recentlyPlayed) addTrack(item);
      break;
    case BrowseSection::HomePlaylists:
      for (const auto& item : homeCache.playlists) addCollection(item);
      break;
    case BrowseSection::LibraryPlaylists:
      for (const auto& item : libraryCache.playlists) addCollection(item);
      break;
    case BrowseSection::LibraryPodcasts:
      for (const auto& item : libraryCache.podcasts) addCollection(item);
      break;
    case BrowseSection::SearchTracks:
      for (const auto& item : searchCache.tracks) addTrack(item);
      break;
    case BrowseSection::SearchPlaylists:
      for (const auto& item : searchCache.playlists) addCollection(item);
      break;
  }
  return page;
}

BackgroundBackend::BrowsePage BackgroundBackend::buildTracks(
    std::string title, std::string subtitle,
    std::vector<SpotifyEngine::Track> tracks, uint32_t depth) const {
  BrowsePage page;
  page.title = std::move(title);
  page.subtitle = std::move(subtitle);
  page.depth = depth;
  page.episodePage = !tracks.empty() &&
                     std::all_of(tracks.begin(), tracks.end(),
                                 [](const SpotifyEngine::Track& track) {
                                   return track.podcast;
                                 });
  page.entries.reserve(tracks.size());
  for (auto& track : tracks) {
    if (track.uri.empty()) continue;
    BrowseEntry entry;
    entry.type = BrowseEntryType::Track;
    entry.name = track.name.empty() ? "Unknown track" : track.name;
    entry.subtitle = track.artist;
    entry.track = std::move(track);
    page.entries.push_back(std::move(entry));
  }
  return page;
}

BackgroundBackend::BrowsePage BackgroundBackend::buildRemoteTracks(
    const SpotifyEngine::Collection& collection,
    SpotifyEngine::TrackPage tracks, uint32_t depth) const {
  auto page = buildTracks(collection.name, collection.subtitle,
                          std::move(tracks.tracks), depth);
  page.episodePage =
      collection.kind == SpotifyEngine::Collection::Kind::SHOW;
  page.remotelyPaged = true;
  page.remoteHasMore = tracks.hasMore;
  page.remoteOffset = tracks.offset;
  page.remoteNextOffset = tracks.nextOffset;
  page.remoteCollection = collection;
  return page;
}

void BackgroundBackend::openBrowseItem(uint64_t generation,
                                        uint32_t index) {
  BrowseEntry entry;
  {
    std::scoped_lock lock(browseMutex);
    if (generation != activeBrowseGeneration ||
        index >= visibleBrowseEntries.size())
      return;
    entry = visibleBrowseEntries[index];
  }
  if (entry.type == BrowseEntryType::PreviousPage ||
      entry.type == BrowseEntryType::NextPage) {
    changeBrowsePage(entry.sourceIndex);
    return;
  }
  if (entry.type == BrowseEntryType::PreviousRemotePage ||
      entry.type == BrowseEntryType::NextRemotePage) {
    changeRemoteBrowsePage(entry.sourceIndex);
    return;
  }
  if (entry.type == BrowseEntryType::Track) {
    playBrowseItem(generation, index);
    return;
  }
  if (entry.type == BrowseEntryType::Section) {
    const uint64_t next = beginChild(entry.name, entry.subtitle);
    completeBrowse(next, buildSection(entry));
    return;
  }

  const uint64_t next = beginChild(entry.name, entry.subtitle);
  const uint32_t depth = core.browseHeader().depth;
  if (entry.collection.kind == SpotifyEngine::Collection::Kind::LIKED ||
      entry.collection.kind == SpotifyEngine::Collection::Kind::PLAYLIST ||
      entry.collection.kind == SpotifyEngine::Collection::Kind::SHOW) {
    engine.fetchPlaylistTrackPage(
        entry.collection, 0, RemoteTrackPageSize,
        [this, next, depth, collection = entry.collection](
            SpotifyEngine::Result<SpotifyEngine::TrackPage> result) {
          if (!result.ok()) {
            failBrowse(next, result.error);
            return;
          }
          completeBrowse(next, buildRemoteTracks(
                                   collection, std::move(result.value), depth));
        });
    return;
  }
  engine.fetchPlaylistTracks(
      entry.collection,
      [this, next, depth, collection = entry.collection](
          SpotifyEngine::Result<std::vector<SpotifyEngine::Track>> result) {
        if (!result.ok()) {
          failBrowse(next, result.error);
          return;
        }
        completeBrowse(next,
                       buildTracks(collection.name, collection.subtitle,
                                   std::move(result.value), depth));
  });
}

void BackgroundBackend::changeBrowsePage(size_t offset) {
  BrowsePage page;
  uint64_t generation = 0;
  {
    std::scoped_lock lock(browseMutex);
    if (currentBrowsePage.entries.empty()) return;
    page = std::move(currentBrowsePage);
    page.pageOffset = offset;
    visibleBrowseEntries.clear();
    activeBrowseGeneration = core.beginBrowse(
        page.title, page.subtitle, static_cast<uint32_t>(browseHistory.size()),
        monotonicMs());
    generation = activeBrowseGeneration;
  }
  completeBrowse(generation, std::move(page));
}

void BackgroundBackend::changeRemoteBrowsePage(size_t offset) {
  SpotifyEngine::Collection collection;
  std::string title;
  std::string subtitle;
  uint32_t depth = 0;
  uint64_t generation = 0;
  {
    std::scoped_lock lock(browseMutex);
    if (!currentBrowsePage.remotelyPaged ||
        currentBrowsePage.remoteCollection.uri.empty())
      return;
    collection = currentBrowsePage.remoteCollection;
    title = currentBrowsePage.title;
    subtitle = currentBrowsePage.subtitle;
    depth = static_cast<uint32_t>(browseHistory.size());
    visibleBrowseEntries.clear();
    activeBrowseGeneration =
        core.beginBrowse(title, subtitle, depth, monotonicMs());
    generation = activeBrowseGeneration;
  }
  engine.fetchPlaylistTrackPage(
      collection, offset, RemoteTrackPageSize,
      [this, generation, depth, collection = std::move(collection)](
          SpotifyEngine::Result<SpotifyEngine::TrackPage> result) {
        if (!result.ok()) {
          failBrowse(generation, result.error);
          return;
        }
        completeBrowse(generation, buildRemoteTracks(
                                       collection, std::move(result.value),
                                       depth));
      });
}

void BackgroundBackend::backBrowse(uint64_t generation) {
  BrowsePage previous;
  {
    std::scoped_lock lock(browseMutex);
    // Back does not address an item, so it is safe (and much more reliable) to
    // apply it to the page that is current when the command is processed.  A
    // large asynchronous collection such as Liked Songs can advance the
    // generation after the overlay rendered but before its B command reaches
    // this worker.  Rejecting that harmlessly-stale generation left the user
    // trapped on the page.
    if (browseHistory.empty()) return;
    previous = std::move(browseHistory.back());
    browseHistory.pop_back();
    activeBrowseGeneration = core.beginBrowse(
        previous.title, previous.subtitle,
        static_cast<uint32_t>(browseHistory.size()), monotonicMs());
    generation = activeBrowseGeneration;
  }
  completeBrowse(generation, std::move(previous));
}

void BackgroundBackend::playBrowseItem(uint64_t generation,
                                        uint32_t index) {
  BrowsePlayMode playMode = BrowsePlayMode::Tracks;
  std::string pageTitle;
  std::vector<SpotifyEngine::Track> tracks;
  size_t trackIndex = 0;
  {
    std::scoped_lock lock(browseMutex);
    if (generation != activeBrowseGeneration ||
        index >= visibleBrowseEntries.size() ||
        visibleBrowseEntries[index].type != BrowseEntryType::Track)
      return;
    const size_t sourceIndex = visibleBrowseEntries[index].sourceIndex;
    for (size_t cursor = 0; cursor < sourceIndex; ++cursor)
      if (currentBrowsePage.entries[cursor].type == BrowseEntryType::Track)
        ++trackIndex;
    playMode = currentBrowsePage.playMode;
    pageTitle = currentBrowsePage.title;
    if (playMode == BrowsePlayMode::Tracks) {
      tracks.reserve(currentBrowsePage.entries.size());
      for (const auto& entry : currentBrowsePage.entries)
        if (entry.type == BrowseEntryType::Track)
          tracks.push_back(entry.track);
    }
  }
  if (playMode == BrowsePlayMode::Playlist) {
    engine.playPlaylistTrack(trackIndex);
  } else if (playMode == BrowsePlayMode::Queue) {
    engine.playQueueTrack(trackIndex);
  } else {
    engine.playTracks(std::move(tracks), trackIndex, pageTitle);
  }
  nxlog::pushHeapUsage("browse play dispatched");
}

void BackgroundBackend::browseCurrentPlaylist(bool queue) {
  if (queue) {
    const uint64_t generation =
        beginRoot("Queue", "Coming up in this playback session");
    auto page = buildTracks("Queue", "Coming up", engine.currentQueue(), 0);
    page.playMode = BrowsePlayMode::Queue;
    completeBrowse(generation, std::move(page));
    return;
  }
  auto session = engine.playbackSession();
  const std::string title = session.name.empty() ? "Current playlist"
                                                  : session.name;
  const uint64_t generation = beginRoot(title, "Current playback session");
  auto page = buildTracks(title, "Current playback session",
                          std::move(session.tracks), 0);
  page.playMode = BrowsePlayMode::Playlist;
  completeBrowse(generation, std::move(page));
}

SwitifyControlResult BackgroundBackend::processBrowse(
    const ControlMessage& message) {
  if (engine.state() != SpotifyEngine::State::Ready)
    return SwitifyControlResult_BackendUnavailable;
  switch (message.command) {
    case SwitifyIpcCommand_BrowseHome:
      requestHome();
      break;
    case SwitifyIpcCommand_BrowseLibrary:
      requestLibrary();
      break;
    case SwitifyIpcCommand_BrowseSearch:
      requestSearch(std::string(message.query.data(),
                                strnlen(message.query.data(),
                                        message.query.size())));
      break;
    case SwitifyIpcCommand_BrowseOpen:
      openBrowseItem(message.generation, message.value);
      break;
    case SwitifyIpcCommand_BrowseBack:
      backBrowse(message.generation);
      break;
    case SwitifyIpcCommand_BrowsePlay:
      playBrowseItem(message.generation, message.value);
      break;
    case SwitifyIpcCommand_BrowseCurrentPlaylist:
      browseCurrentPlaylist(false);
      break;
    case SwitifyIpcCommand_BrowseQueue:
      browseCurrentPlaylist(true);
      break;
    default:
      return SwitifyControlResult_InvalidCommand;
  }
  return SwitifyControlResult_Ok;
}

void BackgroundBackend::publishArtworkUrl(const std::string& url) {
  if (artworkUrlPublished && lastArtworkUrl == url) return;

  std::ofstream output(ArtworkUrlTemporary, std::ios::trunc);
  if (!output.good()) return;
  output << url;
  output.close();
  if (!output.good()) {
    std::remove(ArtworkUrlTemporary);
    return;
  }
  std::remove(ArtworkUrlFile);
  if (std::rename(ArtworkUrlTemporary, ArtworkUrlFile) != 0) {
    std::remove(ArtworkUrlTemporary);
    return;
  }
  lastArtworkUrl = url;
  artworkUrlPublished = true;
}

void BackgroundBackend::publishSnapshot() {
  const auto state = engine.state();
  const auto playback = engine.nowPlaying();
  const auto session = engine.playbackSessionInfo();
  const uint64_t now = monotonicMs();

  publishArtworkUrl(playback.hasTrack ? playback.imageUrl : std::string{});

  core.publishServiceState(serviceState(state), now);
  core.publishConnection(state == SpotifyEngine::State::Ready,
                         std::ifstream(CredentialsFile).good(), now);
  core.publishTrack(playback.hasTrack ? playback.name : std::string{},
                    playback.hasTrack ? playback.artist : std::string{},
                    playback.hasTrack ? playback.album : engine.statusText(),
                    now);
  core.publishPlaybackState(
      !playback.hasTrack
          ? SwitifyPlaybackState_Stopped
          : (playback.paused ? SwitifyPlaybackState_Paused
                             : SwitifyPlaybackState_Playing),
      playback.positionMs, playback.durationMs, now);
  core.publishVolume(static_cast<uint32_t>(
                         std::clamp(playback.volume, 0.0f, 1.0f) * 1000.0f),
                     now);
  core.publishQueue(session.hasCurrent
                        ? static_cast<uint32_t>(session.currentIndex)
                        : SWITIFY_IPC_INVALID_INDEX,
                    static_cast<uint32_t>(session.trackCount), now);
  core.publishShuffle(playback.shuffle, now);
  SwitifyRepeatMode repeat = SwitifyRepeatMode_Off;
  if (playback.repeatMode == SpotifyEngine::RepeatMode::All)
    repeat = SwitifyRepeatMode_All;
  else if (playback.repeatMode == SpotifyEngine::RepeatMode::One)
    repeat = SwitifyRepeatMode_One;
  core.publishRepeat(repeat, now);
  core.publishAudioQuality(engine.highQualityEnabled(), now);
  core.publishSeekable(playback.hasTrack && playback.durationMs != 0, now);
}

void BackgroundBackend::run() {
  auto nextSnapshot = std::chrono::steady_clock::now();
  auto nextResourceLog =
      std::chrono::steady_clock::now() + ResourceLogInterval;
  nextReconnect = std::chrono::steady_clock::now() + ReconnectInterval;

  while (running.load()) {
    try {
      ControlMessage message;
      for (size_t processed = 0; processed < 8 && core.tryPop(message);
           ++processed)
        process(message);

      const auto now = std::chrono::steady_clock::now();
      if (now >= nextSnapshot) {
        publishSnapshot();
        nextSnapshot = now + SnapshotInterval;
      }
      if (now >= nextResourceLog) {
        nxlog::pushHeapUsage("periodic");
        nextResourceLog = now + ResourceLogInterval;
      }
      if (engine.state() == SpotifyEngine::State::Error &&
          now >= nextReconnect) {
        nxlog::push("Background Spotify reconnect requested");
        engine.retryConnection();
        nextReconnect = now + ReconnectInterval;
      } else if (engine.state() != SpotifyEngine::State::Error) {
        nextReconnect = now + ReconnectInterval;
      }
    } catch (const std::bad_alloc&) {
      uint64_t generation = 0;
      {
        std::scoped_lock lock(browseMutex);
        generation = activeBrowseGeneration;
      }
      nxlog::push("Background request ran out of memory; service retained");
      failBrowse(generation, "Spotify content exceeded available memory");
      core.publishResult(SwitifyControlResult_BackendUnavailable,
                         monotonicMs());
    } catch (const std::exception& error) {
      uint64_t generation = 0;
      {
        std::scoped_lock lock(browseMutex);
        generation = activeBrowseGeneration;
      }
      nxlog::pushf("Background request failed safely: %s", error.what());
      failBrowse(generation, error.what());
      core.publishResult(SwitifyControlResult_BackendUnavailable,
                         monotonicMs());
    } catch (...) {
      uint64_t generation = 0;
      {
        std::scoped_lock lock(browseMutex);
        generation = activeBrowseGeneration;
      }
      nxlog::push("Background request failed safely");
      failBrowse(generation, "Unknown background request failure");
      core.publishResult(SwitifyControlResult_BackendUnavailable,
                         monotonicMs());
    }
    std::this_thread::sleep_for(PollInterval);
  }
}

}  // namespace switify::phase2

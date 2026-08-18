#pragma once

// SpotifyEngine owns the authenticated cspot session, native playback, and
// Spotify's authenticated browse data layer. UI code consumes immutable snapshots and
// never touches cspot directly.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cspot {
class LoginBlob;
class SpircHandler;
struct Context;
}  // namespace cspot
namespace bell {
class WrappedSemaphore;
class MDNSService;
}  // namespace bell
class ZeroconfHttpServer;
class NXPlayer;
class NXAudioSink;

class SpotifyEngine {
 public:
  enum class State { NoConfig, WaitingForPhone, Connecting, Ready, Error };
  enum class RepeatMode { Off, All, One };

  struct Track {
    std::string id;
    std::string name;
    std::string artist;
    std::string album;
    std::string uri;
    std::string imageUrl;
    uint32_t durationMs = 0;
    bool explicitContent = false;
    bool podcast = false;
  };

  struct Collection {
    enum class Kind { PLAYLIST, LIKED, ALBUM, ARTIST, SHOW };

    std::string id;
    std::string uri;
    std::string name;
    std::string subtitle;
    std::string description;
    std::string imageUrl;
    int itemCount = 0;
    Kind kind = Kind::PLAYLIST;
  };

  // Compatibility name retained for the POC-era activity and handover.
  using Playlist = Collection;

  struct Profile {
    std::string displayName;
    std::string id;
    std::string imageUrl;
  };

  struct ResumeContext {
    Collection collection;
    Track lastTrack;
  };

  struct HomeShelf {
    std::string title;
    std::string subtitle;
    std::vector<Collection> items;
  };

  struct NowPlaying {
    std::string id;
    std::string uri;
    std::string name;
    std::string artist;
    std::string album;
    std::string imageUrl;
    uint32_t durationMs = 0;
    uint32_t positionMs = 0;
    float volume = 0.8f;
    bool paused = true;
    bool hasTrack = false;
    bool shuffle = false;
    RepeatMode repeatMode = RepeatMode::Off;
    bool saved = false;
    bool savedKnown = false;
    bool savePending = false;
  };

  struct PlaybackSession {
    std::string name;
    std::vector<Track> tracks;
    size_t currentIndex = 0;
    bool hasCurrent = false;
  };

  // Lightweight session state for frequently refreshed UIs.  The full
  // PlaybackSession copies every track, which is intentionally reserved for
  // infrequent operations such as opening the overlay's playlist browser.
  struct PlaybackSessionInfo {
    std::string name;
    size_t trackCount = 0;
    size_t currentIndex = 0;
    bool hasCurrent = false;
    uint64_t revision = 0;
  };

  struct TrackPage {
    std::vector<Track> tracks;
    size_t offset = 0;
    size_t nextOffset = 0;
    size_t totalCount = 0;
    bool hasMore = false;
  };

  struct HomeData {
    Profile profile;
    std::vector<ResumeContext> resumeContexts;
    std::vector<Track> recentlyPlayed;
    std::vector<Track> topTracks;
    std::vector<Collection> playlists;
    std::vector<Collection> podcasts;
    std::vector<HomeShelf> shelves;
    std::string warning;
  };

  struct LibraryData {
    std::vector<Collection> playlists;
    std::vector<Collection> albums;
    std::vector<Collection> podcasts;
  };

  struct SearchResults {
    std::vector<Track> tracks;
    std::vector<Track> episodes;
    std::vector<Collection> albums;
    std::vector<Collection> artists;
    std::vector<Collection> playlists;
    std::vector<Collection> podcasts;
  };

  template <typename T>
  struct Result {
    T value{};
    std::string error;
    bool ok() const { return error.empty(); }
  };

  using ObserverId = uint64_t;

  static SpotifyEngine& instance();

  // The standalone NRO enables phone pairing. Headless consumers disable it
  // and require credentials already created by the NRO.
  void setInteractiveAuthentication(bool enabled);
  void start();
  void retryConnection();
  void shutdown();

  void playPause();
  void next();
  void previous();
  void seekToMs(uint32_t positionMs);
  void setVolume(float value);
  void setHighQuality(bool enabled);
  bool highQualityEnabled();
  void toggleShuffle();
  void cycleRepeatMode();
  void setRepeatMode(RepeatMode mode);
  void setEqualizer(bool enabled, float bassDb, float midDb, float trebleDb);
  bool forgetCredentials();

  State state() const;
  std::string statusText();
  Profile profile();
  NowPlaying nowPlaying();
  PlaybackSession playbackSession();
  PlaybackSessionInfo playbackSessionInfo();
  std::vector<Track> playbackSessionPage(size_t offset, size_t count);
  std::vector<Track> currentQueue();

  // Synchronous memory-cache lookup for UI construction on the render
  // thread.  This avoids showing a loading screen when a collection was
  // already fetched earlier in the same session.
  bool cachedDashboard(HomeData& dashboard);
  bool cachedLibrary(LibraryData& library);
  bool cachedPlaylistTracks(const Collection& collection,
                            std::vector<Track>& tracks);

  // All callbacks run on a worker thread.  Borealis callers must hop back to
  // the render thread with brls::sync().
  void fetchDashboard(std::function<void(Result<HomeData>)> cb,
                      bool forceRefresh = false);
  void fetchLibrary(std::function<void(Result<LibraryData>)> cb,
                    bool forceRefresh = false);
  void fetchPlaylistTracks(
      const Collection& collection,
      std::function<void(Result<std::vector<Track>>)> cb);
  // Fetches one Spotify source page without waiting for the entire collection.
  // The fixed-heap background service uses this for incremental browsing; the
  // standalone app deliberately keeps complete collections in its session
  // cache and materializes their UI rows in batches.
  void fetchPlaylistTrackPage(
      const Collection& collection, size_t offset, size_t limit,
      std::function<void(Result<TrackPage>)> cb);
  // Builds a fixed-size queue whose candidates are sampled from the complete
  // remote collection. This keeps the overlay's memory bounded without making
  // its visible 50-row page the effective Shuffle universe.
  void fetchShuffledCollectionWindow(
      const Collection& collection, std::vector<Track> visiblePage,
      size_t selectedIndex, size_t maximumTracks,
      std::function<void(Result<std::vector<Track>>)> cb);
  void search(const std::string& query,
              std::function<void(Result<SearchResults>)> cb);
  void setTrackSaved(const std::string& uri, bool saved,
                     std::function<void(Result<bool>)> cb);

  void playTracks(std::vector<Track> tracks, size_t startIndex,
                  const std::string& sourceName = "",
                  uint32_t positionMs = 0);
  void playPlaylistTrack(size_t index);
  void playQueueTrack(size_t upcomingIndex);

  ObserverId subscribe(std::function<void()> cb);
  void unsubscribe(ObserverId id);

 private:
  SpotifyEngine() = default;

  void run();
  void launchWorker(std::function<void()> work);
  void setState(State state, const std::string& status);
  void notifyUpdate();
  uint32_t computedPositionLocked() const;
  void savePlaybackSession();
  bool restorePlaybackSession();
  void refreshTrackSavedState(const std::string& uri);
  bool applyPendingShuffleLocked();
  void schedulePendingShuffleRetry();
  bool waitForApiRateLimitWindow();

  std::string userToken(const std::string& scopes);
  std::string spotifyClientToken();
  std::string login5AccessToken();
  std::string partnerRequest(const std::string& operation,
                             const std::string& variablesJson);
  std::string connectRequest(const std::string& url);
  std::string apiRequest(const std::string& method,
                         const std::string& path,
                         const std::string& jsonBody = "");

  std::thread engineThread;
  std::atomic<bool> running{false};
  std::atomic<bool> interactiveAuthentication{true};
  std::atomic<State> currentState{State::NoConfig};
  std::atomic<bool> pausedState{true};

  std::mutex dataMutex;
  std::string status;
  Profile profileData;
  HomeData dashboardCache;
  LibraryData libraryCache;
  bool hasDashboardCache = false;
  bool hasLibraryCache = false;
  std::unordered_map<std::string, std::vector<Track>> collectionTracksCache;
  std::deque<std::string> collectionTrackCacheOrder;
  size_t cachedCollectionTrackCount = 0;
  std::unordered_map<std::string, bool> likedStateCache;
  std::unordered_set<std::string> likedStatePending;
  std::unordered_set<std::string> likedMutationPending;
  std::unordered_map<std::string, uint64_t> likedStateGeneration;
  NowPlaying nowPlayingData;
  float advertisedVolume = 0.8f;
  bool highQuality = false;
  uint32_t positionBaseMs = 0;
  std::chrono::steady_clock::time_point positionMeasuredAt{};
  std::string activeSourceName;
  std::vector<Track> activePlaylist;
  size_t activePlaylistIndex = 0;
  bool activePlaylistHasCurrent = false;
  uint64_t activePlaylistRevision = 1;
  std::vector<Track> activeQueue;
  size_t activeQueueIndex = 0;
  bool activeQueueHasCurrent = false;
  // Shuffle can be requested before cspot has promoted the current stream to
  // the active queue. Retry it on TRACK_INFO so its visual and audible state
  // cannot silently diverge.
  bool shuffleReorderPending = false;
  std::atomic<bool> shuffleRetryWorkerActive{false};
  // A persisted session is restored as paused UI state. The first explicit
  // Play command loads it into cspot at the saved track and position.
  bool restoredSessionPendingStart = false;
  bool equalizerEnabled = false;
  float equalizerBassDb = 0.0f;
  float equalizerMidDb = 0.0f;
  float equalizerTrebleDb = 0.0f;
  std::mutex playbackPersistenceMutex;

  std::mutex observerMutex;
  std::map<ObserverId, std::function<void()>> observers;
  std::atomic<ObserverId> nextObserverId{1};

  std::mutex workerMutex;
  struct DataWorker {
    std::thread thread;
    std::shared_ptr<std::atomic_bool> completed;
  };
  std::vector<DataWorker> dataWorkers;

  std::mutex retryMutex;
  std::condition_variable retryCondition;
  bool retryRequested = false;

  std::mutex tokenMutex;
  struct CachedUserToken {
    std::string token;
    long long expiresAt = 0;
    long long retryAfter = 0;
  };
  std::unordered_map<std::string, CachedUserToken> userTokenCache;
  size_t preferredTokenClient = 0;
  std::string cachedClientToken;
  long long clientTokenExpiresAt = 0;
  std::string cachedLogin5Token;
  long long login5TokenExpiresAt = 0;
  long long login5RetryAfter = 0;
  std::atomic<long long> apiRetryAfter{0};

  std::shared_ptr<cspot::LoginBlob> loginBlob;
  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<cspot::SpircHandler> handler;
  std::shared_ptr<NXPlayer> player;
  NXAudioSink* switchAudioSink = nullptr;
  std::unique_ptr<ZeroconfHttpServer> zeroconfServer;
  std::unique_ptr<bell::MDNSService> mdnsService;
  std::shared_ptr<bell::WrappedSemaphore> loggedInSemaphore;
};

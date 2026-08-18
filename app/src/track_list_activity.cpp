#include "track_list_activity.hpp"

#include "platform_controls.hpp"
#include "playback_shortcuts.hpp"
#include "tabs.hpp"
#include "ui_helpers.hpp"

#include <unordered_set>

TrackListActivity::TrackListActivity(SpotifyEngine::Collection collection)
    : collection(std::move(collection)),
      alive(std::make_shared<std::atomic_bool>(true)) {}

TrackListActivity::~TrackListActivity() { alive->store(false); }

void TrackListActivity::onContentAvailable() {
  registerPlaybackShortcuts(this);
  registerAction(
      "Player", brls::BUTTON_START,
      [](brls::View*) {
        showTheaterMode();
        return true;
      });
  registerAction(
      "Screen off", brls::BUTTON_BACK,
      [](brls::View*) {
        if (!swplatform::toggleScreenOff())
          brls::Application::notify(
              "Screen-off mode is unavailable on this console");
        return true;
      });

  title->setText(collection.name);
  subtitle->setText(collection.subtitle.empty() ? "Spotify collection"
                                                 : collection.subtitle);
  description->setText(collection.description);
  description->setVisibility(collection.description.empty()
                                 ? brls::Visibility::GONE
                                 : brls::Visibility::VISIBLE);
  if (!collection.imageUrl.empty())
    swui::loadRemoteImage(artwork, collection.imageUrl);

  playAll->setFocusable(false);
  playAll->setBackgroundColor(swui::surfaceRaised());
  playAll->registerClickAction([this](brls::View*) {
    if (!currentTracks.empty())
      SpotifyEngine::instance().playTracks(currentTracks, 0, collection.name);
    return true;
  });
  playAll->addGestureRecognizer(new brls::TapGestureRecognizer(playAll));
  playAll->getFocusEvent()->subscribe([this](brls::View*) {
    playAllLabel->setTextColor(swui::textPrimary());
  });
  playAll->getFocusLostEvent()->subscribe([this](brls::View*) {
    playAllLabel->setTextColor(nvgRGB(8, 20, 12));
  });

  std::vector<SpotifyEngine::Track> cachedTracks;
  if (SpotifyEngine::instance().cachedPlaylistTracks(collection,
                                                     cachedTracks)) {
    SpotifyEngine::Result<std::vector<SpotifyEngine::Track>> cached;
    cached.value = std::move(cachedTracks);
    showTracks(std::move(cached));
    return;
  }

  list->addView(swui::makeStatePanel(
      "Loading tracks", "Fetching this collection from Spotify."));

  auto token = alive;
  if (collection.kind == SpotifyEngine::Collection::Kind::LIKED ||
      collection.kind == SpotifyEngine::Collection::Kind::PLAYLIST) {
    remotePagination = true;
    SpotifyEngine::instance().fetchPlaylistTrackPage(
        collection, 0, 50,
        [this, token](SpotifyEngine::Result<SpotifyEngine::TrackPage> result) mutable {
          brls::sync([this, token, result = std::move(result)]() mutable {
            if (!token->load()) return;
            showTrackPage(std::move(result), true);
          });
        });
  } else {
    SpotifyEngine::instance().fetchPlaylistTracks(
        collection,
        [this, token](SpotifyEngine::Result<std::vector<SpotifyEngine::Track>> result) mutable {
          brls::sync([this, token, result = std::move(result)]() mutable {
            if (!token->load()) return;
            showTracks(std::move(result));
          });
        });
  }
}

void TrackListActivity::showTracks(
    SpotifyEngine::Result<std::vector<SpotifyEngine::Track>> result) {
  list->clearViews();
  if (!result.ok()) {
    subtitle->setText("Could not load this collection");
    list->addView(swui::makeStatePanel("Spotify returned an error", result.error,
                                       nvgRGB(55, 31, 36)));
    return;
  }
  currentTracks = std::move(result.value);
  if (currentTracks.empty()) {
    subtitle->setText("No playable tracks");
    list->addView(swui::makeStatePanel(
        "This collection is empty",
        "Spotify did not return any playable items for this collection."));
    return;
  }

  remotePagination = false;
  remoteHasMore = false;
  remoteLoadPending = false;
  updateTrackSummary();
  const bool podcast = collection.kind == SpotifyEngine::Collection::Kind::SHOW;
  playAllLabel->setText(podcast ? "Play episodes" : "Play all");
  playAll->setFocusable(true);
  playAll->setBackgroundColor(swui::accent());
  playAllLabel->setTextColor(playAll->isFocused() ? swui::textPrimary()
                                                  : nvgRGB(8, 20, 12));

  renderedTrackCount = 0;
  loadMoreButton = nullptr;
  appendTrackRows();
}

void TrackListActivity::showTrackPage(
    SpotifyEngine::Result<SpotifyEngine::TrackPage> result, bool firstPage) {
  remoteLoadPending = false;
  if (!result.ok()) {
    if (firstPage) {
      list->clearViews();
      subtitle->setText("Could not load this collection");
      list->addView(swui::makeStatePanel("Spotify returned an error",
                                         result.error, nvgRGB(55, 31, 36)));
    } else {
      if (loadMoreButton) loadMoreButton->setFocusable(true);
      brls::Application::notify(result.error);
    }
    return;
  }

  if (firstPage) {
    list->clearViews();
    currentTracks.clear();
    renderedTrackCount = 0;
    loadMoreButton = nullptr;
  } else if (loadMoreButton) {
    auto* previousButton = loadMoreButton;
    loadMoreButton = nullptr;
    list->removeView(previousButton);
  }

  std::unordered_set<std::string> existing;
  existing.reserve(currentTracks.size() + result.value.tracks.size());
  for (const auto& track : currentTracks) existing.insert(track.uri);
  for (auto& track : result.value.tracks) {
    if (!track.uri.empty() && existing.insert(track.uri).second)
      currentTracks.push_back(std::move(track));
  }
  remoteNextOffset = result.value.nextOffset;
  remoteHasMore = result.value.hasMore;

  if (currentTracks.empty()) {
    subtitle->setText("No playable tracks");
    list->addView(swui::makeStatePanel(
        "This collection is empty",
        "Spotify did not return any playable items for this collection."));
    return;
  }

  playAllLabel->setText("Play loaded songs");
  playAll->setFocusable(true);
  playAll->setBackgroundColor(swui::accent());
  playAllLabel->setTextColor(playAll->isFocused() ? swui::textPrimary()
                                                  : nvgRGB(8, 20, 12));
  updateTrackSummary();
  appendTrackRows(!firstPage);
}

void TrackListActivity::requestNextTrackPage() {
  if (!remotePagination || !remoteHasMore || remoteLoadPending) return;
  remoteLoadPending = true;
  if (loadMoreButton) loadMoreButton->setFocusable(false);
  auto token = alive;
  SpotifyEngine::instance().fetchPlaylistTrackPage(
      collection, remoteNextOffset, 50,
      [this, token](SpotifyEngine::Result<SpotifyEngine::TrackPage> result) mutable {
        brls::sync([this, token, result = std::move(result)]() mutable {
          if (!token->load()) return;
          showTrackPage(std::move(result), false);
        });
      });
}

void TrackListActivity::updateTrackSummary() {
  const bool podcast = collection.kind == SpotifyEngine::Collection::Kind::SHOW;
  std::string detail = std::to_string(currentTracks.size());
  if (remotePagination && remoteHasMore) detail += "+";
  detail += podcast ? " episodes" : " songs loaded";
  if (!collection.subtitle.empty()) detail += "  |  " + collection.subtitle;
  subtitle->setText(detail);
}

void TrackListActivity::appendTrackRows(bool focusFirstNew) {
  constexpr size_t RenderBatchSize = 100;
  const size_t end =
      std::min(currentTracks.size(), renderedTrackCount + RenderBatchSize);
  brls::View* firstNew = nullptr;

  for (size_t index = renderedTrackCount; index < end; ++index) {
    auto* row = new swui::TrackRow();
    row->setTrack(currentTracks[index], index, [this, index] {
      SpotifyEngine::instance().playTracks(currentTracks, index,
                                           collection.name);
    });
    list->addView(row);
    if (!firstNew) firstNew = row;
  }
  renderedTrackCount = end;

  if (renderedTrackCount < currentTracks.size()) {
    const size_t remaining = currentTracks.size() - renderedTrackCount;
    loadMoreButton = swui::makeActionButton(
        std::string("Load more ") +
            (collection.kind == SpotifyEngine::Collection::Kind::SHOW
                 ? "episodes ("
                 : "songs (") +
            std::to_string(remaining) + " remaining)",
        [this, token = alive](brls::View*) {
          if (!token->load() || !loadMoreButton) return true;
          auto* previousButton = loadMoreButton;
          loadMoreButton = nullptr;
          previousButton->setFocusable(false);
          brls::sync([this, token, previousButton] {
            if (!token->load()) return;
            list->removeView(previousButton);
            appendTrackRows(true);
          });
          return true;
        });
    loadMoreButton->setMarginTop(10);
    list->addView(loadMoreButton);
  } else if (remotePagination && remoteHasMore) {
    loadMoreButton = swui::makeActionButton(
        "Load next 50 songs", [this](brls::View*) {
          requestNextTrackPage();
          return true;
        });
    loadMoreButton->setMarginTop(10);
    list->addView(loadMoreButton);
  }

  if (focusFirstNew && firstNew) brls::Application::giveFocus(firstNew);
}
